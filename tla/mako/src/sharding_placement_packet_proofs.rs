use super::*;
verus! {
proof fn frozen_rejects_acquire(c: Constants, s: State, g: nat, txn: int, key: int, grant: Grant)
    requires inv(c,s), current(s,g), before_decision(s,g), source_frozen(s,g),
        admission(s,txn,key,grant)
    ensures !s.plans[g].keys.contains(key)
{
    if s.plans[g].keys.contains(key) {
        assert(plan_inv(c,s,g));
        assert(c.keys.contains(key));
        assert(c.shards.contains(grant.owner));
        assert(key_inv(c,s,key));
        assert(replica(s,grant.owner,key).role is Serving);
        assert(grant.owner == directory(s)[key].owner);
        assert(directory(s)[key] == s.plans[g].old[key]);
        assert(grant.owner == s.plans[g].src);
        assert(replica(s,s.plans[g].src,key).role is Frozen
            || replica(s,s.plans[g].src,key).role is Retired);
    }
}

proof fn packet_acquire(c: Constants, s: State, txn: int, key: int, grant: Grant, packet: Packet)
    requires inv(c,s), enabled(c,s,Action::Acquire { txn,key,grant }), s.packets.contains(packet)
    ensures packet_inv(apply(c,s,Action::Acquire { txn,key,grant }),packet)
{
    let post = apply(c,s,Action::Acquire { txn,key,grant });
    let g = packet.generation;
    assert(packet_inv(s,packet));
    if current(s,g) && before_decision(s,g) && packet.round == 1 {
        frozen_rejects_acquire(c,s,g,txn,key,grant);
        assert forall|t: int, k: int| post.sessions.contains_key(t) && s.plans[g].keys.contains(k)
            implies !post.sessions[t].held.contains_key(k) by {
            assert(!s.sessions[t].held.contains_key(k));
        }
        assert(drained(post,s.plans[g].keys));
    }
}

proof fn packet_resolve(c: Constants, s: State, txn: int, writes: Map<int,Option<int>>, packet: Packet)
    requires inv(c,s), enabled(c,s,Action::Resolve { txn,writes }), s.packets.contains(packet)
    ensures packet_inv(apply(c,s,Action::Resolve { txn,writes }),packet)
{
    let post = apply(c,s,Action::Resolve { txn,writes });
    let g = packet.generation;
    assert(packet_inv(s,packet));
    if current(s,g) && before_decision(s,g) && packet.round == 1 {
        assert(plan_inv(c,s,g));
        assert forall|k: int| s.plans[g].keys.contains(k) implies !writes.contains_key(k) by {
            assert(!s.sessions[txn].held.contains_key(k));
        }
        assert forall|k: int| s.plans[g].keys.contains(k)
            implies replica(post,s.plans[g].src,k) == replica(s,s.plans[g].src,k) by {
            assert(c.keys.contains(k));
        }
        assert(source_frozen(post,g));
        assert(drained(post,s.plans[g].keys));
        assert(c.keys.contains(packet.key));
        assert(post.logical[packet.key] == s.logical[packet.key]);
    }
}

proof fn packet_begin(c: Constants, s: State, nonce: int, src: int, dst: int,
    table: int, lo: int, hi: Option<int>, packet: Packet)
    requires inv(c,s), enabled(c,s,Action::Begin { nonce,src,dst,table,lo,hi }), s.packets.contains(packet)
    ensures packet_inv(apply(c,s,Action::Begin { nonce,src,dst,table,lo,hi }),packet)
{
    assert(packet_inv(s,packet));
    assert(plan_inv(c,s,packet.generation));
    assert(packet.generation < s.next_generation);
}

proof fn packet_deliver(c: Constants, s: State, generation: nat, command: Command, owner: int, packet: Packet)
    requires inv(c,s), enabled(c,s,Action::Deliver { generation,command,owner }), s.packets.contains(packet)
    ensures packet_inv(apply(c,s,Action::Deliver { generation,command,owner }),packet)
{
    let a = Action::Deliver { generation,command,owner };
    let post = apply(c,s,a);
    let g = packet.generation;
    prepare_step(c,s,a);
    assert(packet_inv(s,packet));
    if current(s,g) && before_decision(s,g) && packet.round == 1 && current(s,generation) {
        assert(generation == g);
        assert(plan_inv(c,s,g));
        assert(command_inv(s,g,command));
        assert(!(command is Commit || command is Abort));
        assert forall|k: int| s.plans[g].keys.contains(k) implies {
            let r = replica(post,s.plans[g].src,k);
            (r.role is Frozen || r.role is Retired) && r.fence == g && !r.terminal
        } by {
            assert(c.keys.contains(k));
            assert(replica(s,s.plans[g].src,k).role is Frozen
                || replica(s,s.plans[g].src,k).role is Retired);
            if command is Freeze {
                assert(!local_guard(s,g,command,s.plans[g].src));
            }
            match command {
                Command::Start => {},
                Command::Freeze => {},
                Command::Final => {},
                Command::Retire => {},
                Command::Commit => {},
                Command::Abort => {},
            }
        }
        assert(source_frozen(post,g));
    }
}

proof fn packet_seal(c: Constants, s: State, generation: nat, packet: Packet)
    requires inv(c,s), enabled(c,s,Action::Seal { generation }), s.packets.contains(packet)
    ensures packet_inv(apply(c,s,Action::Seal { generation }),packet)
{
    let a = Action::Seal { generation };
    let post = apply(c,s,a);
    let g = packet.generation;
    prepare_step(c,s,a);
    assert(packet_inv(s,packet));
    if current(s,g) && before_decision(s,g) && packet.round == 1 {
        assert(generation == g);
        assert(plan_inv(c,s,g));
        assert forall|k: int| s.plans[g].keys.contains(k)
            implies replica(post,s.plans[g].src,k) == replica(s,s.plans[g].src,k) by {
            assert(c.keys.contains(k));
        }
        assert(source_frozen(post,g));
    }
}

proof fn packet_capture(c: Constants, s: State, generation: nat, round: nat, key: int, packet: Packet)
    requires inv(c,s), enabled(c,s,Action::Capture { generation,round,key }),
        apply(c,s,Action::Capture { generation,round,key }).packets.contains(packet)
    ensures packet_inv(apply(c,s,Action::Capture { generation,round,key }),packet)
{
    if s.packets.contains(packet) {
        assert(packet_inv(s,packet));
    } else {
        assert(packet == (Packet { generation,round,key,cell: replica(s,s.plans[generation].src,key).cell }));
        assert(plan_inv(c,s,generation));
        assert(c.keys.contains(key));
        assert(key_inv(c,s,key));
        assert(replica(s,s.plans[generation].src,key).role is Serving
            || replica(s,s.plans[generation].src,key).role is Frozen);
        assert(s.plans[generation].src == directory(s)[key].owner);
        assert(packet.cell == s.logical[key]);
    }
}

pub(super) proof fn packet_step(c: Constants, s: State, a: Action, packet: Packet)
    requires inv(c,s), enabled(c,s,a), apply(c,s,a).packets.contains(packet)
    ensures packet_inv(apply(c,s,a),packet)
{
    prepare_step(c,s,a);
    if s.packets.contains(packet) { assert(packet_inv(s,packet)); }
    match a {
        Action::Open { .. } => { assert(packet_inv(apply(c,s,a),packet)); },
        Action::Acquire { txn,key,grant } => { packet_acquire(c,s,txn,key,grant,packet); },
        Action::Resolve { txn,writes } => { packet_resolve(c,s,txn,writes,packet); },
        Action::Release { .. } => { assert(packet_inv(apply(c,s,a),packet)); },
        Action::Begin { nonce,src,dst,table,lo,hi } => { packet_begin(c,s,nonce,src,dst,table,lo,hi,packet); },
        Action::RequestFreeze => { assert(packet_inv(apply(c,s,a),packet)); },
        Action::RequestFinal => { assert(packet_inv(apply(c,s,a),packet)); },
        Action::RequestRetire => { assert(packet_inv(apply(c,s,a),packet)); },
        Action::Commit => { assert(packet_inv(apply(c,s,a),packet)); },
        Action::Abort => { assert(packet_inv(apply(c,s,a),packet)); },
        Action::Finish => { assert(packet_inv(apply(c,s,a),packet)); },
        Action::Deliver { generation,command,owner } => { packet_deliver(c,s,generation,command,owner,packet); },
        Action::Drain { .. } => { assert(packet_inv(apply(c,s,a),packet)); },
        Action::Seal { generation } => { packet_seal(c,s,generation,packet); },
        Action::Receive { .. } => { assert(packet_inv(apply(c,s,a),packet)); },
        Action::Capture { generation,round,key } => { packet_capture(c,s,generation,round,key,packet); },
        Action::DeliverCopy { .. } => { assert(packet_inv(apply(c,s,a),packet)); },
        Action::Cache { .. } => { assert(packet_inv(apply(c,s,a),packet)); },
        Action::Reply { .. } => { assert(packet_inv(apply(c,s,a),packet)); },
        Action::Stutter => {},
    }
}
}
