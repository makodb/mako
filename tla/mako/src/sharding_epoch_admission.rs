//! Proposed epoch-cached admission over the existing process-local engine epoch.
//! Publication identities and epoch floors are ghost provenance, not additional
//! runtime counters. Immutable views cover all physical aliases, including rows
//! that are absent in storage. Atomic publication/announcement ordering and the
//! engine's settlement-before-Exit rule are refinement obligations.
use vstd::prelude::*;
use vstd::imap::IMap;
use vstd::iset::ISet;
pub use crate::sharding_commit_validation::Grant;
pub use crate::sharding_commit_validation::authority::Key;
#[path = "sharding_epoch_admission_proofs.rs"]
pub mod proofs;
#[path = "sharding_epoch_admission_witnesses.rs"]
pub mod witnesses;

verus! {

pub type View = IMap<Key,Grant>;
pub struct Cache { pub epoch: nat, pub view: nat }
pub struct Reader {
    pub worker: int,
    pub epoch: nat,
    pub announced: bool,
    pub entered: bool,
    pub view: Option<nat>,
    pub finished: bool,
    pub attempt: nat,
}
pub struct State {
    pub epoch: nat,
    pub history: Seq<View>,
    pub floor: Map<nat,nat>,
    pub caches: Map<int,Cache>,
    pub readers: Map<int,Reader>,
    pub completed: nat,
}
pub enum Action {
    Probe { id: int, worker: int },
    Announce { id: int },
    Validate { id: int },
    Resample { id: int },
    Capture { id: int },
    Retry { id: int },
    Exit { id: int },
    Publish { view: View },
    Tick,
    Complete { cut: nat },
    Stutter,
}
pub open spec fn initial(view: View) -> State {
    State { epoch: 1, history: seq![view], floor: Map::empty().insert(1,0),
        caches: Map::empty(), readers: Map::empty(), completed: 0 }
}
pub open spec fn current_index(s: State) -> nat { (s.history.len() - 1) as nat }
pub open spec fn current_view(s: State) -> View { s.history[current_index(s) as int] }
pub open spec fn live(s: State, id: int) -> bool {
    s.readers.contains_key(id) && !s.readers[id].finished
}
/// Announced probes also block a barrier. An unpublished probe does not; its
/// later announcement is harmless only because Validate performs the recheck.
pub open spec fn drained(s: State, cut: nat) -> bool {
    forall|id: int| #[trigger] s.readers.contains_key(id)
        && s.readers[id].announced && !s.readers[id].finished
        ==> cut <= s.readers[id].epoch
}
/// Ghost reduction to the engine's existing one announcement slot per worker.
/// Reader records describe logical guards; this is not a runtime lease registry.
pub open spec fn worker_readers(s: State, worker: int) -> Set<int> {
    s.readers.dom().filter(|id: int| s.readers[id].worker == worker
        && s.readers[id].announced && !s.readers[id].finished)
}
pub open spec fn minimum_epoch(s: State, ids: Set<int>) -> Option<nat>
    decreases ids.len()
{
    if ids.len() == 0 { None }
    else {
        let id = ids.choose();
        let rest = minimum_epoch(s,ids.remove(id));
        Some(if rest is Some && rest.unwrap() < s.readers[id].epoch {
            rest.unwrap()
        } else { s.readers[id].epoch })
    }
}
pub open spec fn worker_slot(s: State, worker: int) -> Option<nat> {
    minimum_epoch(s,worker_readers(s,worker))
}
pub open spec fn slots_drained(s: State, cut: nat) -> bool {
    forall|worker: int| #[trigger] worker_slot(s,worker) is Some
        ==> cut <= worker_slot(s,worker).unwrap()
}

pub open spec fn enabled(s: State, a: Action) -> bool {
    match a {
        Action::Probe { id,.. } => !s.readers.contains_key(id),
        Action::Announce { id } => live(s,id) && !s.readers[id].entered
            && !s.readers[id].announced,
        Action::Validate { id } => live(s,id) && s.readers[id].announced
            && !s.readers[id].entered
            && s.readers[id].epoch == s.epoch,
        Action::Resample { id } => live(s,id) && !s.readers[id].entered,
        Action::Capture { id } => live(s,id) && s.readers[id].entered
            && s.readers[id].view is None,
        Action::Retry { id } => live(s,id) && s.readers[id].entered
            && s.readers[id].view is Some,
        Action::Exit { id } => live(s,id),
        Action::Publish { .. } => true,
        Action::Tick => true,
        Action::Complete { cut } => 0 < cut <= s.epoch && s.completed <= cut
            && drained(s,cut),
        Action::Stutter => true,
    }
}
pub open spec fn put_reader(s: State, id: int, reader: Reader) -> State {
    State { readers: s.readers.insert(id,reader), ..s }
}
/// A cache hit is tied to the retained entry epoch, not the current epoch.
/// A miss loads the actual current immutable object; it never retags old data.
pub open spec fn capture_index(s: State, id: int) -> nat {
    let r = s.readers[id];
    if s.caches.contains_key(r.worker) && s.caches[r.worker].epoch == r.epoch {
        s.caches[r.worker].view
    } else { current_index(s) }
}
pub open spec fn apply(s: State, a: Action) -> State {
    match a {
        Action::Probe { id,worker } => put_reader(s,id,Reader {
            worker, epoch: s.epoch, announced: false, entered: false,
            view: None, finished: false, attempt: 0 }),
        Action::Announce { id } => put_reader(s,id,Reader {
            announced: true, ..s.readers[id] }),
        Action::Validate { id } => put_reader(s,id,Reader {
            entered: true, ..s.readers[id] }),
        Action::Resample { id } => put_reader(s,id,Reader {
            epoch: s.epoch, announced: false, view: None, ..s.readers[id] }),
        Action::Capture { id } => {
            let r = s.readers[id];
            let view = capture_index(s,id);
            State {
                caches: s.caches.insert(r.worker,Cache { epoch: r.epoch, view }),
                readers: s.readers.insert(id,Reader { view: Some(view), ..r }), ..s }
        },
        // The whole logical transaction, including all engine retries, owns one
        // pin. Advancing this epoch would let the barrier pass its old view.
        Action::Retry { id } => put_reader(s,id,Reader {
            epoch: s.readers[id].epoch,
            attempt: s.readers[id].attempt + 1, ..s.readers[id] }),
        Action::Exit { id } => put_reader(s,id,Reader {
            announced: false, finished: true, ..s.readers[id] }),
        Action::Publish { view } => State { history: s.history.push(view), ..s },
        Action::Tick => State {
            epoch: s.epoch + 1, floor: s.floor.insert(s.epoch + 1,current_index(s)), ..s },
        Action::Complete { cut } => State { completed: cut, ..s },
        Action::Stutter => s,
    }
}
pub open spec fn floor_inv(s: State) -> bool {
    s.epoch >= 1 && s.history.len() > 0 && s.completed <= s.epoch
    && s.floor.contains_key(1) && s.floor[1] == 0
    && (forall|e: nat| #[trigger] s.floor.contains_key(e) <==> 1 <= e <= s.epoch)
    && (forall|e: nat| #[trigger] s.floor.contains_key(e) ==> s.floor[e] < s.history.len())
    && (forall|e: nat, f: nat| s.floor.contains_key(e) && s.floor.contains_key(f) && e <= f
        ==> #[trigger] s.floor[e] <= #[trigger] s.floor[f])
}
pub open spec fn cache_inv(s: State, cache: Cache) -> bool {
    1 <= cache.epoch <= s.epoch && s.floor.contains_key(cache.epoch)
    && s.floor[cache.epoch] <= cache.view < s.history.len()
}
pub open spec fn reader_inv(s: State, reader: Reader) -> bool {
    1 <= reader.epoch <= s.epoch && s.floor.contains_key(reader.epoch)
    && (reader.finished ==> !reader.announced)
    && (!reader.entered ==> reader.view is None && reader.attempt == 0)
    && (reader.entered && !reader.finished ==> reader.announced && s.completed <= reader.epoch)
    && (reader.view is Some ==> reader.entered
        && s.floor[reader.epoch] <= reader.view.unwrap() < s.history.len())
}
pub open spec fn inv(s: State) -> bool {
    floor_inv(s)
    && (forall|worker: int| #[trigger] s.caches.contains_key(worker) ==> cache_inv(s,s.caches[worker]))
    && (forall|id: int| #[trigger] s.readers.contains_key(id) ==> reader_inv(s,s.readers[id]))
}
/// Grant checking always refers to the immutable captured object, never to a
/// newly sampled mutable authority or a certificate for an unrelated first key.
pub open spec fn authorized(s: State, id: int, key: Key, grant: Grant) -> bool {
    live(s,id) && s.readers[id].entered && s.readers[id].view is Some
    && s.history[s.readers[id].view.unwrap() as int].contains_key(key)
    && s.history[s.readers[id].view.unwrap() as int][key] == grant
}
pub open spec fn certified(s: State, id: int, footprint: View) -> bool {
    live(s,id) && s.readers[id].entered && s.readers[id].view is Some
    && (forall|key: Key| #[trigger] footprint.contains_key(key)
        ==> authorized(s,id,key,footprint[key]))
}
pub open spec fn interval(table: int, lo: int, hi: Option<int>) -> ISet<Key> {
    ISet::new(|key: Key| key.0 == table && lo <= key.1 && (hi is None || key.1 < hi.unwrap()))
}
pub open spec fn interval_footprint(routes: View, table: int, lo: int, hi: Option<int>) -> View {
    IMap::new(|key: Key| interval(table,lo,hi).contains(key), |key: Key| routes[key])
}
/// The initial object is carried by states[0]; the behavior imposes no finite
/// bound on the number of publications, epochs, readers, workers, or retries.
pub open spec fn behavior(states: Seq<State>, actions: Seq<Action>) -> bool {
    states.len() == actions.len() + 1
    && states[0].history.len() == 1 && states[0] == initial(states[0].history[0])
    && (forall|i: int| 0 <= i < actions.len() ==> enabled(#[trigger] states[i],actions[i])
        && states[i+1] == apply(states[i],actions[i]))
}

} // verus!
