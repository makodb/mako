#undef NDEBUG
#include "Transaction.hh"
#include "clp.h"
#include <stdlib.h>
#include <inttypes.h>
#include <rusty/box.hpp>

uint64_t nallocated;
uint64_t nfreed;
double delay;
std::atomic<bool> stop{false};

class Tracker {
public:
    Tracker() {
        __sync_fetch_and_add(&nallocated, 1);
    }
    ~Tracker() {
        __sync_fetch_and_add(&nfreed, 1);
    }
};


void* tracker_run(void* argument) {
    int tid = reinterpret_cast<uintptr_t>(argument);
    TThread::set_id(tid);

    while (!stop) {
        double this_delay = delay + (drand48() - 0.5) * delay / 3;
        usleep(useconds_t(this_delay * 0.7e6));
        TRANSACTION {
            Transaction::rcu_delete(new Tracker);
            usleep(useconds_t(this_delay * 0.3e6));
        } RETRY(false);
    }
    return nullptr;
}


struct EpochWorker {
    unsigned owner;
    std::atomic<unsigned>* entered;
    std::atomic<bool> release{false};
    int slot = -1;
};

// @unsafe - legacy pthread test harness; worker arguments outlive joined threads.
static void* epoch_worker(void* argument) {
    auto& worker = *static_cast<EpochWorker*>(argument);
    TThread::set_shard_index(worker.owner);
    Transaction::register_thread();
    worker.slot = TThread::id();
    Transaction::register_thread();
    assert(TThread::id() == worker.slot);
    assert(Transaction::epoch_enter() != 0);
    worker.entered->fetch_add(1);
    while (!worker.release.load()) usleep(1);
    Transaction::epoch_leave();
    return nullptr;
}

// @unsafe - colocated owners must never overwrite one another's STO slot.
static void test_process_worker_slots() {
    std::atomic<unsigned> entered{0};
    EpochWorker first{0, &entered}, second{1, &entered};
    pthread_t first_thread, second_thread;
    assert(pthread_create(&first_thread, nullptr, epoch_worker, &first) == 0);
    assert(pthread_create(&second_thread, nullptr, epoch_worker, &second) == 0);
    while (entered.load() != 2) usleep(1);
    assert(first.slot != second.slot);
    const auto cut = Transaction::epoch_cut();
    assert(cut != 0 && !Transaction::epoch_drained(cut));
    first.release.store(true);
    pthread_join(first_thread, nullptr);
    assert(!Transaction::epoch_drained(cut));
    second.release.store(true);
    pthread_join(second_thread, nullptr);
    assert(Transaction::epoch_drained(cut));
}

// @unsafe - exercises the real STO clock and worker announcement, without a
// second simulated clock or a transaction registry.
static void test_epoch_guards() {
    TThread::set_id(MAX_THREADS - 1);
    TThread::set_mode(0);
    auto& slot = Transaction::tinfo[TThread::id()];

    const auto retained = Transaction::epoch_enter();
    assert(retained != 0);
    const auto cut = Transaction::epoch_cut();
    assert(cut > retained && !Transaction::epoch_drained(cut));
    assert(Transaction::epoch_enter() == retained);
    Transaction::epoch_leave();
    Transaction::rcu_quiesce();
    assert(!Transaction::epoch_drained(cut));

    // A newer worker can still reference an object retired by the old guard.
    TThread::set_id(MAX_THREADS - 2);
    assert(Transaction::epoch_enter() == cut);
    TThread::set_id(MAX_THREADS - 1);
    bool reclaimed = false;
    Transaction::rcu_call([](void* pointer) {
        *static_cast<bool*>(pointer) = true;
    }, &reclaimed);

    // A logical context retains its oldest epoch across complete OCC attempts.
    Sto::start_transaction();
    assert(slot.epoch.load() == retained);
    Sto::silent_abort();
    assert(!Transaction::epoch_drained(cut));
    Sto::start_transaction();
    assert(slot.epoch.load() == retained);
    Sto::silent_abort();
    Transaction::epoch_leave();
    assert(Transaction::epoch_drained(cut));
    assert(Transaction::epoch_enter() >= cut);
    assert(Transaction::epoch_drained(cut));
    Transaction::epoch_leave();
    slot.rcu_set.clean_until(cut);
    assert(!reclaimed);
    const auto retirement_cut = Transaction::epoch_cut();
    assert(!Transaction::epoch_drained(retirement_cut));
    TThread::set_id(MAX_THREADS - 2);
    Transaction::epoch_leave();
    TThread::set_id(MAX_THREADS - 1);
    assert(Transaction::epoch_drained(retirement_cut));
    slot.rcu_set.clean_until(retirement_cut);
    assert(reclaimed);

    auto saved_start = std::move(slot.trans_start_callback);
    auto saved_end = std::move(slot.trans_end_callback);
    unsigned starts = 0, ends = 0;
    Transaction::epoch_type engine_cut = 0;
    slot.trans_start_callback = [&] {
        assert(slot.epoch.load() != 0);
        ++starts;
    };
    slot.trans_end_callback = [&] {
        assert(!Transaction::epoch_drained(engine_cut));
        ++ends;
    };

    // Idle helper resets do not enter either STO or Masstree RCU. The actual
    // request enters lazily, including when sharding admission is disabled.
    TThread::set_mode(1);
    Sto::start_transaction();
    engine_cut = Transaction::epoch_cut();
    assert(starts == 0 && Transaction::epoch_drained(engine_cut));
    TThread::txn->ensure_epoch();
    TThread::txn->ensure_epoch();
    assert(starts == 1);
    engine_cut = Transaction::epoch_cut();
    Transaction::rcu_quiesce();
    assert(!Transaction::epoch_drained(engine_cut));
    Sto::silent_abort();
    assert(!TThread::txn->in_progress() && !Sto::in_progress());
    Sto::silent_abort();
    assert(ends == 1 && Transaction::epoch_drained(engine_cut));

    // Failure while starting an engine callback cannot leak its new guard.
    TThread::set_mode(0);
    slot.trans_start_callback = [] { throw Transaction::Abort(); };
    bool failed = false;
    try {
        Sto::start_transaction();
    } catch (Transaction::Abort&) {
        failed = true;
    }
    assert(failed && slot.epoch.load() == 0 && slot.epoch_depth == 0);
    assert(ends == 1);
    slot.trans_start_callback = std::move(saved_start);
    slot.trans_end_callback = std::move(saved_end);
    auto transaction = rusty::Box<Transaction>::from_raw(TThread::txn);
    TThread::txn = nullptr;
}

static const Clp_Option options[] = {
    { "delay", 'd', 'd', Clp_ValDouble, Clp_Negate },
    { "nthreads", 'j', 'j', Clp_ValInt, 0 },
    { "nepochs", 'e', 'e', Clp_ValInt, 0 }
};

// @unsafe - joins real pthread workers and the advancer before final reclamation.
int main(int argc, char* argv[]) {
    unsigned nthreads = 4;
    TRcuSet::epoch_type nepochs = 10;
    delay = 0.000001;

    Clp_Parser *clp = Clp_NewParser(argc, argv, arraysize(options), options);
    int opt;
    while ((opt = Clp_Next(clp)) != Clp_Done) {
        switch (opt) {
        case 'd':
            delay = clp->val.d;
            break;
        case 'j':
            nthreads = clp->val.i;
            break;
        case 'e':
            nepochs = clp->val.i;
            break;
        default:
            abort();
        }
    }
    Clp_DeleteParser(clp);

    if (nthreads > MAX_THREADS) {
        printf("Asked for %d threads but MAX_THREADS is %d\n", nthreads, MAX_THREADS);
        exit(1);
    }

    test_process_worker_slots();
    test_epoch_guards();
    const auto starting_epoch = Transaction::epoch_current();
    pthread_t tids[nthreads];
    for (uintptr_t i = 0; i < nthreads; ++i)
        pthread_create(&tids[i], NULL, tracker_run, reinterpret_cast<void*>(i));
    pthread_t advancer;
    pthread_create(&advancer, NULL, Transaction::epoch_advancer, NULL);
    // Join the advancer before the final reclamation scan.

    while (Transaction::epoch_current() < starting_epoch + nepochs)
        usleep(useconds_t(delay * 1e6));
    stop = true;

    for (unsigned i = 0; i < nthreads; ++i)
        pthread_join(tids[i], NULL);
    Transaction::global_epochs.run.store(false);
    pthread_join(advancer, nullptr);
    const auto final_cut = Transaction::epoch_cut();
    assert(final_cut != 0 && Transaction::epoch_drained(final_cut));

    auto nfreed_before = nfreed;
    for (unsigned i = 0; i < nthreads; ++i)
        Transaction::tinfo[i].rcu_set.clean_until(final_cut);

    printf("created %" PRIu64 ", deleted %" PRIu64 ", finally deleted %" PRIu64 "\n", nallocated, nfreed_before, nfreed);
    assert(nallocated == nfreed);
}
