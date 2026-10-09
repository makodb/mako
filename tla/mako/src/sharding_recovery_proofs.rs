use vstd::prelude::*;
use vstd::imap::IMap;
use super::*;

verus! {

pub open spec fn log_ok(log: Seq<Checkpoint>, engine: Seq<EngineWrite>) -> bool {
    log.len() > 0
    && forall|i: int| 0 <= i < log.len() ==> {
        &&& log[i].engine_offset <= engine.len()
        &&& log[i].incarnation <= log.last().incarnation
        &&& log[i].processed.subset_of(log.last().processed)
    }
}
pub open spec fn durable_inv(s: State, a: Authority) -> bool {
    log_ok(s.journals[a],s.engines[a])
    && reconstructed(s.journals[a],s.engines[a]) == project(s.core,a)
    && (!(a is Participant) ==> s.engines[a].len() == 0)
}
pub open spec fn runtime_inv(s: State, a: Authority) -> bool {
    let r = s.runtime[a]; let log = s.journals[a]; let engine = s.engines[a];
    &&& r.cursor <= log.len() && r.engine_cursor <= engine.len()
    &&& if r.cursor == 0 { r.cache is None && r.engine_cursor == 0 }
        else { r.cache is Some && log[r.cursor-1].engine_offset <= r.engine_cursor
            && r.cache.unwrap() == engine_fold(log[r.cursor-1].image,engine,log[r.cursor-1].engine_offset,r.engine_cursor) }
    &&& (r.mode is Down ==> r == down())
    &&& (r.mode is Recovered ==> r.cursor > 0 && r.admission_floor == head(s,a).incarnation)
    &&& (r.mode is Admitted || r.mode is Active ==> r.cursor > 0 && r.admission_floor < head(s,a).incarnation)
    &&& (r.mode is Active ==> caught_up(s,a) && registered(s,a) && head(s,a).membership == member(s,a))
}
pub open spec fn registry_inv(c: p::Constants, s: State) -> bool {
    &&& s.core.owner_floor.dom() == c.shards
    &&& s.core.members.dom().subset_of(c.shards)
    &&& forall|n: int| s.core.members.contains_key(n) ==> s.core.members[n] == s.core.owner_floor[n]
    &&& forall|k: int| c.keys.contains(k) ==> s.core.members.contains_key(p::directory(s.core.placement)[k].owner)
    &&& (s.core.placement.active is Some ==> {
        let plan = s.core.placement.plans[s.core.placement.active.unwrap()];
        s.core.members.contains_key(plan.src) && s.core.members.contains_key(plan.dst)
    })
}
pub open spec fn copy_backing(s: State, n: int, k: int) -> bool {
    let r = p::replica(s.core.placement,n,k);
    &&& (r.role is Ready ==> r.round == 1 && r.covered)
    &&& (r.covered && !r.terminal && (r.role is Stage || r.role is Ready) ==> {
        let packet = p::Packet { generation: r.fence,round: r.round,key: k,cell: r.cell };
        s.copied.contains(packet) && s.core.placement.plans[r.fence].dst == n
    })
}
pub open spec fn protocol_inv(c: p::Constants, s: State) -> bool {
    &&& s.settled == s.core.placement.sessions.dom().filter(|t: int| s.core.placement.sessions[t].resolved)
    &&& forall|t: int| s.pending.contains_key(t) ==> p::can_resolve(s.core.placement,t,s.pending[t])
    &&& s.copied.subset_of(s.copying) && s.copying.subset_of(s.core.placement.packets)
    &&& s.cleaned.subset_of(s.cleaning)
    &&& forall|entry: (nat,int,p::Command)| s.cleaning.contains(entry) ==> {
        s.core.placement.commands.contains((entry.0,entry.2))
            && s.core.placement.plans.contains_key(entry.0)
            && destructive(s.core.placement,entry.0,entry.2,entry.1)
    }
    &&& forall|n: int,k: int| c.shards.contains(n) && c.keys.contains(k) ==> copy_backing(s,n,k)
}
pub open spec fn namespace_inv(s: State) -> bool {
    s.core.placement.sessions.dom().subset_of(s.lease_names)
        && forall|t: int| s.lease_names.contains(t) <==> s.journals[Authority::Transaction(t)].len() > 1
}
pub open spec fn inv(c: p::Constants, s: State) -> bool {
    &&& p::inv(c,s.core.placement)
    &&& (forall|a: Authority| s.journals.contains_key(a) && s.engines.contains_key(a) && s.runtime.contains_key(a))
    &&& forall|a: Authority| durable_inv(s,a) && runtime_inv(s,a)
    &&& registry_inv(c,s) && protocol_inv(c,s) && namespace_inv(s)
}

pub proof fn log_push(log: Seq<Checkpoint>, engine: Seq<EngineWrite>, record: Checkpoint)
    requires log_ok(log,engine),record.engine_offset <= engine.len(),
        record.incarnation >= log.last().incarnation,log.last().processed.subset_of(record.processed)
    ensures log_ok(log.push(record),engine)
{
    assert forall|i: int| 0 <= i < log.push(record).len() implies {
        &&& log.push(record)[i].engine_offset <= engine.len()
        &&& log.push(record)[i].incarnation <= log.push(record).last().incarnation
        &&& log.push(record)[i].processed.subset_of(log.push(record).last().processed)
    } by {
        if i < log.len() {
            assert(log[i].processed.subset_of(log.last().processed));
            assert(log[i].processed.subset_of(record.processed));
        }
    }
}
pub proof fn complete_cache(s: State, a: Authority)
    requires durable_inv(s,a),runtime_inv(s,a),caught_up(s,a)
    ensures s.runtime[a].cache == Some(project(s.core,a))
{
    journal_replacement(s.journals[a],s.journals[a].len());
}
pub proof fn lemma_init(c: p::Constants)
    requires p::constants_ok(c)
    ensures inv(c,initial(c))
{
    p::lemma_init(c);
    let s = initial(c);
    assert forall|a: Authority| durable_inv(s,a) && runtime_inv(s,a) by {
        checkpoint_reconstruction(Seq::empty(),Seq::empty(),head(s,a));
    }
    assert(s.settled =~= s.core.placement.sessions.dom().filter(|t: int| s.core.placement.sessions[t].resolved));
}

pub proof fn placement_step(c: p::Constants, s: State, action: Action)
    requires inv(c,s),enabled(c,s,action)
    ensures p::inv(c,apply(c,s,action).core.placement)
{
    match action {
        Action::Commit { envelope: e } => {
            if p::enabled(c,s.core.placement,e.action) { p::lemma_step(c,s.core.placement,e.action); }
        },
        Action::EngineSettle { txn,committed } => {
            let writes = if committed { s.pending[txn] } else { Map::empty() };
            assert(p::can_resolve(s.core.placement,txn,writes));
            p::lemma_step(c,s.core.placement,p::Action::Resolve { txn,writes });
        },
        _ => {},
    }
}
pub proof fn authority_present(c: p::Constants, s: State, a: Authority)
    requires inv(c,s)
    ensures s.journals.contains_key(a),s.engines.contains_key(a),s.runtime.contains_key(a)
{}
pub proof fn authority_facts(c: p::Constants, s: State, a: Authority)
    requires inv(c,s)
    ensures durable_inv(s,a),runtime_inv(s,a),log_ok(s.journals[a],s.engines[a]),
        s.journals[a].len() > 0,head(s,a).engine_offset <= s.engines[a].len(),
        s.runtime[a].cursor <= s.journals[a].len(),s.runtime[a].engine_cursor <= s.engines[a].len()
{
    assert(durable_inv(s,a));
    assert(runtime_inv(s,a));
    assert(log_ok(s.journals[a],s.engines[a]));
    assert(s.journals[a][s.journals[a].len()-1].engine_offset <= s.engines[a].len());
}
pub proof fn durable_step(c: p::Constants, s: State, action: Action, a: Authority)
    requires inv(c,s),enabled(c,s,action)
    ensures durable_inv(apply(c,s,action),a)
{
    authority_facts(c,s,a);
    let z = apply(c,s,action);
    match action {
        Action::Commit { envelope: e } => {
            let owner = actor(s.core.placement,e.action);
            if a == owner {
                log_push(s.journals[a],s.engines[a],head(z,a));
                checkpoint_reconstruction(s.journals[a],s.engines[a],head(z,a));
            } else if p::enabled(c,s.core.placement,e.action) {
                placement_locality(c,s.core,e.action,a);
            }
        },
        Action::Admit { authority } => {
            if a == authority {
                complete_cache(s,a);
                log_push(s.journals[a],s.engines[a],head(z,a));
                checkpoint_reconstruction(s.journals[a],s.engines[a],head(z,a));
            }
        },
        Action::Remove { .. } | Action::Join { .. } => {
            if a == Authority::Coordinator {
                log_push(s.journals[a],s.engines[a],head(z,a));
                checkpoint_reconstruction(s.journals[a],s.engines[a],head(z,a));
            }
        },
        Action::EngineSettle { txn,committed } => {
            let writes = if committed { s.pending[txn] } else { Map::empty() };
            transaction_locality(c,s.core,txn,writes,a);
            match a {
                Authority::Participant(n) => {
                    let write = transaction_write(s.core.placement,txn,writes,n);
                    if write.cells.dom() != Set::<int>::empty() {
                        engine_append(s.journals[a],s.engines[a],write);
                    } else {
                        assert(write.cells =~= Map::empty());
                        empty_overlay(project(s.core,a));
                    }
                    assert forall|i: int| 0 <= i < z.journals[a].len() implies
                        z.journals[a][i].engine_offset <= z.engines[a].len()
                        && z.journals[a][i].incarnation <= z.journals[a].last().incarnation
                        && z.journals[a][i].processed.subset_of(z.journals[a].last().processed) by {}
                },
                _ => {},
            }
        },
        _ => {},
    }
}
pub proof fn runtime_step(c: p::Constants, s: State, action: Action, a: Authority)
    requires inv(c,s),enabled(c,s,action),durable_inv(apply(c,s,action),a)
    ensures runtime_inv(apply(c,s,action),a)
{
    authority_facts(c,s,a);
    let z = apply(c,s,action); let old = s.runtime[a];
    match action {
        Action::ReplayCheckpoint { authority } => {
            if a == authority {
                assert(s.journals[a][old.cursor as int].engine_offset <= s.engines[a].len());
            }
        },
        Action::InstallSnapshot { authority,index } => {
            if a == authority { assert(s.journals[a][index as int].engine_offset <= s.engines[a].len()); }
        },
        Action::ReplayEngine { authority } => {
            if a == authority {
                assert(old.cursor > 0);
                assert(old.cache.unwrap() == engine_fold(s.journals[a][old.cursor-1].image,s.engines[a],
                    s.journals[a][old.cursor-1].engine_offset,old.engine_cursor));
            }
        },
        Action::Admit { authority } => {
            if a == authority { complete_cache(s,a); }
        },
        Action::EngineSettle { txn,committed } => {
            if !active(s,a) && old.cursor > 0 {
                match a {
                    Authority::Participant(n) => {
                        let writes = if committed { s.pending[txn] } else { Map::empty() };
                        let write = transaction_write(s.core.placement,txn,writes,n);
                        if write.cells.dom() != Set::<int>::empty() {
                            engine_prefix(s.journals[a][old.cursor-1].image,s.engines[a],write,
                                s.journals[a][old.cursor-1].engine_offset,old.engine_cursor);
                        }
                    },
                    _ => {},
                }
            }
        },
        Action::Remove { owner } | Action::Join { owner } => {
            if active(s,a) {
                match a { Authority::Participant(n) => { assert(n != owner); }, _ => {} }
            }
        },
        _ => {},
    }
    if active(z,a) {
        journal_replacement(z.journals[a],z.journals[a].len());
    }
}

pub proof fn assemble_registry(c: p::Constants, s: State)
    requires s.core.owner_floor.dom() == c.shards,s.core.members.dom().subset_of(c.shards),
        forall|n: int| s.core.members.contains_key(n) ==> s.core.members[n] == s.core.owner_floor[n],
        forall|k: int| c.keys.contains(k) ==> s.core.members.contains_key(p::directory(s.core.placement)[k].owner),
        s.core.placement.active is Some ==> {
            let plan = s.core.placement.plans[s.core.placement.active.unwrap()];
            s.core.members.contains_key(plan.src) && s.core.members.contains_key(plan.dst)
        },
    ensures registry_inv(c,s)
{
    reveal(registry_inv);
}
pub proof fn registry_facts(c: p::Constants, s: State)
    requires registry_inv(c,s)
    ensures s.core.owner_floor.dom() == c.shards,s.core.members.dom().subset_of(c.shards),
        forall|n: int| s.core.members.contains_key(n) ==> s.core.members[n] == s.core.owner_floor[n],
        forall|k: int| c.keys.contains(k) ==> s.core.members.contains_key(p::directory(s.core.placement)[k].owner),
        s.core.placement.active is Some ==> {
            let plan = s.core.placement.plans[s.core.placement.active.unwrap()];
            s.core.members.contains_key(plan.src) && s.core.members.contains_key(plan.dst)
        },
{
    reveal(registry_inv);
}
pub proof fn registry_step(c: p::Constants, s: State, action: Action)
    requires inv(c,s),enabled(c,s,action),p::inv(c,apply(c,s,action).core.placement)
    ensures registry_inv(c,apply(c,s,action))
{
    hide(registry_inv);
    assert(registry_inv(c,s));
    registry_facts(c,s);
    let z = apply(c,s,action);
    match action {
        Action::Commit { envelope: e } => {
            assert(z.core.members == s.core.members && z.core.owner_floor == s.core.owner_floor);
            if p::enabled(c,s.core.placement,e.action) {
                match e.action {
                    p::Action::Begin { src,dst,.. } => {
                        assert(s.core.members.contains_key(src) && s.core.members.contains_key(dst));
                        assert(z.core.placement.active == Some(s.core.placement.next_generation));
                        assert(z.core.placement.plans[s.core.placement.next_generation].src == src);
                        assert(z.core.placement.plans[s.core.placement.next_generation].dst == dst);
                    },
                    p::Action::Commit => {
                        let g = s.core.placement.active.unwrap();
                        assert(z.core.placement.active == s.core.placement.active);
                        assert(z.core.placement.plans == s.core.placement.plans);
                        assert(p::plan_inv(c,s.core.placement,g));
                        assert(s.core.members.contains_key(s.core.placement.plans[g].dst));
                        assert forall|k: int| c.keys.contains(k) implies
                            z.core.members.contains_key(p::directory(z.core.placement)[k].owner) by {
                            assert(s.core.members.contains_key(p::directory(s.core.placement)[k].owner));
                        }
                    },
                    p::Action::Finish => { assert(z.core.placement.active is None); },
                    _ => {
                        assert(z.core.placement.active == s.core.placement.active);
                        if z.core.placement.active is Some {
                            let g = z.core.placement.active.unwrap();
                            assert(z.core.placement.plans[g] == s.core.placement.plans[g]);
                        }
                    },
                }
                if !(e.action is Commit) {
                    assert(p::directory(z.core.placement) == p::directory(s.core.placement));
                }
            } else {
                assert(z.core.placement == s.core.placement);
            }
            assert(z.core.owner_floor.dom() == c.shards);
            assert(z.core.members.dom().subset_of(c.shards));
            assert forall|n: int| z.core.members.contains_key(n) implies z.core.members[n] == z.core.owner_floor[n] by {
                assert(s.core.members.contains_key(n));
                assert(s.core.members[n] == s.core.owner_floor[n]);
            }
            assert forall|k: int| c.keys.contains(k) implies z.core.members.contains_key(p::directory(z.core.placement)[k].owner) by {
                if !(e.action is Commit) || !p::enabled(c,s.core.placement,e.action) {
                    assert(s.core.members.contains_key(p::directory(s.core.placement)[k].owner));
                }
            }
            if z.core.placement.active is Some {
                let plan = z.core.placement.plans[z.core.placement.active.unwrap()];
                assert(z.core.members.contains_key(plan.src) && z.core.members.contains_key(plan.dst));
            }
        },
        Action::Remove { owner } => {
            assert(z.core.owner_floor == s.core.owner_floor);
            assert forall|k: int| c.keys.contains(k) implies z.core.members.contains_key(p::directory(z.core.placement)[k].owner) by {
                assert(p::directory(s.core.placement).contains_key(k));
            }
        },
        Action::Join { owner } => {
            assert(s.core.owner_floor.contains_key(owner));
            assert(z.core.owner_floor == s.core.owner_floor.insert(owner,s.core.owner_floor[owner]+1));
            assert(z.core.owner_floor.dom() =~= s.core.owner_floor.dom());
            assert(z.core.members.dom().subset_of(c.shards));
            assert forall|n: int| z.core.members.contains_key(n) implies z.core.members[n] == z.core.owner_floor[n] by {
                if n != owner {
                    assert(s.core.members.contains_key(n));
                    assert(s.core.members[n] == s.core.owner_floor[n]);
                }
            }
            assert forall|k: int| c.keys.contains(k) implies z.core.members.contains_key(p::directory(z.core.placement)[k].owner) by {
                assert(s.core.members.contains_key(p::directory(s.core.placement)[k].owner));
            }
        },
        _ => {
            assert(z.core.owner_floor == s.core.owner_floor);
            assert(z.core.members == s.core.members);
            assert(z.core.placement.directory == s.core.placement.directory);
            assert(z.core.placement.plans == s.core.placement.plans);
            assert(z.core.placement.active == s.core.placement.active);
        },
    }
    assemble_registry(c,z);
}
pub proof fn backing_step(c: p::Constants, s: State, action: Action, n: int, k: int)
    requires inv(c,s),enabled(c,s,action),c.shards.contains(n),c.keys.contains(k),
        p::inv(c,apply(c,s,action).core.placement)
    ensures copy_backing(apply(c,s,action),n,k)
{
    let before = s.core.placement; let z = apply(c,s,action); let after = z.core.placement;
    assert(copy_backing(s,n,k));
    assert(p::key_inv(c,before,k));
    assert(p::key_inv(c,after,k));
    match action {
        Action::Commit { envelope: e } => {
            if p::enabled(c,before,e.action) {
                match e.action {
                    p::Action::DeliverCopy { packet } => {
                        assert(p::packet_inv(before,packet));
                        if p::copy_guard(before,packet) && packet.key == k && before.plans[packet.generation].dst == n {
                            assert(s.copied.contains(packet));
                            assert(p::replica(after,n,k).cell == packet.cell);
                            assert(p::replica(after,n,k).fence == packet.generation);
                            assert(p::replica(after,n,k).round == packet.round);
                        }
                    },
                    p::Action::Deliver { generation,command,owner } => {
                        assert(p::command_inv(before,generation,command));
                        match command {
                            p::Command::Start => {},
                            p::Command::Freeze => {},
                            p::Command::Final => {},
                            p::Command::Retire => {},
                            p::Command::Commit => {},
                            p::Command::Abort => {},
                        }
                    },
                    p::Action::Seal { generation } => {
                        if before.plans[generation].dst == n && before.plans[generation].keys.contains(k) {
                            assert(p::replica(before,n,k).round == 1 && p::replica(before,n,k).covered);
                        }
                    },
                    p::Action::Begin { .. } => {
                        assert(!(p::replica(before,n,k).role is Stage || p::replica(before,n,k).role is Ready));
                    },
                    _ => {},
                }
            }
        },
        Action::EngineSettle { txn,committed } => {
            assert(p::session_inv(c,before,txn));
            let writes = if committed { s.pending[txn] } else { Map::empty() };
            if writes.contains_key(k) && before.sessions[txn].held[k].owner == n {
                assert(before.sessions[txn].held[k] == p::directory(before)[k]);
                assert(p::replica(before,n,k).role is Serving || p::replica(before,n,k).role is Frozen);
            }
        },
        _ => {},
    }
}
pub proof fn protocol_step(c: p::Constants, s: State, action: Action)
    requires inv(c,s),enabled(c,s,action),p::inv(c,apply(c,s,action).core.placement)
    ensures protocol_inv(c,apply(c,s,action))
{
    let z = apply(c,s,action); let before = s.core.placement; let after = z.core.placement;
    assert forall|t: int| z.pending.contains_key(t) implies p::can_resolve(after,t,z.pending[t]) by {
        match action {
            Action::Commit { envelope: e } => {
                if p::enabled(c,before,e.action) {
                    match e.action {
                        p::Action::Open { txn } => { assert(t != txn); },
                        p::Action::Release { txn,.. } => { assert(t != txn); },
                        _ => {},
                    }
                }
            },
            _ => {},
        }
    }
    assert(z.settled =~= after.sessions.dom().filter(|t: int| after.sessions[t].resolved)) by {
        assert forall|t: int| z.settled.contains(t) <==> after.sessions.contains_key(t) && after.sessions[t].resolved by {
            match action {
                Action::Commit { envelope: e } => {
                    if p::enabled(c,before,e.action) {
                        match e.action { p::Action::Open { txn } => {}, _ => {} }
                    }
                },
                _ => {},
            }
        }
    }
    assert forall|entry: (nat,int,p::Command)| z.cleaning.contains(entry) implies
        after.commands.contains((entry.0,entry.2)) && after.plans.contains_key(entry.0)
        && destructive(after,entry.0,entry.2,entry.1) by {
        match action {
            Action::Commit { envelope: e } => {
                if p::enabled(c,before,e.action) {
                    match e.action {
                        p::Action::Begin { .. } => {
                            if before.plans.contains_key(entry.0) { assert(p::plan_inv(c,before,entry.0)); }
                        },
                        _ => {},
                    }
                }
            },
            Action::StartCleanup { generation,command,.. } => {
                assert(p::command_inv(before,generation,command));
            },
            _ => {},
        }
    }
    assert forall|n: int,k: int| c.shards.contains(n) && c.keys.contains(k) implies copy_backing(z,n,k) by {
        backing_step(c,s,action,n,k);
    }
}
pub proof fn namespace_step(c: p::Constants, s: State, action: Action)
    requires inv(c,s),enabled(c,s,action)
    ensures namespace_inv(apply(c,s,action))
{
    assert(namespace_inv(s));
    let z = apply(c,s,action);
    assert forall|t: int| z.core.placement.sessions.contains_key(t) implies z.lease_names.contains(t) by {
        match action {
            Action::Commit { envelope: e } => {
                if p::enabled(c,s.core.placement,e.action) {
                    match e.action {
                        p::Action::Open { txn } | p::Action::Acquire { txn,.. } | p::Action::Release { txn,.. } => {},
                        _ => {},
                    }
                }
            },
            _ => {},
        }
    }
    assert forall|t: int| z.lease_names.contains(t) <==> z.journals[Authority::Transaction(t)].len() > 1 by {
        authority_facts(c,s,Authority::Transaction(t));
        assert(s.lease_names.contains(t) <==> s.journals[Authority::Transaction(t)].len() > 1);
        match action {
            Action::Admit { authority } => { match authority { Authority::Transaction(_) => {}, _ => {} } },
            Action::Commit { envelope: e } => { match actor(s.core.placement,e.action) { Authority::Transaction(_) => {}, _ => {} } },
            _ => {},
        }
    }
}
pub proof fn lemma_step(c: p::Constants, s: State, action: Action)
    requires inv(c,s),enabled(c,s,action)
    ensures inv(c,apply(c,s,action))
{
    placement_step(c,s,action);
    assert forall|a: Authority| durable_inv(apply(c,s,action),a) && runtime_inv(apply(c,s,action),a) by {
        durable_step(c,s,action,a);
        runtime_step(c,s,action,a);
    }
    registry_step(c,s,action);
    protocol_step(c,s,action);
    namespace_step(c,s,action);
}
pub proof fn theorem_finite_behavior(c: p::Constants, states: Seq<State>, index: nat)
    requires p::constants_ok(c),behavior(c,states),index < states.len()
    ensures inv(c,states[index as int])
    decreases index
{
    if index == 0 { lemma_init(c); }
    else {
        theorem_finite_behavior(c,states,(index-1) as nat);
        let previous = states[index-1]; let current = states[index as int];
        let j: int = index as int - 1;
        assert(0 <= j < states.len()-1);
        assert(next(c,states[j],states[j+1]));
        let action = choose|a: Action| current == dispatch(c,previous,a);
        if enabled(c,previous,action) { lemma_step(c,previous,action); }
    }
}

// Recovery uses committed authority images and engine outcomes, not ghost state.
pub open spec fn recovered_leases(s: State, t: int) -> Option<Map<int,p::Grant>> {
    match reconstructed(s.journals[Authority::Transaction(t)],s.engines[Authority::Transaction(t)]) {
        Image::Leases(leases) => leases,
        _ => None,
    }
}
pub open spec fn recovered_participant(s: State, n: int) -> ParticipantImage {
    participant_image(reconstructed(s.journals[Authority::Participant(n)],s.engines[Authority::Participant(n)]))
}
pub proof fn recovered_active_replica(c: p::Constants, s: State, g: nat, n: int, k: int)
    requires inv(c,s),s.core.placement.active == Some(g),c.shards.contains(n),
        s.core.placement.plans[g].keys.contains(k)
    ensures c.keys.contains(k),recovered_participant(s,n).replicas.contains_key(k),
        recovered_participant(s,n).replicas[k] == p::replica(s.core.placement,n,k)
{
    assert(p::inv(c,s.core.placement));
    assert(s.core.placement.plans.contains_key(g));
    assert(p::plan_inv(c,s.core.placement,g));
    assert(c.keys.contains(k));
    assert(s.core.placement.physical.contains_key((n,k)));
    authority_facts(c,s,Authority::Participant(n));
}
// The pinned vstd proves finiteness of map/flatten for finite input sets.
// Use that proved construction rather than unrestricted Set::new.
pub open spec fn finite_union<T>(names: Set<int>, values: spec_fn(int) -> Set<T>) -> Set<T> {
    names.map(values).flatten()
}
pub proof fn finite_union_membership<T>(names: Set<int>, values: spec_fn(int) -> Set<T>, value: T)
    ensures finite_union(names,values).contains(value)
        <==> exists|n: int| names.contains(n) && values(n).contains(value)
{
    broadcast use Set::lemma_map_contains;
    broadcast use Set::lemma_flatten_contains;
    if finite_union(names,values).contains(value) {
        let part = choose|part: Set<T>| names.map(values).contains(part) && part.contains(value);
        let name = choose|name: int| names.contains(name) && values(name) == part;
        assert(values(name).contains(value));
    }
    if exists|name: int| names.contains(name) && values(name).contains(value) {
        let name = choose|name: int| names.contains(name) && values(name).contains(value);
        assert(names.map(values).contains(values(name)));
    }
}
pub open spec fn rebuild(c: p::Constants, s: State) -> p::State {
    match reconstructed(s.journals[Authority::Coordinator],s.engines[Authority::Coordinator]) {
        Image::Coordinator(coordinator) => p::State {
            logical: Map::new(c.keys,|k: int| recovered_participant(s,coordinator.directory.last()[k].owner).replicas[k].cell),
            sessions: Map::new(s.lease_names.filter(|t: int| recovered_leases(s,t) is Some),|t: int| p::Session {
                held: recovered_leases(s,t).unwrap(),resolved: s.settled.contains(t),
            }),
            physical: IMap::new(|q: (int,int)| c.shards.contains(q.0) && c.keys.contains(q.1),
                |q: (int,int)| recovered_participant(s,q.0).replicas[q.1]),
            certificates: finite_union(c.shards,|n: int| recovered_participant(s,n).certificates),
            packets: finite_union(c.shards,|n: int| recovered_participant(s,n).packets),
            directory: coordinator.directory,views: coordinator.views,next_generation: coordinator.next_generation,
            active: coordinator.active,plans: coordinator.plans,phases: coordinator.phases,
            commands: coordinator.commands,received: coordinator.received,outcomes: coordinator.outcomes,replies: coordinator.replies,
        },
        _ => p::initial(c),
    }
}
pub proof fn theorem_reconstruction(c: p::Constants, s: State)
    requires inv(c,s)
    ensures rebuild(c,s) == s.core.placement,p::inv(c,rebuild(c,s))
{
    authority_facts(c,s,Authority::Coordinator);
    assert(namespace_inv(s));
    assert(protocol_inv(c,s));
    let old = s.core.placement; let new = rebuild(c,s);
    assert(new.logical =~= old.logical) by {
        assert forall|k: int| c.keys.contains(k) implies new.logical[k] == old.logical[k] by {
            assert(p::key_inv(c,old,k));
            assert(durable_inv(s,Authority::Participant(p::directory(old)[k].owner)));
        }
    }
    assert(new.physical =~= old.physical) by {
        assert forall|q: (int,int)| old.physical.contains_key(q) implies new.physical[q] == old.physical[q] by {
            assert(durable_inv(s,Authority::Participant(q.0)));
        }
    }
    assert(new.sessions =~= old.sessions) by {
        assert forall|t: int| new.sessions.contains_key(t) <==> old.sessions.contains_key(t) by {
            assert(durable_inv(s,Authority::Transaction(t)));
        }
        assert forall|t: int| old.sessions.contains_key(t) implies new.sessions[t] == old.sessions[t] by {
            assert(durable_inv(s,Authority::Transaction(t)));
        }
    }
    assert(new.certificates =~= old.certificates) by {
        assert forall|gc: (nat,p::Certificate)| new.certificates.contains(gc) <==> old.certificates.contains(gc) by {
            finite_union_membership(c.shards,|n: int| recovered_participant(s,n).certificates,gc);
            if old.certificates.contains(gc) {
                certificate_plan(c,old,gc.0,gc.1);
                assert(p::plan_inv(c,old,gc.0));
                let owner = certificate_owner(old,gc.0,gc.1);
                authority_facts(c,s,Authority::Participant(owner));
                assert(c.shards.contains(owner));
                assert(recovered_participant(s,owner).certificates.contains(gc));
            }
            if new.certificates.contains(gc) {
                let owner = choose|n: int| c.shards.contains(n) && recovered_participant(s,n).certificates.contains(gc);
                assert(durable_inv(s,Authority::Participant(owner)));
            }
        }
    }
    assert(new.packets =~= old.packets) by {
        assert forall|packet: p::Packet| new.packets.contains(packet) <==> old.packets.contains(packet) by {
            finite_union_membership(c.shards,|n: int| recovered_participant(s,n).packets,packet);
            if old.packets.contains(packet) {
                assert(p::packet_inv(old,packet));
                assert(p::plan_inv(c,old,packet.generation));
                let owner = old.plans[packet.generation].src;
                authority_facts(c,s,Authority::Participant(owner));
                assert(recovered_participant(s,owner).packets.contains(packet));
            }
            if new.packets.contains(packet) {
                let owner = choose|n: int| c.shards.contains(n) && recovered_participant(s,n).packets.contains(packet);
                assert(durable_inv(s,Authority::Participant(owner)));
            }
        }
    }
}
pub proof fn theorem_reachable_reconstruction(c: p::Constants, states: Seq<State>, index: nat)
    requires p::constants_ok(c),behavior(c,states),index < states.len()
    ensures rebuild(c,states[index as int]) == states[index as int].core.placement,
        p::inv(c,rebuild(c,states[index as int]))
{
    theorem_finite_behavior(c,states,index);
    theorem_reconstruction(c,states[index as int]);
}

pub proof fn theorem_serving_gate(c: p::Constants, s: State, a: Authority)
    requires inv(c,s),active(s,a)
    ensures caught_up(s,a),s.runtime[a].cache == Some(project(s.core,a)),registered(s,a),
        head(s,a).membership == member(s,a),head(s,a).incarnation > s.runtime[a].admission_floor
{
    authority_facts(c,s,a);
    complete_cache(s,a);
}
pub proof fn theorem_acceptance_is_not_commit(c: p::Constants, s: State, e: Envelope)
    requires enabled(c,s,Action::Accept { envelope: e })
    ensures apply(c,s,Action::Accept { envelope: e }).core == s.core,
        apply(c,s,Action::Accept { envelope: e }).journals == s.journals,
        apply(c,s,Action::Accept { envelope: e }).engines == s.engines,
        apply(c,s,Action::AppendUnknown { envelope: e }) == s
{}
pub proof fn theorem_unknown_holds_leases(c: p::Constants, s: State, t: int, k: int)
    requires inv(c,s),s.pending.contains_key(t),s.core.placement.sessions[t].held.contains_key(k)
    ensures !s.settled.contains(t),!s.core.placement.sessions[t].resolved,
        !p::enabled(c,s.core.placement,p::Action::Release { txn: t,key: k }),
        !p::drained(s.core.placement,Set::empty().insert(k)),
        apply(c,s,Action::EngineUnknown { txn: t }) == s
{}
pub proof fn theorem_restart_does_not_settle(c: p::Constants, s: State, action: Action)
    requires action is Crash || action is Boot || action is ReplayCheckpoint || action is InstallSnapshot
        || action is ReplayEngine || action is Complete || action is Admit || action is Activate
    ensures apply(c,s,action).pending == s.pending,apply(c,s,action).settled == s.settled,
        apply(c,s,action).core.placement == s.core.placement
{}
pub proof fn theorem_stale_incarnation(c: p::Constants, s: State, e: Envelope)
    requires e.incarnation != head(s,actor(s.core.placement,e.action)).incarnation
    ensures dispatch(c,s,Action::Accept { envelope: e }) == s,
        dispatch(c,s,Action::Commit { envelope: e }) == s
{}
pub proof fn theorem_stale_target(c: p::Constants, s: State, e: Envelope)
    requires e.action is Acquire,e.target != target(s,e.action)
    ensures dispatch(c,s,Action::Accept { envelope: e }) == s,
        dispatch(c,s,Action::Commit { envelope: e }) == s
{}
pub proof fn theorem_duplicate_entry(c: p::Constants, s: State, e: Envelope)
    requires head(s,actor(s.core.placement,e.action)).processed.contains(e)
    ensures dispatch(c,s,Action::Commit { envelope: e }) == s
{}
pub proof fn theorem_safe_removal(c: p::Constants, s: State, n: int)
    requires enabled(c,s,Action::Remove { owner: n })
    ensures owner_idle(s,n),!active(s,Authority::Participant(n)),
        !apply(c,s,Action::Remove { owner: n }).core.members.contains_key(n),
        apply(c,s,Action::Remove { owner: n }).core.owner_floor == s.core.owner_floor
{}
pub proof fn theorem_rejoin_fence(c: p::Constants, s: State, n: int)
    requires inv(c,s),enabled(c,s,Action::Join { owner: n })
    ensures apply(c,s,Action::Join { owner: n }).core.members[n] == s.core.owner_floor[n]+1,
        !active(apply(c,s,Action::Join { owner: n }),Authority::Participant(n)),
        head(apply(c,s,Action::Join { owner: n }),Authority::Participant(n)).incarnation == head(s,Authority::Participant(n)).incarnation
{
    authority_facts(c,s,Authority::Participant(n));
    assert(!active(s,Authority::Participant(n)));
}

// A decision is a coordinator checkpoint; participant evidence comes from
// different committed journals. These consequences are not transition guards.
pub proof fn theorem_durable_decision(c: p::Constants, s: State, g: nat)
    requires inv(c,s),s.core.placement.plans.contains_key(g),s.core.placement.phases[g] is Committed
    ensures s.core.placement.outcomes[s.core.placement.plans[g].nonce] == (p::Outcome { generation: g,committed: true }),
        reconstructed(s.journals[Authority::Coordinator],s.engines[Authority::Coordinator]) == project(s.core,Authority::Coordinator),
        s.core.placement.active == Some(g) ==> {
            p::directory(s.core.placement).dom() == c.keys
            && forall|k: int| s.core.placement.plans[g].keys.contains(k) ==>
                p::directory(s.core.placement)[k] == (p::Grant { owner: s.core.placement.plans[g].dst,epoch: g })
        }
{
    authority_facts(c,s,Authority::Coordinator);
    assert(p::plan_inv(c,s.core.placement,g));
    assert forall|k: int| s.core.placement.plans[g].keys.contains(k) && s.core.placement.active == Some(g) implies
        p::directory(s.core.placement)[k] == (p::Grant { owner: s.core.placement.plans[g].dst,epoch: g }) by {
        assert(p::key_inv(c,s.core.placement,k));
    }
}
pub proof fn theorem_ready_backed_by_durable_copies(c: p::Constants, s: State, g: nat, k: int)
    requires inv(c,s),s.core.placement.plans.contains_key(g),s.core.placement.active == Some(g),
        p::before_decision(s.core.placement,g),s.core.placement.certificates.contains((g,p::Certificate::Ready)),
        s.core.placement.plans[g].keys.contains(k)
    ensures {
        let n = s.core.placement.plans[g].dst;
        let r = p::replica(s.core.placement,n,k);
        r.role is Ready && r.fence == g && r.round == 1 && r.cell == s.core.placement.logical[k]
            && s.copied.contains(p::Packet { generation: g,round: 1,key: k,cell: s.core.placement.logical[k] })
            && recovered_participant(s,n).certificates.contains((g,p::Certificate::Ready))
    },
{
    assert(p::plan_inv(c,s.core.placement,g));
    assert(p::key_inv(c,s.core.placement,k));
    let n = s.core.placement.plans[g].dst;
    authority_facts(c,s,Authority::Participant(n));
    assert(copy_backing(s,n,k));
}
pub proof fn theorem_cleanup_decision(c: p::Constants, s: State, g: nat, n: int, cmd: p::Command)
    requires inv(c,s),s.cleaned.contains((g,n,cmd))
    ensures s.core.placement.plans.contains_key(g),
        cmd is Commit ==> s.core.placement.phases[g] is Committed && n == s.core.placement.plans[g].src,
        cmd is Abort ==> s.core.placement.phases[g] is Aborted && n == s.core.placement.plans[g].dst,
        cmd is Commit || cmd is Abort
{
    assert(s.cleaning.contains((g,n,cmd)));
    assert(p::command_inv(s.core.placement,g,cmd));
}
pub proof fn theorem_durable_cleanup_receipt(c: p::Constants, s: State, e: Envelope, g: nat, n: int, cmd: p::Command)
    requires inv(c,s),enabled(c,s,Action::Commit { envelope: e }),
        e.action == (p::Action::Deliver { generation: g,command: cmd,owner: n }),destructive(s.core.placement,g,cmd,n)
    ensures s.cleaned.contains((g,n,cmd))
{}
pub proof fn theorem_retained_floors(c: p::Constants, s: State, action: Action, a: Authority, n: int, nonce: int)
    requires inv(c,s),enabled(c,s,action),c.shards.contains(n)
    ensures apply(c,s,action).core.placement.next_generation >= s.core.placement.next_generation,
        head(apply(c,s,action),a).incarnation >= head(s,a).incarnation,
        head(s,a).processed.subset_of(head(apply(c,s,action),a).processed),
        apply(c,s,action).core.owner_floor[n] >= s.core.owner_floor[n],
        s.core.placement.outcomes.contains_key(nonce) ==> apply(c,s,action).core.placement.outcomes.contains_key(nonce)
            && apply(c,s,action).core.placement.outcomes[nonce] == s.core.placement.outcomes[nonce]
{
    match action {
        Action::Commit { envelope: e } => {
            if p::enabled(c,s.core.placement,e.action) && s.core.placement.outcomes.contains_key(nonce) {
                p::lemma_outcome_once(c,s.core.placement,e.action,nonce);
            }
        },
        _ => {},
    }
}

pub proof fn theorem_separate_authorities(c: p::Constants, s: State, e: Envelope, other: Authority)
    requires enabled(c,s,Action::Commit { envelope: e }),other != actor(s.core.placement,e.action)
    ensures apply(c,s,Action::Commit { envelope: e }).journals[other] == s.journals[other],
        apply(c,s,Action::Commit { envelope: e }).engines == s.engines
{}
pub proof fn theorem_engine_does_not_commit_sharding_metadata(c: p::Constants, s: State, t: int, committed: bool)
    ensures apply(c,s,Action::EngineSettle { txn: t,committed }).journals == s.journals
{}
pub proof fn theorem_engine_terminal_once(c: p::Constants, s: State, t: int, committed: bool)
    requires enabled(c,s,Action::EngineSettle { txn: t,committed })
    ensures !enabled(c,apply(c,s,Action::EngineSettle { txn: t,committed }),Action::EngineSettle { txn: t,committed: true }),
        !enabled(c,apply(c,s,Action::EngineSettle { txn: t,committed }),Action::EngineSettle { txn: t,committed: false })
{}
pub proof fn theorem_checkpoint_idempotence(image: Image, checkpoint: Checkpoint)
    ensures replace_checkpoint(replace_checkpoint(image,checkpoint),checkpoint) == replace_checkpoint(image,checkpoint)
{}
pub proof fn theorem_engine_overlay_idempotence(image: Image, write: EngineWrite)
    ensures overlay(overlay(image,write),write) == overlay(image,write)
{
    match image {
        Image::Participant(_) => {
            assert(participant_image(overlay(overlay(image,write),write)).replicas
                =~= participant_image(overlay(image,write)).replicas);
        },
        _ => {},
    }
}
pub proof fn theorem_local_evidence(c: p::Constants, s: State, g: nat, cert: p::Certificate)
    requires inv(c,s),s.core.placement.certificates.contains((g,cert))
    ensures recovered_participant(s,certificate_owner(s.core.placement,g,cert)).certificates.contains((g,cert)),
        s.core.placement.active == Some(g) && p::before_decision(s.core.placement,g)
            && (cert is Drained || cert is Ready || cert is Retired) ==>
            p::source_frozen(s.core.placement,g) && p::drained(s.core.placement,s.core.placement.plans[g].keys),
        s.core.placement.active == Some(g) && (cert is SourceDone || cert is DestinationDone) ==>
            p::terminal(s.core.placement.phases[g]) && forall|k: int| s.core.placement.plans[g].keys.contains(k) ==>
                p::replica(s.core.placement,certificate_owner(s.core.placement,g,cert),k).fence == g
                && p::replica(s.core.placement,certificate_owner(s.core.placement,g,cert),k).terminal
{
    assert(p::plan_inv(c,s.core.placement,g));
    assert(durable_inv(s,Authority::Participant(certificate_owner(s.core.placement,g,cert))));
}
pub proof fn theorem_commit_construction(c: p::Constants, s: State, e: Envelope, k: int)
    requires inv(c,s),enabled(c,s,Action::Commit { envelope: e }),e.action is Commit,
        p::enabled(c,s.core.placement,e.action),
        s.core.placement.plans[s.core.placement.active.unwrap()].keys.contains(k)
    ensures {
        let g = s.core.placement.active.unwrap(); let plan = s.core.placement.plans[g];
        p::replica(s.core.placement,plan.src,k).role is Retired
            && p::replica(s.core.placement,plan.dst,k).role is Ready
            && p::drained(s.core.placement,plan.keys)
            && s.copied.contains(p::Packet { generation: g,round: 1,key: k,cell: s.core.placement.logical[k] })
            && recovered_participant(s,plan.src).certificates.contains((g,p::Certificate::Retired))
            && apply(c,s,Action::Commit { envelope: e }).core.placement.phases[g] is Committed
    },
{
    let old = s.core.placement; let g = old.active.unwrap();
    assert(p::plan_inv(c,old,g));
    assert(old.certificates.contains((g,p::Certificate::Retired)));
    assert(p::key_inv(c,old,k));
    assert(copy_backing(s,old.plans[g].dst,k));
    theorem_local_evidence(c,s,g,p::Certificate::Retired);
}
pub proof fn theorem_cleanup_authorization(c: p::Constants, s: State, g: nat, n: int, cmd: p::Command, k: int)
    requires inv(c,s),enabled(c,s,Action::StartCleanup { generation: g,owner: n,command: cmd }),
        s.core.placement.plans[g].keys.contains(k)
    ensures s.core.placement.active == Some(g),p::terminal(s.core.placement.phases[g]),
        p::directory(s.core.placement)[k].owner != n,
        forall|t: int| s.core.placement.sessions.contains_key(t) && s.core.placement.sessions[t].held.contains_key(k)
            ==> s.core.placement.sessions[t].held[k].owner != n
{
    let old = s.core.placement;
    assert(p::command_inv(old,g,cmd));
    assert(p::plan_inv(c,old,g));
    assert(p::key_inv(c,old,k));
    if old.active != Some(g) {
        assert(p::replica(old,n,k).fence >= g);
        assert(p::replica(old,n,k).fence == g ==> p::replica(old,n,k).terminal);
        assert(false);
    }
    assert forall|t: int| old.sessions.contains_key(t) && old.sessions[t].held.contains_key(k) implies
        old.sessions[t].held[k].owner != n by {
        assert(p::session_inv(c,old,t));
    }
}

pub proof fn theorem_stale_membership(c: p::Constants, s: State, e: Envelope)
    requires e.membership != member(s,actor(s.core.placement,e.action))
    ensures dispatch(c,s,Action::Accept { envelope: e }) == s,
        dispatch(c,s,Action::Commit { envelope: e }) == s
{}
pub proof fn theorem_stale_migration_endpoints(c: p::Constants, s: State, e: Envelope)
    requires e.action is Begin,e.target != target(s,e.action)
    ensures dispatch(c,s,Action::Accept { envelope: e }) == s,
        dispatch(c,s,Action::Commit { envelope: e }) == s
{}
pub proof fn theorem_admitted_read(c: p::Constants, s: State, t: int, k: int, ti: nat, pi: nat, membership: nat)
    requires inv(c,s),read(s,t,k,ti,pi,membership) is Some
    ensures read(s,t,k,ti,pi,membership) == Some(s.core.placement.logical[k]),
        active(s,Authority::Transaction(t)),
        active(s,Authority::Participant(s.core.placement.sessions[t].held[k].owner)),
        head(s,Authority::Transaction(t)).incarnation == ti,
        credential(s,Authority::Participant(s.core.placement.sessions[t].held[k].owner),pi,membership)
{
    p::lemma_read_matches_logical(c,s.core.placement,t,k);
}
pub proof fn theorem_stale_lease_read(s: State, t: int, k: int, ti: nat, pi: nat, membership: nat)
    requires p::live_lease(s.core.placement,t,k),
        pi != head(s,Authority::Participant(s.core.placement.sessions[t].held[k].owner)).incarnation
        || membership != member(s,Authority::Participant(s.core.placement.sessions[t].held[k].owner))
        || ti != head(s,Authority::Transaction(t)).incarnation
    ensures read(s,t,k,ti,pi,membership) is None
{}
pub proof fn theorem_cleanup_copy_fence(c: p::Constants, s: State, packet: p::Packet, e: Envelope)
    requires copy_fenced(s,packet),e.action == (p::Action::DeliverCopy { packet })
    ensures !enabled(c,s,Action::StartCopy { packet }),
        !enabled(c,s,Action::Accept { envelope: e }),!enabled(c,s,Action::Commit { envelope: e })
{}
pub proof fn theorem_cleanup_waits_for_engine(c: p::Constants, s: State, g: nat, n: int, cmd: p::Command, packet: p::Packet)
    requires enabled(c,s,Action::DurableCleanup { generation: g,owner: n,command: cmd }),
        s.copying.contains(packet),packet.generation == g,s.core.placement.plans[g].dst == n
    ensures s.copied.contains(packet)
{}
pub proof fn theorem_cleanup_completion_once(c: p::Constants, s: State, g: nat, n: int, cmd: p::Command)
    requires s.cleaned.contains((g,n,cmd))
    ensures dispatch(c,s,Action::DurableCleanup { generation: g,owner: n,command: cmd }) == s
{}
pub proof fn theorem_crash_keeps_durability(c: p::Constants, s: State, a: Authority)
    ensures apply(c,s,Action::Crash { authority: a }).journals == s.journals,
        apply(c,s,Action::Crash { authority: a }).lease_names == s.lease_names,
        apply(c,s,Action::Crash { authority: a }).engines == s.engines,
        apply(c,s,Action::Crash { authority: a }).settled == s.settled,
        apply(c,s,Action::Crash { authority: a }).copying == s.copying,
        apply(c,s,Action::Crash { authority: a }).copied == s.copied,
        apply(c,s,Action::Crash { authority: a }).cleaning == s.cleaning,
        apply(c,s,Action::Crash { authority: a }).cleaned == s.cleaned,
        !active(apply(c,s,Action::Crash { authority: a }),a)
{}
pub open spec fn prefix<T>(before: Seq<T>, after: Seq<T>) -> bool {
    before.len() <= after.len() && forall|i: int| 0 <= i < before.len() ==> before[i] == after[i]
}
pub proof fn theorem_committed_prefix_step(c: p::Constants, s: State, action: Action, a: Authority)
    ensures prefix(s.journals[a],apply(c,s,action).journals[a]),
        prefix(s.engines[a],apply(c,s,action).engines[a])
{
    match action {
        Action::Commit { .. } | Action::Remove { .. } | Action::Join { .. } | Action::Admit { .. } => {},
        Action::EngineSettle { .. } => { match a { Authority::Participant(_) => {}, _ => {} } },
        _ => {},
    }
}
pub proof fn theorem_committed_prefix_forever(c: p::Constants, states: Seq<State>, begin: nat, end: nat, a: Authority)
    requires behavior(c,states),begin <= end < states.len()
    ensures prefix(states[begin as int].journals[a],states[end as int].journals[a]),
        prefix(states[begin as int].engines[a],states[end as int].engines[a])
    decreases end-begin
{
    if begin < end {
        theorem_committed_prefix_forever(c,states,begin,(end-1) as nat,a);
        let before = states[end-1]; let after = states[end as int];
        let j: int = end as int - 1;
        assert(0 <= j < states.len()-1);
        assert(next(c,states[j],states[j+1]));
        let action = choose|action: Action| after == dispatch(c,before,action);
        if enabled(c,before,action) { theorem_committed_prefix_step(c,before,action,a); }
        assert forall|i: int| 0 <= i < states[begin as int].journals[a].len() implies
            states[begin as int].journals[a][i] == states[end as int].journals[a][i] by {
            assert(i < before.journals[a].len());
        }
        assert forall|i: int| 0 <= i < states[begin as int].engines[a].len() implies
            states[begin as int].engines[a][i] == states[end as int].engines[a][i] by {
            assert(i < before.engines[a].len());
        }
    }
}

pub proof fn theorem_engine_recovery_scope(c: p::Constants, s: State, crashed: Authority, recovered: Authority)
    ensures engine_image(apply(c,s,Action::Crash { authority: crashed }),recovered) == engine_image(s,recovered)
{}
pub proof fn theorem_copy_authority(c: p::Constants, s: State, packet: p::Packet, other: Authority)
    requires other != Authority::Participant(s.core.placement.plans[packet.generation].dst)
    ensures engine_image(apply(c,s,Action::StartCopy { packet }),other) == engine_image(s,other),
        engine_image(apply(c,s,Action::DurableCopy { packet }),other) == engine_image(s,other),
        apply(c,s,Action::StartCopy { packet }).journals == s.journals,
        apply(c,s,Action::DurableCopy { packet }).journals == s.journals
{
    match other {
        Authority::Participant(_) => {
            assert(engine_image(apply(c,s,Action::StartCopy { packet }),other).copying =~= engine_image(s,other).copying);
            assert(engine_image(apply(c,s,Action::DurableCopy { packet }),other).copied =~= engine_image(s,other).copied);
        },
        _ => {},
    }
}
pub proof fn theorem_cleanup_authority(c: p::Constants, s: State, g: nat, n: int, cmd: p::Command, other: Authority)
    requires other != Authority::Participant(n)
    ensures engine_image(apply(c,s,Action::StartCleanup { generation: g,owner: n,command: cmd }),other) == engine_image(s,other),
        engine_image(apply(c,s,Action::DurableCleanup { generation: g,owner: n,command: cmd }),other) == engine_image(s,other)
{
    match other {
        Authority::Participant(_) => {
            assert(engine_image(apply(c,s,Action::StartCleanup { generation: g,owner: n,command: cmd }),other).cleaning
                =~= engine_image(s,other).cleaning);
            assert(engine_image(apply(c,s,Action::DurableCleanup { generation: g,owner: n,command: cmd }),other).cleaned
                =~= engine_image(s,other).cleaned);
        },
        _ => {},
    }
}

pub proof fn theorem_discoverable_transaction_journals(c: p::Constants, s: State, action: Action, txn: int)
    requires inv(c,s)
    ensures s.lease_names.contains(txn) <==> s.journals[Authority::Transaction(txn)].len() > 1,
        s.core.placement.sessions.contains_key(txn) ==> s.lease_names.contains(txn),
        s.lease_names.subset_of(apply(c,s,action).lease_names)
{
    match action {
        Action::Commit { .. } | Action::Remove { .. } | Action::Join { .. } | Action::Admit { .. } => {},
        _ => {},
    }
}
} // verus!
