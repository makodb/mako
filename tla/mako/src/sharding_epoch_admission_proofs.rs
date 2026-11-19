use vstd::prelude::*;
use super::*;
use super::View;

verus! {

pub proof fn initial_safe(view: View)
    ensures inv(initial(view))
{
    let s = initial(view);
    assert forall|e: nat| s.floor.contains_key(e) <==> 1 <= e <= s.epoch by {}
    assert forall|e: nat, f: nat| s.floor.contains_key(e) && s.floor.contains_key(f) && e <= f
        implies s.floor[e] <= s.floor[f] by {}
}

pub proof fn replace_reader_preserves(s: State, id: int, reader: Reader)
    requires inv(s), reader_inv(s,reader)
    ensures inv(put_reader(s,id,reader))
{
    let out = put_reader(s,id,reader);
    assert forall|other: int| out.readers.contains_key(other)
        implies reader_inv(out,out.readers[other]) by {
        if other != id { assert(reader_inv(s,s.readers[other])); }
    }
}

pub proof fn probe_preserves(s: State, id: int, worker: int)
    requires inv(s), enabled(s,Action::Probe { id,worker })
    ensures inv(apply(s,Action::Probe { id,worker }))
{
    let out = apply(s,Action::Probe { id,worker });
    assert(reader_inv(s,out.readers[id]));
    replace_reader_preserves(s,id,out.readers[id]);
}

/// This induction step derives entry's completed-cut bound from the actual
/// announce/recheck protocol; that bound is not an action guard.
pub proof fn validate_preserves(s: State, id: int)
    requires inv(s), enabled(s,Action::Validate { id })
    ensures inv(apply(s,Action::Validate { id }))
{
    assert(reader_inv(s,s.readers[id]));
    let out = apply(s,Action::Validate { id });
    assert(s.completed <= s.readers[id].epoch);
    assert(reader_inv(s,out.readers[id]));
    replace_reader_preserves(s,id,out.readers[id]);
}

pub proof fn announce_preserves(s: State, id: int)
    requires inv(s), enabled(s,Action::Announce { id })
    ensures inv(apply(s,Action::Announce { id }))
{
    assert(reader_inv(s,s.readers[id]));
    replace_reader_preserves(s,id,apply(s,Action::Announce { id }).readers[id]);
}
pub proof fn resample_preserves(s: State, id: int)
    requires inv(s), enabled(s,Action::Resample { id })
    ensures inv(apply(s,Action::Resample { id }))
{
    assert(reader_inv(s,s.readers[id]));
    replace_reader_preserves(s,id,apply(s,Action::Resample { id }).readers[id]);
}

/// Provenance for either capture branch. In particular a cache miss must load
/// the current object, rather than retagging a stale object's publication ID.
pub proof fn capture_provenance(s: State, id: int)
    requires inv(s), enabled(s,Action::Capture { id })
    ensures s.floor[s.readers[id].epoch] <= capture_index(s,id) < s.history.len(),
        cache_inv(s,Cache { epoch: s.readers[id].epoch, view: capture_index(s,id) })
{
    let r = s.readers[id];
    assert(reader_inv(s,r));
    if s.caches.contains_key(r.worker) && s.caches[r.worker].epoch == r.epoch {
        assert(cache_inv(s,s.caches[r.worker]));
    } else {
        assert(s.floor[r.epoch] < s.history.len());
        assert(s.floor[r.epoch] <= current_index(s));
    }
}
pub proof fn capture_preserves(s: State, id: int)
    requires inv(s), enabled(s,Action::Capture { id })
    ensures inv(apply(s,Action::Capture { id }))
{
    capture_provenance(s,id);
    let r = s.readers[id];
    let out = apply(s,Action::Capture { id });
    assert(reader_inv(s,r));
    assert forall|worker: int| out.caches.contains_key(worker)
        implies cache_inv(out,out.caches[worker]) by {
        if worker != r.worker { assert(cache_inv(s,s.caches[worker])); }
    }
    assert forall|other: int| out.readers.contains_key(other)
        implies reader_inv(out,out.readers[other]) by {
        if other != id { assert(reader_inv(s,s.readers[other])); }
    }
}

pub proof fn retry_preserves(s: State, id: int)
    requires inv(s), enabled(s,Action::Retry { id })
    ensures inv(apply(s,Action::Retry { id }))
{
    assert(reader_inv(s,s.readers[id]));
    replace_reader_preserves(s,id,apply(s,Action::Retry { id }).readers[id]);
}
pub proof fn exit_preserves(s: State, id: int)
    requires inv(s), enabled(s,Action::Exit { id })
    ensures inv(apply(s,Action::Exit { id }))
{
    assert(reader_inv(s,s.readers[id]));
    replace_reader_preserves(s,id,apply(s,Action::Exit { id }).readers[id]);
}

pub proof fn publish_preserves(s: State, view: View)
    requires inv(s)
    ensures inv(apply(s,Action::Publish { view }))
{
    let out = apply(s,Action::Publish { view });
    assert forall|e: nat| out.floor.contains_key(e)
        implies out.floor[e] < out.history.len() by {
        assert(s.floor[e] < s.history.len());
    }
    assert(floor_inv(out));
    assert forall|worker: int| out.caches.contains_key(worker)
        implies cache_inv(out,out.caches[worker]) by {
        assert(cache_inv(s,s.caches[worker]));
    }
    assert forall|id: int| out.readers.contains_key(id)
        implies reader_inv(out,out.readers[id]) by {
        assert(reader_inv(s,s.readers[id]));
    }
}

pub proof fn tick_preserves(s: State)
    requires inv(s)
    ensures inv(apply(s,Action::Tick))
{
    let out = apply(s,Action::Tick);
    assert forall|e: nat| out.floor.contains_key(e) <==> 1 <= e <= out.epoch by {
        assert(s.floor.contains_key(e) <==> 1 <= e <= s.epoch);
    }
    assert forall|e: nat| out.floor.contains_key(e)
        implies out.floor[e] < out.history.len() by {
        if e != out.epoch { assert(s.floor[e] < s.history.len()); }
    }
    assert forall|e: nat, f: nat| out.floor.contains_key(e) && out.floor.contains_key(f) && e <= f
        implies out.floor[e] <= out.floor[f] by {
        if f != out.epoch {
            assert(e != out.epoch);
            assert(s.floor[e] <= s.floor[f]);
        } else if e != out.epoch {
            assert(s.floor[e] < s.history.len());
            assert(s.floor[e] <= current_index(s));
        }
    }
    assert(floor_inv(out));
    assert forall|worker: int| out.caches.contains_key(worker)
        implies cache_inv(out,out.caches[worker]) by {
        assert(cache_inv(s,s.caches[worker]));
        assert(s.caches[worker].epoch != out.epoch);
    }
    assert forall|id: int| out.readers.contains_key(id)
        implies reader_inv(out,out.readers[id]) by {
        assert(reader_inv(s,s.readers[id]));
        assert(s.readers[id].epoch != out.epoch);
    }
}

/// Completion needs the announcement drain, not just publication or a tick.
/// Old unpublished probes are intentionally not excluded by this invariant.
pub proof fn complete_preserves(s: State, cut: nat)
    requires inv(s), enabled(s,Action::Complete { cut })
    ensures inv(apply(s,Action::Complete { cut }))
{
    let out = apply(s,Action::Complete { cut });
    assert forall|id: int| out.readers.contains_key(id)
        implies reader_inv(out,out.readers[id]) by {
        let r = s.readers[id];
        assert(reader_inv(s,r));
        if r.entered && !r.finished {
            assert(r.announced);
            assert(cut <= r.epoch);
        }
    }
}

pub proof fn step_preserves(s: State, a: Action)
    requires inv(s), enabled(s,a)
    ensures inv(apply(s,a))
{
    match a {
        Action::Probe { id,worker } => probe_preserves(s,id,worker),
        Action::Announce { id } => announce_preserves(s,id),
        Action::Validate { id } => validate_preserves(s,id),
        Action::Resample { id } => resample_preserves(s,id),
        Action::Capture { id } => capture_preserves(s,id),
        Action::Retry { id } => retry_preserves(s,id),
        Action::Exit { id } => exit_preserves(s,id),
        Action::Publish { view } => publish_preserves(s,view),
        Action::Tick => tick_preserves(s),
        Action::Complete { cut } => complete_preserves(s,cut),
        Action::Stutter => {},
    }
}
pub proof fn invariant_at(states: Seq<State>, actions: Seq<Action>, i: int)
    requires behavior(states,actions), 0 <= i < states.len()
    ensures inv(states[i])
    decreases i
{
    if i == 0 { initial_safe(states[0].history[0]); }
    else {
        invariant_at(states,actions,i-1);
        assert(enabled(states[i-1],actions[i-1]));
        step_preserves(states[i-1],actions[i-1]);
    }
}
pub proof fn theorem_finite_behavior(states: Seq<State>, actions: Seq<Action>)
    requires behavior(states,actions)
    ensures forall|i: int| 0 <= i < states.len() ==> inv(states[i])
{
    assert forall|i: int| 0 <= i < states.len() implies inv(states[i]) by {
        invariant_at(states,actions,i);
    }
}

/// Every history entry and every already recorded floor is immutable. This is
/// also useful to a composed migration protocol tracking a Freeze publication.
pub proof fn step_preserves_provenance(s: State, a: Action)
    requires inv(s), enabled(s,a)
    ensures s.epoch <= apply(s,a).epoch, s.completed <= apply(s,a).completed,
        s.history.len() <= apply(s,a).history.len(),
        forall|i: int| 0 <= i < s.history.len() ==> apply(s,a).history[i] == s.history[i],
        forall|e: nat| s.floor.contains_key(e) ==> apply(s,a).floor.contains_key(e)
            && apply(s,a).floor[e] == s.floor[e]
{
    match a {
        Action::Publish { view } => {
            assert forall|i: int| 0 <= i < s.history.len()
                implies apply(s,a).history[i] == s.history[i] by {}
        },
        Action::Tick => {
            assert forall|e: nat| s.floor.contains_key(e)
                implies apply(s,a).floor.contains_key(e) && apply(s,a).floor[e] == s.floor[e] by {
                assert(e <= s.epoch);
            }
        },
        _ => {},
    }
}

pub proof fn completed_excludes_older_views(s: State, cut: nat, id: int)
    requires inv(s), 0 < cut <= s.completed, s.readers.contains_key(id),
        s.readers[id].entered && !s.readers[id].finished
    ensures s.readers[id].epoch >= cut,
        s.readers[id].view is Some ==> s.readers[id].view.unwrap() >= s.floor[cut]
{
    let r = s.readers[id];
    assert(reader_inv(s,r));
    assert(s.floor.contains_key(cut));
    assert(s.floor.contains_key(r.epoch));
    assert(s.floor[cut] <= s.floor[r.epoch]);
}

pub proof fn stale_announcement_cannot_enter(s: State, cut: nat, id: int)
    requires inv(s), 0 < cut <= s.completed, s.readers.contains_key(id),
        s.readers[id].epoch < cut
    ensures !enabled(s,Action::Validate { id })
{}

pub proof fn old_announcement_blocks_completion(s: State, cut: nat, id: int)
    requires s.readers.contains_key(id), s.readers[id].announced,
        !s.readers[id].finished, s.readers[id].epoch < cut
    ensures !enabled(s,Action::Complete { cut })
{
    assert(!drained(s,cut));
}

/// An idle worker's old cache may remain allocated indefinitely. It cannot be
/// reused by a newly entered post-cut ticket, and is refreshed on Capture.
pub proof fn post_cut_cache_is_refreshed(s: State, cut: nat, id: int)
    requires inv(s), 0 < cut <= s.completed, enabled(s,Action::Capture { id }),
        s.caches.contains_key(s.readers[id].worker),
        s.caches[s.readers[id].worker].epoch < cut
    ensures capture_index(s,id) == current_index(s),
        apply(s,Action::Capture { id }).caches[s.readers[id].worker].epoch == s.readers[id].epoch,
        apply(s,Action::Capture { id }).readers[id].view == Some(current_index(s))
{
    completed_excludes_older_views(s,cut,id);
}

/// Consumer-facing exclusion: when the immutable objects at/after a completed
/// cut deny a grant, no active reader can authorize that grant, even if a stale
/// cache or a late unpublished probe still exists in this process.
pub proof fn completed_cut_rejects_grant(s: State, cut: nat, id: int, key: Key, grant: Grant)
    requires inv(s), 0 < cut <= s.completed,
        forall|i: int| s.floor[cut] <= i < s.history.len() ==>
            !s.history[i].contains_key(key) || s.history[i][key] != grant
    ensures !authorized(s,id,key,grant)
{
    if authorized(s,id,key,grant) {
        completed_excludes_older_views(s,cut,id);
        assert(reader_inv(s,s.readers[id]));
        let i = s.readers[id].view.unwrap() as int;
        assert(s.floor[cut] <= i < s.history.len());
        assert(!s.history[i].contains_key(key) || s.history[i][key] != grant);
    }
}

pub proof fn retry_retains_pin_and_view(s: State, id: int)
    requires inv(s), enabled(s,Action::Retry { id })
    ensures apply(s,Action::Retry { id }).readers[id].epoch == s.readers[id].epoch,
        apply(s,Action::Retry { id }).readers[id].view == s.readers[id].view,
        apply(s,Action::Retry { id }).readers[id].announced,
        apply(s,Action::Retry { id }).readers[id].attempt == s.readers[id].attempt + 1
{
    assert(reader_inv(s,s.readers[id]));
}

/// Neither background ticks nor unrelated/new publications revoke an already
/// captured immutable view. Authorization remains a fact about that object.
pub proof fn background_preserves_authorization(s: State, a: Action, id: int, key: Key, grant: Grant)
    requires inv(s), authorized(s,id,key,grant), a is Tick || a is Publish
    ensures authorized(apply(s,a),id,key,grant),
        apply(s,a).readers[id] == s.readers[id]
{
    assert(reader_inv(s,s.readers[id]));
    match a {
        Action::Publish { view } => {
            assert(s.history[s.readers[id].view.unwrap() as int]
                == s.history.push(view)[s.readers[id].view.unwrap() as int]);
        },
        _ => {},
    }
}

/// Full immutable-view certification covers future discovery, not merely keys
/// that were present or operations already executed at admission time.
pub proof fn certificate_covers_future_footprint(s: State, id: int, scope: View, footprint: View)
    requires certified(s,id,scope), footprint.dom().subset_of(scope.dom()),
        forall|key: Key| footprint.contains_key(key) ==> footprint[key] == scope[key]
    ensures certified(s,id,footprint)
{
    assert forall|key: Key| footprint.contains_key(key)
        implies authorized(s,id,key,footprint[key]) by {
        assert(scope.contains_key(key));
        assert(authorized(s,id,key,scope[key]));
    }
}
pub proof fn interval_certificate_covers_absence_and_aliases(s: State, id: int,
    routes: View, table: int, lo: int, hi: Option<int>, coordinate: int, row: int)
    requires interval(table,lo,hi).subset_of(routes.dom()),
        certified(s,id,interval_footprint(routes,table,lo,hi)),
        lo <= coordinate, hi is None || coordinate < hi.unwrap()
    ensures authorized(s,id,(table,coordinate,row),routes[(table,coordinate,row)])
{
    let key = (table,coordinate,row);
    let footprint = interval_footprint(routes,table,lo,hi);
    assert(interval(table,lo,hi).contains(key));
    assert(footprint.contains_key(key));
    assert(authorized(s,id,key,footprint[key]));
}

pub proof fn minimum_epoch_characterization(s: State, ids: Set<int>)
    requires ids.subset_of(s.readers.dom())
    ensures minimum_epoch(s,ids) is None <==> ids.len() == 0,
        minimum_epoch(s,ids) is Some ==> (
            (exists|id: int| ids.contains(id)
                && minimum_epoch(s,ids).unwrap() == s.readers[id].epoch)
            && (forall|id: int| ids.contains(id)
                ==> minimum_epoch(s,ids).unwrap() <= s.readers[id].epoch))
    decreases ids.len()
{
    if ids.len() > 0 {
        let id = ids.choose();
        assert(ids.contains(id));
        let rest_ids = ids.remove(id);
        assert(rest_ids.subset_of(s.readers.dom()));
        minimum_epoch_characterization(s,rest_ids);
        let rest = minimum_epoch(s,rest_ids);
        if rest is Some && rest.unwrap() < s.readers[id].epoch {
            let witness = choose|other: int| rest_ids.contains(other)
                && rest.unwrap() == s.readers[other].epoch;
            assert(ids.contains(witness));
            assert(exists|other: int| ids.contains(other)
                && minimum_epoch(s,ids).unwrap() == s.readers[other].epoch);
        } else {
            assert(minimum_epoch(s,ids).unwrap() == s.readers[id].epoch);
            assert(exists|other: int| ids.contains(other)
                && minimum_epoch(s,ids).unwrap() == s.readers[other].epoch);
        }
        assert forall|other: int| ids.contains(other)
            implies minimum_epoch(s,ids).unwrap() <= s.readers[other].epoch by {
            if other != id {
                assert(rest_ids.contains(other));
                assert(rest_ids.len() > 0 && rest is Some);
                assert(rest.unwrap() <= s.readers[other].epoch);
            }
        }
    }
}

/// Scanning one minimum announcement per worker is exactly the logical drain.
/// A retry or another nested guard must not overwrite an older retained pin.
pub proof fn worker_slots_refine_reader_drain(s: State, cut: nat)
    ensures slots_drained(s,cut) <==> drained(s,cut)
{
    if drained(s,cut) {
        assert forall|worker: int| worker_slot(s,worker) is Some
            implies cut <= worker_slot(s,worker).unwrap() by {
            let ids = worker_readers(s,worker);
            assert(ids.subset_of(s.readers.dom()));
            minimum_epoch_characterization(s,ids);
            let id = choose|id: int| ids.contains(id)
                && worker_slot(s,worker).unwrap() == s.readers[id].epoch;
            assert(s.readers.contains_key(id));
            assert(s.readers[id].announced && !s.readers[id].finished);
            assert(cut <= s.readers[id].epoch);
        }
    }
    if slots_drained(s,cut) {
        assert forall|id: int| s.readers.contains_key(id)
            && s.readers[id].announced && !s.readers[id].finished
            implies cut <= s.readers[id].epoch by {
            let worker = s.readers[id].worker;
            let ids = worker_readers(s,worker);
            assert(ids.subset_of(s.readers.dom()) && ids.contains(id));
            assert(ids.len() > 0);
            minimum_epoch_characterization(s,ids);
            assert(worker_slot(s,worker) is Some);
            assert(cut <= worker_slot(s,worker).unwrap());
            assert(worker_slot(s,worker).unwrap() <= s.readers[id].epoch);
        }
    }
}

pub proof fn retained_guard_bounds_worker_slot(s: State, id: int)
    requires s.readers.contains_key(id), s.readers[id].announced,
        !s.readers[id].finished
    ensures worker_slot(s,s.readers[id].worker) is Some,
        worker_slot(s,s.readers[id].worker).unwrap() <= s.readers[id].epoch
{
    let ids = worker_readers(s,s.readers[id].worker);
    assert(ids.subset_of(s.readers.dom()) && ids.contains(id));
    assert(ids.len() > 0);
    minimum_epoch_characterization(s,ids);
}

} // verus!
