//! Non-atomic publication for one already registered MAP table, at integrated
//! source 44c6b5d5c278a2916a3112cf2fa4d6d7c2a4691f:
//! config_manager.h:376-431,1106-1161; config_watcher.h:74-91;
//! cluster_config.cc:180-222. Unrelated stable fields/table registration are omitted.
//! One writer has completed the helper's reads of the old partition; its planned
//! output is the actual transformation in sharding_partition, not an ideal oracle.
//! Each KV get/put is a separate action. The writer has NO mutex with the loader;
//! count then version are written LAST. The loader samples version, count, starts
//! and owners separately, never rechecks version, and atomically installs only its
//! local cache. Successful point reads suffice for the counterexample: no failure,
//! corruption, stale RPC response, second writer or process crash is required.
//! EarlierVersion abstracts a completed change to an omitted unrelated field,
//! making the watcher one version behind while the partition remains coherent.
//! This is a source-derived operational abstraction, not a C++ refinement proof.
use super::sharding_partition::*;
use vstd::prelude::*;

verus! {

pub struct Publication {
    pub old: Seq<Segment>,
    pub lo: int,
    pub hi: Option<int>,
    pub dest: int,
}
pub open spec fn planned(c: Publication) -> Seq<Segment> {
    reassign(c.old, c.lo, c.hi, c.dest)
}
pub enum LoaderPhase { Idle, Version, Count, Start, Owner, Install }
pub struct State {
    pub starts: Map<int, int>,
    pub owners: Map<int, int>,
    pub count: nat,
    pub version: nat,
    /// 2*i=start i, 2*i+1=owner i, 2*n=count, 2*n+1=version, 2*n+2=done.
    pub writer_cursor: nat,
    pub phase: LoaderPhase,
    pub observed_version: nat,
    pub loading_version: nat,
    pub loading_count: nat,
    pub loading_cursor: nat,
    pub pending_start: int,
    pub loading: Seq<Segment>,
    pub cache: Seq<Segment>,
    pub cache_version: nat,
    pub last_version: nat,
}
pub enum Action { EarlierVersion, WriteField, Poll, LoadVersion, LoadCount, LoadStart, LoadOwner, Install }

pub open spec fn start_fields(p: Seq<Segment>) -> Map<int, int>
    decreases p.len(),
{
    if p.len() == 0 { Map::empty() }
    else { start_fields(p.drop_last()).insert(p.len() as int - 1, p.last().start) }
}
pub open spec fn owner_fields(p: Seq<Segment>) -> Map<int, int>
    decreases p.len(),
{
    if p.len() == 0 { Map::empty() }
    else { owner_fields(p.drop_last()).insert(p.len() as int - 1, p.last().shard) }
}
pub open spec fn get_field(m: Map<int, int>, i: int) -> int {
    if m.dom().contains(i) { m[i] } else { 0 }
}
pub open spec fn initial(c: Publication) -> State {
    State {
        starts: start_fields(c.old), owners: owner_fields(c.old),
        count: c.old.len(), version: 0, writer_cursor: 0,
        phase: LoaderPhase::Idle, observed_version: 0, loading_version: 0,
        loading_count: 0, loading_cursor: 0, pending_start: 0, loading: Seq::empty(),
        cache: c.old, cache_version: 0, last_version: 0,
    }
}
pub open spec fn init(s: State, c: Publication) -> bool {
    s == initial(c)
}
pub open spec fn enabled(s: State, c: Publication, a: Action) -> bool {
    match a {
        Action::EarlierVersion => s.version == 0 && s.writer_cursor == 0 && s.phase == LoaderPhase::Idle,
        Action::WriteField => s.writer_cursor <= 2 * planned(c).len() + 1,
        Action::Poll => s.phase == LoaderPhase::Idle,
        Action::LoadVersion => s.phase == LoaderPhase::Version,
        Action::LoadCount => s.phase == LoaderPhase::Count,
        Action::LoadStart => s.phase == LoaderPhase::Start && s.loading_cursor < s.loading_count,
        Action::LoadOwner => s.phase == LoaderPhase::Owner && s.loading_cursor < s.loading_count,
        Action::Install => s.phase == LoaderPhase::Install,
    }
}
pub open spec fn apply(s: State, c: Publication, a: Action) -> State {
    match a {
        Action::EarlierVersion => State { version: 1, ..s },
        Action::WriteField => {
            let p = planned(c);
            let j = s.writer_cursor;
            if j < 2 * p.len() {
                let i = (j / 2) as int;
                if j % 2 == 0 {
                    State { starts: s.starts.insert(i, p[i].start), writer_cursor: j + 1, ..s }
                } else {
                    State { owners: s.owners.insert(i, p[i].shard), writer_cursor: j + 1, ..s }
                }
            } else if j == 2 * p.len() {
                State { count: p.len(), writer_cursor: j + 1, ..s }
            } else {
                State { version: s.version + 1, writer_cursor: j + 1, ..s }
            }
        },
        Action::Poll => if s.version == s.last_version { s } else {
            State { observed_version: s.version, phase: LoaderPhase::Version, ..s }
        },
        Action::LoadVersion => State {
            loading_version: s.version, phase: LoaderPhase::Count, ..s
        },
        Action::LoadCount => State {
            loading_count: s.count, loading_cursor: 0, loading: Seq::empty(),
            phase: if s.count == 0 { LoaderPhase::Install } else { LoaderPhase::Start }, ..s
        },
        Action::LoadStart => State {
            pending_start: get_field(s.starts, s.loading_cursor as int), phase: LoaderPhase::Owner, ..s
        },
        Action::LoadOwner => State {
            loading: s.loading.push(Segment {
                start: s.pending_start, shard: get_field(s.owners, s.loading_cursor as int),
            }),
            loading_cursor: s.loading_cursor + 1,
            phase: if s.loading_cursor + 1 == s.loading_count {
                LoaderPhase::Install
            } else { LoaderPhase::Start }, ..s
        },
        // Local cache lock makes this install atomic locally, not the preceding KV reads.
        // Watcher bookkeeping uses Poll's version, not LoadVersion's version.
        Action::Install => State {
            cache: s.loading, cache_version: s.loading_version,
            last_version: s.observed_version, phase: LoaderPhase::Idle, ..s
        },
    }
}
pub open spec fn next(s: State, z: State, c: Publication) -> bool {
    exists|a: Action| #[trigger] enabled(s, c, a) && z == apply(s, c, a)
}
pub open spec fn behavior(b: Seq<State>, c: Publication) -> bool {
    b.len() > 0 && init(b[0], c)
        && forall|i: int| 0 <= i < b.len() - 1 ==> #[trigger] next(b[i], b[i + 1], c)
}
/// A publication-safety claim falsified below, even though the cache is wellformed.
pub open spec fn snapshot_is_old_or_new(s: State, c: Publication) -> bool {
    s.cache == c.old || s.cache == planned(c)
}
pub open spec fn outside_routes_unchanged(s: State, c: Publication) -> bool {
    forall|k: int| k >= 0 && !in_range(k, c.lo, c.hi) ==>
        #[trigger] route(s.cache, k) == route(c.old, k)
}

pub open spec fn torn_input() -> Publication {
    Publication {
        old: seq![Segment { start: 0, shard: 0 }, Segment { start: 10, shard: 1 },
            Segment { start: 20, shard: 0 }],
        lo: 1, hi: Some(2), dest: 2,
    }
}
pub open spec fn torn_target() -> Seq<Segment> {
    seq![Segment { start: 0, shard: 0 }, Segment { start: 1, shard: 2 },
        Segment { start: 2, shard: 0 }, Segment { start: 10, shard: 1 },
        Segment { start: 20, shard: 0 }]
}
pub open spec fn torn_cache() -> Seq<Segment> {
    seq![Segment { start: 0, shard: 0 }, Segment { start: 1, shard: 2 },
        Segment { start: 2, shard: 0 }]
}
/// Explicit action-driven finite execution, not an assumed malformed state.
pub open spec fn torn_trace() -> Seq<State> {
    let c = torn_input();
    let s0 = initial(c);
    let s1 = apply(s0, c, Action::EarlierVersion);
    let s2 = apply(s1, c, Action::WriteField);
    let s3 = apply(s2, c, Action::WriteField);
    let s4 = apply(s3, c, Action::WriteField);
    let s5 = apply(s4, c, Action::WriteField);
    let s6 = apply(s5, c, Action::WriteField);
    let s7 = apply(s6, c, Action::WriteField);
    let s8 = apply(s7, c, Action::Poll);
    let s9 = apply(s8, c, Action::LoadVersion);
    let s10 = apply(s9, c, Action::LoadCount);
    let s11 = apply(s10, c, Action::LoadStart);
    let s12 = apply(s11, c, Action::LoadOwner);
    let s13 = apply(s12, c, Action::LoadStart);
    let s14 = apply(s13, c, Action::LoadOwner);
    let s15 = apply(s14, c, Action::LoadStart);
    let s16 = apply(s15, c, Action::LoadOwner);
    let s17 = apply(s16, c, Action::Install);
    seq![s0, s1, s2, s3, s4, s5, s6, s7, s8, s9, s10, s11, s12, s13, s14, s15, s16, s17]
}

/// Checks that the input is a legitimate source-0 migration, and that the
/// publisher's five segments are the output of the proved metadata algorithm.
pub proof fn torn_input_is_valid()
    ensures wellformed(torn_input().old), valid_range(torn_input().lo, torn_input().hi),
        planned(torn_input()) == torn_target(), wellformed(torn_target()),
        forall|k: int| in_range(k, 1, Some(2)) ==> #[trigger] route(torn_input().old, k) == 0,
        route(torn_input().old, 15) == 1, route(torn_target(), 15) == 1,
{
    reveal_with_fuel(insert_boundary, 8);
    reveal_with_fuel(coalesce, 8);
    reveal_with_fuel(route, 8);
    let p = torn_input().old;
    assert(wellformed(p));
    let m = materialize(p, 1, Some(2));
    assert(m =~= seq![Segment { start: 0, shard: 0 }, Segment { start: 1, shard: 0 },
        Segment { start: 2, shard: 0 }, Segment { start: 10, shard: 1 }, Segment { start: 20, shard: 0 }]);
    assert(remap(m, 1, Some(2), 2) =~= torn_target());
    assert(coalesce(torn_target()) =~= torn_target());
    reassign_partition_theorem(p, 1, Some(2), 2, 15);
}

proof fn step_is_next(s: State, c: Publication, a: Action)
    requires enabled(s, c, a),
    ensures next(s, apply(s, c, a), c),
{
    assert(exists|b: Action| #[trigger] enabled(s, c, b) && apply(s, c, a) == apply(s, c, b));
}

pub proof fn torn_trace_is_behavior()
    ensures behavior(torn_trace(), torn_input()),
{
    torn_input_is_valid();
    let c = torn_input();
    let b = torn_trace();
    step_is_next(b[0], c, Action::EarlierVersion);
    step_is_next(b[1], c, Action::WriteField);
    step_is_next(b[2], c, Action::WriteField);
    step_is_next(b[3], c, Action::WriteField);
    step_is_next(b[4], c, Action::WriteField);
    step_is_next(b[5], c, Action::WriteField);
    step_is_next(b[6], c, Action::WriteField);
    step_is_next(b[7], c, Action::Poll);
    step_is_next(b[8], c, Action::LoadVersion);
    step_is_next(b[9], c, Action::LoadCount);
    step_is_next(b[10], c, Action::LoadStart);
    step_is_next(b[11], c, Action::LoadOwner);
    step_is_next(b[12], c, Action::LoadStart);
    step_is_next(b[13], c, Action::LoadOwner);
    step_is_next(b[14], c, Action::LoadStart);
    step_is_next(b[15], c, Action::LoadOwner);
    step_is_next(b[16], c, Action::Install);
    assert forall|i: int| 0 <= i < b.len() - 1 implies #[trigger] next(b[i], b[i + 1], c) by {
        if i == 0 {} else if i == 1 {} else if i == 2 {} else if i == 3 {}
        else if i == 4 {} else if i == 5 {} else if i == 6 {} else if i == 7 {}
        else if i == 8 {} else if i == 9 {} else if i == 10 {} else if i == 11 {}
        else if i == 12 {} else if i == 13 {} else if i == 14 {} else if i == 15 {} else {}
    }
}

/// A fully enabled 17-step execution from coherent initialization installs a
/// wellformed but neither-old-nor-new snapshot. Key 15 is OUTSIDE the moved [1,2)
/// interval: both complete partitions route to shard 1, while this cache routes 0.
/// Writer is paused after six field writes, before count and version publication.
/// This refutes concurrent snapshot and outside-range routing safety, not merely
/// a structural wellformedness claim. No data-migration claim is assumed.
pub proof fn torn_snapshot_counterexample()
    ensures
        behavior(torn_trace(), torn_input()),
        wellformed(torn_input().old), wellformed(planned(torn_input())),
        planned(torn_input()) == torn_target(),
        torn_trace().last().cache == torn_cache(),
        wellformed(torn_trace().last().cache),
        !snapshot_is_old_or_new(torn_trace().last(), torn_input()),
        !outside_routes_unchanged(torn_trace().last(), torn_input()),
        !in_range(15, torn_input().lo, torn_input().hi),
        route(torn_input().old, 15) == 1,
        route(planned(torn_input()), 15) == 1,
        route(torn_trace().last().cache, 15) == 0,
        torn_trace().last().writer_cursor == 6,
        torn_trace().last().count == 3,
        torn_trace().last().version == 1,
        torn_trace().last().cache_version == 1,
        torn_trace().last().last_version == 1,
{
    torn_input_is_valid();
    torn_trace_is_behavior();
    reveal_with_fuel(start_fields, 4);
    reveal_with_fuel(owner_fields, 4);
    reveal_with_fuel(route, 8);
    let z = torn_trace().last();
    assert(z.cache =~= torn_cache());
    assert(wellformed(z.cache));
    assert(z.cache[1] != torn_input().old[1]);
    assert(route(z.cache, 15) == 0);
}

} // verus!
