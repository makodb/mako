#pragma once

#include "runtime/worker_context.hh"

#include <atomic>
#include <functional>

namespace mako {

// Native admission and deferred reclamation. The transaction algorithm and
// its read/write sets live in Rust and do not belong to this runtime.
class EpochRuntime {
public:
    using epoch_type = uint64_t;
    struct alignas(128) Slot {
        std::atomic<epoch_type> epoch{0};
        unsigned epoch_depth = 0;
    };
    struct State {
        std::atomic<epoch_type> global_epoch;
        std::atomic<epoch_type> active_epoch;
        std::atomic<bool> run;
    };
    static Slot tinfo[MAX_THREADS];
    static State global_epochs;
    using epoch_advance_callback_type = std::function<void(epoch_type)>;

    static void register_thread();
    static void set_epoch_advance_callback(epoch_advance_callback_type callback);
    static void start_epoch_advancer();
    static epoch_type epoch_enter();
    static void epoch_leave();
    static epoch_type epoch_cut();
    static bool epoch_drained(epoch_type cut);
    static epoch_type epoch_current();

    // @unsafe - Only the owning worker changes its nesting and deferred queue.
    static void rcu_quiesce() {
        auto& slot = tinfo[TThread::id()];
        if (slot.epoch_depth == 0)
            slot.epoch.store(0, std::memory_order_seq_cst);
    }
private:
    static void epoch_advancer_loop();
    static epoch_advance_callback_type epoch_advance_callback;
};

} // namespace mako
