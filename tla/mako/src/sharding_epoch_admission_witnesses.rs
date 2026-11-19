//! Enabled schedules for split enrollment and retained transaction lifetimes.
//! Negative schedules explicitly identify the one disabled/incorrect transition;
//! they are counterexamples to weakened algorithms, not asserted executions of
//! the correct model. No native-runtime behavior is assumed by these witnesses.
use vstd::prelude::*;
use super::*;
use super::View;
use super::proofs::*;

verus! {

pub open spec fn grant() -> Grant { Grant { owner: 0, version: 1 } }
pub open spec fn key() -> Key { (0,10,7) }
pub open spec fn serving() -> View { View::empty().insert(key(),grant()) }
pub open spec fn enrolled(s: State, id: int, worker: int) -> State {
    let a = apply(s,Action::Probe { id,worker });
    let b = apply(a,Action::Announce { id });
    let c = apply(b,Action::Validate { id });
    apply(c,Action::Capture { id })
}
pub proof fn advance(s: State, a: Action) -> (out: State)
    requires inv(s), enabled(s,a)
    ensures out == apply(s,a), inv(out)
{
    step_preserves(s,a);
    apply(s,a)
}
pub proof fn enroll(s: State, id: int, worker: int) -> (out: State)
    requires inv(s), !s.readers.contains_key(id)
    ensures out == enrolled(s,id,worker), inv(out),
        out.epoch == s.epoch, out.history == s.history, out.floor == s.floor,
        out.completed == s.completed,
        out.readers.contains_key(id), out.readers[id].entered,
        out.readers[id].announced, !out.readers[id].finished,
        out.readers[id].epoch == s.epoch, out.readers[id].view is Some,
        out.readers[id].worker == worker, out.readers[id].attempt == 0,
        out.caches.contains_key(worker), out.caches[worker].epoch == s.epoch,
        out.caches[worker].view == out.readers[id].view.unwrap(),
        out.readers.dom() == s.readers.dom().insert(id),
        forall|other: int| s.readers.contains_key(other) ==> out.readers[other] == s.readers[other]
{
    let a = advance(s,Action::Probe { id,worker });
    let b = advance(a,Action::Announce { id });
    let c = advance(b,Action::Validate { id });
    advance(c,Action::Capture { id })
}

/// A tick between sample and recheck forces resampling. Announcing the sampled
/// value is not itself successful entry, and does not acquire a cached view.
pub proof fn split_entry_rechecks_after_announcement()
{
    initial_safe(serving());
    let a = advance(initial(serving()),Action::Probe { id: 1,worker: 7 });
    let b = advance(a,Action::Publish { view: View::empty() });
    let c = advance(b,Action::Tick);
    let d = advance(c,Action::Announce { id: 1 });
    assert(d.readers[1].epoch == 1 && d.epoch == 2);
    assert(!enabled(d,Action::Validate { id: 1 }));
    assert(!enabled(d,Action::Capture { id: 1 }));
    old_announcement_blocks_completion(d,2,1);
    let e = advance(d,Action::Resample { id: 1 });
    assert(e.readers[1].epoch == 2 && !e.readers[1].announced);
    let f = advance(e,Action::Announce { id: 1 });
    let g = advance(f,Action::Validate { id: 1 });
    let h = advance(g,Action::Capture { id: 1 });
    assert(h.readers[1].view == Some(1nat));
    assert(!authorized(h,1,key(),grant()));
}

/// Freeze's publication and the engine tick are separate. A worker that has a
/// same-epoch cached Serving object can still enroll between them; it must be
/// included in the later drain, rather than spuriously rejected by this model.
pub proof fn publish_before_tick_reuses_cache()
{
    initial_safe(serving());
    let a = enroll(initial(serving()),1,7);
    let b = advance(a,Action::Exit { id: 1 });
    let c = advance(b,Action::Publish { view: View::empty() });
    let d = enroll(c,2,7);
    assert(d.epoch == 1 && current_index(d) == 1);
    assert(d.readers[2].view == Some(0nat));
    assert(authorized(d,2,key(),grant()));
    let e = advance(d,Action::Tick);
    assert(e.floor[2] == 1);
    old_announcement_blocks_completion(e,2,2);
    assert(authorized(e,2,key(),grant()));
    let f = advance(e,Action::Exit { id: 2 });
    let g = advance(f,Action::Complete { cut: 2 });
    assert(g.completed == 2);
}

/// Multiple periodic ticks do not change a captured transaction's object or
/// revoke its read/write admission. This is not a commit-time epoch check.
pub proof fn old_transaction_survives_periodic_ticks()
{
    initial_safe(serving());
    let a = enroll(initial(serving()),1,7);
    let b = advance(a,Action::Publish { view: View::empty() });
    background_preserves_authorization(b,Action::Tick,1,key(),grant());
    let c = advance(b,Action::Tick);
    let d = advance(c,Action::Tick);
    let e = advance(d,Action::Tick);
    assert(e.epoch == 4 && e.readers[1].epoch == 1);
    assert(e.readers[1].view == Some(0nat));
    assert(authorized(e,1,key(),grant()));
    old_announcement_blocks_completion(e,2,1);
    old_announcement_blocks_completion(e,4,1);
}

/// Capture may be delayed after successful entry. Loading a newer view does
/// not change the retained pin's epoch, and the old announcement still drains.
pub proof fn delayed_capture_retains_entry_epoch()
{
    initial_safe(serving());
    let a = advance(initial(serving()),Action::Probe { id: 1,worker: 7 });
    let b = advance(a,Action::Announce { id: 1 });
    let c = advance(b,Action::Validate { id: 1 });
    let d = advance(c,Action::Publish { view: View::empty() });
    let e = advance(d,Action::Tick);
    let f = advance(e,Action::Capture { id: 1 });
    assert(f.readers[1].epoch == 1 && f.readers[1].view == Some(1nat));
    assert(f.caches[7].epoch == 1 && f.caches[7].view == 1);
    old_announcement_blocks_completion(f,2,1);
}

pub proof fn retry_retains_old_pin_until_terminal_exit()
{
    initial_safe(serving());
    let a = enroll(initial(serving()),1,7);
    let b = advance(a,Action::Publish { view: View::empty() });
    let c = advance(b,Action::Tick);
    retry_retains_pin_and_view(c,1);
    let d = advance(c,Action::Retry { id: 1 });
    let e = advance(d,Action::Retry { id: 1 });
    assert(e.readers[1].epoch == 1 && e.readers[1].attempt == 2);
    assert(authorized(e,1,key(),grant()));
    old_announcement_blocks_completion(e,2,1);
    let f = advance(e,Action::Exit { id: 1 });
    let g = advance(f,Action::Complete { cut: 2 });
    assert(!authorized(g,1,key(),grant()));
}

/// A barrier can finish before an old unpublished sample announces. Requiring
/// every announcement >= completed would incorrectly rule out this execution.
pub proof fn late_old_announcement_after_completed_barrier()
{
    initial_safe(serving());
    let a = advance(initial(serving()),Action::Probe { id: 1,worker: 7 });
    let b = advance(a,Action::Publish { view: View::empty() });
    let c = advance(b,Action::Tick);
    let d = advance(c,Action::Complete { cut: 2 });
    let e = advance(d,Action::Announce { id: 1 });
    assert(e.completed == 2 && e.readers[1].epoch == 1 && e.readers[1].announced);
    assert(!e.readers[1].entered && inv(e));
    stale_announcement_cannot_enter(e,2,1);
    let f = advance(e,Action::Resample { id: 1 });
    let g = advance(f,Action::Announce { id: 1 });
    let h = advance(g,Action::Validate { id: 1 });
    let i = advance(h,Action::Capture { id: 1 });
    assert(i.readers[1].epoch == 2 && i.readers[1].view == Some(1nat));
    assert(!authorized(i,1,key(),grant()));
}

/// Idle cache entries need not be proactively invalidated. The old object is
/// rejected by the epoch-keyed cache lookup when that worker next enrolls.
pub proof fn idle_cache_refreshes_after_completed_cut()
{
    initial_safe(serving());
    let a = enroll(initial(serving()),1,7);
    let b = advance(a,Action::Exit { id: 1 });
    let c = advance(b,Action::Publish { view: View::empty() });
    let d = advance(c,Action::Tick);
    let e = advance(d,Action::Complete { cut: 2 });
    assert(e.caches[7].epoch == 1 && e.caches[7].view == 0);
    let f = advance(e,Action::Probe { id: 2,worker: 7 });
    let g = advance(f,Action::Announce { id: 2 });
    let h = advance(g,Action::Validate { id: 2 });
    post_cut_cache_is_refreshed(h,2,2);
    let i = advance(h,Action::Capture { id: 2 });
    assert(i.caches[7].epoch == 2 && i.caches[7].view == 1);
    assert(!authorized(i,2,key(),grant()));
    completed_cut_rejects_grant(i,2,2,key(),grant());
}

/// Negative control: skipping only Validate's equality recheck admits an old
/// cached Serving view after the barrier already completed. All prefix actions
/// are enabled; the forced Validate is explicitly disabled in the real model.
pub proof fn omitted_recheck_admits_late_old_cache()
{
    initial_safe(serving());
    let a = enroll(initial(serving()),1,7);
    let b = advance(a,Action::Exit { id: 1 });
    let c = advance(b,Action::Probe { id: 2,worker: 7 });
    let d = advance(c,Action::Publish { view: View::empty() });
    let e = advance(d,Action::Tick);
    let f = advance(e,Action::Complete { cut: 2 });
    let g = advance(f,Action::Announce { id: 2 });
    assert(!enabled(g,Action::Validate { id: 2 }));
    let bad_entry = apply(g,Action::Validate { id: 2 });
    assert(bad_entry.readers.contains_key(2));
    assert(!reader_inv(bad_entry,bad_entry.readers[2]));
    assert(!inv(bad_entry));
    assert(enabled(bad_entry,Action::Capture { id: 2 }));
    let bad_capture = apply(bad_entry,Action::Capture { id: 2 });
    assert(bad_capture.completed == 2 && bad_capture.floor[2] == 1);
    assert(bad_capture.readers[2].view == Some(0nat));
    assert(authorized(bad_capture,2,key(),grant()));
}

/// Completing the pre-publication epoch is a genuinely enabled operation, but
/// does NOT establish a post-publication grace period: floor[1] still points to
/// the old object. After Tick, omitting the drain would finish too early.
pub proof fn publication_or_tick_alone_is_not_completion()
{
    initial_safe(serving());
    let a = enroll(initial(serving()),1,7);
    let b = advance(a,Action::Publish { view: View::empty() });
    let c = advance(b,Action::Complete { cut: 1 });
    assert(c.floor[1] == 0 && c.completed == 1);
    assert(authorized(c,1,key(),grant()));
    let d = advance(c,Action::Tick);
    assert(d.floor[2] == 1);
    old_announcement_blocks_completion(d,2,1);
    let bad_completion = apply(d,Action::Complete { cut: 2 });
    assert(bad_completion.readers.contains_key(1));
    assert(!reader_inv(bad_completion,bad_completion.readers[1]));
    assert(bad_completion.completed == 2 && !inv(bad_completion));
    assert(authorized(bad_completion,1,key(),grant()));
}

/// A correct immutable-view certificate ranges over an unbounded ownership
/// interval and every physical alias, regardless of storage presence.
pub proof fn unbounded_scope_covers_every_alias(coordinate: int, row: int)
    requires 10 <= coordinate
{
    let routes = View::new(|k: Key| k.0 == 0 && 10 <= k.1, |k: Key| grant());
    initial_safe(routes);
    let a = enroll(initial(routes),1,7);
    let scope = interval_footprint(routes,0,10,None);
    assert forall|k: Key| scope.contains_key(k) implies authorized(a,1,k,scope[k]) by {
        assert(routes.contains_key(k));
    }
    assert(certified(a,1,scope));
    interval_certificate_covers_absence_and_aliases(a,1,routes,0,10,None,coordinate,row);
    assert(authorized(a,1,(0,coordinate,row),grant()));
}

/// Certifying one key cannot authorize a later key, a physical alias, or a
/// missing tail. The captured view must certify the whole requested footprint.
pub proof fn first_key_certificate_does_not_cover_alias_or_tail()
{
    initial_safe(serving());
    let a = enroll(initial(serving()),1,7);
    let alias: Key = (0,10,8);
    let tail: Key = (0,999,7);
    let footprint = serving().insert(alias,grant()).insert(tail,grant());
    assert(authorized(a,1,key(),grant()));
    assert(!authorized(a,1,alias,grant()));
    assert(!authorized(a,1,tail,grant()));
    assert(footprint.contains_key(alias));
    assert(!certified(a,1,footprint));
}

/// One existing engine slot summarizes nested/retained contexts. A newer
/// context on the same worker cannot make the old context disappear from drain.
pub proof fn newer_guard_does_not_hide_retained_worker_pin()
{
    initial_safe(serving());
    let a = enroll(initial(serving()),1,7);
    let b = advance(a,Action::Publish { view: View::empty() });
    let c = advance(b,Action::Tick);
    let d = enroll(c,2,7);
    assert(d.readers[1].epoch == 1 && d.readers[2].epoch == 2);
    retained_guard_bounds_worker_slot(d,1);
    assert(worker_slot(d,7) is Some && worker_slot(d,7).unwrap() < 2);
    worker_slots_refine_reader_drain(d,2);
    assert(!slots_drained(d,2));
    old_announcement_blocks_completion(d,2,1);
    let e = advance(d,Action::Exit { id: 1 });
    assert(drained(e,2));
    worker_slots_refine_reader_drain(e,2);
    assert(slots_drained(e,2));
    let f = advance(e,Action::Complete { cut: 2 });
    assert(!f.readers[2].finished && f.completed == 2);
}

} // verus!
