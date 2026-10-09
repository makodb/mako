//! Trusted-interface service bridge. Placement certificates describe semantic
//! state; Emission records describe actual authenticated participant responses.
//! Network fairness applies ONLY to the latter. Raw work is issued by a fair
//! scheduler, and terminates by primitive I/O progress, never by an assumed
//! 'cleanup/copy eventually completes' condition.
use super::*;
use super::actual as a;
use super::io;
#[path = "sharding_progress_physical.rs"]
pub mod physical;
use physical::World;
#[path = "sharding_progress_service_witness.rs"]
pub mod witness;

verus! {

pub struct Emission { pub generation:nat, pub owner:int, pub certificate:p::Certificate }
pub struct Network { pub records:spec_fn(nat)->Set<Emission> }
impl Network { pub open spec fn emitted(self,t:nat) -> Set<Emission> { (self.records)(t) } }
pub open spec fn emission(s:p::State,g:nat,cert:p::Certificate) -> Emission {
    Emission { generation:g,certificate:cert,
        owner:if cert is Ready || cert is DestinationDone { s.plans[g].dst } else { s.plans[g].src } }
}

/// One actual raw scan/put/delete, accepted metadata/log call, or local
/// continuation. Errors retry the same primitive and do not fabricate success.
pub struct Atomic {
    pub operation:io::Request,
    pub evidence:spec_fn(nat)->Option<physical::Evidence>,
    pub done:spec_fn(nat)->bool,
    pub returns:spec_fn(nat)->io::Tick,
}
impl Atomic {
    pub open spec fn finished(self,t:nat) -> bool { (self.done)(t) }
    pub open spec fn tick(self,t:nat) -> io::Tick { (self.returns)(t) }
    pub open spec fn observed(self,t:nat)->Option<physical::Evidence> { (self.evidence)(t) }
}
pub open spec fn atomic_runs(x:Atomic,base:nat) -> bool {
    !x.finished(base) && forall|t:nat| t >= base ==>
        x.finished(t+1) == (x.finished(t) || x.tick(t) is Ok)
}
pub open spec fn atomic_pending_forever(x:Atomic,t:nat) -> bool {
    forall|u:nat| u >= t ==> !x.finished(u)
}
pub open spec fn atomic_returns(x:Atomic,base:nat) -> bool {
    forall|t:nat| t >= base && #[trigger] atomic_pending_forever(x,t) ==>
        exists|u:nat| u >= t && !(x.tick(u) is Wait)
}
pub open spec fn atomic_stable(x:Atomic,stable:nat) -> bool {
    forall|t:nat| t >= stable ==> !(x.tick(t) is Error)
}
pub proof fn atomic_eventually_succeeds(x:Atomic,base:nat,stable:nat) -> (u:nat)
    requires atomic_runs(x,base),atomic_returns(x,base),atomic_stable(x,stable)
    ensures u >= base,x.finished(u)
{
    let start = if base > stable { base } else { stable };
    if !(exists|u:nat| u >= start && x.finished(u)) {
        assert(atomic_pending_forever(x,start));
        let v = choose|u:nat| u >= start && !(x.tick(u) is Wait);
        assert(x.tick(v) is Ok);
        assert(x.finished(v+1));
    }
    choose|u:nat| u >= start && x.finished(u)
}
pub proof fn first_atomic_success(x:Atomic,base:nat,u:nat) -> (v:nat)
    requires atomic_runs(x,base),u >= base,x.finished(u)
    ensures base <= v < u,!x.finished(v),x.tick(v) is Ok
    decreases u-base
{
    assert(u > base);
    let v = (u-1) as nat;
    if x.finished(v) { first_atomic_success(x,base,v) } else { v }
}

pub proof fn traversal_emit_step(x:io::Execution,base:nat,u:nat) -> (v:nat)
    requires io::runs(x,base),!(x.state(base).stage is Done),u >= base,x.state(u).stage is Done
    ensures base <= v < u,x.state(v).stage is Emit,x.tick(v) is Ok
    decreases u-base
{
    assert(u > base);
    let v = (u-1) as nat;
    if x.state(v).stage is Done { traversal_emit_step(x,base,v) }
    else {
        io::state_wf(x,base,v);
        v
    }
}

pub open spec fn atomic_operation<C>(b:a::Execution,w:World<C>,g:nat,t:nat,action:p::Action,x:Atomic) -> bool {
    match action {
        p::Action::Capture { .. } => x.observed(t) is Some
            && x.operation == x.observed(t).unwrap().request
            && physical::atomic_data(b,w,g,t,action,x.observed(t).unwrap()),
        p::Action::DeliverCopy { packet } => if p::copy_guard(b.state(t),packet) {
            x.observed(t) is Some && x.operation == x.observed(t).unwrap().request
                && physical::atomic_data(b,w,g,t,action,x.observed(t).unwrap())
        } else { x.operation is Continue && x.observed(t) is None },
        p::Action::Seal { .. } | p::Action::Receive { .. } => false,
        _ => x.operation is Continue && x.observed(t) is None,
    }
}

/// Atomic bindings are actual successful native response/ClosedTransfer
/// projections. They do not claim an operation will succeed or be scheduled.
/// Capture binds Source::scan's authentic first-row/absence result; DeliverCopy
/// binds the correctly guarded RawStore point effect, or its rejected no-op.
/// The native scan/put/delete encoding lemmas are in transfer_progress.rs.
pub open spec fn atomic_binding<C>(b:a::Execution,w:World<C>,g:nat,action:p::Action,x:Atomic,start:nat) -> bool {
    atomic_runs(x,start)
        && forall|t:nat| t >= start && !x.finished(t) && x.tick(t) is Ok ==>
            atomic_operation(b,w,g,t,action,x) && b.action(t) == action
}

/// A real final mirror closes with the checked CompletedCopy/seal/Ready path.
/// Already-ready retry uses the retained native Ready response, not another
/// authorize_transfer(false) call. A continuously enabled Seal is still Stage.
pub open spec fn seal_binding<C>(b:a::Execution,w:World<C>,g:nat,id:nat,start:nat) -> bool {
    let j = w.job(id); let x = j.run;
    physical::job_bound(b,w,g,id) && j.origin <= start && io::runs(x,start)
        && x.state(start).kind is Mirror && !(x.state(start).stage is Done)
        && x.state(j.origin).kind is Mirror
        && forall|t:nat| t >= start && x.state(t).stage is Emit && x.tick(t) is Ok ==>
            b.action(t) == (p::Action::Seal { generation:g })
}

/// Scheduler fairness submits a named enabled call, not a successful result.
/// Availability is stated at raw primitives. Seal's full traversal can restart
/// arbitrarily often before its stabilization point; per-key recurring success
/// is not substituted for eventual error-free execution of the required pass.
pub open spec fn submitted_work<C>(c:p::Constants,b:a::Execution,w:World<C>,base:nat,g:nat) -> bool {
    forall|action:p::Action,t:nat| t >= base && !(action is Receive)
        && #[trigger] a::continuously_needed(c,b,g,action,t) ==>
        if action is Seal {
            exists|id:nat,start:nat,stable:nat| start >= t
                && seal_binding(b,w,g,id,start) && io::primitive_service(w.job(id).run,start) && io::stable_raw_io(w.job(id).run,stable)
        } else {
            exists|x:Atomic,start:nat,stable:nat| start >= t
                && atomic_binding(b,w,g,action,x,start) && atomic_returns(x,start) && atomic_stable(x,stable)
        }
}

pub open spec fn destructive(s:p::State,g:nat,cert:p::Certificate) -> bool {
    cert is SourceDone && s.phases[g] is Committed
        || cert is DestinationDone && s.phases[g] is Aborted
}

/// Required native per-step coupling: job image is the ACTUAL guarded selected
/// raw window, and Emit is the actual checked control-result emission. For a
/// destructive terminal disposition, a Local job is forbidden: deletion must
/// finish. Ready may already have its retained emission record from successful
/// execute_final; it is never regenerated by retrying an already-ready mirror.
pub open spec fn receipt_binding<C>(b:a::Execution,w:World<C>,net:Network,g:nat,cert:p::Certificate,
    id:nat,start:nat) -> bool {
    let j = w.job(id); let x = j.run;
    &&& physical::job_bound(b,w,g,id) && j.origin <= start && io::runs(x,start)
    &&& j.owner == emission(b.state(start),g,cert).owner
    &&& if destructive(b.state(start),g,cert) { x.state(start).kind is Cleanup }
        else if cert is Ready { x.state(start).kind is Mirror
            && (x.state(start).stage is Emit || x.state(start).stage is Done) }
        else { x.state(start).kind is Local }
    &&& forall|u:nat| u >= start && x.state(u).stage is Done ==>
        net.emitted(u).contains(emission(b.state(start),g,cert))
}

/// Safety provenance of an actual minted receipt: a finite prefix of the raw
/// driver, starting at its native entry point, has really reached Emit/Done.
/// This constrains existing records; it supplies no eventual record.
pub open spec fn receipt_origin<C>(b:a::Execution,w:World<C>,g:nat,cert:p::Certificate,id:nat,end:nat) -> bool {
    let j = w.job(id); let x = j.run;
    &&& physical::job_bound(b,w,g,id) && j.origin <= end
    &&& j.owner == emission(b.state(end),g,cert).owner
    &&& if destructive(b.state(end),g,cert) { x.state(j.origin).kind is Cleanup }
        else if cert is Ready { x.state(j.origin).kind is Mirror }
        else { x.state(j.origin).kind is Local }
    &&& x.state(end).stage is Done
}

/// Actual emitted records retain their authenticated sender while receipt is
/// pending. Mere membership of p::State.certificates creates no such record.
pub open spec fn emission_registry<C>(b:a::Execution,w:World<C>,net:Network,base:nat,g:nat) -> bool {
    &&& forall|cert:p::Certificate| b.state(base).received.contains((g,cert)) ==>
        exists|id:nat| receipt_origin(b,w,g,cert,id,base)
    &&& forall|t:nat,cert:p::Certificate| t >= base
        && net.emitted(t).contains(emission(b.state(t),g,cert)) ==>
        b.state(t).certificates.contains((g,cert))
    &&& forall|t:nat,cert:p::Certificate| t >= base
        && net.emitted(t).contains(emission(b.state(t),g,cert)) ==>
        exists|id:nat| receipt_origin(b,w,g,cert,id,t)
    &&& forall|t:nat,cert:p::Certificate| t >= base
        && b.action(t) == (p::Action::Receive { generation:g,certificate:cert }) ==>
        net.emitted(t).contains(emission(b.state(t),g,cert))
    &&& forall|t:nat,u:nat,cert:p::Certificate| base <= t <= u
        && net.emitted(t).contains(emission(b.state(t),g,cert))
        && p::current(b.state(u),g) && !b.state(u).received.contains((g,cert)) ==>
        net.emitted(u).contains(emission(b.state(u),g,cert))
}

pub open spec fn submitted_receipts<C>(c:p::Constants,b:a::Execution,w:World<C>,net:Network,base:nat,g:nat) -> bool {
    forall|t:nat,cert:p::Certificate| t >= base
        && #[trigger] a::continuously_needed(c,b,g,p::Action::Receive { generation:g,certificate:cert },t)
        && !net.emitted(t).contains(emission(b.state(t),g,cert)) ==>
        exists|id:nat,start:nat,stable:nat| start >= t
            && receipt_binding(b,w,net,g,cert,id,start)
            && io::primitive_service(w.job(id).run,start) && io::stable_raw_io(w.job(id).run,stable)
}

pub open spec fn emitted_forever(c:p::Constants,b:a::Execution,net:Network,g:nat,cert:p::Certificate,t:nat) -> bool {
    forall|u:nat| u >= t ==> net.emitted(u).contains(emission(b.state(u),g,cert))
        && a::primitive(b.state(u),g,p::Action::Receive { generation:g,certificate:cert })
        && p::enabled(c,b.state(u),p::Action::Receive { generation:g,certificate:cert })
}

/// Authentic retried messages must eventually deliver only AFTER emission.
/// This does not require the network to manufacture a cleanup/Ready receipt.
pub open spec fn wire_fair(c:p::Constants,b:a::Execution,net:Network,base:nat,g:nat) -> bool {
    forall|t:nat,cert:p::Certificate| t >= base
        && #[trigger] emitted_forever(c,b,net,g,cert,t) ==>
        exists|u:nat| u >= t && b.action(u) == (p::Action::Receive { generation:g,certificate:cert })
}

pub proof fn emitted_receipt_eventually_available<C>(c:p::Constants,b:a::Execution,w:World<C>,net:Network,
    base:nat,g:nat,t:nat,cert:p::Certificate) -> (u:nat)
    requires a::execution(c,b,base),submitted_receipts(c,b,w,net,base,g),t >= base,
        b.state(t).plans.contains_key(g),
        forall|v:nat| v >= t ==> a::primitive(b.state(v),g,p::Action::Receive { generation:g,certificate:cert })
            && p::enabled(c,b.state(v),p::Action::Receive { generation:g,certificate:cert })
    ensures u >= t,net.emitted(u).contains(emission(b.state(u),g,cert))
{
    if net.emitted(t).contains(emission(b.state(t),g,cert)) { t }
    else {
        assert(a::continuously_needed(c,b,g,p::Action::Receive { generation:g,certificate:cert },t));
        let (id,start,stable) = choose|id:nat,start:nat,stable:nat| start >= t
            && receipt_binding(b,w,net,g,cert,id,start)
            && io::primitive_service(w.job(id).run,start) && io::stable_raw_io(w.job(id).run,stable);
        let x = w.job(id).run;
        let ready = if start > stable { start } else { stable };
        let u = io::eventually_emits(x,start,stable,ready);
        if !(x.state(w.job(id).origin).kind is Local) {
            physical::job_completion(b,w,g,id,u);
        }
        a::metadata_span(c,b,base,g,t,start);
        a::metadata_span(c,b,base,g,start,u);
        u
    }
}

pub proof fn atomic_binding_eventually_executes<C>(b:a::Execution,w:World<C>,g:nat,
    action:p::Action,x:Atomic,start:nat,stable:nat)->(v:nat)
    requires atomic_binding(b,w,g,action,x,start),atomic_returns(x,start),atomic_stable(x,stable)
    ensures v >= start,b.action(v) == action
{
    hide(atomic_operation);
    let end = atomic_eventually_succeeds(x,start,stable);
    let v = first_atomic_success(x,start,end);
    v
}

pub proof fn seal_binding_eventually_executes<C>(b:a::Execution,w:World<C>,g:nat,
    id:nat,start:nat,stable:nat)->(v:nat)
    requires seal_binding(b,w,g,id,start),io::primitive_service(w.job(id).run,start),
        io::stable_raw_io(w.job(id).run,stable)
    ensures v >= start,b.action(v) == (p::Action::Seal { generation:g })
{
    hide(physical::job_bound);
    hide(physical::complete);
    let x = w.job(id).run;
    let ready = if start > stable { start } else { stable };
    let end = io::eventually_emits(x,start,stable,ready);
    physical::job_completion(b,w,g,id,end);
    traversal_emit_step(x,start,end)
}

/// Derived abstract fairness. Receive now depends on finite raw cleanup (or
/// exact final mirror), real checked emission, and only then wire delivery.
/// Capture and point-copy calls require their raw SourceScan/Put/Delete returns;
/// Seal requires a full raw traversal, not just an abstract coverage predicate.
pub proof fn theorem_service_implies_abstract_fairness<C>(c:p::Constants,b:a::Execution,w:World<C>,net:Network,
    base:nat,g:nat)
    requires a::execution(c,b,base),b.state(base).plans.contains_key(g),
        submitted_work(c,b,w,base,g),submitted_receipts(c,b,w,net,base,g),
        emission_registry(b,w,net,base,g),wire_fair(c,b,net,base,g)
    ensures a::fair(c,b,base,g)
{
    hide(physical::job_bound);
    hide(a::execution);
    hide(atomic_binding);
    hide(seal_binding);
    hide(receipt_binding);
    hide(receipt_origin);
    hide(submitted_receipts);
    hide(io::primitive_service);
    hide(io::stable_raw_io);
    hide(atomic_returns);
    hide(atomic_stable);
    assert forall|action:p::Action| a::call_fair(c,b,base,g,action) by {
        assert forall|t:nat| t >= base
            && #[trigger] a::continuously_needed(c,b,g,action,t)
            implies exists|u:nat| u >= t && b.action(u) == action by {
            a::metadata_span(c,b,base,g,base,t);
            match action {
                p::Action::Receive { generation,certificate } => {
                    let u = emitted_receipt_eventually_available(c,b,w,net,base,g,t,certificate);
                    assert forall|v:nat| v >= u implies
                        net.emitted(v).contains(emission(b.state(v),g,certificate))
                            && a::primitive(b.state(v),g,action) && p::enabled(c,b.state(v),action) by { }
                    assert(emitted_forever(c,b,net,g,certificate,u));
                    let v = choose|v:nat| v >= u && b.action(v) == action;
                },
                p::Action::Seal { generation } => {
                    let (id,start,stable) = choose|id:nat,start:nat,stable:nat| start >= t
                        && seal_binding(b,w,g,id,start) && io::primitive_service(w.job(id).run,start) && io::stable_raw_io(w.job(id).run,stable);
                    let v = seal_binding_eventually_executes(b,w,g,id,start,stable);
                },
                _ => {
                    let (x,start,stable) = choose|x:Atomic,start:nat,stable:nat| start >= t
                        && atomic_binding(b,w,g,action,x,start) && atomic_returns(x,start) && atomic_stable(x,stable);
                    let v = atomic_binding_eventually_executes(b,w,g,action,x,start,stable);
                },
            }
        }
    }
}

pub proof fn done_span(x:io::Execution,origin:nat,t:nat,u:nat)
    requires io::runs(x,origin),origin <= t <= u,x.state(t).stage is Done
    ensures x.state(u).stage is Done
    decreases u-t
{
    if u > t {
        let v = (u-1) as nat;
        done_span(x,origin,t,v); io::state_wf(x,origin,v);
        assert(x.state(v+1) == io::step(x.state(v),x.tick(v)));
        assert(io::step(x.state(v),x.tick(v)).stage is Done);
    }
}

pub proof fn receipt_origin_span<C>(c:p::Constants,b:a::Execution,w:World<C>,base:nat,g:nat,
    cert:p::Certificate,id:nat,t:nat,u:nat)
    requires a::execution(c,b,base),base <= t <= u,b.state(t).plans.contains_key(g),
        b.state(t).certificates.contains((g,cert)),receipt_origin(b,w,g,cert,id,t)
    ensures receipt_origin(b,w,g,cert,id,u)
{
    a::metadata_span(c,b,base,g,t,u); a::state_at(c,b,base,t);
    if cert is SourceDone || cert is DestinationDone {
        assert(p::plan_inv(c,b.state(t),g));
        assert(p::terminal(b.state(t).phases[g]));
    }
    let j = w.job(id);
    done_span(j.run,j.origin,t,u);
}

pub proof fn received_is_certified(c:p::Constants,b:a::Execution,base:nat,g:nat,
    cert:p::Certificate,t:nat)
    requires a::execution(c,b,base),base <= t,b.state(base).plans.contains_key(g),
        b.state(t).received.contains((g,cert))
    ensures b.state(t).certificates.contains((g,cert))
{
    a::metadata_span(c,b,base,g,base,t);
    a::state_at(c,b,base,t);
    assert(p::plan_inv(c,b.state(t),g));
}

pub proof fn new_receipt_action(c:p::Constants,b:a::Execution,base:nat,g:nat,
    cert:p::Certificate,t:nat)
    requires a::execution(c,b,base),base <= t,
        !b.state(t).received.contains((g,cert)),b.state(t+1).received.contains((g,cert))
    ensures b.action(t) == (p::Action::Receive { generation:g,certificate:cert }),
        b.state(t).certificates.contains((g,cert))
{
    assert(b.state(t+1) == p::dispatch(c,b.state(t),b.action(t)));
    match b.action(t) {
        p::Action::Receive { generation,certificate } => {
            assert(generation == g && certificate == cert);
        },
        _ => { assert(false); },
    }
}

pub proof fn received_provenance<C>(c:p::Constants,b:a::Execution,w:World<C>,net:Network,
    base:nat,g:nat,cert:p::Certificate,t:nat)->(id:nat)
    requires a::execution(c,b,base),emission_registry(b,w,net,base,g),t >= base,
        b.state(base).plans.contains_key(g),b.state(t).received.contains((g,cert))
    ensures receipt_origin(b,w,g,cert,id,t)
    decreases t-base
{
    hide(physical::job_bound);
    hide(a::execution);
    hide(receipt_origin);
    if t == base { choose|id:nat| receipt_origin(b,w,g,cert,id,t) }
    else {
        let v = (t-1) as nat;
        a::metadata_span(c,b,base,g,base,v);
        let id = if b.state(v).received.contains((g,cert)) {
            received_is_certified(c,b,base,g,cert,v);
            received_provenance(c,b,w,net,base,g,cert,v)
        } else {
            new_receipt_action(c,b,base,g,cert,v);
            assert(net.emitted(v).contains(emission(b.state(v),g,cert)));
            choose|id:nat| receipt_origin(b,w,g,cert,id,v)
        };
        receipt_origin_span(c,b,w,base,g,cert,id,v,t);
        id
    }
}

pub struct CleanupWitness { pub job:nat,pub time:nat }
impl CleanupWitness {
    pub open spec fn valid<C>(&self,b:a::Execution,w:World<C>,g:nat,end:nat)->bool {
        let j = w.job(self.job);
        &&& j.generation == g
        &&& j.owner == if b.state(end).phases[g] is Committed {
            b.state(end).plans[g].src
        } else { b.state(end).plans[g].dst }
        &&& j.origin <= self.time <= end && j.run.state(self.time).stage is Done
        &&& physical::complete(w.image(self.time,j.owner),w.image(j.origin,j.owner),Map::empty(),w.window(g))
    }
}

pub open spec fn physical_cleanup<C>(b:a::Execution,w:World<C>,g:nat,t:nat)->bool {
    exists|witness:CleanupWitness| #[trigger] witness.valid(b,w,g,t)
}

pub proof fn generation_domain(c:p::Constants,b:a::Execution,base:nat,g:nat,t:nat)
    requires a::execution(c,b,base),base <= t,b.state(t).plans.contains_key(g)
    ensures b.state(t).phases.contains_key(g)
{
    a::state_at(c,b,base,t);
}

pub proof fn destructive_origin_cleanup<C>(b:a::Execution,w:World<C>,g:nat,
    cert:p::Certificate,id:nat,t:nat)
    requires receipt_origin(b,w,g,cert,id,t),destructive(b.state(t),g,cert),
        b.state(t).plans.contains_key(g),b.state(t).phases.contains_key(g)
    ensures physical_cleanup(b,w,g,t)
{
    hide(physical::job_bound);
    hide(physical::complete);
    hide(CleanupWitness::valid);
    let j = w.job(id);
    let u = physical::job_completion(b,w,g,id,t);
    let owner = if b.state(t).phases[g] is Committed { b.state(t).plans[g].src } else { b.state(t).plans[g].dst };
    assert(j.owner == owner);
    assert(physical::complete(w.image(u,owner),w.image(j.origin,owner),Map::<C,Seq<u8>>::empty(),w.window(g)));
    let witness = CleanupWitness { job:id,time:u };
    assert(witness.valid(b,w,g,t)) by {
        reveal(CleanupWitness::valid);
    }
}

pub proof fn completion_includes_physical_cleanup<C>(c:p::Constants,b:a::Execution,w:World<C>,net:Network,base:nat,g:nat,t:nat)
    requires a::execution(c,b,base),emission_registry(b,w,net,base,g),base <= t,
        b.state(base).plans.contains_key(g),completed(b.state(t),g)
    ensures physical_cleanup(b,w,g,t)
{
    hide(physical::job_bound);
    hide(a::execution);
    hide(emission_registry);
    hide(receipt_origin);
    hide(physical_cleanup);
    generation_domain(c,b,base,g,t);
    let cert = if b.state(t).phases[g] is Committed { p::Certificate::SourceDone } else { p::Certificate::DestinationDone };
    let id = received_provenance(c,b,w,net,base,g,cert,t);
    destructive_origin_cleanup(b,w,g,cert,id,t);
}

pub proof fn theorem_service_eventual_completion<C>(c:p::Constants,b:a::Execution,w:World<C>,net:Network,
    base:nat,g:nat,keys:Seq<int>,recovered_prefix:Seq<p::State>) -> (u:nat)
    requires p::constants_ok(c),p::behavior(c,recovered_prefix),recovered_prefix.last() == b.state(base),
        a::stable_steps(c,b,base),b.state(base).plans.contains_key(g),a::finite_image(b.state(base),g,keys),
        a::engines_settle(b,base,g),a::finite_admission(b,base,g),
        submitted_work(c,b,w,base,g),submitted_receipts(c,b,w,net,base,g),
        emission_registry(b,w,net,base,g),wire_fair(c,b,net,base,g)
    ensures u >= base,completed(b.state(u),g),physical_cleanup(b,w,g,u)
{
    a::reachable_progress_shape(c,recovered_prefix);
    theorem_service_implies_abstract_fairness(c,b,w,net,base,g);
    let u = a::theorem_actual_eventual_completion(c,b,base,g,keys,recovered_prefix);
    completion_includes_physical_cleanup(c,b,w,net,base,g,u);
    u
}

} // verus!
