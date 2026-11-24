// Native epoch admission and reclamation support, independent of transactions.
#include "runtime/epoch.hh"
#include "macros.h"
#include <limits>
#include <unistd.h>
#include <rusty/mutex.hpp>
#include <rusty/thread.hpp>
#include <rusty/option.hpp>

mako::EpochRuntime::Slot mako::EpochRuntime::tinfo[MAX_THREADS];
mako::EpochRuntime::State mako::EpochRuntime::global_epochs{1, 0, false};
mako::EpochRuntime::epoch_advance_callback_type mako::EpochRuntime::epoch_advance_callback;

namespace {

rusty::Mutex<int> epoch_advance_callback_mutex{0};

class epoch_advancer_owner {
public:
    using entry_type = void (*)();

    explicit epoch_advancer_owner(entry_type entry) {
        mako::EpochRuntime::global_epochs.run.store(true, std::memory_order_release);
        try {
            worker_ = rusty::Some(rusty::thread::spawn(entry));
        } catch (...) {
            mako::EpochRuntime::global_epochs.run.store(false,
                                                  std::memory_order_release);
            throw;
        }
    }

    epoch_advancer_owner(const epoch_advancer_owner&) = delete;
    epoch_advancer_owner& operator=(const epoch_advancer_owner&) = delete;

    ~epoch_advancer_owner() {
        mako::EpochRuntime::global_epochs.run.store(false, std::memory_order_release);
        if (worker_.is_some())
            worker_.take().unwrap().join();
    }

private:
    rusty::Option<rusty::thread::JoinHandle<rusty::thread::Unit>> worker_;
};

} // namespace

void mako::EpochRuntime::set_epoch_advance_callback(
    epoch_advance_callback_type callback) {
    auto lock = epoch_advance_callback_mutex.lock().unwrap();
    epoch_advance_callback = std::move(callback);
}

// @safe: C++ guarantees thread-safe, one-shot initialization of local statics
void mako::EpochRuntime::start_epoch_advancer() {
    static epoch_advancer_owner owner(&mako::EpochRuntime::epoch_advancer_loop);
    (void)owner;
}

// @unsafe - shared by ordinary, helper, transfer and replay initialization.
// TThread::assign_stable_id owns the one process-wide STO/RCU slot counter
// (also behind mako::silo::try_allocate_thread_id), so every adapter draws
// from the same space and two workers can never publish to one tinfo slot.
// Slots are never recycled while a prior worker might still publish to them.
void mako::EpochRuntime::register_thread() {
    (void)TThread::assign_stable_id();
}

// @unsafe - one worker owns its slot/depth. Sequential consistency orders entry
// against cut scans: an announcement missed by a scan must fail the recheck.
mako::EpochRuntime::epoch_type mako::EpochRuntime::epoch_enter() {
    const int worker = TThread::id();
    ALWAYS_ASSERT(worker >= 0 && worker < MAX_THREADS);
    auto& thr = tinfo[worker];
    if (thr.epoch_depth != 0) {
        if (thr.epoch_depth == std::numeric_limits<unsigned>::max()) return 0;
        ++thr.epoch_depth;
        return thr.epoch.load(std::memory_order_seq_cst);
    }
    for (;;) {
        const auto sampled = global_epochs.global_epoch.load(std::memory_order_seq_cst);
        if (sampled == std::numeric_limits<epoch_type>::max()) return 0;
        thr.epoch.store(sampled, std::memory_order_seq_cst);
        if (global_epochs.global_epoch.load(std::memory_order_seq_cst) == sampled) {
            thr.epoch_depth = 1;
            return sampled;
        }
        thr.epoch.store(0, std::memory_order_seq_cst);
    }
}

// @unsafe - the oldest announcement survives until every nested owner leaves.
void mako::EpochRuntime::epoch_leave() {
    const int worker = TThread::id();
    ALWAYS_ASSERT(worker >= 0 && worker < MAX_THREADS);
    auto& thr = tinfo[worker];
    ALWAYS_ASSERT(thr.epoch_depth != 0);
    if (--thr.epoch_depth == 0)
        thr.epoch.store(0, std::memory_order_seq_cst);
}

// @safe - the timer and migration fences advance the same nonwrapping clock.
mako::EpochRuntime::epoch_type mako::EpochRuntime::epoch_cut() {
    auto old = global_epochs.global_epoch.load(std::memory_order_seq_cst);
    for (;;) {
        if (old == std::numeric_limits<epoch_type>::max()) return 0;
        if (global_epochs.global_epoch.compare_exchange_weak(
                old, old + 1, std::memory_order_seq_cst))
            return old + 1;
    }
}

// @safe - observes existing announcements without acquiring an epoch guard.
bool mako::EpochRuntime::epoch_drained(epoch_type cut) {
    if (cut == 0 || cut > epoch_current()) return false;
    for (const auto& thr : tinfo) {
        const auto announced = thr.epoch.load(std::memory_order_seq_cst);
        if (announced != 0 && announced < cut) return false;
    }
    return true;
}

// @safe - observation is not admission; storage callers must enter a guard.
mako::EpochRuntime::epoch_type mako::EpochRuntime::epoch_current() {
    return global_epochs.global_epoch.load(std::memory_order_seq_cst);
}

// @unsafe - process-wide timer owned by start_epoch_advancer(); reclamation
// follows actual announcements, never an assumed grace period inferred from
// elapsed timer ticks. The timer and migration fences advance the same clock
// through epoch_cut(), so a concurrent cut is never overwritten.
void mako::EpochRuntime::epoch_advancer_loop() {

    usleep(100000);
    while (global_epochs.run.load(std::memory_order_acquire)) {
        const epoch_type cut = epoch_cut();
        if (cut == 0) break;
        epoch_type oldest = cut;
        for (const auto& thr : tinfo) {
            const epoch_type announced =
                thr.epoch.load(std::memory_order_seq_cst);
            if (announced != 0 && announced < oldest) oldest = announced;
        }
        global_epochs.active_epoch.store(oldest, std::memory_order_seq_cst);

        epoch_advance_callback_type callback;
        {
            auto lock = epoch_advance_callback_mutex.lock().unwrap();
            callback = epoch_advance_callback;
        }
        if (callback)
            callback(cut);

        usleep(100000);
    }
}
