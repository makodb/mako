//! One physical timeline is shared by every issued job, cached response and
//! network receipt. Cell is the actual native cell type at instantiation; the
//! dense labels are an exhaustive inventory, not a replacement physical store.
use super::*;
use super::{actual as a,io};
use crate::sharding_bytes;

verus! {
#[verifier::reject_recursive_types(C)]
pub struct Window<C> { pub contains:spec_fn(C)->bool, pub logical:Map<int,C> }
#[verifier::reject_recursive_types(C)]
pub struct Job<C> {
    pub id:nat, pub generation:nat, pub owner:int, pub origin:nat,
    pub labels:Seq<C>, pub run:io::Execution,
}
#[verifier::reject_recursive_types(C)]
pub struct World<C> {
    pub images:spec_fn(nat,int)->Map<C,Seq<u8>>,
    pub windows:spec_fn(nat)->Window<C>,
    pub jobs:spec_fn(nat)->Job<C>,
    pub issued:spec_fn(nat)->Set<nat>,
}
impl<C> World<C> {
    pub open spec fn image(self,t:nat,owner:int)->Map<C,Seq<u8>> { (self.images)(t,owner) }
    pub open spec fn window(self,g:nat)->Window<C> { (self.windows)(g) }
    pub open spec fn job(self,id:nat)->Job<C> { (self.jobs)(id) }
    pub open spec fn issued_at(self,t:nat)->Set<nat> { (self.issued)(t) }
}
impl<C> Window<C> { pub open spec fn includes(self,k:C)->bool { (self.contains)(k) } }
pub open spec fn indices<C>(labels:Seq<C>)->Set<nat> {
    Seq::new(labels.len(),|i:int| i as nat).to_set()
}
pub open spec fn value<C>(image:Map<C,Seq<u8>>,k:C)->Option<Seq<u8>> {
    if image.contains_key(k) { Some(image[k]) } else { None }
}
pub open spec fn encode<C>(image:Map<C,Seq<u8>>,labels:Seq<C>)->Map<nat,Seq<u8>> {
    Map::new(indices(labels).filter(|i:nat| image.contains_key(labels[i as int])),|i:nat| image[labels[i as int]])
}
pub open spec fn inventory<C>(image:Map<C,Seq<u8>>,window:Window<C>,labels:Seq<C>)->bool {
    forall|k:C| window.includes(k) && image.contains_key(k) ==> labels.to_set().contains(k)
}
pub open spec fn outside<C>(image:Map<C,Seq<u8>>,initial:Map<C,Seq<u8>>,window:Window<C>)->bool {
    forall|k:C| !window.includes(k) ==> value(image,k) == value(initial,k)
}
pub open spec fn complete<C>(image:Map<C,Seq<u8>>,initial:Map<C,Seq<u8>>,source:Map<C,Seq<u8>>,window:Window<C>)->bool {
    forall|k:C| value(image,k) == if window.includes(k) { value(source,k) } else { value(initial,k) }
}
pub open spec fn until_done<C>(j:Job<C>,t:nat)->bool {
    j.origin <= t && forall|u:nat| j.origin <= u < t ==> !(j.run.state(u).stage is Done)
}
pub open spec fn logical_window<C>(b:a::Execution,w:World<C>,g:nat,t:nat)->bool {
    b.state(t).plans.contains_key(g)
        && w.window(g).logical.dom() == b.state(t).plans[g].keys
        && (forall|k:int| w.window(g).logical.contains_key(k) ==> w.window(g).includes(w.window(g).logical[k]))
        && (forall|k:int,l:int| w.window(g).logical.contains_key(k) && w.window(g).logical.contains_key(l)
            && w.window(g).logical[k] == w.window(g).logical[l] ==> k == l)
}
/// Exact initialization is indispensable: a terminal metadata mask is never
/// used to construct the raw image, and a pre-Done job needs its real history.
pub open spec fn job_bound<C>(b:a::Execution,w:World<C>,g:nat,id:nat)->bool {
    let j = w.job(id); let win = w.window(g); let s = b.state(j.origin);
    let initial = w.image(j.origin,j.owner);
    let source = if j.run.state(j.origin).kind is Mirror { w.image(j.origin,s.plans[g].src) } else { Map::empty() };
    &&& j.id == id && j.generation == g && w.issued_at(j.origin).contains(id)
    &&& logical_window(b,w,g,j.origin)
    &&& j.owner == s.plans[g].src || j.owner == s.plans[g].dst
    &&& j.labels.no_duplicates()
    &&& forall|i:int| 0 <= i < j.labels.len() ==> win.includes(j.labels[i])
    &&& io::runs(j.run,j.origin)
    &&& j.run.state(j.origin) == io::initial(j.run.state(j.origin).kind,encode(source,j.labels),encode(initial,j.labels),j.labels.len())
    &&& if j.run.state(j.origin).kind is Local { j.labels.len() == 0 }
        else {
            inventory(initial,win,j.labels) && inventory(source,win,j.labels)
            && (j.run.state(j.origin).kind is Mirror ==> j.owner == s.plans[g].dst
                && p::before_decision(s,g) && p::source_frozen(s,g) && p::drained(s,s.plans[g].keys)
                && (forall|k:int| win.logical.contains_key(k) ==> j.labels.to_set().contains(win.logical[k])))
            && (j.run.state(j.origin).kind is Cleanup ==>
                j.owner == s.plans[g].src && s.phases[g] is Committed
                    || j.owner == s.plans[g].dst && s.phases[g] is Aborted)
        }
    &&& forall|t:nat| #[trigger] until_done(j,t) ==> {
        &&& logical_window(b,w,g,t)
        &&& j.run.state(t).image == encode(w.image(t,j.owner),j.labels)
        // Local metadata/receipt continuations hold no storage borrow and must
        // not suppress newly admitted ordinary transactions.
        &&& !(j.run.state(j.origin).kind is Local) ==>
            outside(w.image(t,j.owner),initial,win) && inventory(w.image(t,j.owner),win,j.labels)
        &&& j.run.state(j.origin).kind is Mirror && p::before_decision(b.state(t),g) ==> {
            &&& encode(w.image(t,s.plans[g].src),j.labels) == encode(source,j.labels)
            &&& forall|k:int| win.logical.contains_key(k) ==> p::replica(b.state(t),s.plans[g].src,k).cell.value
                == sharding_bytes::value_option(value(source,win.logical[k]))
        }
    }
}
pub proof fn index_member<C>(labels:Seq<C>,i:nat)
    ensures indices(labels).contains(i) == (i < labels.len())
{
    if i < labels.len() { assert(Seq::new(labels.len(),|j:int| j as nat)[i as int] == i); }
}
pub proof fn lookup<C>(image:Map<C,Seq<u8>>,labels:Seq<C>,i:nat)
    requires i < labels.len()
    ensures io::value(encode(image,labels),i) == value(image,labels[i as int])
{ index_member(labels,i); }
pub proof fn exact_completion<C>(image:Map<C,Seq<u8>>,initial:Map<C,Seq<u8>>,source:Map<C,Seq<u8>>,win:Window<C>,labels:Seq<C>)
    requires inventory(image,win,labels),inventory(source,win,labels),encode(image,labels) == encode(source,labels),outside(image,initial,win)
    ensures complete(image,initial,source,win)
{
    assert forall|k:C| value(image,k) == if win.includes(k) { value(source,k) } else { value(initial,k) } by {
        if win.includes(k) && (image.contains_key(k) || source.contains_key(k)) {
            assert(labels.to_set().contains(k));
            let i = choose|i:int| 0 <= i < labels.len() && labels[i] == k;
            lookup(image,labels,i as nat); lookup(source,labels,i as nat);
        }
    }
}
pub proof fn job_completion<C>(b:a::Execution,w:World<C>,g:nat,id:nat,end:nat)->(u:nat)
    requires job_bound(b,w,g,id),end >= w.job(id).origin,w.job(id).run.state(end).stage is Done,
        !(w.job(id).run.state(w.job(id).origin).kind is Local)
    ensures w.job(id).origin <= u <= end,w.job(id).run.state(u).stage is Done,
        w.job(id).generation == g,
        complete(w.image(u,w.job(id).owner),w.image(w.job(id).origin,w.job(id).owner),
            if w.job(id).run.state(w.job(id).origin).kind is Mirror {
                w.image(w.job(id).origin,b.state(w.job(id).origin).plans[g].src)
            } else { Map::empty() },w.window(g))
{
    let j = w.job(id);
    let u = io::first_completion(j.run,j.origin,end);
    io::state_wf(j.run,j.origin,u);
    assert(until_done(j,u));
    let source = if j.run.state(j.origin).kind is Mirror { w.image(j.origin,b.state(j.origin).plans[g].src) } else { Map::empty() };
    exact_completion(w.image(u,j.owner),w.image(j.origin,j.owner),source,w.window(g),j.labels);
    u
}
/// A raw result can be reused for several ghost keys in the same cursor gap.
/// Its time, request, actual job and returned first row remain authenticated.
pub struct Evidence { pub job:nat,pub time:nat,pub request:io::Request }
pub open spec fn raw_evidence<C>(b:a::Execution,w:World<C>,g:nat,t:nat,e:Evidence)->bool {
    let j = w.job(e.job);
    job_bound(b,w,g,e.job) && j.origin <= e.time <= t && until_done(j,e.time)
        && j.run.tick(e.time) is Ok && io::request(j.run.state(e.time)) == e.request
}
pub open spec fn gap(image:Map<nat,Seq<u8>>,from:nat,row:Option<nat>,key:nat)->bool {
    io::first(image,from,row) && from <= key && (row is None || key <= row.unwrap())
}
pub open spec fn covers_absence(image:Map<nat,Seq<u8>>,from:nat,row:Option<nat>,key:nat)->bool {
    gap(image,from,row,key) && (row is None || key < row.unwrap())
}
pub open spec fn atomic_data<C>(b:a::Execution,w:World<C>,g:nat,t:nat,action:p::Action,e:Evidence)->bool {
    let j = w.job(e.job); let win = w.window(g); let run = j.run.state(e.time);
    &&& raw_evidence(b,w,g,t,e) && run.kind is Mirror
    &&& until_done(j,t)
    &&& match action {
        p::Action::Capture { generation,round,key } => {
            &&& generation == g && round == 1 && win.logical.contains_key(key)
            &&& exists|i:nat| i < j.labels.len() && j.labels[i as int] == win.logical[key]
                && match e.request {
                    io::Request::SourceScan { from } => {
                        let row = io::scan(run.source,from,run.bound);
                        gap(run.source,from,row,i)
                            && p::replica(b.state(t),b.state(t).plans[g].src,key).cell.value
                                == sharding_bytes::value_option(io::value(run.source,i))
                    },
                    _ => false,
                }
        },
        p::Action::DeliverCopy { packet } => {
            &&& packet.generation == g && packet.round == 1 && win.logical.contains_key(packet.key)
            &&& exists|i:nat| i < j.labels.len() && j.labels[i as int] == win.logical[packet.key]
                && sharding_bytes::value_option(value(w.image(t+1,j.owner),win.logical[packet.key])) == packet.cell.value
                && match e.request {
                    io::Request::Put { key,value } => key == i && packet.cell.value == Some(sharding_bytes::value_code(value)),
                    io::Request::Delete { key } => key == i && packet.cell.value == None,
                    io::Request::DestinationScan { from } => packet.cell.value == None
                        && covers_absence(run.image,from,io::scan(run.image,from,run.bound),i),
                    _ => false,
                }
        },
        _ => false,
    }
}
} // verus!
