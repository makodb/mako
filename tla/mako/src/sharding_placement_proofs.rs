use vstd::prelude::*;
use super::*;
#[path = "sharding_placement_key_proofs.rs"]
mod keys;
#[path = "sharding_placement_packet_proofs.rs"]
mod packets;
use keys::key_step;
use packets::packet_step;

verus! {
pub open spec fn maintenance(a: Action) -> bool { !(a is Open || a is Resolve) }
pub open spec fn stable_replica(s: State, n: int, k: int) -> bool {
    let r = replica(s,n,k);
    r.terminal && if directory(s)[k].owner == n {
        r.role is Serving && r.epoch == directory(s)[k].epoch
    } else { r.role is Empty }
}
pub open spec fn source_frozen(s: State, g: nat) -> bool {
    forall|k: int| s.plans[g].keys.contains(k) ==> {
        let r = replica(s,s.plans[g].src,k);
        (r.role is Frozen || r.role is Retired) && r.fence == g && !r.terminal
    }
}
pub open spec fn key_inv(c: Constants, s: State, k: int) -> bool {
    let d = directory(s)[k];
    &&& c.shards.contains(d.owner) && d.epoch < s.next_generation
    &&& replica(s,d.owner,k).cell == s.logical[k]
    &&& replica(s,d.owner,k).epoch == d.epoch
    &&& !(replica(s,d.owner,k).role is Empty || replica(s,d.owner,k).role is Stage)
    &&& forall|n: int| c.shards.contains(n) ==> {
        let r = replica(s,n,k);
        &&& r.fence < s.next_generation && r.round <= 1
        &&& (r.role is Serving || r.role is Frozen ==> n == d.owner && r.epoch == d.epoch)
        &&& if s.active is Some && s.plans[s.active.unwrap()].keys.contains(k) {
            let g = s.active.unwrap(); let p = s.plans[g]; let phase = s.phases[g];
            if n != p.src && n != p.dst { stable_replica(s,n,k) }
            else if phase is Committed {
                &&& d == (Grant { owner: p.dst, epoch: g })
                &&& r.fence == g
                &&& if n == p.src {
                    r.role is Retired && !r.terminal || r.role is Empty && r.terminal
                } else {
                    r.epoch == g && (r.role is Ready && !r.terminal || r.role is Serving && r.terminal)
                }
            } else {
                &&& d == p.old[k]
                &&& if n == p.src {
                    &&& r.epoch == p.old[k].epoch
                    &&& (r.role is Serving && r.terminal && r.fence <= g
                        || (r.role is Frozen || r.role is Retired) && !r.terminal && r.fence == g)
                    &&& (before_decision(s,g) && r.role is Serving ==> r.fence < g)
                    &&& (phase is Copy ==> r.role is Serving)
                    &&& (phase is Freezing ==> !(r.role is Retired))
                    &&& (phase is Final ==> r.role is Frozen)
                    &&& (phase is Retiring ==> r.role is Frozen || r.role is Retired)
                } else {
                    &&& (r.role is Empty && r.terminal && r.fence <= g
                        || (r.role is Stage || r.role is Ready) && !r.terminal && r.fence == g && r.epoch == g)
                    &&& (before_decision(s,g) && r.role is Empty ==> r.fence < g)
                    &&& r.round <= 1
                    &&& (phase is Copy || phase is Freezing ==> !(r.role is Ready)
                        && (r.role is Stage ==> r.round == 0))
                    &&& (phase is Retiring ==> r.role is Ready)
                    &&& (before_decision(s,g) && (r.role is Ready || r.role is Stage && r.round == 1 && r.covered)
                        ==> r.cell == s.logical[k])
                }
            }
        } else { stable_replica(s,n,k) }
    }
}
pub open spec fn plan_inv(c: Constants, s: State, g: nat) -> bool {
    let p = s.plans[g]; let phase = s.phases[g];
    &&& 0 < g < s.next_generation
    &&& p.src != p.dst && c.shards.contains(p.src) && c.shards.contains(p.dst)
    &&& partition::valid_range(p.lo,p.hi)
    &&& p.keys == selected(c,p.table,p.lo,p.hi) && p.keys != Set::<int>::empty()
    &&& p.old.dom() == c.keys
    &&& forall|k: int| p.keys.contains(k) ==> p.old[k].owner == p.src && p.old[k].epoch < g
    &&& (!current(s,g) ==> terminal(phase))
    &&& (terminal(phase) <==> s.outcomes.contains_key(p.nonce))
    &&& (terminal(phase) ==> s.outcomes[p.nonce] == (Outcome { generation: g, committed: phase is Committed }))
    &&& forall|k: int| p.keys.contains(k) && !current(s,g) ==> {
        let a = replica(s,p.src,k); let b = replica(s,p.dst,k);
        a.fence >= g && b.fence >= g
            && (a.fence == g ==> a.terminal) && (b.fence == g ==> b.terminal)
    }
    &&& (current(s,g) && s.certificates.contains((g,Certificate::SourceDone)) ==>
        terminal(phase) && forall|k: int| p.keys.contains(k) ==>
            replica(s,p.src,k).fence == g && replica(s,p.src,k).terminal)
    &&& (current(s,g) && s.certificates.contains((g,Certificate::DestinationDone)) ==>
        terminal(phase) && forall|k: int| p.keys.contains(k) ==>
            replica(s,p.dst,k).fence == g && replica(s,p.dst,k).terminal)
    &&& (current(s,g) && before_decision(s,g) ==> {
        &&& (s.certificates.contains((g,Certificate::Drained))
            || s.certificates.contains((g,Certificate::Ready))
            || s.certificates.contains((g,Certificate::Retired))
            || phase is Final || phase is Retiring
            ==> source_frozen(s,g) && drained(s,p.keys))
        &&& (s.certificates.contains((g,Certificate::Ready)) || phase is Retiring ==>
            forall|k: int| p.keys.contains(k) ==> replica(s,p.dst,k).role is Ready)
        &&& (s.certificates.contains((g,Certificate::Retired)) ==>
            forall|k: int| p.keys.contains(k) ==> replica(s,p.src,k).role is Retired)
    })
}
pub open spec fn command_inv(s: State, g: nat, cmd: Command) -> bool {
    s.plans.contains_key(g) && match cmd {
        Command::Start => true,
        Command::Freeze => !(s.phases[g] is Copy),
        Command::Final => s.phases[g] is Final || s.phases[g] is Retiring || terminal(s.phases[g]),
        Command::Retire => s.phases[g] is Retiring || terminal(s.phases[g]),
        Command::Commit => s.phases[g] is Committed,
        Command::Abort => s.phases[g] is Aborted,
    }
}
pub open spec fn packet_inv(s: State, p: Packet) -> bool {
    s.plans.contains_key(p.generation) && s.plans[p.generation].keys.contains(p.key) && p.round <= 1
        && (current(s,p.generation) && before_decision(s,p.generation) && p.round == 1 ==>
            p.cell == s.logical[p.key] && source_frozen(s,p.generation)
                && drained(s,s.plans[p.generation].keys))
}
pub open spec fn session_inv(c: Constants, s: State, t: int) -> bool {
    t >= 0 && s.sessions[t].held.dom().subset_of(c.keys)
        && forall|k: int| s.sessions[t].held.contains_key(k) ==> {
            let grant = s.sessions[t].held[k];
            grant == directory(s)[k]
                && (replica(s,grant.owner,k).role is Serving || replica(s,grant.owner,k).role is Frozen)
        }
}
pub open spec fn inv(c: Constants, s: State) -> bool {
    &&& constants_ok(c)
    &&& s.next_generation > 0
    &&& s.logical.dom() == c.keys
    &&& s.physical.dom() == ISet::new(|q: (int,int)| c.shards.contains(q.0) && c.keys.contains(q.1))
    &&& s.directory.len() > 0
    &&& forall|i: int| 0 <= i < s.directory.len() ==> s.directory[i].dom() == c.keys
        && forall|k: int| c.keys.contains(k) ==> c.shards.contains(s.directory[i][k].owner)
            && s.directory[i][k].epoch < s.next_generation
    &&& forall|k: int| c.keys.contains(k) ==> key_inv(c,s,k)
    &&& forall|t: int| s.sessions.contains_key(t) ==> session_inv(c,s,t)
    &&& s.plans.dom() == s.phases.dom()
    &&& (s.active is Some ==> s.plans.contains_key(s.active.unwrap()))
    &&& forall|g: nat| s.plans.contains_key(g) ==> plan_inv(c,s,g)
    &&& forall|g: nat, h: nat| s.plans.contains_key(g) && s.plans.contains_key(h)
        && s.plans[g].nonce == s.plans[h].nonce ==> g == h
    &&& forall|g: nat, cmd: Command| s.commands.contains((g,cmd)) ==> command_inv(s,g,cmd)
    &&& forall|g: nat, cert: Certificate| s.certificates.contains((g,cert)) ==> s.plans.contains_key(g)
    &&& s.received.subset_of(s.certificates)
    &&& forall|p: Packet| s.packets.contains(p) ==> packet_inv(s,p)
    &&& forall|nonce: int| s.outcomes.contains_key(nonce) ==> s.plans.contains_key(s.outcomes[nonce].generation)
        && s.plans[s.outcomes[nonce].generation].nonce == nonce
    &&& forall|nonce: int| #[trigger] s.replies.contains_key(nonce) ==> s.outcomes.contains_key(nonce) && s.replies[nonce] == s.outcomes[nonce]
    &&& forall|client: int| s.views.contains_key(client) ==> s.views[client] < s.directory.len()
}
pub proof fn lemma_read_matches_logical(c: Constants, s: State, txn: int, key: int)
    requires inv(c,s), read(s,txn,key) is Some
    ensures s.logical.contains_key(key), read(s,txn,key) == Some(s.logical[key])
{
    assert(session_inv(c,s,txn));
    assert(s.sessions[txn].held.contains_key(key));
    assert(s.sessions[txn].held[key] == directory(s)[key]);
    assert(key_inv(c,s,key));
}
pub proof fn lemma_open_frame(c: Constants, s: State, txn: int)
    requires inv(c,s), enabled(c,s,Action::Open { txn })
    ensures apply(c,s,Action::Open { txn }).logical == s.logical,
        apply(c,s,Action::Open { txn }).sessions == s.sessions.insert(txn,Session { held: Map::empty(), resolved: false })
{}
pub proof fn lemma_resolve_frame(c: Constants, s: State, txn: int, writes: Map<int,Option<int>>)
    requires inv(c,s), enabled(c,s,Action::Resolve { txn,writes })
    ensures apply(c,s,Action::Resolve { txn,writes }).logical == write_logical(s.logical,txn,writes),
        apply(c,s,Action::Resolve { txn,writes }).sessions == s.sessions.insert(txn,Session { resolved: true, ..s.sessions[txn] })
{}
pub proof fn lemma_maintenance_frame(c: Constants, s: State, a: Action)
    requires inv(c,s), enabled(c,s,a), maintenance(a)
    ensures apply(c,s,a).logical == s.logical,
        apply(c,s,a).sessions.dom() == s.sessions.dom(),
        forall|t: int| s.sessions.contains_key(t) ==> apply(c,s,a).sessions[t].resolved == s.sessions[t].resolved
{ match a { Action::Acquire { .. } => {}, Action::Release { .. } => {}, _ => {} } }
pub proof fn lemma_init(c: Constants)
    requires constants_ok(c)
    ensures inv(c,initial(c))
{}
proof fn old_command_rejected(c: Constants, s: State, g: nat, cmd: Command, owner: int)
    requires inv(c,s), s.plans.contains_key(g), !current(s,g),
        owner == s.plans[g].src || owner == s.plans[g].dst
    ensures !local_guard(s,g,cmd,owner)
{
    let p = s.plans[g];
    assert(plan_inv(c,s,g));
    assert(exists|k: int| p.keys.contains(k)) by {
        if !(exists|k: int| p.keys.contains(k)) { assert(p.keys =~= Set::<int>::empty()); }
    }
    let k = choose|k: int| p.keys.contains(k);
    assert(replica(s,owner,k).fence >= g);
    assert(replica(s,owner,k).fence == g ==> replica(s,owner,k).terminal);
}
proof fn old_delivery_stutters(c: Constants, s: State, g: nat, cmd: Command, owner: int)
    requires inv(c,s), s.plans.contains_key(g), !current(s,g)
    ensures delivered(s,g,cmd,owner) == s
{
    old_command_rejected(c,s,g,cmd,s.plans[g].src);
    old_command_rejected(c,s,g,cmd,s.plans[g].dst);
    assert(delivered(s,g,cmd,owner).physical =~= s.physical);
}
proof fn active_local(c: Constants, s: State, g: nat, owner: int)
    requires inv(c,s), s.plans.contains_key(g),
        owner == s.plans[g].src || owner == s.plans[g].dst,
        forall|k: int| s.plans[g].keys.contains(k) ==>
            replica(s,owner,k).fence == g && !replica(s,owner,k).terminal
    ensures current(s,g)
{
    let p = s.plans[g];
    assert(plan_inv(c,s,g));
    assert(exists|k: int| p.keys.contains(k)) by {
        if !(exists|k: int| p.keys.contains(k)) { assert(p.keys =~= Set::<int>::empty()); }
    }
    let k = choose|k: int| p.keys.contains(k);
}
proof fn copy_active(c: Constants, s: State, packet: Packet)
    requires inv(c,s), s.packets.contains(packet), copy_guard(s,packet)
    ensures current(s,packet.generation)
{
    assert(packet_inv(s,packet));
    assert(plan_inv(c,s,packet.generation));
}
proof fn prepare_step(c: Constants, s: State, a: Action)
    requires inv(c,s), enabled(c,s,a)
    ensures
        a is Deliver && !current(s,a->Deliver_generation) ==> apply(c,s,a) == s,
        a is Drain ==> current(s,a->Drain_generation),
        a is Seal ==> current(s,a->Seal_generation),
        a is DeliverCopy && copy_guard(s,a->DeliverCopy_packet) ==> current(s,a->DeliverCopy_packet.generation)
{
    match a {
        Action::Deliver { generation,command,owner } => {
            assert(command_inv(s,generation,command));
            if !current(s,generation) { old_delivery_stutters(c,s,generation,command,owner); }
        },
        Action::Drain { generation } => { active_local(c,s,generation,s.plans[generation].src); },
        Action::Seal { generation } => { active_local(c,s,generation,s.plans[generation].dst); },
        Action::DeliverCopy { packet } => { if copy_guard(s,packet) { copy_active(c,s,packet); } },
        _ => {},
    }
}
proof fn frozen_rejects_admission(c: Constants, s: State, g: nat, txn: int, key: int, grant: Grant)
    requires inv(c,s), current(s,g), before_decision(s,g), source_frozen(s,g),
        s.plans[g].keys.contains(key)
    ensures !admission(s,txn,key,grant)
{
    assert(plan_inv(c,s,g));
    assert(c.keys.contains(key));
    assert(key_inv(c,s,key));
}
proof fn fence_step(c: Constants, s: State, a: Action, n: int, k: int)
    requires inv(c,s), enabled(c,s,a), c.shards.contains(n), c.keys.contains(k)
    ensures replica(apply(c,s,a),n,k).fence >= replica(s,n,k).fence,
        replica(apply(c,s,a),n,k).fence == replica(s,n,k).fence && replica(s,n,k).terminal
            ==> replica(apply(c,s,a),n,k).terminal
{
    match a {
        Action::Deliver { generation,command,owner } => {
            if s.plans[generation].keys.contains(k) {
                if n == s.plans[generation].src { assert(local_guard(s,generation,command,n) ==> match command {
                    Command::Freeze => replica(s,n,k).fence < generation,
                    Command::Abort => replica(s,n,k).fence <= generation,
                    _ => true,
                }); }
                if n == s.plans[generation].dst { assert(local_guard(s,generation,command,n) ==> match command {
                    Command::Start => replica(s,n,k).fence < generation,
                    Command::Abort => replica(s,n,k).fence <= generation,
                    _ => true,
                }); }
            }
        },
        _ => {},
    }
}
proof fn plan_step(c: Constants, s: State, a: Action, g: nat)
    requires inv(c,s), enabled(c,s,a), apply(c,s,a).plans.contains_key(g)
    ensures plan_inv(c,apply(c,s,a),g)
{
    prepare_step(c,s,a);
    if s.plans.contains_key(g) { assert(plan_inv(c,s,g)); }
    match a {
        Action::Open { .. } => { assert(plan_inv(c,apply(c,s,a),g)); },
        Action::Acquire { txn,key,grant } => {
            if current(s,g) && before_decision(s,g) && source_frozen(s,g) && s.plans[g].keys.contains(key) {
                frozen_rejects_admission(c,s,g,txn,key,grant);
            }
            assert(plan_inv(c,apply(c,s,a),g));
        },
        Action::Resolve { .. } => { assert(plan_inv(c,apply(c,s,a),g)); },
        Action::Release { .. } => { assert(plan_inv(c,apply(c,s,a),g)); },
        Action::Begin { .. } => { assert(plan_inv(c,apply(c,s,a),g)); },
        Action::RequestFreeze => { assert(plan_inv(c,apply(c,s,a),g)); },
        Action::RequestFinal => { assert(plan_inv(c,apply(c,s,a),g)); },
        Action::RequestRetire => { assert(plan_inv(c,apply(c,s,a),g)); },
        Action::Commit => { assert(plan_inv(c,apply(c,s,a),g)); },
        Action::Abort => { assert(plan_inv(c,apply(c,s,a),g)); },
        Action::Finish => { assert(plan_inv(c,apply(c,s,a),g)); },
        Action::Deliver { generation,command,owner } => {
            assert(command_inv(s,generation,command));
            assert forall|k: int| s.plans[g].keys.contains(k) && !current(s,g) implies {
                let z = apply(c,s,a); let p = s.plans[g];
                replica(z,p.src,k).fence >= g && replica(z,p.dst,k).fence >= g
                    && (replica(z,p.src,k).fence == g ==> replica(z,p.src,k).terminal)
                    && (replica(z,p.dst,k).fence == g ==> replica(z,p.dst,k).terminal)
            } by {
                fence_step(c,s,a,s.plans[g].src,k);
                fence_step(c,s,a,s.plans[g].dst,k);
            }
            match command {
                Command::Start => { assert(plan_inv(c,apply(c,s,a),g)); },
                Command::Freeze => { assert(plan_inv(c,apply(c,s,a),g)); },
                Command::Final => { assert(plan_inv(c,apply(c,s,a),g)); },
                Command::Retire => { assert(plan_inv(c,apply(c,s,a),g)); },
                Command::Commit => { assert(plan_inv(c,apply(c,s,a),g)); },
                Command::Abort => { assert(plan_inv(c,apply(c,s,a),g)); },
            }
        },
        Action::Drain { .. } => { assert(plan_inv(c,apply(c,s,a),g)); },
        Action::Seal { generation } => {
            if g == generation && before_decision(s,g) {
                let k = choose|k: int| s.plans[g].keys.contains(k);
                assert(exists|k: int| s.plans[g].keys.contains(k)) by {
                    if !(exists|k: int| s.plans[g].keys.contains(k)) { assert(s.plans[g].keys =~= Set::<int>::empty()); }
                }
                assert(key_inv(c,s,k));
                assert(s.phases[g] is Final || s.phases[g] is Retiring);
            }
            assert(plan_inv(c,apply(c,s,a),g));
        },
        Action::Receive { .. } => { assert(plan_inv(c,apply(c,s,a),g)); },
        Action::Capture { .. } => { assert(plan_inv(c,apply(c,s,a),g)); },
        Action::DeliverCopy { .. } => { assert(plan_inv(c,apply(c,s,a),g)); },
        Action::Cache { .. } => { assert(plan_inv(c,apply(c,s,a),g)); },
        Action::Reply { .. } => { assert(plan_inv(c,apply(c,s,a),g)); },
        Action::Stutter => {},
    }
}
proof fn session_step(c: Constants, s: State, a: Action, t: int)
    requires inv(c,s), enabled(c,s,a), apply(c,s,a).sessions.contains_key(t)
    ensures session_inv(c,apply(c,s,a),t)
{
    prepare_step(c,s,a);
    if s.sessions.contains_key(t) { assert(session_inv(c,s,t)); }
    if s.active is Some { assert(plan_inv(c,s,s.active.unwrap())); }
    match a {
        Action::Open { .. } => { assert(session_inv(c,apply(c,s,a),t)); },
        Action::Acquire { txn,key,grant } => {
            assert(c.keys.contains(key));
            assert(key_inv(c,s,key));
            assert(session_inv(c,apply(c,s,a),t));
        },
        Action::Resolve { .. } => { assert(session_inv(c,apply(c,s,a),t)); },
        Action::Release { .. } => { assert(session_inv(c,apply(c,s,a),t)); },
        Action::Begin { .. } => { assert(session_inv(c,apply(c,s,a),t)); },
        Action::RequestFreeze => { assert(session_inv(c,apply(c,s,a),t)); },
        Action::RequestFinal => { assert(session_inv(c,apply(c,s,a),t)); },
        Action::RequestRetire => { assert(session_inv(c,apply(c,s,a),t)); },
        Action::Commit => {
            assert(s.certificates.contains((s.active.unwrap(),Certificate::Retired)));
            assert(session_inv(c,apply(c,s,a),t));
        },
        Action::Abort => { assert(session_inv(c,apply(c,s,a),t)); },
        Action::Finish => { assert(session_inv(c,apply(c,s,a),t)); },
        Action::Deliver { generation,command,owner } => {
            assert(command_inv(s,generation,command));
            let z = apply(c,s,a);
            assert forall|k: int| s.sessions[t].held.contains_key(k) implies {
                let grant = s.sessions[t].held[k];
                grant == directory(z)[k]
                    && (replica(z,grant.owner,k).role is Serving || replica(z,grant.owner,k).role is Frozen)
            } by {
                assert(key_inv(c,s,k));
                if s.plans[generation].keys.contains(k) && command is Retire
                    && local_guard(s,generation,command,s.plans[generation].src) {
                    assert(drained(s,s.plans[generation].keys));
                }
            }
            assert(session_inv(c,apply(c,s,a),t));
        },
        Action::Drain { .. } => { assert(session_inv(c,apply(c,s,a),t)); },
        Action::Seal { .. } => { assert(session_inv(c,apply(c,s,a),t)); },
        Action::Receive { .. } => { assert(session_inv(c,apply(c,s,a),t)); },
        Action::Capture { .. } => { assert(session_inv(c,apply(c,s,a),t)); },
        Action::DeliverCopy { .. } => { assert(session_inv(c,apply(c,s,a),t)); },
        Action::Cache { .. } => { assert(session_inv(c,apply(c,s,a),t)); },
        Action::Reply { .. } => { assert(session_inv(c,apply(c,s,a),t)); },
        Action::Stutter => {},
    }
}
proof fn command_step(c: Constants, s: State, a: Action, g: nat, cmd: Command)
    requires inv(c,s), enabled(c,s,a), apply(c,s,a).commands.contains((g,cmd))
    ensures command_inv(apply(c,s,a),g,cmd)
{
    if s.commands.contains((g,cmd)) { assert(command_inv(s,g,cmd)); }
    if s.plans.contains_key(g) { assert(plan_inv(c,s,g)); }
    match a {
        Action::Begin { .. } => {},
        Action::RequestFreeze => {}, Action::RequestFinal => {}, Action::RequestRetire => {},
        Action::Commit => {}, Action::Abort => {},
        _ => {},
    }
}
pub proof fn lemma_outcome_once(c: Constants, s: State, a: Action, nonce: int)
    requires inv(c,s), enabled(c,s,a), s.outcomes.contains_key(nonce)
    ensures apply(c,s,a).outcomes.contains_key(nonce),
        apply(c,s,a).outcomes[nonce] == s.outcomes[nonce]
{
    if s.active is Some { assert(plan_inv(c,s,s.active.unwrap())); }
    match a { Action::Commit => {}, Action::Abort => {}, _ => {} }
}
proof fn reply_step(c: Constants, s: State, a: Action, nonce: int)
    requires inv(c,s), enabled(c,s,a), apply(c,s,a).replies.contains_key(nonce)
    ensures apply(c,s,a).outcomes.contains_key(nonce),
        apply(c,s,a).replies[nonce] == apply(c,s,a).outcomes[nonce]
{
    if s.replies.contains_key(nonce) {
        assert(s.replies[nonce] == s.outcomes[nonce]);
        assert(s.outcomes.contains_key(nonce));
        lemma_outcome_once(c,s,a,nonce);
    }
    match a { Action::Reply { .. } => {}, _ => {} }
}
proof fn shape_step(c: Constants, s: State, a: Action)
    requires inv(c,s), enabled(c,s,a)
    ensures apply(c,s,a).logical.dom() == s.logical.dom(),
        apply(c,s,a).physical.dom() == s.physical.dom(),
        apply(c,s,a).plans.dom() == apply(c,s,a).phases.dom()
{
    let z = apply(c,s,a);
    if let Action::DeliverCopy { packet } = a {
        assert(packet_inv(s,packet));
        assert(plan_inv(c,s,packet.generation));
        assert(s.physical.contains_key((s.plans[packet.generation].dst,packet.key)));
    }
    match a {
        Action::Begin { .. } => {}, Action::RequestFreeze => {}, Action::RequestFinal => {},
        Action::RequestRetire => {}, Action::Commit => {}, Action::Abort => {},
        Action::Resolve { .. } => {}, Action::Deliver { .. } => {}, Action::Seal { .. } => {},
        Action::DeliverCopy { .. } => {}, _ => {},
    }
    assert(z.physical.dom() =~= s.physical.dom());
    assert(z.logical.dom() =~= s.logical.dom());
    assert(z.plans.dom() =~= z.phases.dom());
}
pub proof fn lemma_step(c: Constants, s: State, a: Action)
    requires inv(c,s), enabled(c,s,a)
    ensures inv(c,apply(c,s,a))
{
    hide(key_inv);
    hide(plan_inv);
    hide(packet_inv);
    hide(session_inv);
    hide(command_inv);
    shape_step(c,s,a);
    let z = apply(c,s,a);
    assert forall|k: int| c.keys.contains(k) implies key_inv(c,z,k) by { key_step(c,s,a,k); }
    assert forall|g: nat| z.plans.contains_key(g) implies plan_inv(c,z,g) by { plan_step(c,s,a,g); }
    assert forall|p: Packet| z.packets.contains(p) implies packet_inv(z,p) by { packet_step(c,s,a,p); }
    assert forall|t: int| z.sessions.contains_key(t) implies session_inv(c,z,t) by { session_step(c,s,a,t); }
    assert forall|g: nat, cmd: Command| z.commands.contains((g,cmd)) implies command_inv(z,g,cmd)
        by { command_step(c,s,a,g,cmd); }
    assert forall|nonce: int| #[trigger] z.replies.contains_key(nonce) implies
        z.outcomes.contains_key(nonce) && z.replies[nonce] == z.outcomes[nonce]
        by { reply_step(c,s,a,nonce); }
    match a {
        Action::Open { .. } => { assert(inv(c,z)); },
        Action::Acquire { .. } => { assert(inv(c,z)); },
        Action::Resolve { .. } => { assert(inv(c,z)); },
        Action::Release { .. } => { assert(inv(c,z)); },
        Action::Begin { nonce,.. } => {
            reveal(plan_inv);
            assert forall|g: nat| s.plans.contains_key(g) implies
                g < s.next_generation && s.plans[g].nonce != nonce by {
                assert(plan_inv(c,s,g));
            }
            assert(inv(c,z));
        },
        Action::RequestFreeze => { assert(inv(c,z)); },
        Action::RequestFinal => { assert(inv(c,z)); },
        Action::RequestRetire => { assert(inv(c,z)); },
        Action::Commit => {
            reveal(plan_inv);
            assert(plan_inv(c,s,s.active.unwrap()));
            assert(inv(c,z));
        },
        Action::Abort => { assert(inv(c,z)); },
        Action::Finish => { assert(inv(c,z)); },
        Action::Deliver { generation,command,owner } => {
            reveal(command_inv);
            assert(command_inv(s,generation,command));
            assert(inv(c,z));
        },
        Action::Drain { .. } => { assert(inv(c,z)); },
        Action::Seal { .. } => { assert(inv(c,z)); },
        Action::Receive { .. } => { assert(inv(c,z)); },
        Action::Capture { .. } => { assert(inv(c,z)); },
        Action::DeliverCopy { .. } => { assert(inv(c,z)); },
        Action::Cache { .. } => { assert(inv(c,z)); },
        Action::Reply { .. } => { assert(inv(c,z)); },
        Action::Stutter => {},
    }
}

pub proof fn lemma_dispatch(c: Constants, s: State, a: Action)
    requires inv(c,s)
    ensures inv(c,dispatch(c,s,a)), !enabled(c,s,a) ==> dispatch(c,s,a) == s
{
    if enabled(c,s,a) { lemma_step(c,s,a); }
}
pub proof fn theorem_invariant(c: Constants, states: Seq<State>, i: int)
    requires constants_ok(c), behavior(c,states), 0 <= i < states.len()
    ensures inv(c,states[i])
    decreases i
{
    if i == 0 { lemma_init(c); } else {
        theorem_invariant(c,states,i-1);
        let j = i - 1;
        assert(next(c,states[j],states[j+1]));
        let a = choose|a: Action| states[i] == #[trigger] dispatch(c,states[i-1],a);
        lemma_dispatch(c,states[i-1],a);
    }
}
pub proof fn lemma_no_dual_serving(c: Constants, s: State, k: int, n: int, m: int)
    requires inv(c,s), c.keys.contains(k), c.shards.contains(n), c.shards.contains(m),
        replica(s,n,k).role is Serving, replica(s,m,k).role is Serving
    ensures n == m, replica(s,n,k).cell == s.logical[k]
{
    assert(key_inv(c,s,k));
}
pub proof fn lemma_seal_exact(c: Constants, s: State, g: nat)
    requires inv(c,s), enabled(c,s,Action::Seal { generation: g }), before_decision(s,g)
    ensures forall|k: int| s.plans[g].keys.contains(k) ==>
        replica(s,s.plans[g].dst,k).cell == s.logical[k]
        && replica(s,s.plans[g].src,k).cell == s.logical[k]
{
    prepare_step(c,s,Action::Seal { generation: g });
    assert(plan_inv(c,s,g));
    assert forall|k: int| s.plans[g].keys.contains(k) implies
        replica(s,s.plans[g].dst,k).cell == s.logical[k]
        && replica(s,s.plans[g].src,k).cell == s.logical[k] by {
        assert(key_inv(c,s,k));
    }
}
pub proof fn lemma_held_grant_stable(c: Constants, s: State, a: Action, txn: int, k: int)
    requires inv(c,s), enabled(c,s,a), s.sessions.contains_key(txn),
        s.sessions[txn].held.contains_key(k),
        apply(c,s,a).sessions.contains_key(txn), apply(c,s,a).sessions[txn].held.contains_key(k)
    ensures directory(apply(c,s,a))[k] == directory(s)[k]
{
    lemma_step(c,s,a);
    assert(session_inv(c,s,txn));
    assert(session_inv(c,apply(c,s,a),txn));
    match a { Action::Open { .. } => {}, Action::Acquire { .. } => {}, Action::Release { .. } => {}, _ => {} }
}
pub proof fn lemma_frozen_store_stable(c: Constants, s: State, a: Action, g: nat, k: int)
    requires inv(c,s), enabled(c,s,a), current(s,g), before_decision(s,g),
        source_frozen(s,g), drained(s,s.plans[g].keys), s.plans[g].keys.contains(k),
        current(apply(c,s,a),g), before_decision(apply(c,s,a),g)
    ensures apply(c,s,a).logical[k] == s.logical[k],
        replica(apply(c,s,a),s.plans[g].src,k).cell == replica(s,s.plans[g].src,k).cell
{
    let z = apply(c,s,a);
    assert(plan_inv(c,s,g));
    lemma_step(c,s,a);
    assert(key_inv(c,s,k));
    assert(key_inv(c,z,k));
    match a {
        Action::Resolve { txn,writes } => {
            assert(session_inv(c,s,txn));
            assert(!writes.contains_key(k));
        },
        _ => {},
    }
}
pub proof fn lemma_copy_rejected(s: State, packet: Packet, c: Constants)
    requires !copy_guard(s,packet)
    ensures apply(c,s,Action::DeliverCopy { packet }) == s
{}
pub proof fn lemma_snapshots_immutable(c: Constants, s: State, a: Action)
    requires inv(c,s), enabled(c,s,a)
    ensures apply(c,s,a).directory.len() >= s.directory.len(),
        forall|i: int| 0 <= i < s.directory.len() ==> apply(c,s,a).directory[i] == s.directory[i]
{
    match a { Action::Commit => {}, _ => {} }
}
pub proof fn lemma_terminal_replay(c: Constants, s: State, nonce: int, src: int, dst: int, table: int, lo: int, hi: Option<int>)
    requires inv(c,s), s.outcomes.contains_key(nonce)
    ensures dispatch(c,s,Action::Begin { nonce,src,dst,table,lo,hi }) == s,
        enabled(c,s,Action::Reply { nonce }),
        apply(c,s,Action::Reply { nonce }).replies[nonce] == s.outcomes[nonce]
{}
pub proof fn lemma_resolved_replay(c: Constants, s: State, txn: int, key: int, writes: Map<int,Option<int>>)
    requires inv(c,s), s.sessions.contains_key(txn), s.sessions[txn].resolved
    ensures read(s,txn,key) is None, !can_resolve(s,txn,writes),
        dispatch(c,s,Action::Open { txn }) == s,
        dispatch(c,s,Action::Resolve { txn,writes }) == s
{}

/// Segment.shard is an immutable GRANT LABEL here, not a physical shard ID.
/// Equal labels decode to equal owner+incarnation, so coalescing cannot erase
/// an incarnation boundary. The finite-key projection connects the actual
/// insert/remap/coalesce algorithm to atomic pointwise publication.
pub open spec fn partition_represents(c: Constants, s: State, table: int, p: Seq<partition::Segment>, decode: spec_fn(int) -> Grant) -> bool {
    partition::wellformed(p) && forall|k: int| c.keys.contains(k) && c.table[k] == table ==>
        directory(s)[k] == decode(partition::route(p,c.coordinate[k]))
}
pub proof fn theorem_commit_partition(c: Constants, s: State, p: Seq<partition::Segment>, decode: spec_fn(int) -> Grant, label: int)
    requires inv(c,s), enabled(c,s,Action::Commit),
        partition_represents(c,s,s.plans[s.active.unwrap()].table,p,decode), label >= 0,
        decode(label) == (Grant { owner: s.plans[s.active.unwrap()].dst, epoch: s.active.unwrap() })
    ensures partition_represents(c,apply(c,s,Action::Commit),s.plans[s.active.unwrap()].table,
        partition::reassign(p,s.plans[s.active.unwrap()].lo,s.plans[s.active.unwrap()].hi,label),decode)
{
    let g = s.active.unwrap(); let plan = s.plans[g];
    assert(plan_inv(c,s,g));
    partition::reassign_partition_theorem(p,plan.lo,plan.hi,label,0);
    assert forall|k: int| c.keys.contains(k) && c.table[k] == plan.table implies
        directory(apply(c,s,Action::Commit))[k] ==
            decode(partition::route(partition::reassign(p,plan.lo,plan.hi,label),c.coordinate[k])) by {
        partition::reassign_partition_theorem(p,plan.lo,plan.hi,label,c.coordinate[k]);
    }
}

pub proof fn lemma_obsolete_control_rejected(c: Constants, s: State, g: nat, cmd: Command, owner: int)
    requires inv(c,s), s.commands.contains((g,cmd)), !current(s,g)
    ensures dispatch(c,s,Action::Deliver { generation: g, command: cmd, owner }) == s
{
    assert(command_inv(s,g,cmd));
    old_delivery_stutters(c,s,g,cmd,owner);
}
pub proof fn lemma_obsolete_copy_rejected(c: Constants, s: State, packet: Packet)
    requires inv(c,s), s.packets.contains(packet), !current(s,packet.generation)
    ensures dispatch(c,s,Action::DeliverCopy { packet }) == s
{
    if copy_guard(s,packet) { copy_active(c,s,packet); }
}
pub proof fn lemma_freeze_blocks_admission(c: Constants, s: State, g: nat, txn: int, key: int, grant: Grant)
    requires inv(c,s), current(s,g), before_decision(s,g), source_frozen(s,g),
        s.plans[g].keys.contains(key)
    ensures dispatch(c,s,Action::Acquire { txn,key,grant }) == s
{
    frozen_rejects_admission(c,s,g,txn,key,grant);
}
pub proof fn lemma_exact_drain_observes_every_session(c: Constants, s: State, g: nat, txn: int, key: int)
    requires inv(c,s), s.plans.contains_key(g), s.plans[g].keys.contains(key),
        s.sessions.contains_key(txn), s.sessions[txn].held.contains_key(key)
    ensures !drained(s,s.plans[g].keys), !enabled(c,s,Action::Drain { generation: g })
{}

} // verus!
