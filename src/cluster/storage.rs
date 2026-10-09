//! Bounded-space exact migration over native ordered storage cursors.
//! Store is implemented by the checked participant-borrowing transfer adapter.
//! RawStore primitives and authenticated Source scans are the named engine/RPC
//! boundaries. They never reenter ordinary leases or turn errors into EOF.
use vstd::prelude::*;
use crate::types::*;
use crate::bytes::compare;
#[cfg(verus_keep_ghost)]
use crate::bytes::cmp_spec;
#[cfg(verus_keep_ghost)]
#[path = "storage_proofs.rs"]
mod proofs;

verus! {

pub open spec fn same_plan(a: MigrationPlan, b: MigrationPlan) -> bool {
    a.generation == b.generation && a.nonce == b.nonce && a.source == b.source
    && a.destination == b.destination && a.range.table == b.range.table
    && a.range.lo@ == b.range.lo@
    && match (a.range.hi,b.range.hi) {
        (None,None) => true, (Some(x),Some(y)) => x@ == y@, _ => false,
    }
    && a.old@.len() == b.old@.len()
    && forall|i: int| 0 <= i < a.old@.len() ==>
        a.old@[i].start@ == b.old@[i].start@ && a.old@[i].grant == b.old@[i].grant
}

pub(crate) fn plans_match(a: &MigrationPlan, b: &MigrationPlan) -> (yes: bool)
    ensures yes == same_plan(*a,*b),
{
    if a.generation != b.generation || a.nonce.client != b.nonce.client
        || a.nonce.sequence != b.nonce.sequence || a.source != b.source
        || a.destination != b.destination || a.range.table != b.range.table
        || compare(&a.range.lo,&b.range.lo) != 0 { return false; }
    match (&a.range.hi,&b.range.hi) {
        (None,None) => {},
        (Some(x),Some(y)) => { if compare(x,y) != 0 { return false; } },
        _ => return false,
    }
    if a.old.len() != b.old.len() { return false; }
    let mut i = 0usize;
    while i < a.old.len()
        invariant i <= a.old.len(), a.old.len() == b.old.len(),
            forall|j: int| 0 <= j < i ==> a.old@[j].start@ == b.old@[j].start@
                && a.old@[j].grant == b.old@[j].grant,
        decreases a.old.len() - i,
    {
        if compare(&a.old[i].start,&b.old[i].start) != 0
            || a.old[i].grant.owner != b.old[i].grant.owner
            || a.old[i].grant.epoch != b.old[i].grant.epoch { return false; }
        i += 1;
    }
    true
}

/// Non-cloneable capability. There is deliberately no public constructor.
/// Borrowing binds the complete immutable plan without duplicating its routing map.
pub struct CompletedCopy<'a> {
    plan: &'a MigrationPlan,
    owner: u32,
    round: u64,
}
pub struct CompletedCleanup<'a> {
    plan: &'a MigrationPlan,
    owner: u32,
    terminal: bool,
}
impl<'a> CompletedCopy<'a> {
    pub closed spec fn matches_spec(&self,plan: MigrationPlan,owner: u32) -> bool {
        same_plan(*self.plan,plan) && self.owner == owner && owner == plan.destination && self.round == 1
    }
    pub fn matches(&self, plan: &MigrationPlan, owner: u32) -> (yes: bool)
        ensures yes == self.matches_spec(*plan,owner),
    {
        self.owner == owner && owner == plan.destination && self.round == 1
            && plans_match(self.plan,plan)
    }
}
impl<'a> CompletedCleanup<'a> {
    pub closed spec fn matches_spec(&self,plan: MigrationPlan,owner: u32) -> bool {
        same_plan(*self.plan,plan) && self.owner == owner
            && (owner == plan.source || owner == plan.destination) && self.terminal
    }
    pub fn matches(&self, plan: &MigrationPlan, owner: u32) -> (yes: bool)
        ensures yes == self.matches_spec(*plan,owner),
    {
        self.owner == owner && (owner == plan.source || owner == plan.destination)
            && self.terminal && plans_match(self.plan,plan)
    }
}

/// Logical identity includes both routing coordinate and physical row key.
pub type Identity = (Seq<u8>, Seq<u8>);
pub type Cell = (u64, Identity);
pub type Image = Map<Cell, Seq<u8>>;
pub open spec fn identity(row: Row) -> Identity { (row.coordinate@,row.key@) }
pub open spec fn row_cell(table: u64, row: Row) -> Cell { (table,identity(row)) }
pub open spec fn order(a: Identity,b: Identity) -> int {
    if a.0 == b.0 { cmp_spec(a.1,b.1) } else { cmp_spec(a.0,b.0) }
}
pub open spec fn in_range(r: KeyRange,k: Cell) -> bool {
    crate::bytes::contains_spec(r,k.0,k.1.0)
}
pub open spec fn beyond(after: Option<Identity>,k: Identity) -> bool {
    match after { None => true, Some(a) => order(a,k) < 0 }
}
pub open spec fn value(image: Image,k: Cell) -> Option<Seq<u8>> {
    if image.dom().contains(k) { Some(image[k]) } else { None }
}
pub open spec fn row_option(row: Option<Row>) -> Option<Identity> {
    match row { None => None, Some(r) => Some(identity(r)) }
}
/// None proves absence over the ENTIRE remaining range, not only visited rows.
pub open spec fn first(image: Image,r: KeyRange,after: Option<Identity>,row: Option<Row>) -> bool {
    match row {
        None => forall|k: Cell| in_range(r,k) && beyond(after,k.1) ==> !image.dom().contains(k),
        Some(x) => in_range(r,row_cell(r.table,x)) && beyond(after,identity(x))
            && value(image,row_cell(r.table,x)) == Some(x.value@)
            && forall|k: Cell| in_range(r,k) && beyond(after,k.1)
                && image.dom().contains(k) ==> order(identity(x),k.1) <= 0,
    }
}
pub open spec fn mirrored(current: Image,initial: Image,source: Image,r: KeyRange,after: Option<Identity>) -> bool {
    forall|k: Cell| value(current,k) ==
        if in_range(r,k) && !beyond(after,k.1) { value(source,k) } else { value(initial,k) }
}
pub open spec fn complete(current: Image,initial: Image,source: Image,r: KeyRange) -> bool {
    forall|k: Cell| value(current,k) == if in_range(r,k) { value(source,k) } else { value(initial,k) }
}
pub open spec fn preserves_outside(current: Image,initial: Image,r: KeyRange) -> bool {
    forall|k: Cell| !in_range(r,k) ==> value(current,k) == value(initial,k)
}
/// Error returns preserve the successfully processed ordered prefix; they are
/// not whole-handler stutters. The witness includes scan-certified gaps.
pub open spec fn prefix(current:Image,initial:Image,source:Image,r:KeyRange) -> bool {
    exists|cursor:Option<Identity>| mirrored(current,initial,source,r,cursor)
}
pub proof fn prefix_at(current:Image,initial:Image,source:Image,r:KeyRange,cursor:Option<Identity>)
    requires mirrored(current,initial,source,r,cursor),
    ensures prefix(current,initial,source,r),
{}

/// Authenticated frozen-source RPC boundary. Each successful scan is over the
/// same image and rechecks Frozen/generation/exact drain. Abort/failure is Err,
/// never EOF. A response captured before abort remains authentic old data.
pub trait Source {
    spec fn image(&self) -> Image;
    spec fn wf(&self) -> bool;
    fn scan(&self,plan: &MigrationPlan,after: Option<&Row>) -> (result: Result<Option<Row>,Status>)
        requires self.wf(),
        ensures result is Ok ==> first(self.image(),plan.range,
            match after { None => None, Some(x) => Some(identity(*x)) },result->Ok_0);
}

/// Checked adapter boundary, implemented by transfer::GuardedStore, not FFI.
/// The image is a selected-range window. Rows outside that window are absent
/// from this view; unrelated admitted transactions may mutate outside storage.
/// Consequently preserves_outside frames this view, not the whole engine.
/// Each raw native mutation separately has a physical one-key frame.
/// The adapter retains one participant borrow, binding the complete plan:
/// final copy requires destination Stage/nonterminal/round=1; cleanup requires
/// Empty/terminal and the retained pending cleanup disposition. Frozen source
/// reads require exact drain. Nonreplicated handoff plus admission and drain
/// provide window exclusion; a metadata mutex alone is not a database lock.
/// An engine error aborts that primitive without altering any selected cell.
pub trait Store {
    type Context;
    /// Erased immutable authority frame retained across generic driver calls.
    #[verifier::prophetic]
    spec fn context(&self) -> Self::Context;
    spec fn image(&self) -> Image;
    spec fn wf(&self) -> bool;
    spec fn authorized(&self,plan: MigrationPlan,owner: u32,cleanup: bool) -> bool;
    fn validate(&self,plan: &MigrationPlan,owner: u32,cleanup: bool) -> (status: Status)
        requires self.wf(),
        ensures status == Status::Ok ==> self.authorized(*plan,owner,cleanup);
    fn scan(&self,plan: &MigrationPlan,after: Option<&Row>) -> (result: Result<Option<Row>,Status>)
        requires self.wf(),
        ensures result is Ok ==> first(self.image(),plan.range,
            match after { None => None, Some(x) => Some(identity(*x)) },result->Ok_0);
    fn put(&mut self,plan: &MigrationPlan,owner: u32,row: &Row) -> (status: Status)
        requires old(self).wf(), old(self).authorized(*plan,owner,false),
            in_range(plan.range,row_cell(plan.range.table,*row)),
        ensures final(self).wf(), final(self).authorized(*plan,owner,false),
            final(self).context() == old(self).context(),
            status == Status::Ok ==> final(self).image()
                == old(self).image().insert(row_cell(plan.range.table,*row),row.value@),
            status != Status::Ok ==> final(self).image() == old(self).image();
    fn delete(&mut self,plan: &MigrationPlan,owner: u32,row: &Row,cleanup: bool) -> (status: Status)
        requires old(self).wf(), old(self).authorized(*plan,owner,cleanup),
            in_range(plan.range,row_cell(plan.range.table,*row)),
        ensures final(self).wf(), final(self).authorized(*plan,owner,cleanup),
            final(self).context() == old(self).context(),
            status == Status::Ok ==> final(self).image()
                == old(self).image().remove(row_cell(plan.range.table,*row)),
            status != Status::Ok ==> final(self).image() == old(self).image();
}
fn compare_rows(a: &Row,b: &Row) -> (result: i32)
    ensures result as int == order(identity(*a),identity(*b)),
{
    let c = compare(&a.coordinate,&b.coordinate);
    if c == 0 { compare(&a.key,&b.key) } else { c }
}

/// Merge complete ordered streams with two lookaheads and one consumed row.
pub fn mirror<'a,S: Store,R: Source>(plan: &'a MigrationPlan,owner: u32,store: &mut S,source: &R)
    -> (result: Result<CompletedCopy<'a>,Status>)
    requires old(store).wf(), source.wf(),
    ensures final(store).wf(), result is Ok ==> result->Ok_0.matches_spec(*plan,owner)
        && complete(final(store).image(),old(store).image(),source.image(),plan.range),
        preserves_outside(final(store).image(),old(store).image(),plan.range),
        final(store).context() == old(store).context(),
        prefix(final(store).image(),old(store).image(),source.image(),plan.range),
{
    proof { prefix_at(store.image(),store.image(),source.image(),plan.range,None); }
    if owner != plan.destination { return Err(Status::Invalid); }
    let status = store.validate(plan,owner,false);
    match status { Status::Ok => {}, _ => return Err(status) }
    let ghost initial = store.image();
    let mut src = match source.scan(plan,None) { Ok(row) => row, Err(e) => return Err(e) };
    let mut dst = match store.scan(plan,None) { Ok(row) => row, Err(e) => return Err(e) };
    let mut after: Option<Row> = None;
    while src.is_some() || dst.is_some()
        invariant store.wf(),source.wf(),store.authorized(*plan,owner,false),
            initial == old(store).image(),
            store.context() == old(store).context(),
            first(source.image(),plan.range,row_option(after),src),
            first(initial,plan.range,row_option(after),dst),
            mirrored(store.image(),initial,source.image(),plan.range,row_option(after)),
            preserves_outside(store.image(),initial,plan.range),
            prefix(store.image(),initial,source.image(),plan.range),
        decreases proofs::remaining(initial,source.image(),plan.range,row_option(after)).len(),
    {
        let ghost before = row_option(after);
        let ghost current = store.image();
        let ghost s = src;
        let ghost d = dst;
        let comparison = match (&src,&dst) {
            (Some(a),Some(b)) => compare_rows(a,b), (Some(_),None) => -1, _ => 1,
        };
        if comparison <= 0 {
            let row = src.unwrap();
            proof { proofs::step(current,initial,source.image(),plan.range,before,s,d,row); }
            let status = store.put(plan,owner,&row);
            proof { proofs::frame_effect(current,initial,store.image(),plan.range,row_cell(plan.range.table,row),Some(row.value@)); }
            match status { Status::Ok => {}, _ => return Err(status) }
            proof {
                assert(store.image() == proofs::effect(current,source.image(),plan.range,row));
                proofs::progress(initial,source.image(),plan.range,before,row);
                if comparison < 0 { proofs::advance_cached(initial,plan.range,before,d,identity(row)); }
                prefix_at(store.image(),initial,source.image(),plan.range,Some(identity(row)));
            }
            after = Some(row);
            src = match source.scan(plan,after.as_ref()) { Ok(row) => row, Err(e) => return Err(e) };
            if comparison == 0 {
                dst = match store.scan(plan,after.as_ref()) { Ok(row) => row, Err(e) => return Err(e) };
                proof { proofs::scan_frame(store.image(),initial,source.image(),plan.range,row_option(after),dst); }
            }
        } else {
            let row = dst.unwrap();
            proof {
                if let Some(x) = s { proofs::order_laws(identity(row),identity(x)); }
                proofs::step(current,initial,source.image(),plan.range,before,s,d,row);
                assert(!source.image().dom().contains(row_cell(plan.range.table,row)));
            }
            let status = store.delete(plan,owner,&row,false);
            proof { proofs::frame_effect(current,initial,store.image(),plan.range,row_cell(plan.range.table,row),None); }
            match status { Status::Ok => {}, _ => return Err(status) }
            proof {
                assert(store.image() == proofs::effect(current,source.image(),plan.range,row));
                proofs::progress(initial,source.image(),plan.range,before,row);
                proofs::advance_cached(source.image(),plan.range,before,s,identity(row));
                prefix_at(store.image(),initial,source.image(),plan.range,Some(identity(row)));
            }
            after = Some(row);
            dst = match store.scan(plan,after.as_ref()) { Ok(row) => row, Err(e) => return Err(e) };
            proof { proofs::scan_frame(store.image(),initial,source.image(),plan.range,row_option(after),dst); }
        }
    }
    proof { proofs::finish(store.image(),initial,source.image(),plan.range,row_option(after)); }
    Ok(CompletedCopy { plan,owner,round: 1 })
}

/// Delete precisely the guarded range, preserving every other table and row.
pub fn cleanup<'a,S: Store>(plan: &'a MigrationPlan,owner: u32,store: &mut S)
    -> (result: Result<CompletedCleanup<'a>,Status>)
    requires old(store).wf(),
    ensures final(store).wf(), result is Ok ==> result->Ok_0.matches_spec(*plan,owner)
        && complete(final(store).image(),old(store).image(),Map::empty(),plan.range),
        prefix(final(store).image(),old(store).image(),Map::empty(),plan.range),
        preserves_outside(final(store).image(),old(store).image(),plan.range),
        final(store).context() == old(store).context(),
{
    proof { prefix_at(store.image(),store.image(),Map::empty(),plan.range,None); }
    if owner != plan.source && owner != plan.destination { return Err(Status::Invalid); }
    let status = store.validate(plan,owner,true);
    match status { Status::Ok => {}, _ => return Err(status) }
    let ghost initial = store.image();
    let mut after: Option<Row> = None;
    let mut dst = match store.scan(plan,None) { Ok(row) => row, Err(e) => return Err(e) };
    while dst.is_some()
        invariant store.wf(),store.authorized(*plan,owner,true),
            initial == old(store).image(),
            store.context() == old(store).context(),
            first(initial,plan.range,row_option(after),dst),
            mirrored(store.image(),initial,Map::empty(),plan.range,row_option(after)),
            preserves_outside(store.image(),initial,plan.range),
            prefix(store.image(),initial,Map::empty(),plan.range),
        decreases proofs::remaining(initial,Map::empty(),plan.range,row_option(after)).len(),
    {
        let ghost current = store.image();
        let ghost before = row_option(after);
        let ghost d = dst;
        let row = dst.unwrap();
        proof { proofs::step(current,initial,Map::empty(),plan.range,before,None,d,row); }
        let status = store.delete(plan,owner,&row,true);
        proof { proofs::frame_effect(current,initial,store.image(),plan.range,row_cell(plan.range.table,row),None); }
        match status { Status::Ok => {}, _ => return Err(status) }
        proof { proofs::progress(initial,Map::empty(),plan.range,before,row); }
        after = Some(row);
        proof { prefix_at(store.image(),initial,Map::empty(),plan.range,row_option(after)); }
        dst = match store.scan(plan,after.as_ref()) { Ok(row) => row, Err(e) => return Err(e) };
        proof { proofs::scan_frame(store.image(),initial,Map::empty(),plan.range,row_option(after),dst); }
    }
    proof { proofs::finish(store.image(),initial,Map::empty(),plan.range,row_option(after)); }
    Ok(CompletedCleanup { plan,owner,terminal: true })
}

} // verus!
