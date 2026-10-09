//! Exclusive loader history, before native participant activation. These ghost
//! scopes abstract real engine commits; they are NOT native registry RPCs.
//! The engine byte/frame boundary is identical to the live transaction boundary.
//! Initial authoritative bytes must actually be absent. Empty nonowner private
//! images are masked, not silently imported as arbitrary initial model values.
use super::*;
verus! {
/// Runtime TxnId is two u64s. Open requires nonnegative model identities, so
/// bootstrap occurrences occupy the disjoint interval ABOVE that entire space.
/// The model's -1 initial-cell sentinel remains distinct from every writer.
pub open spec fn bootstrap_id(ordinal:nat) -> int {
    340282366920938463463374607431768211456int + ordinal
}
pub proof fn bootstrap_runtime_disjoint(ordinal:nat,id:crate::types::TxnId)
    ensures bootstrap_id(ordinal) > transaction(id),transaction(id) >= 0,
{
    crate::migration_refinement::nonce_injective(id,id);
    assert(id.client as int <= 18446744073709551615int);
    assert(id.sequence as int <= 18446744073709551615int);
}

/// Observations of the actual startup lifecycle, not an engine/model oracle.
/// `activated` means the post-loader participant exists and can answer HELLO;
/// `compatible` records equality of the fixed peer's published HELLO catalog.
/// Local activation ends loader bypass immediately; it is distinct from data
/// readiness. Ordinary lease admission waits until every fixed peer publishes a
/// post-load node with the same HELLO catalog. HELLO itself need not await data
/// readiness, so this does not create a cyclic bootstrap wait. Standard
/// f_mode==0 benchmark workers also wait every shard's NFSSync load_phase.
/// These real boundaries supply the observations below: no successful migration
/// or live application request may interleave this loading prefix. Migration
/// begin separately checks every peer HELLO. Restarts/recovery are not covered.
pub struct BootstrapLifecycle {
    pub fixed:Set<int>,
    pub loaded:Set<int>,
    pub activated:Set<int>,
    pub compatible:Set<int>,
    pub migration_issued:bool,
    pub live_request_issued:bool,
}
pub open spec fn bootstrap_exclusive(c:p::Constants,l:BootstrapLifecycle) -> bool {
    l.fixed == c.shards && l.loaded.subset_of(l.fixed)
        && l.activated.subset_of(l.loaded) && l.compatible.subset_of(l.activated)
        && !l.migration_issued && !l.live_request_issued
}
pub open spec fn bootstrap_ready(c:p::Constants,l:BootstrapLifecycle) -> bool {
    bootstrap_exclusive(c,l) && l.loaded == l.fixed
        && l.activated == l.fixed && l.compatible == l.fixed
}
pub open spec fn bootstrap_progress(before:BootstrapLifecycle,after:BootstrapLifecycle) -> bool {
    before.fixed == after.fixed && before.loaded.subset_of(after.loaded)
        && before.activated.subset_of(after.activated)
        && before.compatible.subset_of(after.compatible)
}

/// Retain actual images, bytes, commit outcome and lifecycle observations. The
/// ordinal is the position in this sequence, not an invented native TxnId.
pub struct BootstrapCommit {
    pub owner:int,
    pub before:EngineImages,
    pub after:EngineImages,
    pub writes:Map<int,Option<Seq<u8>>>,
    pub outcome:EngineOutcome,
    pub lifecycle:BootstrapLifecycle,
}
pub open spec fn bootstrap_cell(events:Seq<BootstrapCommit>,key:int,code:spec_fn(Seq<u8>)->int) -> p::Cell
    decreases events.len(),
{
    if events.len() == 0 { p::empty_cell() }
    else if events.last().writes.contains_key(key) {
        p::Cell { value:encoded_writes(events.last().writes,code)[key],writer:bootstrap_id((events.len()-1) as nat) }
    } else { bootstrap_cell(events.drop_last(),key,code) }
}
/// Exact initial directory and migration metadata, with only cell contents and
/// historical sessions permitted to differ from the empty initial state.
pub open spec fn bootstrap_layout(c:p::Constants,s:p::State) -> bool {
    s == (p::State { logical:s.logical,sessions:s.sessions,physical:s.physical,..p::initial(c) })
        && s.logical.dom() == c.keys
        && (forall|q:(int,int)| s.physical.contains_key(q) <==> c.shards.contains(q.0) && c.keys.contains(q.1))
        && (forall|q:(int,int)| s.physical.contains_key(q) ==>
            s.physical[q] == (p::Replica { cell:s.physical[q].cell,..p::initial(c).physical[q] })
            && s.physical[q].cell == if q.0 == c.owners[q.1] { s.logical[q.1] } else { p::empty_cell() })
}
pub open spec fn bootstrap_closed_sessions(s:p::State,count:nat) -> bool {
    (forall|t:int| s.sessions.contains_key(t) <==> bootstrap_id(0) <= t < bootstrap_id(count))
        && forall|t:int| s.sessions.contains_key(t) ==> s.sessions[t].resolved
            && s.sessions[t].held == Map::<int,p::Grant>::empty()
}
/// Direct physical frame for a loader on its original owner. This is exactly
/// atomic_bytes after the ghost initial-owner acquisition, not a relaxed frame.
pub open spec fn loader_bytes(c:p::Constants,event:BootstrapCommit,labels:Map<int,Cell>) -> bool {
    event.outcome is Committed && c.shards.contains(event.owner)
        && event.writes.dom().subset_of(c.keys)
        && (forall|k:int| event.writes.contains_key(k) ==> c.owners[k] == event.owner)
        && forall|owner:int,k:int| c.shards.contains(owner) && c.keys.contains(k) ==>
            event.before.contains_key(owner) && event.after.contains_key(owner)
            && crate::storage::value(event.after[owner],labels[k]) ==
                if owner == event.owner && event.writes.contains_key(k) { event.writes[k] }
                else { crate::storage::value(event.before[owner],labels[k]) }
}

/// The concrete images form one unbroken history rooted in actual absent owner
/// bytes. This rules out attaching unrelated before/after snapshots to a prefix.
pub open spec fn bootstrap_trace(c:p::Constants,events:Seq<BootstrapCommit>,images:EngineImages,labels:Map<int,Cell>) -> bool
    decreases events.len(),
{
    if events.len() == 0 {
        forall|k:int| c.keys.contains(k) ==> images.contains_key(c.owners[k])
            && crate::storage::value(images[c.owners[k]],labels[k]) == None
    } else {
        bootstrap_trace(c,events.drop_last(),events.last().before,labels)
            && events.last().after == images
    }
}

/// Fields are private: callers cannot supply a desired model state or journal.
/// Only new(empty actual owner images), actual_commit, and activate construct
/// or consume this ledger. All ghost scopes close within each atomic commit.
pub tracked struct Bootstrap {
    tracked execution:Execution,
    ghost constants:p::Constants,
    ghost state:p::State,
    ghost images:EngineImages,
    ghost labels:Map<int,Cell>,
    ghost code:spec_fn(Seq<u8>)->int,
    ghost events:Seq<BootstrapCommit>,
    ghost lifecycle:BootstrapLifecycle,
}
impl Bootstrap {
    pub closed spec fn state(&self) -> p::State { self.state }
    pub closed spec fn images(&self) -> EngineImages { self.images }
    pub closed spec fn history(&self) -> Seq<BootstrapCommit> { self.events }
    pub closed spec fn lifecycle(&self) -> BootstrapLifecycle { self.lifecycle }
    pub closed spec fn valid(&self,c:p::Constants,labels:Map<int,Cell>,code:spec_fn(Seq<u8>)->int) -> bool {
        self.constants == c && self.labels == labels && self.code == code
            && p::constants_ok(c) && labels.dom() == c.keys
            && self.execution.closed(c,self.state)
            && bootstrap_layout(c,self.state)
            && bootstrap_closed_sessions(self.state,self.events.len())
            && bootstrap_exclusive(c,self.lifecycle)
            && engine_observed(self.state,self.images,labels,code)
            && bootstrap_trace(c,self.events,self.images,labels)
            && (forall|k:int| c.keys.contains(k) ==> self.state.logical[k] == bootstrap_cell(self.events,k,code))
            && (forall|i:int| 0 <= i < self.events.len() ==> loader_bytes(c,self.events[i],labels)
                && bootstrap_exclusive(c,self.events[i].lifecycle)
                && !self.events[i].lifecycle.loaded.contains(self.events[i].owner)
                && !self.events[i].lifecycle.activated.contains(self.events[i].owner))
    }
    pub proof fn new(c:p::Constants,images:EngineImages,labels:Map<int,Cell>,code:spec_fn(Seq<u8>)->int,
        lifecycle:BootstrapLifecycle) -> (tracked out:Self)
        requires p::constants_ok(c),labels.dom() == c.keys,
            forall|a:int,b:int| labels.contains_key(a) && labels.contains_key(b) && labels[a] == labels[b] ==> a == b,
            bootstrap_exclusive(c,lifecycle),lifecycle.loaded == Set::<int>::empty(),
            forall|k:int| c.keys.contains(k) ==> images.contains_key(c.owners[k])
                && crate::storage::value(images[c.owners[k]],labels[k]) == None,
        ensures out.valid(c,labels,code),out.state() == p::initial(c),out.images() == images,
            out.history() == Seq::<BootstrapCommit>::empty(),out.lifecycle() == lifecycle,
    {
        let s = p::initial(c);
        assert forall|q:(int,int)| s.physical.contains_key(q) && labels.contains_key(q.1)
            && !(p::replica(s,q.0,q.1).role is Empty)
            && (!(p::replica(s,q.0,q.1).role is Stage) || p::replica(s,q.0,q.1).covered) implies
            images.contains_key(q.0) && p::replica(s,q.0,q.1).cell.value ==
                match crate::storage::value(images[q.0],labels[q.1]) { Some(v) => Some(code(v)),None => None } by {
            assert(q.0 == c.owners[q.1]);
        }
        let tracked execution = Execution::new(c);
        Self { execution,constants:c,state:s,images,labels,code,events:Seq::empty(),lifecycle }
    }

    /// Append one real successful loader commit, with no registry activity.
    /// Exclusive bootstrap makes the initial-home acquisitions legitimate model
    /// scopes; the native loader never has to issue begin/acquire/finish RPCs.
    pub proof fn actual_commit(tracked self,c:p::Constants,labels:Map<int,Cell>,code:spec_fn(Seq<u8>)->int,
        event:BootstrapCommit) -> (tracked out:Self)
        requires self.valid(c,labels,code),event.before == self.images(),loader_bytes(c,event,labels),
            bootstrap_exclusive(c,event.lifecycle),bootstrap_progress(self.lifecycle(),event.lifecycle),
            !event.lifecycle.loaded.contains(event.owner),!event.lifecycle.activated.contains(event.owner),
        ensures out.valid(c,labels,code),out.images() == event.after,
            out.history() == self.history().push(event),out.lifecycle() == event.lifecycle,
            forall|k:int| event.writes.contains_key(k) ==> out.state().logical[k] ==
                transfer::observed(event.after[event.owner],labels[k],bootstrap_id(self.history().len()),code),
    {
        let s = self.state;
        let t = bootstrap_id(self.events.len());
        let target = Map::new(event.writes.dom(),|k:int| p::Grant { owner:c.owners[k],epoch:0 });
        let work = event.writes.dom().to_seq();
        lease_execution::lease_keys(event.writes.dom());
        assert(!s.sessions.contains_key(t));
        let write = log::Write::Session { txn:t,value:p::Session { held:Map::empty(),resolved:false } };
        log::single_write(s,write);
        let opened = log::apply_write(s,write);
        log::accepted(c,s,opened,p::Action::Open { txn:t });
        let tracked open = ClosedTransfer { segment:log::Segment {
            writes:seq![write],states:seq![s,opened],actions:seq![p::Action::Open { txn:t }] } };
        assert forall|k:int| work.to_set().contains(k) implies opened.physical.contains_key((target[k].owner,k))
            && p::replica(opened,target[k].owner,k).role is Serving
            && p::replica(opened,target[k].owner,k).epoch == target[k].epoch by {
            assert(c.keys.contains(k));
            assert(c.shards.contains(c.owners[k]));
        }
        let tracked acquired = lease_execution::acquire_keys(c,opened,t,work,target);
        let held = acquired.after(opened);
        assert(lease_execution::overlay(Map::<int,p::Grant>::empty(),target,work.to_set()) =~= target);
        assert(held.sessions[t].held == target);
        assert forall|q:(int,int)| held.physical.contains_key(q) && labels.contains_key(q.1) implies
            event.before.contains_key(q.0) && event.after.contains_key(q.0)
            && crate::storage::value(event.after[q.0],labels[q.1]) ==
                if event.writes.contains_key(q.1) && held.sessions[t].held[q.1].owner == q.0 { event.writes[q.1] }
                else { crate::storage::value(event.before[q.0],labels[q.1]) } by {
            if event.writes.contains_key(q.1) { assert(c.owners[q.1] == event.owner); }
        }
        assert forall|k:int| event.writes.contains_key(k) implies held.logical.contains_key(k)
            && held.physical.contains_key((held.sessions[t].held[k].owner,k)) by {
            assert(c.shards.contains(c.owners[k]));
        }
        let tracked committed = engine::engine_effect(c,held,t,event.outcome,event.before,event.after,labels,event.writes,code);
        let resolved = committed.after(held);
        let tracked released = lease_execution::release_keys(c,resolved,t,work);
        let next = released.after(resolved);
        assert(target.remove_keys(work.to_set()) =~= Map::<int,p::Grant>::empty());
        assert(next.sessions =~= s.sessions.insert(t,p::Session { held:Map::empty(),resolved:true }));
        assert forall|u:int| next.sessions.contains_key(u) <==> bootstrap_id(0) <= u < bootstrap_id(self.events.len()+1) by {
            if u != t { assert(next.sessions.contains_key(u) == s.sessions.contains_key(u)); }
        }
        assert forall|u:int| next.sessions.contains_key(u) implies next.sessions[u].resolved
            && next.sessions[u].held == Map::<int,p::Grant>::empty() by {
            if u != t { assert(next.sessions[u] == s.sessions[u]); }
        }
        let cells = Map::new(event.writes.dom(),|k:int| transfer::observed(event.after[held.sessions[t].held[k].owner],labels[k],t,code));
        assert forall|k:int| event.writes.contains_key(k) implies cells[k] ==
            (p::Cell { value:encoded_writes(event.writes,code)[k],writer:t }) by {
            assert(c.owners[k] == event.owner);
            assert(crate::storage::value(event.after[event.owner],labels[k]) == event.writes[k]);
        }
        let events = self.events.push(event);
        assert(events.drop_last() =~= self.events);
        assert forall|k:int| c.keys.contains(k) implies next.logical[k] == bootstrap_cell(events,k,code) by {
            if event.writes.contains_key(k) { assert(next.logical[k] == cells[k]); }
            else { assert(next.logical[k] == s.logical[k]); }
        }
        assert forall|q:(int,int)| next.physical.contains_key(q) implies
            next.physical[q] == (p::Replica { cell:next.physical[q].cell,..p::initial(c).physical[q] })
            && next.physical[q].cell == if q.0 == c.owners[q.1] { next.logical[q.1] } else { p::empty_cell() } by {
            if event.writes.contains_key(q.1) && q.0 == c.owners[q.1] {
                assert(next.physical[q].cell == cells[q.1]);
            } else { assert(next.physical[q] == s.physical[q]); }
        }
        assert forall|i:int| 0 <= i < events.len() implies loader_bytes(c,events[i],labels)
            && bootstrap_exclusive(c,events[i].lifecycle)
            && !events[i].lifecycle.loaded.contains(events[i].owner)
            && !events[i].lifecycle.activated.contains(events[i].owner) by {
            if i < self.events.len() { assert(events[i] == self.events[i]); }
            else { assert(i == self.events.len()); assert(events[i] == event); }
        }
        let tracked execution = self.execution.append(c,s,open);
        let tracked execution = execution.append(c,opened,acquired);
        let tracked execution = execution.append(c,held,committed);
        let tracked execution = execution.append(c,resolved,released);
        Self { execution,constants:c,state:next,images:event.after,labels,code,events,lifecycle:event.lifecycle }
    }

    /// Activation consumes the exclusive ledger only after ALL fixed peers have
    /// completed their actual load, activated, and published compatible HELLOs.
    /// No desired state is accepted: the returned Execution is the existing
    /// journal, closed from p::initial through the actual loader byte events.
    pub proof fn activate(tracked self,c:p::Constants,labels:Map<int,Cell>,code:spec_fn(Seq<u8>)->int,
        lifecycle:BootstrapLifecycle) -> (tracked out:(Execution,BootstrapCertificate))
        requires self.valid(c,labels,code),bootstrap_ready(c,lifecycle),bootstrap_progress(self.lifecycle(),lifecycle),
        ensures out.0.closed(c,self.state()),out.1.valid(c,self.state(),self.images(),labels,code),
            out.1.history() == self.history(),out.1.lifecycle() == lifecycle,
    {
        self.execution.finite_history(c,self.state);
        let tracked certificate = BootstrapCertificate {
            constants:c,state:self.state,images:self.images,labels,code,events:self.events,lifecycle,
        };
        (self.execution,certificate)
    }
}

/// Only Bootstrap::activate constructs this certificate. Writer provenance is
/// the last actual loader occurrence, including successful deletes/tombstones.
pub tracked struct BootstrapCertificate {
    ghost constants:p::Constants,
    ghost state:p::State,
    ghost images:EngineImages,
    ghost labels:Map<int,Cell>,
    ghost code:spec_fn(Seq<u8>)->int,
    ghost events:Seq<BootstrapCommit>,
    ghost lifecycle:BootstrapLifecycle,
}
impl BootstrapCertificate {
    pub closed spec fn history(&self) -> Seq<BootstrapCommit> { self.events }
    pub closed spec fn lifecycle(&self) -> BootstrapLifecycle { self.lifecycle }
    pub closed spec fn valid(&self,c:p::Constants,s:p::State,images:EngineImages,
        labels:Map<int,Cell>,code:spec_fn(Seq<u8>)->int) -> bool {
        self.constants == c && self.state == s && self.images == images && self.labels == labels && self.code == code
            && p::constants_ok(c) && labels.dom() == c.keys && bootstrap_ready(c,self.lifecycle)
            && bootstrap_layout(c,s) && bootstrap_closed_sessions(s,self.events.len())
            && engine_observed(s,images,labels,code)
            && bootstrap_trace(c,self.events,images,labels)
            && (forall|k:int| c.keys.contains(k) ==> s.logical[k] == bootstrap_cell(self.events,k,code))
            && (forall|i:int| 0 <= i < self.events.len() ==> loader_bytes(c,self.events[i],labels)
                && bootstrap_exclusive(c,self.events[i].lifecycle)
                && !self.events[i].lifecycle.loaded.contains(self.events[i].owner)
                && !self.events[i].lifecycle.activated.contains(self.events[i].owner))
            && p::inv(c,s) && exists|states:Seq<p::State>| p::behavior(c,states) && states.last() == s
    }
    /// Public elimination rule for the private activation certificate.
    pub proof fn established_prefix(tracked &self,c:p::Constants,s:p::State,images:EngineImages,
        labels:Map<int,Cell>,code:spec_fn(Seq<u8>)->int)
        requires self.valid(c,s,images,labels,code),
        ensures bootstrap_ready(c,self.lifecycle()),bootstrap_layout(c,s),
            bootstrap_closed_sessions(s,self.history().len()),
            bootstrap_trace(c,self.history(),images,labels),engine_observed(s,images,labels,code),
            forall|k:int| c.keys.contains(k) ==> s.logical[k] == bootstrap_cell(self.history(),k,code),
            forall|i:int| 0 <= i < self.history().len() ==> loader_bytes(c,self.history()[i],labels)
                && bootstrap_exclusive(c,self.history()[i].lifecycle)
                && !self.history()[i].lifecycle.loaded.contains(self.history()[i].owner)
                && !self.history()[i].lifecycle.activated.contains(self.history()[i].owner),
            p::inv(c,s),exists|states:Seq<p::State>| p::behavior(c,states) && states.last() == s,
    {}

    pub proof fn loaded_owner(tracked &self,c:p::Constants,s:p::State,images:EngineImages,
        labels:Map<int,Cell>,code:spec_fn(Seq<u8>)->int,key:int)
        requires self.valid(c,s,images,labels,code),c.keys.contains(key),
        ensures s.logical[key] == bootstrap_cell(self.history(),key,code),
            s.logical[key] == p::replica(s,c.owners[key],key).cell,
            s.logical[key].value == match crate::storage::value(images[c.owners[key]],labels[key]) {
                Some(v) => Some(code(v)),None => None },
            p::replica(s,c.owners[key],key).role is Serving,
            p::replica(s,c.owners[key],key).epoch == 0,
            p::drained(s,c.keys),p::inv(c,s),
            exists|states:Seq<p::State>| p::behavior(c,states) && states.last() == s,
    {
        assert(c.shards.contains(c.owners[key]));
        assert(s.physical.contains_key((c.owners[key],key)));
        assert(p::replica(s,c.owners[key],key).role is Serving);
    }
}
} // verus!
