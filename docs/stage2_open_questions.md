# Stage 2 — open questions for the project owner

Written overnight 2026-09-07 while working autonomously on
`codex/raft-stage2-prep`. Each question below **blocks work I could otherwise
do**. Anything I could answer myself by reading the tree is not here.

Ordered by how much it unblocks.

---

## Q1. ~~What is "async_raft"?~~ — ANSWERED

> "run the consensus logic in memory and publish only after whatever
> supported the committed decision has been written to disk and can't be
> flipped ... the benefit is to improve the throughput of dealing with
> requests"

Confirmed against the code — this is a **two-tier acknowledgement** design,
and it is the safety-critical core of the implementation:

    AckType 0 = MEMORY    raft_server_ack_is_memory       server.h
    AckType 1 = DURABLE   raft_server_ack_is_durable

    AppendEntries      -> returns followerAckType         rcc_rpc.rpc:61
    AppendEntriesDurable  a SECOND RPC the follower sends
                          AFTER fsync                     rcc_rpc.rpc:63-68
                          "Enables speculative commits: leader tracks
                           memory vs durable acknowledgments"
    VoteDurable           same for votes                  rcc_rpc.rpc:30-34
                          "Enables speculative voting"

    raft_server_should_become_secured(already_secured,
                                      durable_vote_count, quorum)
        -> !already_secured && durable_vote_count >= quorum

So the fast path commits on a **memory quorum** (throughput), and "secured" is
the name for your "can't be flipped" property: a leader becomes secured only
once a **durable** quorum confirms. Consumers are `server.cc:4188`, `:4190`
(ack classification on the append reply path) and `:7085` (the secured
transition, gated on `HasDurableStorage()`).

### What this changes about tonight's work

It makes me **more** conservative, not less. This is not textbook Raft — the
safety argument is the memory/durable split, and it is exactly the kind of
invariant that a mechanically-correct refactor can silently weaken. So I am
treating the following as **REVIEW-REQUIRED, not autonomous**, and will not
convert them overnight:

  * `raft_server_ack_is_memory` / `_ack_is_durable` and `AckType`
  * `raft_server_should_become_secured`,
    `raft_server_unsecured_leader_needs_quorum_check`
  * `raft_server_persistence_can_report_durable`,
    `raft_server_sync_reply_is_durable`,
    `raft_server_durable_write_succeeded`
  * `raft_server_async_persistence_should_queue`,
    `raft_server_persistence_ticket_is_ready`,
    `raft_server_persisted_reply_context_is_current`
  * `CommitStatus` (SPECULATIVE vs DURABLE) and the commit-callback tiers

I will restrict overnight conversions to code with **no bearing on the
memory/durable boundary** — the CRC32 checksum object and
`RaftSubmissionProgress` — and leave the speculation path for you.

### Follow-on question this raises (Q1a)

Is the **durable** tier ever allowed to go backwards? `should_become_secured`
takes `already_secured` and is latching (`!already_secured && ...`), which
reads as "once secured, always secured". If that latch is a genuine invariant
rather than an optimisation, it deserves a static_assert or a comment saying
so, because a future conversion could drop the `!already_secured` guard
without any test noticing. **Is "secured" monotonic for the life of a term?**

## Q2. `ReplicatedDBOp` — what should decoding do with an invalid byte?

`replicated_db.h:28` is a `#[repr(u8)]` enum with `PUT = 1, DELETE = 2,
BATCH = 3`. The migration doc records that Stage 1 owns it **only because the
emitted C++ still accepts every raw `u8`, including unnamed values, with the
same switch behaviour as before** — and that a real Rust enum cannot hold those
values. It also notes `KVOperation` is transiently value-created with zero,
and **0 is not a valid variant**.

**Why it blocks:** promoting this to real Rust requires deciding the decode
contract. I cannot pick this for you — it's a wire/storage compatibility
decision.

**Options:** (a) transparent byte newtype `struct ReplicatedDBOp(u8)` with
named constants — preserves all current behaviour, loses exhaustiveness;
(b) real enum plus an explicit `TryFrom<u8>` that rejects invalid bytes —
safer, but changes behaviour for malformed input, which may be reachable from
disk; (c) real enum with an explicit `UNKNOWN = 0` variant — changes the
persisted meaning of zero.

I'd default to **(a)** as behaviour-preserving, but only with your say-so.

---

## Q3. Wrapping arithmetic in the snapshot-size guards — preserve or harden?

The snapshot parser deliberately uses **wrapping** addition at the incumbent
`size_t`/`uint64_t` widths, and the migration doc is explicit that this
"retains malformed-input behavior, including cross-width C++ promotions;
overflow hardening must be a separate correctness change before native Rust
executes this parser."

**Why it blocks:** Rust's default arithmetic panics on overflow in debug and
wraps in release. Writing these in Rust means choosing `wrapping_add`
explicitly (preserve) or `checked_add` (harden). Hardening is a **behaviour
change on malformed input**, i.e. a security-relevant decision on a parser
that reads attacker-influenced bytes from disk.

**What I'd do:** preserve with explicit `wrapping_*` and a TODO, unless you
want the hardening as its own reviewed change.

---

## Q4. The legacy non-IEEE CRC32 table

The production CRC32 lookup table contains **legacy non-IEEE entries**. Tests
pin the existing checksum bytes rather than correcting them, because
correcting the table would change every persisted snapshot. The doc says a fix
"requires a separately versioned format change".

**Why it blocks:** it doesn't block the CRC32 *object* conversion (I can move
the class and keep the table byte-identical). It only blocks anyone who thinks
the table is a bug to fix. **Confirm: leave the table exactly as-is?** I'm
assuming yes.

---

## Q5. Schema 1 or schema 2 for the raft manifest?

`src/rrr/rust-modules.toml` is `schema_version = 2` — canonical `.rs` compiled
directly, C++ generated into the build tree, inline carriers deleted. The
migration doc says raft's Stage 2 should be **schema 1** (extraction: `.rs`
generated by concatenating the inline blocks, inline blocks remain
authoritative, production still compiles the committed C++).

I've built it as **schema 1**, which is what the doc specifies and is the
lower-risk choice — the inline blocks stay the single source of truth and
nothing about production changes.

**Confirm that's what you want**, or say if you'd rather jump straight to
schema 2 and start deleting carriers. The difference is large: schema 1 is
additive and reversible; schema 2 changes what production compiles.

---

## Q6. What happens to the 39-entry `EXPECTED_BLOCKS` ratchet?

`scripts/raft_dsl.sh:23-62` hardcodes an exact 39-block inventory and fails if
it doesn't match. That's a good anti-drift device. But every new conversion now
requires editing that list, and once the crate manifest also lists modules
there are **two inventories to keep in sync**.

**Options:** keep both (belt and braces, two edits per conversion); derive
`EXPECTED_BLOCKS` from the manifest; or drop the ratchet and rely on the
manifest plus hash checks.

**Why it blocks:** it doesn't block a single conversion, but it decides how
annoying the next fifty are. I'd keep both for now and revisit at Stage 3.

---

## Q7. May I run the third RaftLab arm?

There are three persistence modes, not two: unset, `MAKO_RAFT_PERSISTENCE=1`,
and `MAKO_RAFT_PERSISTENCE=1` **plus** `MAKO_RAFT_ASYNC_PERSISTENCE=1`
(`server.cc:2463`). I have only ever run the first two (55 + 50 = 105 tests).
The doc's "155 cases" is 55 + 50 + 50, so the async arm is the missing third.

I'll run it as part of tonight's verification unless it turns out to be
long-running or flaky. Flagging it because **if it fails, I won't know whether
that's my change or a pre-existing condition** — nobody has run it in this
checkout.

---

## Q8. Is `/tmp` acceptable for the persistence tests, or should durability be tested on real disk?

Separate from the conversion, and recorded because it undercuts the value of
the persistence tier: `/tmp` on this host is **tmpfs**, so `fsync()` never
reaches stable storage. Crash-*recovery* is genuinely tested (tmpfs survives
process death); durability against machine failure is not tested at all.

Setting `MAKO_RAFT_PERSISTENCE_PATH` to real local disk would fix it — but I
haven't confirmed this host has non-tmpfs local scratch (`/home` is NFS).

---

## Things I decided myself and did NOT ask about

For the record, so you can object if any of these were wrong:

- **`ballot_t` signedness in the disk decoder** — you said signed; I changed
  the five ballot fields to `i64` and left `slot_id` as `u64` because
  `slotid_t` is genuinely `uint64_t`. That split was my call.
- **Doc comments in DSL blocks must be language-neutral** — the transpiler
  passes `///` through verbatim into the generated C++, so my first `# Safety`
  draft described Rust types inside a C++ function. I reworded it.
- **Module naming in the crate** — `server_h`, `server_cc`, i.e. carrier
  basename with `.` → `_`. Mechanical and collision-free (verified: 0
  collisions across 250 top-level items). Change it if you dislike the names.
