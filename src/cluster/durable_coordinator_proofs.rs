//! Byte-decoding uniqueness, native replay equivalence and durable publication.
//! Native Vec allocation identities are not durable observations: comparison
//! retains every control/nonce/generation/slot and every byte-coordinate route.
use vstd::prelude::*;
use crate::migration::{self as n,Native,Request};
use crate::types::*;
use crate::directory::{RouteTable,route};
use crate::directory_proofs::{equivalent,equivalent_routes};
use crate::durable_codec::{range_same,range_wire,txn_wire,range_prefix_unique,txn_prefix_unique};
use crate::durable_coordinator::{Response,effect,initialized,replays,replay_base,replay_predecessor,replay_bounds,
    native_counts,native_current_bound,native_record_bounds};
use crate::durable_coordinator_codec::*;
use crate::routing_codec_proofs::*;

verus! {
pub open spec fn operation_kind(op: Operation) -> u32 {
    match op { Operation::Begin { .. } => 1, Operation::Request(_) => 2,
        Operation::Receive { .. } => 3, Operation::Reply(_) => 4 }
}
pub open spec fn same_operation(a: Operation,b: Operation) -> bool {
    match (a,b) {
        (Operation::Begin {nonce: an,source: asrc,destination: ad,range: ar},
         Operation::Begin {nonce: bn,source: bsrc,destination: bd,range: br}) =>
            an == bn && asrc == bsrc && ad == bd && range_same(ar,br),
        (Operation::Request(a),Operation::Request(b)) => a == b,
        (Operation::Receive {generation: ag,owner: ao,certificate: ac},
         Operation::Receive {generation: bg,owner: bo,certificate: bc}) => ag == bg && ao == bo && ac == bc,
        (Operation::Reply(a),Operation::Reply(b)) => a == b,
        _ => false,
    }
}
proof fn nonce_sizes()
    ensures forall|n: TxnId| #[trigger] txn_wire(n).len() == 16,
{
    hide(word64); integer_sizes();
}
/// Together with both executable codec postconditions this proves that decoding
/// an encoded record cannot substitute a different operation or begin intent.
pub proof fn operation_injective(a: Operation,b: Operation)
    requires operation_wire(a) == operation_wire(b),
    ensures same_operation(a,b),
{
    hide(word32); hide(word64); hide(txn_wire); hide(range_wire);
    integer_sizes(); nonce_sizes();
    let wire = operation_wire(a);
    assert(wire.subrange(8,12) =~= word32(operation_kind(a)));
    assert(operation_wire(b).subrange(8,12) =~= word32(operation_kind(b)));
    word32_injective(operation_kind(a),operation_kind(b));
    match (a,b) {
        (Operation::Begin {nonce: an,source: asrc,destination: ad,range: ar},
         Operation::Begin {nonce: bn,source: bsrc,destination: bd,range: br}) => {
            assert(wire.skip(12) =~= txn_wire(an)+(word32(asrc)+word32(ad)+range_wire(ar)));
            assert(operation_wire(b).skip(12) =~= txn_wire(bn)+(word32(bsrc)+word32(bd)+range_wire(br)));
            txn_prefix_unique(an,bn,word32(asrc)+word32(ad)+range_wire(ar),word32(bsrc)+word32(bd)+range_wire(br));
            assert(wire.subrange(28,32) =~= word32(asrc));
            assert(operation_wire(b).subrange(28,32) =~= word32(bsrc));
            word32_injective(asrc,bsrc);
            assert(wire.subrange(32,36) =~= word32(ad));
            assert(operation_wire(b).subrange(32,36) =~= word32(bd));
            word32_injective(ad,bd);
            assert(wire.skip(36) =~= range_wire(ar));
            assert(operation_wire(b).skip(36) =~= range_wire(br));
            range_prefix_unique(ar,br,Seq::empty(),Seq::empty());
        },
        (Operation::Request(ar),Operation::Request(br)) => {
            assert(wire.skip(12) =~= word32(request_code(ar)));
            assert(operation_wire(b).skip(12) =~= word32(request_code(br)));
            word32_injective(request_code(ar),request_code(br));
        },
        (Operation::Receive {generation: ag,owner: ao,certificate: ac},
         Operation::Receive {generation: bg,owner: bo,certificate: bc}) => {
            assert(wire.subrange(12,20) =~= word64(ag));
            assert(operation_wire(b).subrange(12,20) =~= word64(bg));
            word64_injective(ag,bg);
            assert(wire.subrange(20,24) =~= word32(ao));
            assert(operation_wire(b).subrange(20,24) =~= word32(bo));
            word32_injective(ao,bo);
            assert(wire.skip(24) =~= word32(certificate_code(ac)));
            assert(operation_wire(b).skip(24) =~= word32(certificate_code(bc)));
            word32_injective(certificate_code(ac),certificate_code(bc));
        },
        (Operation::Reply(an),Operation::Reply(bn)) => {
            assert(wire.skip(12) =~= txn_wire(an));
            assert(operation_wire(b).skip(12) =~= txn_wire(bn));
            txn_prefix_unique(an,bn,Seq::empty(),Seq::empty());
        },
        _ => {},
    }
}
pub proof fn nodes_size(nodes: Seq<u32>)
    ensures nodes_wire(nodes).len() == 4*nodes.len(),
    decreases nodes.len(),
{ if nodes.len() > 0 { nodes_size(nodes.drop_last()); } }
pub proof fn nodes_unique(a: Seq<u32>,b: Seq<u32>)
    requires a.len() == b.len(), nodes_wire(a) == nodes_wire(b),
    ensures a == b,
    decreases a.len(),
{
    hide(word32); integer_sizes();
    if a.len() > 0 {
        nodes_size(a.drop_last()); nodes_size(b.drop_last());
        let p = (4*(a.len()-1)) as int;
        assert(nodes_wire(a).skip(p) =~= word32(a.last()));
        assert(nodes_wire(b).skip(p) =~= word32(b.last()));
        word32_injective(a.last(),b.last());
        assert(nodes_wire(a).take(p) =~= nodes_wire(a.drop_last()));
        assert(nodes_wire(b).take(p) =~= nodes_wire(b.drop_last()));
        nodes_unique(a.drop_last(),b.drop_last());
        assert(a.drop_last().push(a.last()) =~= a);
        assert(b.drop_last().push(b.last()) =~= b);
        assert(a == b);
    } else { assert(a =~= b); }
}
pub proof fn bootstrap_unique(an: Seq<u32>,at: Seq<RouteTable>,bn: Seq<u32>,bt: Seq<RouteTable>)
    requires an.len() <= usize::MAX, bn.len() <= usize::MAX,
        at.len() <= usize::MAX, bt.len() <= usize::MAX,
        bootstrap_wire(an,at) == bootstrap_wire(bn,bt),
    ensures an == bn, at.len() == bt.len(),
        forall|i: int| 0 <= i < at.len() ==> at[i].table == bt[i].table && equivalent(at[i].boundaries@,bt[i].boundaries@),
{
    hide(word32); hide(word64); integer_sizes();
    let a = bootstrap_wire(an,at); let b = bootstrap_wire(bn,bt);
    assert(a.subrange(12,20) =~= word64(an.len() as u64));
    assert(b.subrange(12,20) =~= word64(bn.len() as u64));
    word64_injective(an.len() as u64,bn.len() as u64);
    nodes_size(an); nodes_size(bn);
    let end = (20+4*an.len()) as int;
    assert(a.subrange(20,end) =~= nodes_wire(an));
    assert(b.subrange(20,end) =~= nodes_wire(bn));
    nodes_unique(an,bn);
    assert(a.subrange(end,end+8) =~= word64(at.len() as u64));
    assert(b.subrange(end,end+8) =~= word64(bt.len() as u64));
    word64_injective(at.len() as u64,bt.len() as u64);
    assert(a.skip(end+8) =~= tables_wire(at));
    assert(b.skip(end+8) =~= tables_wire(bt));
    tables_unique(at,bt);
}

/// Stable table identities, rather than volatile handles, determine a begin's
/// unique slot. This invariant follows from native bootstrap and field effects.
pub open spec fn table_slots(v: Native) -> bool {
    v.current.len() <= v.snapshots.len()
    && (forall|i: int| 0 <= i < v.current.len() ==> v.snapshots[v.current[i] as int].table == v.snapshots[i].table)
    && (forall|i: int,j: int| 0 <= i < j < v.current.len() ==> v.snapshots[i].table != v.snapshots[j].table)
    && (forall|i: int| 0 <= i < v.records.len() ==> {
        let r = #[trigger] v.records[i];
        v.snapshots[r.previous as int].table == v.snapshots[r.slot as int].table
        && v.snapshots[r.proposal as int].table == v.snapshots[r.slot as int].table
    })
}
pub proof fn effect_slots(b: Native,z: Native,op: Operation,response: Response)
    requires n::layout(b), n::layout(z), table_slots(b), effect(b,z,op,Ok(response)),
    ensures table_slots(z),
{
    hide(route); hide(equivalent); hide(n::layout);
    hide(crate::directory::source_owned); hide(crate::directory::proper); hide(crate::directory::inside);
    match op {
        Operation::Begin {nonce,source,destination,range} => {
            if z != b {
                let r = z.records.last();
                assert(z.snapshots.drop_last() == b.snapshots);
                assert(z.records.drop_last() == b.records);
                assert forall|j: int| 0 <= j < b.snapshots.len() implies
                    #[trigger] z.snapshots[j] == b.snapshots[j] by {
                    assert(z.snapshots.drop_last()[j] == z.snapshots[j]);
                }
                assert forall|j: int| 0 <= j < z.current.len() implies
                    z.snapshots[z.current[j] as int].table == z.snapshots[j].table by {
                    native_current_bound(b,j);
                    assert(z.current[j] == b.current[j]);
                }
                assert forall|i: int| 0 <= i < z.records.len() implies
                    z.snapshots[z.records[i].previous as int].table == z.snapshots[z.records[i].slot as int].table
                    && z.snapshots[z.records[i].proposal as int].table == z.snapshots[z.records[i].slot as int].table by {
                    native_record_bounds(z,i);
                    if i < b.records.len() {
                        native_record_bounds(b,i);
                        assert(z.records[i] == b.records[i]);
                        let previous = b.records[i].previous as int;
                        let proposal = b.records[i].proposal as int;
                        let slot = b.records[i].slot as int;
                        assert(slot < b.snapshots.len());
                        assert(z.snapshots[previous] == b.snapshots[previous]);
                        assert(z.snapshots[proposal] == b.snapshots[proposal]);
                        assert(z.snapshots[slot] == b.snapshots[slot]);
                    } else {
                        assert(i == b.records.len() && z.records[i] == r);
                        native_current_bound(b,r.slot as int);
                        assert(r.previous == b.current[r.slot as int]);
                        assert(r.previous < b.snapshots.len());
                        assert(r.slot < b.snapshots.len());
                        assert(r.proposal == b.snapshots.len());
                        assert(z.snapshots[r.proposal as int] == z.snapshots.last());
                        assert(z.snapshots.last().table == range.table);
                        assert(b.snapshots[r.previous as int].table == range.table);
                    }
                }
            }
        },
        Operation::Request(Request::Commit) => {
            native_counts(b);
            let i = b.active.unwrap() as int;
            native_record_bounds(b,i);
            let r = b.records[i];
            assert forall|j: int| 0 <= j < z.current.len() implies
                z.snapshots[z.current[j] as int].table == z.snapshots[j].table by {
                if j == r.slot {
                    assert(z.current[j] == r.proposal);
                    assert(b.snapshots[r.proposal as int].table == b.snapshots[r.slot as int].table);
                } else {
                    native_current_bound(b,j);
                    assert(z.current[j] == b.current[j]);
                }
            }
        },
        _ => {},
    }
}
pub proof fn replay_slots(records: Seq<Seq<u8>>,v: Native)
    requires replays(records,v), ensures table_slots(v),
    decreases records.len(),
{
    hide(replays); hide(effect); hide(operation_wire); hide(bootstrap_wire);
    hide(n::valid_initial_table); hide(n::layout);
    replay_bounds(records,v);
    if records.len() == 1 {
        let (nodes,tables) = replay_base(records,v);
    } else {
        let (b,op,response) = replay_predecessor(records,v);
        replay_slots(records.drop_last(),b);
        effect_slots(b,v,op,response);
    }
}
pub open spec fn same_plan(a: MigrationPlan,b: MigrationPlan) -> bool {
    a.generation == b.generation && a.nonce == b.nonce && a.source == b.source && a.destination == b.destination
    && range_same(a.range,b.range)
    && (forall|k: Seq<u8>| route(a.old@,k) == route(b.old@,k))
}
pub open spec fn same_state(a: Native,b: Native) -> bool {
    a.nodes == b.nodes && a.current == b.current && a.active == b.active
    && a.next_generation == b.next_generation && a.published_version == b.published_version
    && a.controls == b.controls && a.records.len() == b.records.len() && a.snapshots.len() == b.snapshots.len()
    && (forall|i: int| 0 <= i < a.records.len() ==> {
        let x = #[trigger] a.records[i]; let y = b.records[i];
        x.previous == y.previous && x.proposal == y.proposal && x.slot == y.slot && same_plan(x.plan,y.plan)
    })
    && (forall|i: int| 0 <= i < a.snapshots.len() ==> (#[trigger] a.snapshots[i]).table == b.snapshots[i].table
        && (forall|k: Seq<u8>| #[trigger] route(a.snapshots[i].boundaries@,k) == route(b.snapshots[i].boundaries@,k)))
}
proof fn same_record(a: Native,b: Native,index: int)
    requires same_state(a,b), 0 <= index < a.records.len(),
    ensures index < b.records.len(),
        a.records[index].previous == b.records[index].previous,
        a.records[index].proposal == b.records[index].proposal,
        a.records[index].slot == b.records[index].slot,
        same_plan(a.records[index].plan,b.records[index].plan),
        a.records[index].plan.nonce == b.records[index].plan.nonce,
        a.records[index].plan.generation == b.records[index].plan.generation,
{
    hide(route);
    let x = a.records[index]; let y = b.records[index];
    assert(x.previous == y.previous && x.proposal == y.proposal && x.slot == y.slot && same_plan(x.plan,y.plan));
}
proof fn same_snapshot_route(a: Native,b: Native,index: int,key: Seq<u8>)
    requires same_state(a,b), 0 <= index < a.snapshots.len(),
    ensures index < b.snapshots.len(), route(a.snapshots[index].boundaries@,key) == route(b.snapshots[index].boundaries@,key),
{
    hide(route);
    assert(a.snapshots[index].table == b.snapshots[index].table
        && (forall|k: Seq<u8>| #[trigger] route(a.snapshots[index].boundaries@,k) == route(b.snapshots[index].boundaries@,k)));
}
proof fn framed_state(a: Native,az: Native,b: Native,bz: Native)
    requires same_state(a,b),
        az.nodes == a.nodes, bz.nodes == b.nodes,
        az.records == a.records, bz.records == b.records,
        az.snapshots == a.snapshots, bz.snapshots == b.snapshots,
        az.controls == bz.controls, az.current == bz.current, az.active == bz.active,
        az.next_generation == bz.next_generation, az.published_version == bz.published_version,
    ensures same_state(az,bz),
{
    hide(route);
}
pub proof fn initial_equivalent(a: Native,b: Native,record: Seq<u8>)
    requires replays(seq![record],a), replays(seq![record],b),
    ensures same_state(a,b),
{
    hide(replays); hide(effect); hide(operation_wire); hide(bootstrap_wire);
    hide(route); hide(n::layout); hide(word32); hide(word64);
    hide(n::bootstrap);
    let (an,at) = replay_base(seq![record],a);
    let (bn,bt) = replay_base(seq![record],b);
    bootstrap_unique(an,at,bn,bt);
    assert(a.current =~= b.current);
    assert(a.controls =~= b.controls);
    assert forall|i: int,k: Seq<u8>| 0 <= i < at.len() implies
        route(at[i].boundaries@,k) == route(bt[i].boundaries@,k) by {
        equivalent_routes(at[i].boundaries@,bt[i].boundaries@,k);
    }
}
/// Begin is the only operation that allocates snapshots and retained plans.
proof fn begin_equivalent(a: Native,az: Native,b: Native,bz: Native,ao: Operation,bo: Operation,ar: Response,br: Response)
    requires n::layout(a), n::layout(az), n::layout(b), n::layout(bz),
        table_slots(a), table_slots(b), same_state(a,b), same_operation(ao,bo),
        effect(a,az,ao,Ok(ar)), effect(b,bz,bo,Ok(br)),
        ao is Begin, bo is Begin,
    ensures same_state(az,bz), ar == br,
{
    hide(route); hide(equivalent); hide(n::layout);
    hide(crate::directory::source_owned); hide(crate::directory::proper); hide(crate::directory::inside);
    match (ao,bo) {
        (Operation::Begin {nonce: an,source: asrc,destination: ad,range: ax},
         Operation::Begin {nonce: bn,source: bsrc,destination: bd,range: bx}) => {
            if exists|i: int| 0 <= i < a.records.len() && n::same_nonce(a.records[i].plan.nonce,an) {
                let i = choose|i: int| 0 <= i < a.records.len() && n::same_nonce(a.records[i].plan.nonce,an);
                same_record(a,b,i);
                n::unique_nonce(a,an,i); n::unique_nonce(b,bn,i);
                assert(az == a && bz == b);
            } else {
                assert forall|i: int| 0 <= i < b.records.len() implies
                    !n::same_nonce(b.records[i].plan.nonce,bn) by {
                    same_record(a,b,i);
                    assert(!n::same_nonce(a.records[i].plan.nonce,an));
                }
                assert(!(exists|i: int| 0 <= i < b.records.len() && n::same_nonce(b.records[i].plan.nonce,bn)));
                let x = az.records.last(); let y = bz.records.last();
                assert(x == az.records[a.records.len() as int]);
                assert(y == bz.records[b.records.len() as int]);
                assert(ax.lo@ == bx.lo@ && n::upper(ax) == n::upper(bx));
                native_current_bound(a,x.slot as int);
                native_current_bound(b,y.slot as int);
                assert(x.previous < a.snapshots.len() && y.previous < b.snapshots.len());
                assert(a.snapshots[x.slot as int].table == ax.table);
                assert(b.snapshots[y.slot as int].table == bx.table);
                if x.slot < y.slot { assert(a.snapshots[x.slot as int].table != a.snapshots[y.slot as int].table); }
                if y.slot < x.slot { assert(a.snapshots[y.slot as int].table != a.snapshots[x.slot as int].table); }
                assert(x.slot == y.slot);
                assert(x.previous == y.previous && x.proposal == y.proposal);
                assert forall|k: Seq<u8>| route(x.plan.old@,k) == route(y.plan.old@,k) by {
                    equivalent_routes(x.plan.old@,a.snapshots[x.previous as int].boundaries@,k);
                    equivalent_routes(y.plan.old@,b.snapshots[y.previous as int].boundaries@,k);
                    same_snapshot_route(a,b,x.previous as int,k);
                }
                assert(same_plan(x.plan,y.plan));
                assert forall|i: int| 0 <= i < az.records.len() implies
                    (#[trigger] az.records[i]).previous == bz.records[i].previous && az.records[i].proposal == bz.records[i].proposal
                    && az.records[i].slot == bz.records[i].slot && same_plan(az.records[i].plan,bz.records[i].plan) by {
                    if i < a.records.len() { assert(az.records[i] == a.records[i] && bz.records[i] == b.records[i]); }
                    else { assert(az.records[i] == x && bz.records[i] == y); }
                }
                assert forall|i: int| 0 <= i < az.snapshots.len() implies
                    (#[trigger] az.snapshots[i]).table == bz.snapshots[i].table
                    && (forall|k: Seq<u8>| #[trigger] route(az.snapshots[i].boundaries@,k)
                        == route(bz.snapshots[i].boundaries@,k)) by {
                    if i < a.snapshots.len() {
                        assert(az.snapshots[i] == a.snapshots[i] && bz.snapshots[i] == b.snapshots[i]);
                        assert(a.snapshots[i].table == b.snapshots[i].table);
                    } else {
                        assert(i == a.snapshots.len());
                        assert(az.snapshots[i] == az.snapshots.last());
                        assert(bz.snapshots[i] == bz.snapshots.last());
                        assert(az.snapshots[i].table == ax.table);
                        assert(bz.snapshots[i].table == bx.table);
                        assert(ax.table == bx.table);
                    }
                    assert forall|k: Seq<u8>| #[trigger] route(az.snapshots[i].boundaries@,k)
                        == route(bz.snapshots[i].boundaries@,k) by {
                        if i < a.snapshots.len() {
                            same_snapshot_route(a,b,i,k);
                        } else {
                            same_snapshot_route(a,b,x.previous as int,k);
                            assert(route(az.snapshots[i].boundaries@,k) ==
                                if crate::directory::inside(k,ax.lo@,n::upper(ax)) {
                                    Grant {owner: ad,ownership_version: a.next_generation}
                                } else { route(a.snapshots[x.previous as int].boundaries@,k) });
                            assert(route(bz.snapshots[i].boundaries@,k) ==
                                if crate::directory::inside(k,bx.lo@,n::upper(bx)) {
                                    Grant {owner: bd,ownership_version: b.next_generation}
                                } else { route(b.snapshots[y.previous as int].boundaries@,k) });
                        }
                    }
                }
                assert(az.nodes == bz.nodes && az.current == bz.current && az.active == bz.active);
                assert(az.next_generation == bz.next_generation && az.published_version == bz.published_version);
                assert(az.controls == a.controls.push(n::initial_control()));
                assert(bz.controls == b.controls.push(n::initial_control()));
                assert(az.controls == bz.controls);
                assert(az.records.len() == bz.records.len() && az.snapshots.len() == bz.snapshots.len());
                assert(same_state(az,bz));
            }
        },
        _ => {},
    }
}
/// Non-admission operations preserve the already reconstructed plan/directory
/// representations and only change the exact native control fields.
pub proof fn step_equivalent(a: Native,az: Native,b: Native,bz: Native,ao: Operation,bo: Operation,ar: Response,br: Response)
    requires n::layout(a), n::layout(az), n::layout(b), n::layout(bz),
        table_slots(a), table_slots(b), same_state(a,b), same_operation(ao,bo),
        effect(a,az,ao,Ok(ar)), effect(b,bz,bo,Ok(br)),
    ensures same_state(az,bz), ar == br,
{
    hide(route); hide(equivalent); hide(n::layout);
    hide(crate::directory::source_owned); hide(crate::directory::proper); hide(crate::directory::inside);
    if ao is Begin {
        assert(bo is Begin);
        begin_equivalent(a,az,b,bz,ao,bo,ar,br);
    } else {
        match (ao,bo) {
            (Operation::Request(_),Operation::Request(_)) => {
                native_counts(a);
                assert(a.active is Some);
                same_record(a,b,a.active.unwrap() as int);
                assert(az.current == bz.current);
                assert(az.controls == bz.controls);
            },
            (Operation::Receive {generation,owner,certificate},Operation::Receive { .. }) => {
                let i = choose|i: int| 0 <= i < a.records.len() && a.records[i].plan.generation == generation;
                same_record(a,b,i);
                n::unique_generation(a,generation,i); n::unique_generation(b,generation,i);
            },
            (Operation::Reply(nonce),Operation::Reply(_)) => {
                let i = choose|i: int| 0 <= i < a.records.len() && n::same_nonce(a.records[i].plan.nonce,nonce);
                same_record(a,b,i);
                n::unique_nonce(a,nonce,i); n::unique_nonce(b,nonce,i);
            },
            _ => {},
        }
        assert(az.records == a.records && bz.records == b.records);
        assert(az.snapshots == a.snapshots && bz.snapshots == b.snapshots);
        assert(az.controls == bz.controls && az.current == bz.current);
        framed_state(a,az,b,bz);
    }
}
/// Recovery is equivalent to the committed native operation fold, not merely
/// well-formed and not a ghost-history-dependent reset to an initial phase.
pub proof fn replay_equivalent(records: Seq<Seq<u8>>,a: Native,b: Native)
    requires replays(records,a), replays(records,b),
    ensures same_state(a,b),
    decreases records.len(),
{
    hide(replays); hide(effect); hide(same_state);
    hide(operation_wire); hide(n::layout);
    replay_bounds(records,a); replay_bounds(records,b);
    if records.len() == 1 {
        assert(records == seq![records[0]]);
        initial_equivalent(a,b,records[0]);
    } else {
        let (ap,ao,ar) = replay_predecessor(records,a);
        let (bp,bo,br) = replay_predecessor(records,b);
        replay_equivalent(records.drop_last(),ap,bp);
        replay_slots(records.drop_last(),ap); replay_slots(records.drop_last(),bp);
        operation_injective(ao,bo);
        step_equivalent(ap,a,bp,b,ao,bo,ar,br);
    }
}

pub open spec fn decision_witness(records: Seq<Seq<u8>>,i: int,b: Native,z: Native,generation: u64,committed: bool) -> bool {
    let request = if committed { Request::Commit } else { Request::Abort };
    1 <= i < records.len() && replays(records.take(i),b)
        && b.active is Some && b.records[b.active.unwrap() as int].plan.generation == generation
        && records[i] == operation_wire(Operation::Request(request))
        && effect(b,z,Operation::Request(request),Ok(Response::Applied))
}
pub open spec fn decision_tuple(records: Seq<Seq<u8>>,t: (int,Native,Native),generation: u64,committed: bool) -> bool {
    decision_witness(records,t.0,t.1,t.2,generation,committed)
}
pub open spec fn has_decision(records: Seq<Seq<u8>>,generation: u64,committed: bool) -> bool {
    exists|t: (int,Native,Native)| #[trigger] decision_tuple(records,t,generation,committed)
}
pub proof fn decision_prefix(records: Seq<Seq<u8>>,generation: u64,committed: bool,entry: Seq<u8>)
    requires has_decision(records,generation,committed),
    ensures has_decision(records.push(entry),generation,committed),
{
    hide(effect); hide(replays); hide(operation_wire);
    let (i,b,z) = choose|t: (int,Native,Native)| #[trigger] decision_tuple(records,t,generation,committed);
    assert(records.push(entry).take(i) == records.take(i));
    assert(decision_witness(records.push(entry),i,b,z,generation,committed));
    assert(decision_tuple(records.push(entry),(i,b,z),generation,committed));
}
pub proof fn publication_has_durable_decision(records: Seq<Seq<u8>>,v: Native)
    requires replays(records,v), v.published_version > 0,
    ensures has_decision(records,v.published_version,true),
    decreases records.len(),
{
    hide(replays); hide(operation_wire); hide(bootstrap_wire); hide(n::layout);
    replay_bounds(records,v);
    if records.len() == 1 {
        let (nodes,tables) = replay_base(records,v);
    } else {
        let (b,op,response) = replay_predecessor(records,v);
        if op is Request && op->Request_0 is Commit {
            assert(response == Response::Applied);
            assert(records.take(records.len() as int-1) == records.drop_last());
            assert(decision_witness(records,records.len() as int-1,b,v,v.published_version,true));
            assert(decision_tuple(records,(records.len() as int-1,b,v),v.published_version,true));
        } else {
            assert(b.published_version == v.published_version);
            publication_has_durable_decision(records.drop_last(),b);
            decision_prefix(records.drop_last(),v.published_version,true,records.last());
            assert(records.drop_last().push(records.last()) == records);
        }
    }
}
pub proof fn outcome_has_durable_decision(records: Seq<Seq<u8>>,v: Native,index: int)
    requires replays(records,v), 0 <= index < v.records.len(), v.controls[index].outcome is Some,
    ensures has_decision(records,v.records[index].plan.generation,v.controls[index].outcome.unwrap().committed),
    decreases records.len(),
{
    hide(replays); hide(operation_wire); hide(bootstrap_wire); hide(n::layout);
    replay_bounds(records,v);
    if records.len() == 1 {
        let (nodes,tables) = replay_base(records,v);
    } else {
        let (b,op,response) = replay_predecessor(records,v);
        if b.controls.len() > index && b.controls[index].outcome == v.controls[index].outcome {
            assert(b.records[index].plan.generation == v.records[index].plan.generation);
            outcome_has_durable_decision(records.drop_last(),b,index);
            decision_prefix(records.drop_last(),v.records[index].plan.generation,v.controls[index].outcome.unwrap().committed,records.last());
            assert(records.drop_last().push(records.last()) == records);
        } else {
            assert(op is Request);
            let r = op->Request_0;
            assert(r is Commit || r is Abort);
            assert(b.active == Some(index as usize));
            let committed = r is Commit;
            assert(v.controls[index].outcome.unwrap().committed == committed);
            assert(records.take(records.len() as int-1) == records.drop_last());
            assert(decision_witness(records,records.len() as int-1,b,v,v.records[index].plan.generation,committed));
            assert(decision_tuple(records,(records.len() as int-1,b,v),v.records[index].plan.generation,committed));
        }
    }
}
} // verus!
