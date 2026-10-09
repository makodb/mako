//! Constructive, enabled recovery paths, including an unbounded engine suffix.
use vstd::prelude::*;
use super::*;

verus! {

pub open spec fn replay_n(c: p::Constants, s: State, a: Authority, n: nat) -> State
    decreases n
{
    if n == 0 { s }
    else { apply(c,replay_n(c,s,a,(n-1) as nat),Action::ReplayEngine { authority: a }) }
}
pub proof fn replay_n_enabled(c: p::Constants, s: State, a: Authority, n: nat)
    requires inv(c,s),valid_authority(c,a),s.runtime[a].mode is Replaying,
        s.runtime[a].cursor == s.journals[a].len(),n + s.runtime[a].engine_cursor <= s.engines[a].len()
    ensures inv(c,replay_n(c,s,a,n)),
        replay_n(c,s,a,n).core == s.core,replay_n(c,s,a,n).journals == s.journals,
        replay_n(c,s,a,n).lease_names == s.lease_names,
        replay_n(c,s,a,n).engines == s.engines,replay_n(c,s,a,n).settled == s.settled,
        replay_n(c,s,a,n).pending == s.pending,replay_n(c,s,a,n).accepted == s.accepted,
        replay_n(c,s,a,n).copying == s.copying,replay_n(c,s,a,n).copied == s.copied,
        replay_n(c,s,a,n).cleaning == s.cleaning,replay_n(c,s,a,n).cleaned == s.cleaned,
        replay_n(c,s,a,n).runtime[a].mode is Replaying,
        replay_n(c,s,a,n).runtime[a].cursor == s.runtime[a].cursor,
        replay_n(c,s,a,n).runtime[a].engine_cursor == s.runtime[a].engine_cursor+n,
        forall|other: Authority| other != a ==> replay_n(c,s,a,n).runtime[other] == s.runtime[other],
        forall|i: nat| i < n ==> enabled(c,replay_n(c,s,a,i),Action::ReplayEngine { authority: a })
    decreases n
{
    if n > 0 {
        replay_n_enabled(c,s,a,(n-1) as nat);
        let before = replay_n(c,s,a,(n-1) as nat);
        assert(enabled(c,before,Action::ReplayEngine { authority: a }));
        lemma_step(c,before,Action::ReplayEngine { authority: a });
        assert forall|i: nat| i < n implies enabled(c,replay_n(c,s,a,i),Action::ReplayEngine { authority: a }) by {
            if i == n-1 { assert(replay_n(c,s,a,i) == before); }
        }
    }
}
pub open spec fn snapshot_start(c: p::Constants, s: State, a: Authority) -> State {
    let crashed = apply(c,s,Action::Crash { authority: a });
    let booted = apply(c,crashed,Action::Boot { authority: a });
    apply(c,booted,Action::InstallSnapshot { authority: a,index: (s.journals[a].len()-1) as nat })
}
pub open spec fn suffix_length(s: State, a: Authority) -> nat {
    (s.engines[a].len()-head(s,a).engine_offset) as nat
}
pub open spec fn reconstructed_runtime(c: p::Constants, s: State, a: Authority) -> State {
    let replayed = replay_n(c,snapshot_start(c,s,a),a,suffix_length(s,a));
    apply(c,replayed,Action::Complete { authority: a })
}
pub open spec fn admitted_runtime(c: p::Constants, s: State, a: Authority) -> State {
    apply(c,reconstructed_runtime(c,s,a),Action::Admit { authority: a })
}
pub open spec fn bootstrap(c: p::Constants, s: State, a: Authority) -> State {
    apply(c,admitted_runtime(c,s,a),Action::Activate { authority: a })
}
pub open spec fn bootstrap_enabled(c: p::Constants, s: State, a: Authority) -> bool {
    let crashed = apply(c,s,Action::Crash { authority: a });
    let booted = apply(c,crashed,Action::Boot { authority: a });
    let start = snapshot_start(c,s,a);
    let replayed = replay_n(c,start,a,suffix_length(s,a));
    enabled(c,s,Action::Crash { authority: a })
        && enabled(c,crashed,Action::Boot { authority: a })
        && enabled(c,booted,Action::InstallSnapshot { authority: a,index: (s.journals[a].len()-1) as nat })
        && (forall|i: nat| i < suffix_length(s,a) ==> enabled(c,replay_n(c,start,a,i),Action::ReplayEngine { authority: a }))
        && enabled(c,replayed,Action::Complete { authority: a })
        && enabled(c,reconstructed_runtime(c,s,a),Action::Admit { authority: a })
        && enabled(c,admitted_runtime(c,s,a),Action::Activate { authority: a })
}
pub open spec fn recovery_frame(s: State, z: State, a: Authority) -> bool {
    z.core == s.core && z.journals == s.journals && z.lease_names == s.lease_names
        && z.engines == s.engines && z.accepted == s.accepted
        && z.pending == s.pending && z.settled == s.settled
        && z.copying == s.copying && z.copied == s.copied
        && z.cleaning == s.cleaning && z.cleaned == s.cleaned
        && forall|other: Authority| other != a ==> z.runtime[other] == s.runtime[other]
}
pub proof fn snapshot_witness(c: p::Constants, s: State, a: Authority)
    requires inv(c,s),valid_authority(c,a)
    ensures inv(c,snapshot_start(c,s,a)),recovery_frame(s,snapshot_start(c,s,a),a),
        snapshot_start(c,s,a).runtime[a].mode is Replaying,
        snapshot_start(c,s,a).runtime[a].cursor == s.journals[a].len(),
        snapshot_start(c,s,a).runtime[a].engine_cursor == head(s,a).engine_offset,
        enabled(c,s,Action::Crash { authority: a }),
        enabled(c,apply(c,s,Action::Crash { authority: a }),Action::Boot { authority: a }),
        enabled(c,apply(c,apply(c,s,Action::Crash { authority: a }),Action::Boot { authority: a }),
            Action::InstallSnapshot { authority: a,index: (s.journals[a].len()-1) as nat })
{
    hide(inv);
    authority_facts(c,s,a);
    lemma_step(c,s,Action::Crash { authority: a });
    let crashed = apply(c,s,Action::Crash { authority: a });
    lemma_step(c,crashed,Action::Boot { authority: a });
    let booted = apply(c,crashed,Action::Boot { authority: a });
    lemma_step(c,booted,Action::InstallSnapshot { authority: a,index: (s.journals[a].len()-1) as nat });
}
pub proof fn reconstruction_witness(c: p::Constants, s: State, a: Authority)
    requires inv(c,s),valid_authority(c,a)
    ensures inv(c,reconstructed_runtime(c,s,a)),recovery_frame(s,reconstructed_runtime(c,s,a),a),
        reconstructed_runtime(c,s,a).runtime[a].mode is Recovered,
        caught_up(reconstructed_runtime(c,s,a),a),
        reconstructed_runtime(c,s,a).runtime[a].admission_floor == head(s,a).incarnation,
        enabled(c,s,Action::Crash { authority: a }),
        enabled(c,apply(c,s,Action::Crash { authority: a }),Action::Boot { authority: a }),
        enabled(c,apply(c,apply(c,s,Action::Crash { authority: a }),Action::Boot { authority: a }),
            Action::InstallSnapshot { authority: a,index: (s.journals[a].len()-1) as nat }),
        forall|i: nat| i < suffix_length(s,a) ==> enabled(c,replay_n(c,snapshot_start(c,s,a),a,i),Action::ReplayEngine { authority: a }),
        enabled(c,replay_n(c,snapshot_start(c,s,a),a,suffix_length(s,a)),Action::Complete { authority: a })
{
    hide(inv);
    hide(snapshot_start);
    hide(replay_n);
    authority_facts(c,s,a);
    snapshot_witness(c,s,a);
    let start = snapshot_start(c,s,a);
    assert(suffix_length(s,a)+start.runtime[a].engine_cursor == start.engines[a].len());
    replay_n_enabled(c,start,a,suffix_length(s,a));
    let replayed = replay_n(c,start,a,suffix_length(s,a));
    lemma_step(c,replayed,Action::Complete { authority: a });
    assert forall|other: Authority| other != a implies
        reconstructed_runtime(c,s,a).runtime[other] == s.runtime[other] by {
        assert(replayed.runtime[other] == start.runtime[other]);
        assert(start.runtime[other] == s.runtime[other]);
    }
}

pub proof fn bootstrap_witness(c: p::Constants, s: State, a: Authority)
    requires inv(c,s),valid_authority(c,a),registered(s,a)
    ensures bootstrap_enabled(c,s,a),inv(c,bootstrap(c,s,a)),active(bootstrap(c,s,a),a),
        bootstrap(c,s,a).core == s.core,bootstrap(c,s,a).engines == s.engines,
        bootstrap(c,s,a).pending == s.pending,bootstrap(c,s,a).settled == s.settled,
        bootstrap(c,s,a).accepted == s.accepted,bootstrap(c,s,a).copied == s.copied,
        head(bootstrap(c,s,a),a).incarnation == head(s,a).incarnation+1,
        head(bootstrap(c,s,a),a).membership == member(s,a),
        head(bootstrap(c,s,a),a).processed == head(s,a).processed,
        head(bootstrap(c,s,a),a).engine_offset == s.engines[a].len(),
        forall|other: Authority| other != a ==> bootstrap(c,s,a).runtime[other] == s.runtime[other]
            && bootstrap(c,s,a).journals[other] == s.journals[other],
        inv(c,admitted_runtime(c,s,a)),!active(admitted_runtime(c,s,a),a),
        admitted_runtime(c,s,a).runtime[a].mode is Admitted,
        admitted_runtime(c,s,a).core == s.core,
        admitted_runtime(c,s,a).pending == s.pending,admitted_runtime(c,s,a).settled == s.settled,
        head(admitted_runtime(c,s,a),a).incarnation == head(s,a).incarnation+1
{
    hide(inv);
    hide(snapshot_start);
    hide(replay_n);
    hide(reconstructed_runtime);
    reconstruction_witness(c,s,a);
    let reconstructed = reconstructed_runtime(c,s,a);
    lemma_step(c,reconstructed,Action::Admit { authority: a });
    let admitted = admitted_runtime(c,s,a);
    lemma_step(c,admitted,Action::Activate { authority: a });
    assert forall|other: Authority| other != a implies
        bootstrap(c,s,a).runtime[other] == s.runtime[other]
        && bootstrap(c,s,a).journals[other] == s.journals[other] by {
        assert(reconstructed.runtime[other] == s.runtime[other]);
        assert(reconstructed.journals[other] == s.journals[other]);
    }
}
// Composition uses only the completed recovery result, not the separate
// preactivation witness and every intermediate replay state.
pub proof fn bootstrap_summary(c: p::Constants, s: State, a: Authority)
    requires inv(c,s),valid_authority(c,a),registered(s,a)
    ensures bootstrap_enabled(c,s,a),inv(c,bootstrap(c,s,a)),active(bootstrap(c,s,a),a),
        bootstrap(c,s,a).core == s.core,
        head(bootstrap(c,s,a),a).incarnation == head(s,a).incarnation+1,
        head(bootstrap(c,s,a),a).membership == member(s,a),
        head(bootstrap(c,s,a),a).processed == head(s,a).processed,
        forall|other: Authority| other != a ==> bootstrap(c,s,a).runtime[other] == s.runtime[other]
            && bootstrap(c,s,a).journals[other] == s.journals[other]
{
    hide(inv);
    hide(bootstrap);
    hide(bootstrap_enabled);
    hide(snapshot_start);
    hide(reconstructed_runtime);
    hide(admitted_runtime);
    hide(replay_n);
    bootstrap_witness(c,s,a);
}

pub open spec fn commit_pair(c: p::Constants, s: State, a: p::Action) -> State {
    let e = envelope(s,a);
    apply(c,apply(c,s,Action::Accept { envelope: e }),Action::Commit { envelope: e })
}
pub proof fn commit_pair_enabled(c: p::Constants, s: State, a: p::Action)
    requires inv(c,s),enabled(c,s,Action::Accept { envelope: envelope(s,a) }),
        !head(s,actor(s.core.placement,a)).processed.contains(envelope(s,a))
    ensures enabled(c,apply(c,s,Action::Accept { envelope: envelope(s,a) }),Action::Commit { envelope: envelope(s,a) }),
        inv(c,commit_pair(c,s,a)),commit_pair(c,s,a).core == with_placement(s.core,p::apply(c,s.core.placement,a)),
        forall|authority: Authority| head(commit_pair(c,s,a),authority).incarnation == head(s,authority).incarnation,
        forall|authority: Authority| head(commit_pair(c,s,a),authority).membership == head(s,authority).membership,
        forall|authority: Authority| active(commit_pair(c,s,a),authority) == active(s,authority)
{
    let e = envelope(s,a);
    lemma_step(c,s,Action::Accept { envelope: e });
    lemma_step(c,apply(c,s,Action::Accept { envelope: e }),Action::Commit { envelope: e });
}

pub struct Interrupted {
    pub ready: State,pub accepted: State,pub crashed: State,pub committed: State,
    pub coordinator_recovered: State,pub destination_committed: State,pub destination_recovered: State,
    pub begin: Envelope,pub start: Envelope,
}
pub open spec fn prepared(c: p::Constants, src: int, dst: int) -> State {
    let one = bootstrap(c,initial(c),Authority::Coordinator);
    let two = bootstrap(c,one,Authority::Participant(src));
    bootstrap(c,two,Authority::Participant(dst))
}
pub open spec fn handoff_ready(c: p::Constants, s: State, src: int, dst: int) -> bool {
    inv(c,s) && s.core == initial_core(c)
        && active(s,Authority::Coordinator) && active(s,Authority::Participant(src)) && active(s,Authority::Participant(dst))
        && head(s,Authority::Coordinator).incarnation == 1 && head(s,Authority::Coordinator).membership == 0
        && head(s,Authority::Coordinator).processed == Set::empty()
        && head(s,Authority::Participant(dst)).incarnation == 1 && head(s,Authority::Participant(dst)).membership == 0
        && head(s,Authority::Participant(dst)).processed == Set::empty()
}
pub proof fn genesis_head(c: p::Constants, a: Authority)
    ensures head(initial(c),a).incarnation == 0,head(initial(c),a).membership == 0,
        head(initial(c),a).processed == Set::<Envelope>::empty()
{}
pub proof fn prepared_witness(c: p::Constants, src: int, dst: int)
    requires p::constants_ok(c),c.shards.contains(src),c.shards.contains(dst),src != dst
    ensures handoff_ready(c,prepared(c,src,dst),src,dst)
{
    hide(bootstrap);
    hide(bootstrap_enabled);
    hide(snapshot_start);
    hide(reconstructed_runtime);
    hide(admitted_runtime);
    hide(replay_n);
    hide(inv);
    lemma_init(c);
    genesis_head(c,Authority::Coordinator);
    genesis_head(c,Authority::Participant(src));
    genesis_head(c,Authority::Participant(dst));
    bootstrap_summary(c,initial(c),Authority::Coordinator);
    let one = bootstrap(c,initial(c),Authority::Coordinator);
    assert(one.journals[Authority::Participant(src)] == initial(c).journals[Authority::Participant(src)]);
    assert(one.journals[Authority::Participant(dst)] == initial(c).journals[Authority::Participant(dst)]);
    assert(head(one,Authority::Participant(src)).incarnation == 0);
    assert(head(one,Authority::Participant(dst)).incarnation == 0);
    bootstrap_summary(c,one,Authority::Participant(src));
    let two = bootstrap(c,one,Authority::Participant(src));
    assert(two.journals[Authority::Participant(dst)] == one.journals[Authority::Participant(dst)]);
    assert(two.journals[Authority::Coordinator] == one.journals[Authority::Coordinator]);
    assert(head(two,Authority::Participant(dst)).incarnation == 0);
    assert(two.runtime[Authority::Coordinator] == one.runtime[Authority::Coordinator]);
    bootstrap_summary(c,two,Authority::Participant(dst));
    let final_state = prepared(c,src,dst);
    assert(final_state.runtime[Authority::Coordinator] == two.runtime[Authority::Coordinator]);
    assert(final_state.runtime[Authority::Participant(src)] == two.runtime[Authority::Participant(src)]);
    assert(final_state.journals[Authority::Coordinator] == two.journals[Authority::Coordinator]);
    assert(head(final_state,Authority::Coordinator).incarnation == 1);
    assert(head(final_state,Authority::Participant(dst)).incarnation == 1);
}
pub open spec fn interrupted_from_ready(c: p::Constants, ready: State,
    nonce: int, src: int, dst: int, table: int, lo: int, hi: Option<int>) -> Interrupted {
    let begin = envelope(ready,p::Action::Begin { nonce,src,dst,table,lo,hi });
    let accepted = apply(c,ready,Action::Accept { envelope: begin });
    let crashed = apply(c,accepted,Action::Crash { authority: Authority::Coordinator });
    let committed = apply(c,crashed,Action::Commit { envelope: begin });
    let coordinator_recovered = bootstrap(c,committed,Authority::Coordinator);
    let local = p::Action::Deliver { generation: 1,command: p::Command::Start,owner: dst };
    let start = envelope(coordinator_recovered,local);
    let destination_committed = commit_pair(c,coordinator_recovered,local);
    let destination_recovered = bootstrap(c,destination_committed,Authority::Participant(dst));
    Interrupted { ready,accepted,crashed,committed,coordinator_recovered,destination_committed,destination_recovered,begin,start }
}
pub open spec fn interrupted(c: p::Constants, nonce: int, src: int, dst: int, table: int, lo: int, hi: Option<int>) -> Interrupted {
    interrupted_from_ready(c,prepared(c,src,dst),nonce,src,dst,table,lo,hi)
}
pub open spec fn interrupted_result(c: p::Constants, w: Interrupted, dst: int) -> bool {
    inv(c,w.destination_recovered)
    && enabled(c,w.ready,Action::Accept { envelope: w.begin })
    && enabled(c,w.accepted,Action::Crash { authority: Authority::Coordinator })
    && enabled(c,w.crashed,Action::AppendUnknown { envelope: w.begin })
    && enabled(c,w.crashed,Action::Commit { envelope: w.begin })
    && enabled(c,w.coordinator_recovered,Action::Accept { envelope: w.start })
    && enabled(c,apply(c,w.coordinator_recovered,Action::Accept { envelope: w.start }),Action::Commit { envelope: w.start })
    && !active(w.crashed,Authority::Coordinator) && !active(w.committed,Authority::Coordinator)
    && w.accepted.core.placement.active is None
    && w.committed.core.placement.active == Some(1)
    && bootstrap_enabled(c,w.committed,Authority::Coordinator)
    && bootstrap_enabled(c,w.destination_committed,Authority::Participant(dst))
    && w.destination_recovered.core.placement.active == Some(1)
    && w.destination_recovered.core.placement.phases[1] is Copy
    && head(w.coordinator_recovered,Authority::Coordinator).incarnation == 2
    && head(w.destination_recovered,Authority::Participant(dst)).incarnation == 2
    && dispatch(c,w.coordinator_recovered,Action::Commit { envelope: w.begin }) == w.coordinator_recovered
    && dispatch(c,w.destination_recovered,Action::Commit { envelope: w.start }) == w.destination_recovered
    && forall|k: int| w.destination_recovered.core.placement.plans[1].keys.contains(k) ==> {
        let r = recovered_participant(w.destination_recovered,dst).replicas[k];
        r.role is Stage && r.fence == 1 && r.epoch == 1 && !r.terminal
    }
}
pub proof fn initial_handoff_shape(c: p::Constants, nonce: int, src: int, dst: int, table: int, lo: int, hi: Option<int>)
    requires p::constants_ok(c),p::enabled(c,p::initial(c),p::Action::Begin { nonce,src,dst,table,lo,hi })
    ensures {
        let begun = p::apply(c,p::initial(c),p::Action::Begin { nonce,src,dst,table,lo,hi });
        let local = p::Action::Deliver { generation: 1,command: p::Command::Start,owner: dst };
        let staged = p::apply(c,begun,local);
        p::initial(c).active is None && p::initial(c).next_generation == 1
            && c.shards.contains(src) && c.shards.contains(dst) && src != dst
            && begun.active == Some(1) && begun.phases[1] is Copy
            && begun.plans[1].src == src && begun.plans[1].dst == dst
            && p::enabled(c,begun,local) && p::local_guard(begun,1,p::Command::Start,dst)
            && staged.active == Some(1) && staged.phases[1] is Copy
            && forall|k: int| staged.plans[1].keys.contains(k) ==> {
                let r = p::replica(staged,dst,k);
                r.role is Stage && r.fence == 1 && r.epoch == 1 && !r.terminal
            }
    },
{
    let begin = p::Action::Begin { nonce,src,dst,table,lo,hi };
    let begun = p::apply(c,p::initial(c),begin);
    p::lemma_init(c);
    p::lemma_step(c,p::initial(c),begin);
    assert forall|k: int| begun.plans[1].keys.contains(k) implies
        p::replica(begun,dst,k).role is Empty && p::replica(begun,dst,k).fence < 1 by {
        assert(p::directory(p::initial(c))[k].owner == src);
        assert(c.owners[k] == src);
    }
    assert(p::local_guard(begun,1,p::Command::Start,dst));
    let local = p::Action::Deliver { generation: 1,command: p::Command::Start,owner: dst };
    p::lemma_step(c,begun,local);
}

pub proof fn interrupted_from_ready_witness(c: p::Constants, ready: State,
    nonce: int, src: int, dst: int, table: int, lo: int, hi: Option<int>)
    requires p::constants_ok(c),handoff_ready(c,ready,src,dst),
        p::enabled(c,p::initial(c),p::Action::Begin { nonce,src,dst,table,lo,hi })
    ensures interrupted_result(c,interrupted_from_ready(c,ready,nonce,src,dst,table,lo,hi),dst)
{
    hide(bootstrap);
    hide(bootstrap_enabled);
    hide(commit_pair);
    hide(snapshot_start);
    hide(reconstructed_runtime);
    hide(admitted_runtime);
    hide(replay_n);
    hide(inv);
    hide(p::initial);
    hide(p::apply);
    hide(p::enabled);
    initial_handoff_shape(c,nonce,src,dst,table,lo,hi);
    let w = interrupted_from_ready(c,ready,nonce,src,dst,table,lo,hi);
    assert(enabled(c,w.ready,Action::Accept { envelope: w.begin }));
    lemma_step(c,w.ready,Action::Accept { envelope: w.begin });
    lemma_step(c,w.accepted,Action::Crash { authority: Authority::Coordinator });
    assert(enabled(c,w.crashed,Action::Commit { envelope: w.begin }));
    lemma_step(c,w.crashed,Action::Commit { envelope: w.begin });
    let participant = Authority::Participant(dst);
    authority_present(c,ready,participant);
    authority_present(c,w.crashed,participant);
    authority_present(c,w.committed,participant);
    theorem_separate_authorities(c,w.crashed,w.begin,participant);
    assert(w.crashed.journals[participant] == ready.journals[participant]);
    assert(w.committed.journals[participant] == ready.journals[participant]);
    assert(active(w.committed,participant));
    bootstrap_summary(c,w.committed,Authority::Coordinator);
    authority_present(c,w.coordinator_recovered,participant);
    assert(w.coordinator_recovered.journals[participant] == w.committed.journals[participant]);
    assert(w.coordinator_recovered.runtime[participant] == w.committed.runtime[participant]);
    assert(head(w.coordinator_recovered,participant).incarnation == 1);
    assert(head(w.coordinator_recovered,participant).membership == 0);
    assert(head(w.coordinator_recovered,participant).processed == Set::<Envelope>::empty());
    assert(w.coordinator_recovered.core.placement == p::apply(c,p::initial(c),
        p::Action::Begin { nonce,src,dst,table,lo,hi }));
    theorem_stale_incarnation(c,w.coordinator_recovered,w.begin);
    let local = p::Action::Deliver { generation: 1,command: p::Command::Start,owner: dst };
    assert(p::enabled(c,w.coordinator_recovered.core.placement,local));
    assert(w.start == envelope(w.coordinator_recovered,local));
    assert(enabled(c,w.coordinator_recovered,Action::Accept { envelope: w.start }));
    commit_pair_enabled(c,w.coordinator_recovered,local);
    bootstrap_summary(c,w.destination_committed,Authority::Participant(dst));
    theorem_stale_incarnation(c,w.destination_recovered,w.start);
    let begun = p::apply(c,p::initial(c),p::Action::Begin { nonce,src,dst,table,lo,hi });
    let staged = p::apply(c,begun,local);
    assert(w.destination_recovered.core.placement == staged);
    assert forall|k: int| w.destination_recovered.core.placement.plans[1].keys.contains(k) implies {
        let r = recovered_participant(w.destination_recovered,dst).replicas[k];
        r.role is Stage && r.fence == 1 && r.epoch == 1 && !r.terminal
    } by {
        recovered_active_replica(c,w.destination_recovered,1,dst,k);
        assert(p::replica(staged,dst,k).role is Stage
            && p::replica(staged,dst,k).fence == 1
            && p::replica(staged,dst,k).epoch == 1
            && !p::replica(staged,dst,k).terminal);
    }
}
pub proof fn interrupted_handoff_witness(c: p::Constants, nonce: int, src: int, dst: int, table: int, lo: int, hi: Option<int>)
    requires p::constants_ok(c),p::enabled(c,p::initial(c),p::Action::Begin { nonce,src,dst,table,lo,hi })
    ensures interrupted_result(c,interrupted(c,nonce,src,dst,table,lo,hi),dst)
{
    hide(prepared);
    hide(interrupted_from_ready);
    hide(inv);
    prepared_witness(c,src,dst);
    interrupted_from_ready_witness(c,prepared(c,src,dst),nonce,src,dst,table,lo,hi);
}

pub open spec fn concrete_constants() -> p::Constants {
    p::Constants {
        keys: Set::empty().insert(0),shards: Set::empty().insert(0).insert(1),
        table: Map::empty().insert(0,0),coordinate: Map::empty().insert(0,0),owners: Map::empty().insert(0,0),
    }
}
pub proof fn concrete_interrupted_handoff()
    ensures interrupted_result(concrete_constants(),interrupted(concrete_constants(),7,0,1,0,0,None),1)
{
    let c = concrete_constants();
    assert(p::selected(c,0,0,None) =~= Set::<int>::empty().insert(0));
    assert(c.shards.len() == 2);
    assert(p::constants_ok(c));
    assert(p::enabled(c,p::initial(c),p::Action::Begin { nonce: 7,src: 0,dst: 1,table: 0,lo: 0,hi: None }));
    interrupted_handoff_witness(c,7,0,1,0,0,None);
}

// A second crash after the admission record committed but before activation
// consumes another incarnation. It cannot reuse the first admission record.
pub proof fn postcommit_preactivation_witness(c: p::Constants, s: State, a: Authority)
    requires inv(c,s),valid_authority(c,a),registered(s,a)
    ensures inv(c,bootstrap(c,admitted_runtime(c,s,a),a)),
        !active(admitted_runtime(c,s,a),a),
        bootstrap_enabled(c,admitted_runtime(c,s,a),a),
        head(bootstrap(c,admitted_runtime(c,s,a),a),a).incarnation == head(s,a).incarnation+2,
        bootstrap(c,admitted_runtime(c,s,a),a).pending == s.pending,
        bootstrap(c,admitted_runtime(c,s,a),a).settled == s.settled
{
    bootstrap_witness(c,s,a);
    bootstrap_witness(c,admitted_runtime(c,s,a),a);
}

pub open spec fn rejoin_ready() -> State {
    let c = concrete_constants();
    bootstrap(c,bootstrap(c,initial(c),Authority::Coordinator),Authority::Participant(1))
}
pub open spec fn rejoin_removed() -> State {
    let c = concrete_constants();
    apply(c,apply(c,rejoin_ready(),Action::Crash { authority: Authority::Participant(1) }),Action::Remove { owner: 1 })
}
pub open spec fn rejoin_registered() -> State {
    apply(concrete_constants(),rejoin_removed(),Action::Join { owner: 1 })
}
pub open spec fn rejoin_serving() -> State {
    bootstrap(concrete_constants(),rejoin_registered(),Authority::Participant(1))
}
pub proof fn rejoin_ready_witness()
    ensures inv(concrete_constants(),rejoin_ready()),rejoin_ready().core == initial_core(concrete_constants()),
        active(rejoin_ready(),Authority::Coordinator),active(rejoin_ready(),Authority::Participant(1)),
        head(rejoin_ready(),Authority::Participant(1)).incarnation == 1
{
    hide(bootstrap);
    hide(bootstrap_enabled);
    hide(snapshot_start);
    hide(reconstructed_runtime);
    hide(admitted_runtime);
    hide(replay_n);
    hide(inv);
    let c = concrete_constants();
    assert(c.shards.len() == 2);
    lemma_init(c);
    genesis_head(c,Authority::Participant(1));
    bootstrap_summary(c,initial(c),Authority::Coordinator);
    let one = bootstrap(c,initial(c),Authority::Coordinator);
    assert(one.journals[Authority::Participant(1)] == initial(c).journals[Authority::Participant(1)]);
    assert(head(one,Authority::Participant(1)).incarnation == 0);
    bootstrap_summary(c,one,Authority::Participant(1));
}
pub proof fn rejoin_registered_witness()
    ensures inv(concrete_constants(),rejoin_registered()),
        rejoin_registered().core.members.contains_key(1),rejoin_registered().core.members[1] == 1,
        head(rejoin_ready(),Authority::Participant(1)).incarnation == 1,
        head(rejoin_registered(),Authority::Participant(1)).incarnation == 1,
        !active(rejoin_registered(),Authority::Participant(1)),
        enabled(concrete_constants(),apply(concrete_constants(),rejoin_ready(),Action::Crash { authority: Authority::Participant(1) }),
            Action::Remove { owner: 1 }),
        enabled(concrete_constants(),rejoin_removed(),Action::Join { owner: 1 })
{
    hide(rejoin_ready);
    hide(inv);
    rejoin_ready_witness();
    let c = concrete_constants();
    lemma_step(c,rejoin_ready(),Action::Crash { authority: Authority::Participant(1) });
    let crashed = apply(c,rejoin_ready(),Action::Crash { authority: Authority::Participant(1) });
    assert(owner_idle(crashed,1));
    lemma_step(c,crashed,Action::Remove { owner: 1 });
    lemma_step(c,rejoin_removed(),Action::Join { owner: 1 });
}
pub proof fn concrete_numerical_owner_rejoin()
    ensures inv(concrete_constants(),rejoin_serving()),
        enabled(concrete_constants(),apply(concrete_constants(),rejoin_ready(),Action::Crash { authority: Authority::Participant(1) }),
            Action::Remove { owner: 1 }),
        enabled(concrete_constants(),rejoin_removed(),Action::Join { owner: 1 }),
        bootstrap_enabled(concrete_constants(),rejoin_registered(),Authority::Participant(1)),
        rejoin_serving().core.members[1] == 1,
        head(rejoin_ready(),Authority::Participant(1)).incarnation == 1,
        head(rejoin_serving(),Authority::Participant(1)).incarnation == 2,
        !credential(rejoin_serving(),Authority::Participant(1),1,0),
        !active(rejoin_registered(),Authority::Participant(1))
{
    hide(bootstrap);
    hide(bootstrap_enabled);
    hide(rejoin_ready);
    hide(rejoin_removed);
    hide(rejoin_registered);
    hide(inv);
    rejoin_registered_witness();
    let c = concrete_constants();
    bootstrap_summary(c,rejoin_registered(),Authority::Participant(1));
}
} // verus!
