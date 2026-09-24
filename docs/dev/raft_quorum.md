# `RaftQuorum<Reply>` — design + user manual

## Why

Phase 8.0 collapsed `TransportFacade` to fiber-synchronous: each per-peer
`send_*(dst, req)` returns its `Reply` by value, blocking the caller's
fiber on an `srpc::IntEvent` until the reply lands. That removes the
need for the legacy `RaftVoteQuorumEvent` / `SendAppendEntriesResults`
broadcast machinery on the facade itself.

But callers still need a *quorum* primitive: when the leader sends N
per-peer RPCs in parallel sub-fibers, it has to wait until enough
replies have come back to make a decision (e.g. majority vote, majority
append). Phase 8.1 routes outbound RPCs through `TransportProxy`, and
the very first piece needed is an N-replies-with-timeout aggregator —
that's `RaftQuorum<Reply>`.

This file is a small, single-header utility whose contract is:

1. Build the quorum with the total number of peer fibers we'll spawn
   (`n_total`) and the threshold needed to declare quorum (`n_needed`).
2. Each sub-fiber, on RPC completion, calls `on_reply(from, reply)`.
3. The leader fiber calls `wait_until_quorum(timeout_us)` and then
   `collect()` to drain the replies it received.

## Threading model

In srpc's reactor model, all fibers within one `PollThread` execute
cooperatively on a single OS thread — there's no true parallelism among
sub-fibers. The leader spawns N sub-fibers, each of which yields on its
own RPC's `IntEvent`. When an RPC reply arrives, the corresponding
sub-fiber resumes, calls `on_reply`, and finishes.

We still use `rusty::Mutex` and `rusty::sync::atomic::Atomic<int>` in
the data structure because:

- The contract should remain correct if a future change moves replies
  to a different thread (e.g. a chained dispatcher running on the srpc
  callback thread before it even reaches the leader's poll thread).
- The cost is negligible — these are uncontended within one fiber loop.

## Why `std::shared_ptr<srpc::IntEvent>` and not `rusty::Arc<srpc::IntEvent>`

The phase 8.x TODO said `rusty::Arc<srpc::IntEvent>`, but the srpc reactor
**owns** every `Event` instance through `Reactor::all_events_` and emits
the user-facing handle as `std::shared_ptr<Ev>` from
`Reactor::create_sp_event<Ev>(args…)`. That's the only legal way to
construct an event — directly heap-allocating one would break the
reactor's wakeup tracking. `rusty::Arc<T>::make(…)` always allocates
its own `ControlBlock + new T(...)`, so it can't be used here.

`RaftQuorum` therefore owns `std::shared_ptr<srpc::IntEvent>` directly
and annotates the field `@unsafe` per `CLAUDE.md`'s srpc-boundary rule.
Callers see a Rusty surface — they never need to touch the `shared_ptr`.

## API

```cpp
template <typename Reply>
class RaftQuorum {
public:
    RaftQuorum(int n_total, int n_needed);

    // Called from a sub-fiber when its peer's RPC reply has arrived.
    // Records (from, reply), bumps the received counter, and signals the
    // wait-event once n_received reaches n_needed. Idempotent past the
    // threshold (extra replies are still collected but don't re-fire).
    void on_reply(siteid_t from, Reply reply);

    // Called from the orchestrator fiber. Yields the fiber until either
    // n_needed replies have arrived OR `timeout_us` microseconds pass.
    // Returns true on quorum, false on timeout.
    bool wait_until_quorum(uint64_t timeout_us);

    // Drains the collected replies. Safe to call any time; commonly
    // called immediately after wait_until_quorum returns.
    std::vector<std::pair<siteid_t, Reply>> collect();

    // Diagnostic accessors (no fiber yield).
    int received() const;
    int n_total() const;
    int n_needed() const;
};
```

## Usage example (from phase 8.1c election path; not yet wired)

```cpp
RaftQuorum<VoteReply> q(/*n_total=*/peers - 1, /*n_needed=*/quorum_size - 1);
for (auto& peer : peers) {
    if (peer == self) continue;
    Fiber::create_run([&q, peer, &transport, req]() {
        VoteReply r = transport.send_vote(peer, req);
        q.on_reply(peer, r);
    });
}
if (!q.wait_until_quorum(/*timeout_us=*/1'000'000)) {
    // election lost / timeout — step down and retry later
    return;
}
for (auto& [from, reply] : q.collect()) {
    if (reply.vote_granted) yes_voters.push_back(from);
    if (reply.max_ballot > current_term) seen_higher_term = true;
}
```

`n_needed = quorum_size - 1` because the candidate's self-vote is
implicit; only the *other* peers contribute to `n_received_`.

## Tests

`tests/raft_quorum_test.cc` covers:

- Basic round-trip: 3 replies into a 3-of-3 quorum → `wait_until_quorum`
  returns true, `collect` yields the three pairs in arrival order.
- Quorum reached early: 2 replies into a 2-of-5 quorum → `wait` returns
  before all 5 arrive; subsequent `on_reply` calls still get collected.
- Timeout: 1 reply into a 3-of-5 quorum, then `wait(10ms)` → returns
  false; `collect` yields just the one pair.
- Concurrency check: replies sequenced from multiple lambdas show
  consistent counter / collect output (single-threaded by design but
  the mutex+atomic must hold).

The unit test runs without a full deptran environment — it only needs
`srpc::Reactor` + `srpc::PollThread` to drive the event loop, which is
the same minimum any other `test_raft_*` target uses.
