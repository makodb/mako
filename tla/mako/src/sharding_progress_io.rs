//! Raw-store progress for the ordered two-lookahead algorithm in
//! src/cluster/storage.rs::mirror/cleanup. Keys are dense ORDER labels of the
//! finite selected source/destination union, not hashes. Successful scan is the
//! exact first row at/after the cursor; put/delete have the native one-key frame.
//! A raw error restarts at the beginning with the partially modified image.
//! Thus infinitely-often successes are deliberately insufficient: after a finite
//! stabilization time all required raw returns succeed, and each pending raw
//! request/local continuation eventually receives service.
use super::*;

verus! {

pub enum Kind { Mirror, Cleanup, Local }
pub enum Stage { ScanSource, ScanDestination, Choose, Put, Delete, Seal, Emit, Done }
pub enum Tick { Wait, Ok, Error }
pub enum Request {
    SourceScan { from: nat }, DestinationScan { from: nat },
    Put { key: nat, value: Seq<u8> }, Delete { key: nat }, Continue { stage: Stage, cursor: nat },
}

pub struct Driver {
    pub kind: Kind,
    pub bound: nat,
    pub source: Map<nat,Seq<u8>>,
    pub image: Map<nat,Seq<u8>>,
    pub cursor: nat,
    pub src: Option<nat>,
    pub dst: Option<nat>,
    pub refresh_destination: bool,
    pub stage: Stage,
}

pub open spec fn bounded(image: Map<nat,Seq<u8>>, bound: nat) -> bool {
    forall|k: nat| image.contains_key(k) ==> k < bound
}
pub open spec fn value(image: Map<nat,Seq<u8>>, k: nat) -> Option<Seq<u8>> {
    if image.contains_key(k) { Some(image[k]) } else { None }
}
pub open spec fn first(image: Map<nat,Seq<u8>>, from: nat, row: Option<nat>) -> bool {
    match row {
        None => forall|k: nat| k >= from ==> !image.contains_key(k),
        Some(k) => k >= from && image.contains_key(k)
            && forall|j: nat| from <= j < k ==> !image.contains_key(j),
    }
}
pub open spec fn scan(image: Map<nat,Seq<u8>>, from: nat, bound: nat) -> Option<nat>
    decreases bound-from
{
    if from >= bound { None }
    else if image.contains_key(from) { Some(from) }
    else { scan(image,from+1,bound) }
}
pub proof fn scan_contract(image: Map<nat,Seq<u8>>, from: nat, bound: nat)
    requires bounded(image,bound)
    ensures first(image,from,scan(image,from,bound))
    decreases bound-from
{
    if from < bound && !image.contains_key(from) { scan_contract(image,from+1,bound); }
}

pub open spec fn choose_source(s: Driver) -> bool {
    s.src is Some && (s.dst is None || s.src.unwrap() <= s.dst.unwrap())
}
pub open spec fn prefix(s: Driver) -> bool {
    forall|k: nat| k < s.cursor ==> value(s.image,k) == value(s.source,k)
}
pub open spec fn wf(s: Driver) -> bool {
    &&& s.cursor <= s.bound && bounded(s.source,s.bound) && bounded(s.image,s.bound)
    &&& (s.kind is Cleanup ==> s.source == Map::<nat,Seq<u8>>::empty())
    &&& (s.kind is Local ==> s.stage is Seal || s.stage is Emit || s.stage is Done)
    &&& prefix(s)
    &&& match s.stage {
        Stage::ScanSource => s.kind is Mirror
            && (!s.refresh_destination ==> first(s.image,s.cursor,s.dst)),
        Stage::ScanDestination => !(s.kind is Local) && first(s.source,s.cursor,s.src),
        Stage::Choose => first(s.source,s.cursor,s.src) && first(s.image,s.cursor,s.dst),
        Stage::Put => first(s.source,s.cursor,s.src) && first(s.image,s.cursor,s.dst) && choose_source(s),
        Stage::Delete => first(s.source,s.cursor,s.src) && first(s.image,s.cursor,s.dst)
            && !choose_source(s) && s.dst is Some,
        Stage::Seal | Stage::Emit | Stage::Done => s.image == s.source,
    }
}

/// The caller supplies an already checked immutable GuardedStore window.
/// Authorization is a sharding proof obligation, not eventual I/O success.
pub open spec fn initial(kind: Kind, source: Map<nat,Seq<u8>>, image: Map<nat,Seq<u8>>, bound: nat) -> Driver {
    Driver { kind,source,image,bound,cursor:0,src:None,dst:None,refresh_destination:true,
        stage: if kind is Mirror { Stage::ScanSource }
            else if kind is Cleanup { Stage::ScanDestination } else { Stage::Seal } }
}
pub proof fn initial_wf(kind: Kind, source: Map<nat,Seq<u8>>, image: Map<nat,Seq<u8>>, bound: nat)
    requires bounded(source,bound), bounded(image,bound),
        kind is Cleanup ==> source == Map::<nat,Seq<u8>>::empty(), kind is Local ==> image == source
    ensures wf(initial(kind,source,image,bound))
{ }

pub open spec fn raw(stage: Stage) -> bool {
    stage is ScanSource || stage is ScanDestination || stage is Put || stage is Delete
}
pub open spec fn request(s: Driver) -> Request {
    match s.stage {
        Stage::ScanSource => Request::SourceScan { from:s.cursor },
        Stage::ScanDestination => Request::DestinationScan { from:s.cursor },
        Stage::Put => Request::Put { key:s.src.unwrap(),value:s.source[s.src.unwrap()] },
        Stage::Delete => Request::Delete { key:s.dst.unwrap() },
        _ => Request::Continue { stage:s.stage,cursor:s.cursor },
    }
}

pub open spec fn advance(s: Driver) -> Driver {
    match s.stage {
        Stage::ScanSource => Driver { src:scan(s.source,s.cursor,s.bound),
            stage:if s.refresh_destination { Stage::ScanDestination } else { Stage::Choose }, ..s },
        Stage::ScanDestination => Driver { dst:scan(s.image,s.cursor,s.bound),stage:Stage::Choose,..s },
        Stage::Choose => Driver { stage:if s.src is None && s.dst is None { Stage::Seal }
            else if choose_source(s) { Stage::Put } else { Stage::Delete },..s },
        Stage::Put => { let k = s.src.unwrap(); Driver {
            image:s.image.insert(k,s.source[k]),cursor:k+1,stage:Stage::ScanSource,
            refresh_destination:s.dst == Some(k),..s } },
        Stage::Delete => { let k = s.dst.unwrap(); Driver {
            image:s.image.remove(k),cursor:k+1,stage:Stage::ScanDestination,..s } },
        Stage::Seal => Driver { stage:Stage::Emit,..s },
        Stage::Emit => Driver { stage:Stage::Done,..s },
        Stage::Done => s,
    }
}
pub open spec fn step(s: Driver, tick: Tick) -> Driver {
    match tick {
        Tick::Wait => s,
        Tick::Ok => advance(s),
        Tick::Error => if raw(s.stage) { initial(s.kind,s.source,s.image,s.bound) } else { s },
    }
}

pub proof fn advance_wf(s: Driver)
    requires wf(s)
    ensures wf(advance(s)), advance(s).source == s.source, advance(s).bound == s.bound,
        advance(s).kind == s.kind
{
    let z = advance(s);
    match s.stage {
        Stage::ScanSource => { scan_contract(s.source,s.cursor,s.bound); },
        Stage::ScanDestination => { scan_contract(s.image,s.cursor,s.bound); },
        Stage::Choose => {
            if s.src is None && s.dst is None {
                assert forall|k: nat| value(s.image,k) == value(s.source,k) by { }
                assert forall|k:nat| s.image.contains_key(k) == s.source.contains_key(k) by {
                    assert(value(s.image,k) == value(s.source,k));
                }
                assert forall|k:nat| s.image.contains_key(k) implies s.image[k] == s.source[k] by {
                    assert(value(s.image,k) == value(s.source,k));
                }
                assert(s.image =~= s.source);
            }
        },
        Stage::Put => {
            let k = s.src.unwrap();
            assert(k < s.bound && s.cursor <= k);
            assert forall|j: nat| j < z.cursor implies value(z.image,j) == value(z.source,j) by {
                if j >= s.cursor && j < k {
                    assert(!s.source.contains_key(j));
                    assert(!s.image.contains_key(j));
                }
            }
            if !z.refresh_destination {
                assert(first(z.image,z.cursor,z.dst)) by {
                    match s.dst {
                        Some(d) => { assert(k < d); },
                        None => {},
                    }
                }
            }
        },
        Stage::Delete => {
            let k = s.dst.unwrap();
            assert(k < s.bound && s.cursor <= k);
            assert(!s.source.contains_key(k));
            assert forall|j: nat| j < z.cursor implies value(z.image,j) == value(z.source,j) by {
                if j >= s.cursor && j < k {
                    assert(!s.source.contains_key(j));
                    assert(!s.image.contains_key(j));
                }
            }
            assert(first(z.source,z.cursor,z.src));
        },
        _ => {},
    }
}

pub proof fn step_wf(s: Driver, tick: Tick)
    requires wf(s)
    ensures wf(step(s,tick)), step(s,tick).source == s.source,
        step(s,tick).bound == s.bound, step(s,tick).kind == s.kind,
        s.stage is Done ==> step(s,tick) == s
{
    match tick {
        Tick::Ok => advance_wf(s),
        Tick::Error => {
            if raw(s.stage) {
                assert(!(s.kind is Local));
                initial_wf(s.kind,s.source,s.image,s.bound);
            }
        },
        _ => {},
    }
}

pub open spec fn stage_rank(stage: Stage) -> nat {
    match stage {
        Stage::ScanSource => 7, Stage::ScanDestination => 6, Stage::Choose => 5,
        Stage::Put | Stage::Delete => 4, Stage::Seal => 2, Stage::Emit => 1, Stage::Done => 0,
    }
}
pub open spec fn rank(s: Driver) -> nat {
    if s.stage is Done { 0 } else { ((s.bound-s.cursor)*8+stage_rank(s.stage)) as nat }
}
pub proof fn success_decreases(s: Driver)
    requires wf(s), !(s.stage is Done)
    ensures rank(advance(s)) < rank(s)
{
    match s.stage {
        Stage::Put => { assert(s.cursor <= s.src.unwrap() < s.bound); },
        Stage::Delete => { assert(s.cursor <= s.dst.unwrap() < s.bound); },
        _ => {},
    }
}

pub struct Execution {
    pub states: spec_fn(nat) -> Driver,
    pub ticks: spec_fn(nat) -> Tick,
}
impl Execution {
    pub open spec fn state(self,t:nat) -> Driver { (self.states)(t) }
    pub open spec fn tick(self,t:nat) -> Tick { (self.ticks)(t) }
}
pub open spec fn runs(b: Execution, base: nat) -> bool {
    wf(b.state(base)) && forall|t:nat| t >= base ==> b.state(t+1) == step(b.state(t),b.tick(t))
}
pub open spec fn pending_forever(b:Execution,r:Request,t:nat) -> bool {
    forall|u:nat| u >= t ==> !(b.state(u).stage is Done) && request(b.state(u)) == r
}
pub open spec fn primitive_service(b: Execution, base: nat) -> bool {
    forall|r: Request,t:nat| t >= base && #[trigger] pending_forever(b,r,t) ==>
        exists|u:nat| u >= t && b.tick(u) is Ok
}
/// This is intentionally eventual all-success, not per-key infinitely-often
/// success. Otherwise a different late row can fail on every restarted pass.
pub open spec fn stable_raw_io(b: Execution, stable: nat) -> bool {
    forall|t:nat| t >= stable ==> !(b.tick(t) is Error)
}
pub proof fn state_wf(b: Execution,base:nat,t:nat)
    requires runs(b,base), t >= base
    ensures wf(b.state(t)), b.state(t).source == b.state(base).source,
        b.state(t).kind == b.state(base).kind, b.state(t).bound == b.state(base).bound
    decreases t-base
{
    if t > base { state_wf(b,base,(t-1) as nat); step_wf(b.state((t-1) as nat),b.tick((t-1) as nat)); }
}
pub proof fn rank_span(b:Execution,base:nat,stable:nat,t:nat,u:nat)
    requires runs(b,base), stable_raw_io(b,stable), base <= t <= u, stable <= t
    ensures rank(b.state(u)) <= rank(b.state(t))
    decreases u-t
{
    if u > t {
        let v = (u-1) as nat;
        rank_span(b,base,stable,t,v); state_wf(b,base,v);
        if b.tick(v) is Ok && !(b.state(v).stage is Done) { success_decreases(b.state(v)); }
    }
}
pub proof fn waiting_state(b:Execution,base:nat,stable:nat,t:nat,u:nat)
    requires runs(b,base),stable_raw_io(b,stable),base <= t <= u,stable <= t,
        forall|v:nat| v >= t ==> !(b.tick(v) is Ok)
    ensures b.state(u) == b.state(t)
    decreases u-t
{
    if u > t {
        let v = (u-1) as nat;
        waiting_state(b,base,stable,t,v);
        assert(b.tick(v) is Wait);
        assert(b.state(v+1) == step(b.state(v),b.tick(v)));
    }
}
pub proof fn primitive_eventually_returns(b:Execution,base:nat,stable:nat,t:nat) -> (u:nat)
    requires runs(b,base),primitive_service(b,base),stable_raw_io(b,stable),
        t >= base,t >= stable,!(b.state(t).stage is Done)
    ensures u >= t,b.tick(u) is Ok
{
    if !(exists|u:nat| u >= t && b.tick(u) is Ok) {
        let r = request(b.state(t));
        assert forall|u:nat| u >= t implies !(b.state(u).stage is Done) && request(b.state(u)) == r by {
            waiting_state(b,base,stable,t,u);
        }
        assert(pending_forever(b,r,t));
    }
    choose|u:nat| u >= t && b.tick(u) is Ok
}

/// A genuine infinite-behavior eventuality, including any number of restarted
/// partial passes before stabilization. No successful whole pass is assumed.
pub proof fn eventually_emits(b:Execution,base:nat,stable:nat,t:nat) -> (u:nat)
    requires runs(b,base),primitive_service(b,base),stable_raw_io(b,stable),t >= base,t >= stable
    ensures u >= t,b.state(u).stage is Done,b.state(u).image == b.state(base).source,
        b.state(base).kind is Cleanup ==> b.state(u).image == Map::<nat,Seq<u8>>::empty()
    decreases rank(b.state(t))
{
    state_wf(b,base,t);
    if b.state(t).stage is Done { t }
    else {
        let v = primitive_eventually_returns(b,base,stable,t);
        state_wf(b,base,v);
        if b.state(v).stage is Done { v }
        else {
            rank_span(b,base,stable,t,v);
            success_decreases(b.state(v));
            eventually_emits(b,base,stable,v+1)
        }
    }
}

pub proof fn first_completion(b:Execution,base:nat,end:nat) -> (u:nat)
    requires base <= end,b.state(end).stage is Done
    ensures base <= u <= end,b.state(u).stage is Done,
        forall|t:nat| base <= t < u ==> !(b.state(t).stage is Done)
    decreases end-base
{
    if exists|t:nat| base <= t < end && b.state(t).stage is Done {
        let t = choose|t:nat| base <= t < end && b.state(t).stage is Done;
        first_completion(b,base,t)
    } else { end }
}

/// A fair all-success execution witnesses every authorized finite starting
/// image, including nonempty cleanup and a mirror with stale destination rows.
pub open spec fn iterate(s:Driver,t:nat) -> Driver
    decreases t
{
    if t == 0 { s } else { advance(iterate(s,(t-1) as nat)) }
}
pub open spec fn prompt(s:Driver) -> Execution {
    Execution { states:|t:nat| iterate(s,t),ticks:|t:nat| Tick::Ok }
}
pub proof fn prompt_witness(s:Driver)
    requires wf(s)
    ensures runs(prompt(s),0),primitive_service(prompt(s),0),stable_raw_io(prompt(s),0)
{
    assert forall|r:Request,t:nat| t >= 0 && #[trigger] pending_forever(prompt(s),r,t)
        implies exists|u:nat| u >= t && prompt(s).tick(u) is Ok by {
        assert(prompt(s).tick(t) is Ok);
    }
}

pub proof fn nonempty_raw_witness() -> (out:(Execution,Execution,nat,nat))
    ensures runs(out.0,0),primitive_service(out.0,0),stable_raw_io(out.0,0),
        runs(out.1,0),primitive_service(out.1,0),stable_raw_io(out.1,0),
        out.0.state(0).image != Map::<nat,Seq<u8>>::empty(),
        out.1.state(0).image != Map::<nat,Seq<u8>>::empty(),
        out.0.state(out.2).stage is Done,
        out.0.state(out.2).image == map![0nat => seq![1u8]],
        out.1.state(out.3).stage is Done,
        out.1.state(out.3).image == Map::<nat,Seq<u8>>::empty()
{
    let source = map![0nat => seq![1u8]];
    let stale = map![1nat => seq![2u8]];
    let copy = initial(Kind::Mirror,source,stale,2);
    let cleanup = initial(Kind::Cleanup,Map::empty(),stale,2);
    initial_wf(Kind::Mirror,source,stale,2);
    initial_wf(Kind::Cleanup,Map::empty(),stale,2);
    prompt_witness(copy); prompt_witness(cleanup);
    assert(prompt(copy).state(0) == copy);
    assert(prompt(cleanup).state(0) == cleanup);
    assert(copy.image.contains_key(1));
    assert(cleanup.image.contains_key(1));
    let copied = eventually_emits(prompt(copy),0,0,0);
    let erased = eventually_emits(prompt(cleanup),0,0,0);
    (prompt(copy),prompt(cleanup),copied,erased)
}

} // verus!
