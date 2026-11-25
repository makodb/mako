# Distributed transaction protocol

The current Rust runtime uses OCC + two-phase commit. The earlier speculative
protocol, Paxos watermarks and dependency rollback described at this path are
historical. They are not the deployed transaction algorithm.

Read [the book's OCC + 2PC chapter](../mako-book.md#occ-and-two-phase-commit) for
the lock barrier, prepare, saved decision and completion sequence. The
[contract document](../developer/upper-layer-contracts.md) records assumptions
and verified properties. The [implementation plan](../plans/occ-2pc.md) records
qualification and remaining work.

The original text remains available in repository history before the `occ-2pc`
change. Historical performance reports retain their original protocol and
revision attribution.
