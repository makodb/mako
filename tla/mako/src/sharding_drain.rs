//! Source-derived modulo-16 staged-writer drain, not a C++ refinement proof.
//!
//! Authority: integrated 44c6b5d5c278a2916a3112cf2fa4d6d7c2a4691f.
//! `migration_fence.cc:165-249,258-285` supplies registration, completion,
//! global BEGIN increments, and the excluded bucket. Counter identities below
//! are physical table *slots*, not routing keys: hash sharing is conservative.
//! Each distinct transaction/slot registration has its own token; the optional
//! overflow registration and each one-op RAII registration use `scope = None`.
//! Registration tokens represent actual counter contributions, not transactions
//! or individual writes. A reused transaction/slot creates no extra token.
//! Register/complete are atomic accounting steps; the concrete load/increment
//! window is coarsened. The witness has no BEGIN inside that window.
//!
//! Finite-set cardinalities represent nonnegative counters without signed-long
//! overflow. Filtering out one bucket and summing table/global cardinalities
//! has exactly the source's zero test: disjoint buckets partition each set.
//! Residual observes one accounting snapshot. The source loads its counters
//! separately; the witness's registration set is unchanged throughout every
//! such sweep. No general concurrent-memory or scan linearizability is claimed.
//! Generation is a ghost unbounded count of BEGIN executions; its low four bits
//! also equal uint64_t's low four bits across uint64_t wraparound. There is NO
//! lifetime, generation-distance, fairness, or deduplication guard.
//!
//! The concrete witness uses one physical table, one byte key [0], and byte
//! values [0] and [7]; it needs no integer-order/byte-order abstraction. Mirrors
//! are successful single-row copies, not snapshot assumptions for general scans.
//! `shard_master.h:229-378` and `cluster_bootstrap.cc:357-396` give the live order:
//! initial mirror, freeze, drain, catch-up mirror, final mirror/checksum, drop,
//! then route publication. The vote below computes the actual single-row FNV
//! checksum (`shard_data.h:66-80,232-235`); it does not assume injectivity.
//!
//! `mbta_wrapper.hh:629-646` registers BEFORE its fence check, then calls put.
//! `MassTrans.hh:825-844,864-873` starts storage only inside that call. Thus an
//! Admitted writer has neither a started storage transaction nor a row lock.
//! Its later successful storage call is one uninterrupted transition here;
//! no copy/checksum runs while a writer holds a lock. The RAII registration
//! survives installation, ends on wrapper return, and only THEN is the client
//! acknowledgement delivered. It is not incorrectly closed by Transaction::finish.
//!
//! `shard_data_service.h:442-455,808-837` executes every phase=-1 request anew.
//! Fifteen lost replies cost 18000ms of the client's 60000ms retry accounting;
//! the sixteenth BEGIN succeeds with cookie 0. All processes remain alive.
//! This proves an enabled operational counterexample, not a C++ execution or a
//! repaired protocol. The conditional coverage bound is explicitly unenforced.
use vstd::prelude::*;

verus! {

pub struct Registration {
    pub generation: nat,
    pub scope: Option<int>,
}

pub struct Accounting {
    pub generation: nat,
    pub live: Map<int, Registration>,
    pub issued: Set<int>,
}

pub enum AccountingAction {
    Register { id: int, scope: Option<int> },
    Complete { id: int },
    Begin,
}

pub open spec fn accounting_init() -> Accounting {
    Accounting { generation: 0, live: Map::empty(), issued: Set::empty() }
}

pub open spec fn accounting_wf(a: Accounting) -> bool {
    &&& a.live.dom().subset_of(a.issued)
    &&& forall|id: int| a.live.dom().contains(id) ==>
        (#[trigger] a.live[id]).generation <= a.generation
}

pub open spec fn accounting_enabled(a: Accounting, op: AccountingAction) -> bool {
    match op {
        AccountingAction::Register { id, .. } => !a.issued.contains(id),
        AccountingAction::Complete { id } => a.live.dom().contains(id),
        AccountingAction::Begin => true,
    }
}

pub open spec fn accounting_apply(a: Accounting, op: AccountingAction) -> Accounting {
    match op {
        AccountingAction::Register { id, scope } => Accounting {
            live: a.live.insert(id, Registration { generation: a.generation, scope }),
            issued: a.issued.insert(id), ..a
        },
        AccountingAction::Complete { id } => Accounting { live: a.live.remove(id), ..a },
        AccountingAction::Begin => Accounting { generation: a.generation + 1, ..a },
    }
}

pub open spec fn bucket(r: Registration) -> nat { r.generation % 16 }

/// None as the query means the source's wildcard table="" drain.
pub open spec fn relevant(r: Registration, table: Option<int>) -> bool {
    r.scope is None || table is None || r.scope == table
}

pub open spec fn table_residual(a: Accounting, table: Option<int>, skip: nat) -> Set<int> {
    a.live.dom().filter(|id: int| a.live[id].scope is Some
        && relevant(a.live[id], table) && bucket(a.live[id]) != skip)
}

pub open spec fn global_residual(a: Accounting, skip: nat) -> Set<int> {
    a.live.dom().filter(|id: int| a.live[id].scope is None && bucket(a.live[id]) != skip)
}

pub open spec fn residual(a: Accounting, table: Option<int>, skip: nat) -> nat {
    table_residual(a, table, skip).len() + global_residual(a, skip).len()
}

/// The zero test excludes exactly the relevant registrations outside `skip`.
/// In particular, zero is NOT a claim that every relevant registration ended.
pub proof fn lemma_residual_zero_exact(a: Accounting, table: Option<int>, skip: nat)
    ensures (residual(a, table, skip) == 0) <==>
        (forall|id: int| a.live.dom().contains(id) && relevant(#[trigger] a.live[id], table)
            ==> bucket(a.live[id]) == skip),
{
    let local = table_residual(a, table, skip);
    let global = global_residual(a, skip);
    if residual(a, table, skip) == 0 {
        assert(local =~= Set::<int>::empty());
        assert(global =~= Set::<int>::empty());
        assert forall|id: int| a.live.dom().contains(id) && relevant(#[trigger] a.live[id], table)
            implies bucket(a.live[id]) == skip by {
            if bucket(a.live[id]) != skip {
                if a.live[id].scope is None { assert(global.contains(id)); }
                else { assert(local.contains(id)); }
            }
        }
    } else if forall|id: int| a.live.dom().contains(id) && relevant(#[trigger] a.live[id], table)
        ==> bucket(a.live[id]) == skip {
        assert(local =~= Set::<int>::empty());
        assert(global =~= Set::<int>::empty());
    }
}

pub proof fn lemma_accounting_step(a: Accounting, op: AccountingAction)
    requires accounting_wf(a), accounting_enabled(a, op),
    ensures accounting_wf(accounting_apply(a, op)),
        // Every surviving token keeps its captured generation and counter.
        forall|id: int| a.live.dom().contains(id)
            && accounting_apply(a, op).live.dom().contains(id) ==>
            #[trigger] accounting_apply(a, op).live[id] == a.live[id],
        op is Begin ==> accounting_apply(a, op).live == a.live,
        op is Complete ==> !accounting_apply(a, op).live.dom().contains(op->Complete_id),
{
    match op {
        AccountingAction::Register { id, .. } => { assert(!a.live.dom().contains(id)); },
        _ => {},
    }
}

/// Precise endpoints: 1 <= drain_generation - registration_generation <= 15.
/// Equality at either endpoint 0 or 16 would alias, so neither is permitted.
pub proof fn lemma_nonaliasing_within_fifteen(registration_generation: nat, drain_generation: nat)
    requires registration_generation < drain_generation < registration_generation + 16,
    ensures registration_generation % 16 != drain_generation % 16,
{
    assert(registration_generation % 16 != drain_generation % 16) by (nonlinear_arith)
        requires registration_generation < drain_generation < registration_generation + 16;
}

/// Explicit nonaliasing, with no lifetime bound, suffices to cover any token.
pub proof fn lemma_zero_residual_covers_nonaliased(
    a: Accounting, table: Option<int>, skip: nat, id: int, r: Registration,
)
    requires residual(a, table, skip) == 0, relevant(r, table), bucket(r) != skip,
        a.live.dom().contains(id) ==> a.live[id] == r,
    ensures !a.live.dom().contains(id),
{
    lemma_residual_zero_exact(a, table, skip);
}

/// `before` is the pre-freeze registration snapshot and surviving tokens retain
/// their records (lemma_accounting_step). The fence precedes BEGIN. This theorem
/// uses a strict <16 bound INCLUDING that BEGIN; code enforces no such bound.
pub proof fn theorem_conditional_prefence_coverage(
    a: Accounting, before: Map<int, Registration>, table: Option<int>,
    fence_generation: nat, drain_generation: nat,
)
    requires
        fence_generation < drain_generation,
        residual(a, table, drain_generation % 16) == 0,
        forall|id: int| before.dom().contains(id) && relevant(#[trigger] before[id], table) ==>
            before[id].generation <= fence_generation
            && drain_generation < before[id].generation + 16,
        forall|id: int| before.dom().contains(id) && a.live.dom().contains(id) ==>
            #[trigger] a.live[id] == before[id],
    ensures forall|id: int| before.dom().contains(id) && relevant(#[trigger] before[id], table)
        ==> !a.live.dom().contains(id),
{
    assert forall|id: int| before.dom().contains(id) && relevant(#[trigger] before[id], table)
        implies !a.live.dom().contains(id) by {
        lemma_nonaliasing_within_fifteen(before[id].generation, drain_generation);
        lemma_zero_residual_covers_nonaliased(a, table, drain_generation % 16, id, before[id]);
    }
}

pub enum WriterPhase { Absent, Registered, Admitted, Rejected, Installed, Returned, Acknowledged }
pub enum MigrationPhase {
    Idle, Begun, InitialCopied, Frozen, Drained, CatchupCopied, FinalCopied, Verified, Dropped, Published,
}

pub struct DrainReply {
    pub cookie: nat,
    pub ok: bool,
}

pub struct DrainState {
    pub accounting: Accounting,
    pub writer: WriterPhase,
    pub migration: MigrationPhase,
    pub source: Option<u8>,
    pub destination: Option<u8>,
    pub source_frozen: bool,
    pub source_moved: bool,
    pub route_destination: bool,
    pub acknowledged: bool,
    pub client_cookie: Option<nat>,
    pub pending_reply: Option<DrainReply>,
    pub lost_replies: nat,
    pub waited_ms: nat,
}

pub enum DrainAction {
    RegisterWriter, CheckFence, RejectWriter, InstallWriter, ReturnWriter, AcknowledgeWriter,
    StartMigration, InitialMirror, FreezeSource, ExecuteDrainRpc, LoseDrainReply, ReceiveDrainReply,
    CatchupMirror, FinalMirror, VerifyChecksum, DropRange, PublishRoute,
}

pub open spec fn initial() -> DrainState {
    DrainState {
        accounting: accounting_init(), writer: WriterPhase::Absent, migration: MigrationPhase::Idle,
        source: Some(0u8), destination: None, source_frozen: false, source_moved: false,
        route_destination: false, acknowledged: false, client_cookie: None, pending_reply: None,
        lost_replies: 0, waited_ms: 0,
    }
}

pub open spec fn fnv_byte(byte: u8) -> nat {
    (((1469598103934665603u64 ^ (byte as u64)) as nat) * 1099511628211nat)
        % 18446744073709551616nat
}

pub open spec fn checksum(row: Option<u8>) -> nat {
    match row {
        None => 0,
        Some(value) => (fnv_byte(0u8) * 1000003 + fnv_byte(value)) % 18446744073709551616nat,
    }
}

pub open spec fn rpc_accounting(s: DrainState) -> Accounting {
    if s.client_cookie is None { accounting_apply(s.accounting, AccountingAction::Begin) }
    else { s.accounting }
}

pub open spec fn rpc_cookie(s: DrainState) -> nat {
    match s.client_cookie {
        None => rpc_accounting(s).generation % 16,
        Some(cookie) => cookie,
    }
}

pub open spec fn enabled(s: DrainState, action: DrainAction) -> bool {
    match action {
        DrainAction::RegisterWriter => s.writer is Absent
            && accounting_enabled(s.accounting, AccountingAction::Register { id: 0, scope: None }),
        DrainAction::CheckFence => s.writer is Registered && !s.source_frozen && !s.source_moved,
        DrainAction::RejectWriter => s.writer is Registered && (s.source_frozen || s.source_moved),
        // No second fence check between the successful check and THIS attempt.
        DrainAction::InstallWriter => s.writer is Admitted,
        DrainAction::ReturnWriter => s.writer is Installed && s.accounting.live.dom().contains(0),
        DrainAction::AcknowledgeWriter => s.writer is Returned,
        DrainAction::StartMigration => s.migration is Idle,
        DrainAction::InitialMirror => s.migration is Begun,
        DrainAction::FreezeSource => s.migration is InitialCopied,
        DrainAction::ExecuteDrainRpc => s.migration is Frozen && s.pending_reply is None
            && s.waited_ms < 60000,
        DrainAction::LoseDrainReply | DrainAction::ReceiveDrainReply => s.pending_reply is Some,
        DrainAction::CatchupMirror => s.migration is Drained,
        DrainAction::FinalMirror => s.migration is CatchupCopied,
        DrainAction::VerifyChecksum => s.migration is FinalCopied
            && checksum(s.source) == checksum(s.destination),
        DrainAction::DropRange => s.migration is Verified,
        DrainAction::PublishRoute => s.migration is Dropped,
    }
}

pub open spec fn apply(s: DrainState, action: DrainAction) -> DrainState {
    match action {
        DrainAction::RegisterWriter => DrainState {
            accounting: accounting_apply(s.accounting, AccountingAction::Register { id: 0, scope: None }),
            writer: WriterPhase::Registered, ..s
        },
        DrainAction::CheckFence => DrainState { writer: WriterPhase::Admitted, ..s },
        DrainAction::RejectWriter => DrainState {
            accounting: accounting_apply(s.accounting, AccountingAction::Complete { id: 0 }),
            writer: WriterPhase::Rejected, ..s
        },
        DrainAction::InstallWriter => DrainState { source: Some(7u8), writer: WriterPhase::Installed, ..s },
        DrainAction::ReturnWriter => DrainState {
            accounting: accounting_apply(s.accounting, AccountingAction::Complete { id: 0 }),
            writer: WriterPhase::Returned, ..s
        },
        DrainAction::AcknowledgeWriter => DrainState { acknowledged: true, writer: WriterPhase::Acknowledged, ..s },
        DrainAction::StartMigration => DrainState { migration: MigrationPhase::Begun, ..s },
        DrainAction::InitialMirror => DrainState {
            destination: s.source, migration: MigrationPhase::InitialCopied, ..s
        },
        DrainAction::FreezeSource => DrainState { source_frozen: true, migration: MigrationPhase::Frozen, ..s },
        DrainAction::ExecuteDrainRpc => DrainState {
            accounting: rpc_accounting(s),
            pending_reply: Some(DrainReply { cookie: rpc_cookie(s),
                ok: residual(rpc_accounting(s), Some(0int), rpc_cookie(s)) == 0 }), ..s
        },
        DrainAction::LoseDrainReply => DrainState {
            pending_reply: None, lost_replies: s.lost_replies + 1, waited_ms: s.waited_ms + 1200, ..s
        },
        DrainAction::ReceiveDrainReply => if s.pending_reply.unwrap().ok {
            DrainState { pending_reply: None, migration: MigrationPhase::Drained, ..s }
        } else {
            DrainState { client_cookie: Some(s.pending_reply.unwrap().cookie), pending_reply: None,
                waited_ms: s.waited_ms + 500, ..s }
        },
        DrainAction::CatchupMirror => DrainState {
            destination: s.source, migration: MigrationPhase::CatchupCopied, ..s
        },
        DrainAction::FinalMirror => DrainState {
            destination: s.source, migration: MigrationPhase::FinalCopied, ..s
        },
        DrainAction::VerifyChecksum => DrainState { migration: MigrationPhase::Verified, ..s },
        // Production drop upgrades the source's guard before deleting.
        DrainAction::DropRange => DrainState {
            source: None, source_moved: true, migration: MigrationPhase::Dropped, ..s
        },
        DrainAction::PublishRoute => DrainState {
            route_destination: true, migration: MigrationPhase::Published, ..s
        },
    }
}

pub open spec fn next(s: DrainState, t: DrainState) -> bool {
    exists|action: DrainAction| enabled(s, action) && t == apply(s, action)
}

pub open spec fn behavior(states: Seq<DrainState>) -> bool {
    states.len() > 0 && states[0] == initial()
        && forall|i: int| 0 <= i < states.len() - 1 ==> #[trigger] next(states[i], states[i + 1])
}

/// A relevant writer cannot pass its first check after source freeze. This
/// does not recall a check that has already returned successfully.
pub proof fn lemma_freeze_rejects_new_admission(s: DrainState)
    requires s.source_frozen, s.writer is Registered,
    ensures !enabled(s, DrainAction::CheckFence), enabled(s, DrainAction::RejectWriter),
        apply(s, DrainAction::RejectWriter).writer is Rejected,
        !apply(s, DrainAction::RejectWriter).accounting.live.dom().contains(0),
{}

/// The registration lifetime covers the paused pre-transaction writer and its
/// installation. Unregister happens on RAII destruction, before the reply/ack.
pub open spec fn writer_counted(s: DrainState) -> bool {
    (s.writer is Registered || s.writer is Admitted || s.writer is Installed) ==>
        s.accounting.live.dom().contains(0) && s.accounting.live[0].scope is None
}

pub proof fn lemma_writer_counted_step(s: DrainState, action: DrainAction)
    requires writer_counted(s), enabled(s, action),
    ensures writer_counted(apply(s, action)),
{}

pub proof fn lemma_next_registration_invariants(s: DrainState, t: DrainState)
    requires accounting_wf(s.accounting), writer_counted(s), next(s, t),
    ensures accounting_wf(t.accounting), writer_counted(t),
{
    let action = choose|action: DrainAction| enabled(s, action) && t == apply(s, action);
    lemma_writer_counted_step(s, action);
    match action {
        DrainAction::RegisterWriter => {
            lemma_accounting_step(s.accounting, AccountingAction::Register { id: 0, scope: None });
        },
        DrainAction::RejectWriter | DrainAction::ReturnWriter => {
            lemma_accounting_step(s.accounting, AccountingAction::Complete { id: 0 });
        },
        DrainAction::ExecuteDrainRpc => {
            if s.client_cookie is None { lemma_accounting_step(s.accounting, AccountingAction::Begin); }
        },
        _ => {},
    }
}

pub proof fn lemma_registration_invariants_at(states: Seq<DrainState>, i: int)
    requires behavior(states), 0 <= i < states.len(),
    ensures accounting_wf(states[i].accounting), writer_counted(states[i]),
    decreases i,
{
    if i > 0 {
        lemma_registration_invariants_at(states, i - 1);
        assert(next(states[i - 1], states[(i - 1) + 1]));
        lemma_next_registration_invariants(states[i - 1], states[i]);
    }
}

/// Genuine reachable-state invariants; neither says that drain zero implies
/// source quiescence. In particular, a counted writer can occupy the skip bucket.
pub proof fn theorem_registration_invariants(states: Seq<DrainState>)
    requires behavior(states),
    ensures forall|i: int| 0 <= i < states.len() ==>
        accounting_wf((#[trigger] states[i]).accounting) && writer_counted(states[i]),
{
    assert forall|i: int| 0 <= i < states.len() implies
        accounting_wf((#[trigger] states[i]).accounting) && writer_counted(states[i]) by {
        lemma_registration_invariants_at(states, i);
    }
}

pub open spec fn published_owner_preserves_ack(s: DrainState) -> bool {
    s.acknowledged && s.route_destination ==> s.destination == Some(7u8)
}

pub proof fn extend(states: Seq<DrainState>, action: DrainAction) -> (after: Seq<DrainState>)
    requires behavior(states), enabled(states.last(), action),
    ensures behavior(after), after == states.push(apply(states.last(), action)),
        after.last() == apply(states.last(), action), after.len() == states.len() + 1,
{
    let t = apply(states.last(), action);
    assert(next(states.last(), t));
    let after = states.push(t);
    assert forall|i: int| 0 <= i < after.len() - 1 implies #[trigger] next(after[i], after[i + 1]) by {
        if i < states.len() - 1 {
            assert(after[i] == states[i]);
            assert(after[i + 1] == states[i + 1]);
        } else {
            assert(after[i] == states.last());
            assert(after[i + 1] == t);
        }
    }
    after
}

pub open spec fn after_lost_begins(s: DrainState, n: nat) -> DrainState {
    DrainState { accounting: Accounting { generation: s.accounting.generation + n, ..s.accounting },
        lost_replies: s.lost_replies + n, waited_ms: s.waited_ms + 1200 * n, ..s }
}

/// Executed-but-unobserved BEGINs, not merely dropped requests. The phase stays
/// -1 because every reply was lost, so every next request increments globally.
pub proof fn append_lost_begins(states: Seq<DrainState>, n: nat) -> (after: Seq<DrainState>)
    requires behavior(states), states.last().migration is Frozen,
        states.last().client_cookie is None, states.last().pending_reply is None,
        states.last().waited_ms + 1200 * n < 60000,
    ensures behavior(after), after.last() == after_lost_begins(states.last(), n),
        after.len() == states.len() + 2 * n,
        forall|i: int| 0 <= i < states.len() ==> #[trigger] after[i] == states[i],
    decreases n,
{
    if n == 0 {
        states
    } else {
        let t = append_lost_begins(states, (n - 1) as nat);
        let t = extend(t, DrainAction::ExecuteDrainRpc);
        let t = extend(t, DrainAction::LoseDrainReply);
        t
    }
}

/// From explicit init, a live bucket-0 one-op writer is skipped after 16 BEGIN
/// executions. It is still BEFORE storage at the successful final copy/vote.
/// It then installs and acknowledges 7; drop/publication exposes old byte 0.
/// No process failure, scan past a locked row, or bounded writer lifetime is used.
pub proof fn witness_wraparound_loses_acknowledged_write() -> (states: Seq<DrainState>)
    ensures behavior(states), states.len() == 46,
        states[5].writer is Admitted, states[5].source_frozen,
        states[36].pending_reply is Some, states[36].pending_reply.unwrap().cookie == 0,
        states[36].pending_reply.unwrap().ok,
        states[37].migration is Drained, states[37].writer is Admitted,
        states[37].accounting.generation == 16, states[37].lost_replies == 15,
        states[37].waited_ms == 18000,
        states[37].accounting.live.dom().contains(0), bucket(states[37].accounting.live[0]) == 0,
        residual(states[37].accounting, Some(0int), 0) == 0,
        states[40].migration is Verified, states[40].writer is Admitted,
        states[40].source == Some(0u8), states[40].destination == Some(0u8),
        states[41].writer is Installed, states[41].accounting.live.dom().contains(0),
        states[42].writer is Returned, !states[42].accounting.live.dom().contains(0),
        states[43].acknowledged, states[43].source == Some(7u8),
        states.last().source is None, states.last().source_moved,
        states.last().route_destination, states.last().destination == Some(0u8),
        !published_owner_preserves_ack(states.last()),
{
    let t = seq![initial()];
    assert(behavior(t));
    let t = extend(t, DrainAction::RegisterWriter);
    let t = extend(t, DrainAction::CheckFence);
    let t = extend(t, DrainAction::StartMigration);
    let t = extend(t, DrainAction::InitialMirror);
    let t = extend(t, DrainAction::FreezeSource);
    let t = append_lost_begins(t, 15);
    let t = extend(t, DrainAction::ExecuteDrainRpc);
    assert(t.last().pending_reply.unwrap().cookie == 0);
    lemma_residual_zero_exact(t.last().accounting, Some(0int), 0);
    assert(t.last().pending_reply.unwrap().ok);
    let t = extend(t, DrainAction::ReceiveDrainReply);
    let t = extend(t, DrainAction::CatchupMirror);
    let t = extend(t, DrainAction::FinalMirror);
    let t = extend(t, DrainAction::VerifyChecksum);
    let t = extend(t, DrainAction::InstallWriter);
    let t = extend(t, DrainAction::ReturnWriter);
    let t = extend(t, DrainAction::AcknowledgeWriter);
    let t = extend(t, DrainAction::DropRange);
    let t = extend(t, DrainAction::PublishRoute);
    t
}

} // verus!
