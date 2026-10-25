//! Scoped durable checkpoints and transaction-engine suffixes. No shared log.
use vstd::prelude::*;
use vstd::imap::IMap;
use crate::sharding_placement as p;

verus! {

pub enum Authority { Coordinator, Participant(int), Transaction(int) }
pub struct Core {
    pub placement: p::State,
    pub members: Map<int, nat>,
    // Never removed when a numerical owner leaves the registry.
    pub owner_floor: Map<int, nat>,
}
pub struct CoordinatorImage {
    pub directory: Seq<Map<int, p::Grant>>, pub views: Map<int, nat>,
    pub next_generation: nat, pub active: Option<nat>,
    pub plans: Map<nat, p::Plan>, pub phases: Map<nat, p::Phase>,
    pub commands: Set<(nat, p::Command)>, pub received: Set<(nat, p::Certificate)>,
    pub outcomes: Map<int, p::Outcome>, pub replies: Map<int, p::Outcome>,
    pub members: Map<int, nat>, pub owner_floor: Map<int, nat>,
}
pub struct ParticipantImage {
    pub replicas: IMap<int, p::Replica>,
    pub certificates: Set<(nat, p::Certificate)>, pub packets: Set<p::Packet>,
}
// Resolution is deliberately NOT in this lease journal. It comes from the
// trusted transaction engine's durable terminal-outcome interface.
pub enum Image {
    Coordinator(CoordinatorImage), Participant(ParticipantImage),
    Leases(Option<Map<int, p::Grant>>),
}
pub struct EngineWrite { pub cells: Map<int, p::Cell> }
pub struct Envelope {
    pub action: p::Action, pub slot: nat, pub incarnation: nat, pub membership: nat,
    pub target: Option<(int, nat, nat)>,
}
pub struct Checkpoint {
    pub image: Image, pub engine_offset: nat,
    pub incarnation: nat, pub membership: nat,
    pub processed: Set<Envelope>,
}

pub open spec fn certificate_owner(s: p::State, g: nat, cert: p::Certificate) -> int {
    match cert {
        p::Certificate::Drained | p::Certificate::Retired | p::Certificate::SourceDone => s.plans[g].src,
        _ => s.plans[g].dst,
    }
}
pub open spec fn participant_image(image: Image) -> ParticipantImage {
    match image {
        Image::Participant(value) => value,
        _ => ParticipantImage { replicas: IMap::empty(), certificates: Set::empty(), packets: Set::empty() },
    }
}
pub open spec fn project(core: Core, a: Authority) -> Image {
    let s = core.placement;
    match a {
        Authority::Coordinator => Image::Coordinator(CoordinatorImage {
            directory: s.directory, views: s.views, next_generation: s.next_generation,
            active: s.active, plans: s.plans, phases: s.phases, commands: s.commands,
            received: s.received, outcomes: s.outcomes, replies: s.replies,
            members: core.members, owner_floor: core.owner_floor,
        }),
        Authority::Participant(n) => Image::Participant(ParticipantImage {
            replicas: IMap::new(|k: int| s.physical.contains_key((n,k)), |k: int| s.physical[(n,k)]),
            certificates: s.certificates.filter(|gc: (nat,p::Certificate)| certificate_owner(s,gc.0,gc.1) == n),
            packets: s.packets.filter(|packet: p::Packet| s.plans[packet.generation].src == n),
        }),
        Authority::Transaction(t) => Image::Leases(if s.sessions.contains_key(t) { Some(s.sessions[t].held) } else { None }),
    }
}
pub open spec fn actor(s: p::State, a: p::Action) -> Authority {
    match a {
        p::Action::Open { txn } | p::Action::Acquire { txn, .. } | p::Action::Release { txn, .. }
            | p::Action::Resolve { txn, .. } => Authority::Transaction(txn),
        p::Action::Deliver { owner, .. } => Authority::Participant(owner),
        p::Action::Drain { generation } | p::Action::Capture { generation, .. } => Authority::Participant(s.plans[generation].src),
        p::Action::Seal { generation } => Authority::Participant(s.plans[generation].dst),
        p::Action::DeliverCopy { packet } => Authority::Participant(s.plans[packet.generation].dst),
        _ => Authority::Coordinator,
    }
}
pub open spec fn with_placement(core: Core, s: p::State) -> Core { Core { placement: s, ..core } }
pub open spec fn overlay(image: Image, write: EngineWrite) -> Image {
    match image {
        Image::Participant(local) => Image::Participant(ParticipantImage {
            replicas: IMap::new(|k: int| local.replicas.contains_key(k), |k: int|
                if write.cells.contains_key(k) { p::Replica { cell: write.cells[k], ..local.replicas[k] } }
                else { local.replicas[k] }), ..local
        }),
        _ => image,
    }
}
pub open spec fn engine_fold(image: Image, engine: Seq<EngineWrite>, start: nat, end: nat) -> Image
    decreases end
{
    if start < end && end <= engine.len() {
        overlay(engine_fold(image,engine,start,(end-1) as nat),engine[end-1])
    } else { image }
}
pub open spec fn replace_checkpoint(_previous: Image, record: Checkpoint) -> Image { record.image }
pub open spec fn journal_fold(log: Seq<Checkpoint>, end: nat) -> Image
    decreases end
{
    if 0 < end && end <= log.len() {
        replace_checkpoint(journal_fold(log,(end-1) as nat),log[end-1])
    } else { Image::Leases(None) }
}
pub open spec fn reconstructed(log: Seq<Checkpoint>, engine: Seq<EngineWrite>) -> Image {
    engine_fold(journal_fold(log,log.len()),engine,log.last().engine_offset,engine.len())
}
pub open spec fn transaction_write(s: p::State, t: int, writes: Map<int,Option<int>>, n: int) -> EngineWrite {
    EngineWrite { cells: Map::new(writes.dom().filter(|k: int| s.sessions[t].held[k].owner == n),
        |k: int| p::Cell { value: writes[k], writer: t }) }
}

pub proof fn journal_replacement(log: Seq<Checkpoint>, end: nat)
    requires 0 < end <= log.len()
    ensures journal_fold(log,end) == log[end-1].image
{}
pub proof fn checkpoint_reconstruction(log: Seq<Checkpoint>, engine: Seq<EngineWrite>, record: Checkpoint)
    requires record.engine_offset == engine.len()
    ensures reconstructed(log.push(record),engine) == record.image
{
    journal_replacement(log.push(record),log.len()+1);
}
pub proof fn engine_prefix(image: Image, before: Seq<EngineWrite>, write: EngineWrite, start: nat, end: nat)
    requires end <= before.len()
    ensures engine_fold(image,before.push(write),start,end) == engine_fold(image,before,start,end)
    decreases end
{
    if start < end {
        engine_prefix(image,before,write,start,(end-1) as nat);
    }
}
pub proof fn engine_append(log: Seq<Checkpoint>, engine: Seq<EngineWrite>, write: EngineWrite)
    requires log.len() > 0, log.last().engine_offset <= engine.len()
    ensures reconstructed(log,engine.push(write)) == overlay(reconstructed(log,engine),write)
{
    engine_prefix(journal_fold(log,log.len()),engine,write,log.last().engine_offset,engine.len());
}
pub proof fn empty_overlay(image: Image)
    ensures overlay(image,EngineWrite { cells: Map::empty() }) == image
{
    match image {
        Image::Participant(local) => {
            assert(participant_image(overlay(image,EngineWrite { cells: Map::empty() })).replicas =~= local.replicas);
        },
        _ => {},
    }
}

pub proof fn certificate_plan(c: p::Constants, s: p::State, generation: nat, certificate: p::Certificate)
    requires p::inv(c,s),s.certificates.contains((generation,certificate))
    ensures s.plans.contains_key(generation)
{}

// Every non-engine placement transition writes exactly one authority's fields.
// This lemma, rather than an atomic shared-log assumption, licenses the append.
pub proof fn placement_locality(c: p::Constants, core: Core, action: p::Action, other: Authority)
    requires p::inv(c,core.placement), p::enabled(c,core.placement,action),
        !(action is Resolve), other != actor(core.placement,action)
    ensures project(with_placement(core,p::apply(c,core.placement,action)),other) == project(core,other)
{
    let s = core.placement;
    let z = p::apply(c,s,action);
    match action {
        p::Action::Begin { .. } => {
            assert forall|g: nat| s.plans.contains_key(g) implies z.plans[g] == s.plans[g] by {
                assert(p::plan_inv(c,s,g));
            }
        },
        p::Action::Deliver { generation,command,owner } => {
            assert(p::command_inv(s,generation,command));
            assert(p::plan_inv(c,s,generation));
        },
        _ => {},
    }
    match other {
        Authority::Participant(n) => {
            let old = participant_image(project(core,other));
            let new = participant_image(project(with_placement(core,z),other));
            assert(old.replicas =~= new.replicas);
            assert(old.certificates =~= new.certificates) by {
                assert forall|gc: (nat,p::Certificate)| old.certificates.contains(gc) <==> new.certificates.contains(gc) by {
                    if s.certificates.contains(gc) {
                        assert(gc == (gc.0,gc.1));
                        assert(s.certificates.contains((gc.0,gc.1)));
                        certificate_plan(c,s,gc.0,gc.1);
                    }
                    match action { p::Action::Deliver { generation,command,owner } => {}, _ => {} }
                }
            }
            assert(old.packets =~= new.packets) by {
                assert forall|packet: p::Packet| old.packets.contains(packet) <==> new.packets.contains(packet) by {
                    if s.packets.contains(packet) { assert(p::packet_inv(s,packet)); }
                }
            }
        },
        _ => {},
    }
}
pub proof fn transaction_locality(c: p::Constants, core: Core, t: int, writes: Map<int,Option<int>>, a: Authority)
    requires p::inv(c,core.placement), p::can_resolve(core.placement,t,writes)
    ensures project(with_placement(core,p::apply(c,core.placement,p::Action::Resolve { txn: t,writes })),a)
        == match a {
            Authority::Participant(n) => overlay(project(core,a),transaction_write(core.placement,t,writes,n)),
            _ => project(core,a),
        }
{
    match a {
        Authority::Participant(n) => {
            let z = p::apply(c,core.placement,p::Action::Resolve { txn: t,writes });
            let left = participant_image(project(with_placement(core,z),a));
            let right = participant_image(overlay(project(core,a),transaction_write(core.placement,t,writes,n)));
            assert(left.replicas =~= right.replicas);
            assert(left.certificates =~= right.certificates);
            assert(left.packets =~= right.packets);
        },
        _ => {},
    }
}
} // verus!
