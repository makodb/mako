//! Finite-prefix algebra and consequences of the producer certificate invariant.
use super::types::*;
use super::log_invariants::*;
use super::invariants::inv;
use vstd::prelude::*;

verus! {

pub proof fn lemma_push(es: Seq<Entry>, e: Entry, ep: nat)
    ensures frontier(es.push(e), ep) == imax(frontier(es, ep), marker_value(e, ep)),
        clock_floor(es.push(e)) == imax(clock_floor(es), entry_clock(e)),
        config_epoch(es.push(e)) == if e is AdvanceSpecEpoch {
            if e->AdvanceSpecEpoch_epoch > config_epoch(es) { e->AdvanceSpecEpoch_epoch } else { config_epoch(es) }
        } else { config_epoch(es) },
{
    assert(es.push(e).drop_last() =~= es);
}
pub proof fn lemma_bounds(es: Seq<Entry>, ep: nat)
    ensures frontier(es, ep) >= 0, clock_floor(es) >= 0,
        frontier(es, ep) <= clock_floor(es),
        forall|j: int| 0 <= j < es.len() ==> marker_value(#[trigger] es[j], ep) <= frontier(es, ep)
            && entry_clock(es[j]) <= clock_floor(es),
    decreases es.len(),
{
    if es.len() > 0 {
        lemma_bounds(es.drop_last(), ep);
        assert forall|j: int| 0 <= j < es.len() implies
            marker_value(#[trigger] es[j], ep) <= frontier(es, ep)
                && entry_clock(es[j]) <= clock_floor(es) by {
            if j < es.len() - 1 { assert(es[j] == es.drop_last()[j]); }
        }
    }
}
pub proof fn lemma_prefix(es: Seq<Entry>, n: int, ep: nat)
    requires 0 <= n <= es.len(),
    ensures frontier(es.take(n), ep) <= frontier(es, ep),
        clock_floor(es.take(n)) <= clock_floor(es),
        config_epoch(es.take(n)) <= config_epoch(es),
    decreases es.len(),
{
    if n < es.len() {
        lemma_prefix(es.drop_last(), n, ep);
        assert(es.drop_last().take(n) =~= es.take(n));
    } else { assert(es.take(n) =~= es); }
}
pub proof fn lemma_prefix_records(a: Seq<Entry>, b: Seq<Entry>, id: int, ep: nat)
    requires a.is_prefix_of(b),
    ensures has_log(a, id) ==> has_log(b, id), has_close(a, ep) ==> has_close(b, ep),
{
    if has_log(a, id) {
        let j = choose|j: int| 0 <= j < a.len() && is_tx(#[trigger] a[j], id);
        assert(a[j] == b[j]);
    }
    if has_close(a, ep) {
        let j = choose|j: int| 0 <= j < a.len() && #[trigger] a[j] is Close && a[j]->Close_epoch == ep;
        assert(a[j] == b[j]);
    }
}
pub proof fn lemma_concat(a: Seq<Entry>, b: Seq<Entry>, ep: nat)
    ensures frontier(a, ep) <= frontier(a + b, ep), clock_floor(a) <= clock_floor(a + b),
        config_epoch(a) <= config_epoch(a + b),
{
    assert((a + b).take(a.len() as int) =~= a);
    lemma_prefix(a + b, a.len() as int, ep);
}
pub proof fn lemma_closed_exact(s: State, c: Constants, i: int, ep: nat)
    requires log_inv(s, c), is_shard(c, i), has_close(s.logs[i].durable, ep),
    ensures frontier(s.logs[i].durable, ep) == frontier(entries(s.logs[i]), ep),
        ep < s.shards[i].epoch,
{
    let d = s.logs[i].durable;
    let es = entries(s.logs[i]);
    let j = choose|j: int| 0 <= j < d.len() && #[trigger] d[j] is Close && d[j]->Close_epoch == ep;
    assert(es[j] == d[j]);
    lemma_bounds(d, ep);
    lemma_concat(d, s.logs[i].pending, ep);
}
pub proof fn lemma_stable_durable(s: State, c: Constants, id: int, i: int)
    requires inv(s, c), s.txns.dom().contains(id), prepared(s.txns[id]),
        stable(s.logs, c, s.txns[id]), is_shard(c, i), log_participant(c, s.txns[id], i),
    ensures has_log(s.logs[i].durable, id), has_log(entries(s.logs[i]), id),
{
    lemma_concat(s.logs[i].durable, s.logs[i].pending, s.txns[id].epoch);
    assert(s.logs[i].durable.is_prefix_of(entries(s.logs[i])));
    lemma_prefix_records(s.logs[i].durable, entries(s.logs[i]), id, s.txns[id].epoch);
}
pub proof fn lemma_stable_certified(s: State, c: Constants, id: int)
    requires inv(s, c), s.txns.dom().contains(id), prepared(s.txns[id]), stable(s.logs, c, s.txns[id]),
    ensures certified(s.txns[id]), all_installed(c, s.txns[id]), !doomed(s, c, s.txns[id]),
{
    let i = s.txns[id].coord;
    lemma_stable_durable(s, c, id, i);
    let es = entries(s.logs[i]);
    let j = choose|j: int| 0 <= j < es.len() && is_tx(#[trigger] es[j], id);
    assert(entry_valid(s, c, i, es[j]));
}
pub proof fn lemma_lost_not_stable(s: State, c: Constants, id: int)
    requires inv(s, c), s.txns.dom().contains(id), prepared(s.txns[id]), lost(s.logs, c, s.txns[id], id),
    ensures !stable(s.logs, c, s.txns[id]),
{
    if stable(s.logs, c, s.txns[id]) {
        let i = choose|i: int| is_shard(c, i) && log_participant(c, s.txns[id], i)
            && !has_log(entries(#[trigger] s.logs[i]), id);
        lemma_stable_durable(s, c, id, i);
    }
}
pub proof fn lemma_view_sound(s: State, c: Constants, observer: int, ep: nat, i: int)
    requires log_inv(s, c), is_shard(c, i),
    ensures 0 <= view(s.views, observer, ep, i).through <= frontier(s.logs[i].durable, ep),
        view(s.views, observer, ep, i).closed ==> has_close(s.logs[i].durable, ep)
            && view(s.views, observer, ep, i).through == frontier(s.logs[i].durable, ep),
{
    lemma_bounds(s.logs[i].durable, ep);
}
pub proof fn lemma_view_stable(s: State, c: Constants, id: int, observer: int)
    requires inv(s, c), s.txns.dom().contains(id),
        below_view(s.views, c, observer, s.txns[id].epoch, s.txns[id].ts),
    ensures stable(s.logs, c, s.txns[id]),
{
    assert forall|i: int| is_shard(c, i) implies
        s.txns[id].ts <= frontier(#[trigger] s.logs[i].durable, s.txns[id].epoch) by {
        lemma_view_sound(s, c, observer, s.txns[id].epoch, i);
    }
}
pub proof fn lemma_final_view_equivalence(s: State, c: Constants, id: int, observer: int)
    requires inv(s, c), s.txns.dom().contains(id), final_ready(s.views, c, observer, s.txns[id].epoch),
    ensures below_view(s.views, c, observer, s.txns[id].epoch, s.txns[id].ts)
        <==> stable(s.logs, c, s.txns[id]),
{
    if below_view(s.views, c, observer, s.txns[id].epoch, s.txns[id].ts) {
        lemma_view_stable(s, c, id, observer);
    }
    if stable(s.logs, c, s.txns[id]) {
        assert forall|i: int| is_shard(c, i) implies
            s.txns[id].ts <= #[trigger] view(s.views, observer, s.txns[id].epoch, i).through by {
            lemma_view_sound(s, c, observer, s.txns[id].epoch, i);
        }
    }
}

pub proof fn lemma_clock_ceiling(es: Seq<Entry>, bound: int)
    requires bound >= 0, forall|j: int| 0 <= j < es.len() ==> entry_clock(#[trigger] es[j]) <= bound,
    ensures clock_floor(es) <= bound,
    decreases es.len(),
{
    if es.len() > 0 {
        assert forall|j: int| 0 <= j < es.drop_last().len() implies
            entry_clock(#[trigger] es.drop_last()[j]) <= bound by {
            assert(es.drop_last()[j] == es[j]);
        }
        lemma_clock_ceiling(es.drop_last(), bound);
    }
}

} // verus!
