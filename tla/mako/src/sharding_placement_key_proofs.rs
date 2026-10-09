use super::*;
verus! {
proof fn key_resolve(c: Constants, s: State, txn: int, writes: Map<int,Option<int>>, k: int)
    requires inv(c,s), enabled(c,s,Action::Resolve { txn,writes }), c.keys.contains(k)
    ensures key_inv(c,apply(c,s,Action::Resolve { txn,writes }),k)
{
    assert(key_inv(c,s,k));
    assert(session_inv(c,s,txn));
    if s.active is Some { assert(plan_inv(c,s,s.active.unwrap())); }
    if writes.contains_key(k) {
        assert(s.sessions[txn].held.contains_key(k));
        assert(s.sessions[txn].held[k] == directory(s)[k]);
        if s.active is Some && s.plans[s.active.unwrap()].keys.contains(k) {
            assert(!(s.phases[s.active.unwrap()] is Final || s.phases[s.active.unwrap()] is Retiring));
        }
    }
}
proof fn key_begin(c: Constants, s: State, a: Action, k: int)
    requires inv(c,s), enabled(c,s,a), a is Begin, c.keys.contains(k)
    ensures key_inv(c,apply(c,s,a),k)
{
    assert(key_inv(c,s,k));
}
proof fn key_freezing(c: Constants, s: State, k: int)
    requires inv(c,s), enabled(c,s,Action::RequestFreeze), c.keys.contains(k)
    ensures key_inv(c,apply(c,s,Action::RequestFreeze),k)
{
    assert(key_inv(c,s,k));
    assert(plan_inv(c,s,s.active.unwrap()));
}
proof fn key_final(c: Constants, s: State, k: int)
    requires inv(c,s), enabled(c,s,Action::RequestFinal), c.keys.contains(k)
    ensures key_inv(c,apply(c,s,Action::RequestFinal),k)
{
    assert(key_inv(c,s,k));
    assert(plan_inv(c,s,s.active.unwrap()));
    assert(s.certificates.contains((s.active.unwrap(),Certificate::Drained)));
}
proof fn key_retiring(c: Constants, s: State, k: int)
    requires inv(c,s), enabled(c,s,Action::RequestRetire), c.keys.contains(k)
    ensures key_inv(c,apply(c,s,Action::RequestRetire),k)
{
    assert(key_inv(c,s,k));
    assert(plan_inv(c,s,s.active.unwrap()));
    assert(s.certificates.contains((s.active.unwrap(),Certificate::Ready)));
}
proof fn key_commit(c: Constants, s: State, k: int)
    requires inv(c,s), enabled(c,s,Action::Commit), c.keys.contains(k)
    ensures key_inv(c,apply(c,s,Action::Commit),k)
{
    assert(key_inv(c,s,k));
    assert(plan_inv(c,s,s.active.unwrap()));
    assert(s.certificates.contains((s.active.unwrap(),Certificate::Retired)));
}
proof fn key_abort(c: Constants, s: State, k: int)
    requires inv(c,s), enabled(c,s,Action::Abort), c.keys.contains(k)
    ensures key_inv(c,apply(c,s,Action::Abort),k)
{
    assert(key_inv(c,s,k));
    assert(plan_inv(c,s,s.active.unwrap()));
}
proof fn key_finish(c: Constants, s: State, k: int)
    requires inv(c,s), enabled(c,s,Action::Finish), c.keys.contains(k)
    ensures key_inv(c,apply(c,s,Action::Finish),k)
{
    assert(key_inv(c,s,k));
    assert(plan_inv(c,s,s.active.unwrap()));
}
proof fn key_start_delivery(c: Constants, s: State, g: nat, owner: int, k: int)
    requires inv(c,s), enabled(c,s,Action::Deliver { generation: g,command: Command::Start,owner }),
        current(s,g), c.keys.contains(k)
    ensures key_inv(c,delivered(s,g,Command::Start,owner),k)
{
    assert(key_inv(c,s,k));
    assert(plan_inv(c,s,g));
    assert(command_inv(s,g,Command::Start));
}
proof fn key_freeze_delivery(c: Constants, s: State, g: nat, owner: int, k: int)
    requires inv(c,s), enabled(c,s,Action::Deliver { generation: g,command: Command::Freeze,owner }),
        current(s,g), c.keys.contains(k)
    ensures key_inv(c,delivered(s,g,Command::Freeze,owner),k)
{
    assert(key_inv(c,s,k));
    assert(plan_inv(c,s,g));
    assert(command_inv(s,g,Command::Freeze));
}
proof fn key_final_delivery(c: Constants, s: State, g: nat, owner: int, k: int)
    requires inv(c,s), enabled(c,s,Action::Deliver { generation: g,command: Command::Final,owner }),
        current(s,g), c.keys.contains(k)
    ensures key_inv(c,delivered(s,g,Command::Final,owner),k)
{
    assert(key_inv(c,s,k));
    assert(plan_inv(c,s,g));
    assert(command_inv(s,g,Command::Final));
}
proof fn key_retire_delivery(c: Constants, s: State, g: nat, owner: int, k: int)
    requires inv(c,s), enabled(c,s,Action::Deliver { generation: g,command: Command::Retire,owner }),
        current(s,g), c.keys.contains(k)
    ensures key_inv(c,delivered(s,g,Command::Retire,owner),k)
{
    assert(key_inv(c,s,k));
    assert(plan_inv(c,s,g));
    assert(command_inv(s,g,Command::Retire));
}
proof fn key_commit_delivery(c: Constants, s: State, g: nat, owner: int, k: int)
    requires inv(c,s), enabled(c,s,Action::Deliver { generation: g,command: Command::Commit,owner }),
        current(s,g), c.keys.contains(k)
    ensures key_inv(c,delivered(s,g,Command::Commit,owner),k)
{
    assert(key_inv(c,s,k));
    assert(plan_inv(c,s,g));
    assert(command_inv(s,g,Command::Commit));
}
proof fn key_abort_delivery(c: Constants, s: State, g: nat, owner: int, k: int)
    requires inv(c,s), enabled(c,s,Action::Deliver { generation: g,command: Command::Abort,owner }),
        current(s,g), c.keys.contains(k)
    ensures key_inv(c,delivered(s,g,Command::Abort,owner),k)
{
    assert(key_inv(c,s,k));
    assert(plan_inv(c,s,g));
    assert(command_inv(s,g,Command::Abort));
}
proof fn key_seal(c: Constants, s: State, g: nat, k: int)
    requires inv(c,s), enabled(c,s,Action::Seal { generation: g }), c.keys.contains(k)
    ensures key_inv(c,apply(c,s,Action::Seal { generation: g }),k)
{
    prepare_step(c,s,Action::Seal { generation: g });
    assert(key_inv(c,s,k));
    assert(plan_inv(c,s,g));
}
proof fn key_copy_delivery(c: Constants, s: State, packet: Packet, k: int)
    requires inv(c,s), enabled(c,s,Action::DeliverCopy { packet }), c.keys.contains(k)
    ensures key_inv(c,apply(c,s,Action::DeliverCopy { packet }),k)
{
    prepare_step(c,s,Action::DeliverCopy { packet });
    assert(packet_inv(s,packet));
    assert(plan_inv(c,s,packet.generation));
    assert(c.keys.contains(packet.key));
    assert(key_inv(c,s,packet.key));
    assert(key_inv(c,s,k));
}
proof fn key_frame(c: Constants, s: State, a: Action, k: int)
    requires inv(c,s), enabled(c,s,a), c.keys.contains(k),
        a is Open || a is Acquire || a is Release || a is Drain || a is Receive
            || a is Capture || a is Cache || a is Reply || a is Stutter
    ensures key_inv(c,apply(c,s,a),k)
{
    assert(key_inv(c,s,k));
    match a {
        Action::Open { .. } => {},
        Action::Acquire { .. } => {},
        Action::Release { .. } => {},
        Action::Drain { .. } => {},
        Action::Receive { .. } => {},
        Action::Capture { .. } => {},
        Action::Cache { .. } => {},
        Action::Reply { .. } => {},
        _ => {},
    }
}
pub(super) proof fn key_step(c: Constants, s: State, a: Action, k: int)
    requires inv(c,s), enabled(c,s,a), c.keys.contains(k)
    ensures key_inv(c,apply(c,s,a),k)
{
    match a {
        Action::Resolve { txn,writes } => { key_resolve(c,s,txn,writes,k); },
        Action::Begin { .. } => { key_begin(c,s,a,k); },
        Action::RequestFreeze => { key_freezing(c,s,k); },
        Action::RequestFinal => { key_final(c,s,k); },
        Action::RequestRetire => { key_retiring(c,s,k); },
        Action::Commit => { key_commit(c,s,k); },
        Action::Abort => { key_abort(c,s,k); },
        Action::Finish => { key_finish(c,s,k); },
        Action::Deliver { generation,command,owner } => {
            prepare_step(c,s,a);
            if current(s,generation) {
                match command {
                    Command::Start => { key_start_delivery(c,s,generation,owner,k); },
                    Command::Freeze => { key_freeze_delivery(c,s,generation,owner,k); },
                    Command::Final => { key_final_delivery(c,s,generation,owner,k); },
                    Command::Retire => { key_retire_delivery(c,s,generation,owner,k); },
                    Command::Commit => { key_commit_delivery(c,s,generation,owner,k); },
                    Command::Abort => { key_abort_delivery(c,s,generation,owner,k); },
                }
            }
        },
        Action::Seal { generation } => { key_seal(c,s,generation,k); },
        Action::DeliverCopy { packet } => { key_copy_delivery(c,s,packet,k); },
        _ => { key_frame(c,s,a,k); },
    }
}
} // verus!
