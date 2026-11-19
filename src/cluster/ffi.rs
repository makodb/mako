//! ABI lifetimes only. Non-null foreign pointers must designate valid aligned
//! objects; spans remain borrowed for the call. Callbacks cannot unwind. Engine
//! handles/contexts outlive stop_node; dispatch transfers its callback only on OK.
#![allow(unsafe_code)]
use std::ffi::c_void;
use std::cell::RefCell;
use crate::epoch_admission::AdmissionSnapshot;
use std::sync::Arc;
use std::sync::atomic::{AtomicU64, Ordering};
use crate::catalog::{Coordinates, TableKind};
use crate::host::{self, AdminRpc, Bytes, Completion, Host, HostAbi, Opener, ReplySink, WarehouseOpen};
use crate::runtime::{runtime, Node};
use crate::types::{Grant, RoutedGrant, Status, TxnId};
use crate::warehouse::HandleKey;
use crate::wire::{self, checked};

fn code(result: Result<(),Status>) -> u32 { match result {Ok(())=>Status::Ok as u32,Err(s)=>s as u32} }
fn flag(value: u32) -> Result<bool,Status> {match value {0=>Ok(false),1=>Ok(true),_=>Err(Status::Invalid)}}
unsafe fn upper<'a>(has_hi: u32, hi: Bytes) -> Result<Option<&'a [u8]>,Status> {
    if flag(has_hi)? {Ok(Some(unsafe {hi.slice()?}))} else {Ok(None)}
}
unsafe fn output<T>(pointer: *mut T, value: T) -> Result<(),Status> {
    if pointer.is_null() {return Err(Status::Invalid);}
    unsafe {pointer.write(value)};
    Ok(())
}
#[no_mangle]
pub extern "C" fn mako_sharding_enabled() -> u32 {u32::from(runtime().enabled())}
#[no_mangle]
pub extern "C" fn mako_sharding_active(owner: u32) -> u32 {u32::from(runtime().node(owner).is_ok())}
#[no_mangle]
pub extern "C" fn mako_sharding_ready(owner: u32) -> u32 {
    u32::from(runtime().node(owner).is_ok_and(|node|node.ready()))
}
#[no_mangle]
pub extern "C" fn mako_sharding_wait_ready(owner: u32) -> u32 {
    code(runtime().node(owner).and_then(|node|node.wait_ready()))
}
#[no_mangle]
pub extern "C" fn mako_sharding_replication_replay(replicated: u32) -> u32 {
    code(flag(replicated).and_then(|replicated|runtime().replication_replay(replicated)))
}
#[no_mangle]
pub unsafe extern "C" fn mako_sharding_quiesce(owner: u32, directory: Bytes) -> u32 {
    code((|| {
        let directory = std::str::from_utf8(unsafe {directory.slice()?}).map_err(|_|Status::Invalid)?;
        runtime().node(owner)?.quiesce(directory)
    })())
}
#[no_mangle]
pub unsafe extern "C" fn mako_sharding_route(table: u64, coordinate: Bytes, route: *mut RoutedGrant) -> u32 {
    code((|| {if route.is_null() {return Err(Status::Invalid);}
        let value = runtime().route(table,unsafe {coordinate.slice()?})?;
        unsafe {output(route,value)} })())
}
/// Thread-bound borrowed handle, valid only until read_end on this same thread.
/// The stable TLS allocation is reused; stale handles must never be reused.
#[repr(C)]
pub struct MakoShardRead { _private: [u8;0] }

struct CapturedRead<N=Node> { node: Arc<N>, view: Arc<AdmissionSnapshot> }
trait ReadIdentity {
    fn same_view(&self, other:&Self) -> bool;
}
impl<N> ReadIdentity for CapturedRead<N> {
    fn same_view(&self, other:&Self) -> bool {
        Arc::ptr_eq(&self.node,&other.node) && Arc::ptr_eq(&self.view,&other.view)
    }
}
// Zero is reserved. Never wrap, even after a worker/cache is destroyed.
static READ_VIEW_IDS: AtomicU64 = AtomicU64::new(0);
fn allocate_read_view_id(ids:&AtomicU64) -> Result<u64,Status> {
    ids.fetch_update(Ordering::Relaxed,Ordering::Relaxed,|last|last.checked_add(1))
        .map(|last|last+1).map_err(|_|Status::Exhausted)
}
struct ReadCache<T> {
    active: bool,
    cached: Option<(u32,u64,T)>,
    view_id: u64,
}
impl<T:ReadIdentity> ReadCache<T> {
    fn new() -> Self { Self {active:false,cached:None,view_id:0} }
    fn begin(&mut self, owner:u32, epoch:u64, load:impl FnOnce()->Result<T,Status>,
             live:impl FnOnce(&T)->Result<(),Status>) -> Result<(),Status> {
        self.begin_with_ids(owner,epoch,load,live,&READ_VIEW_IDS)
    }
    fn begin_with_ids(&mut self, owner:u32, epoch:u64, load:impl FnOnce()->Result<T,Status>,
                      live:impl FnOnce(&T)->Result<(),Status>, ids:&AtomicU64) -> Result<(),Status> {
        if self.active {return Err(Status::Busy);}
        if epoch==0 {return Err(Status::Invalid);}
        if !matches!(&self.cached,Some((o,e,_)) if *o==owner && *e==epoch) {
            let next=load()?;
            live(&next)?;
            // Compare while both owned views remain alive: allocator address
            // reuse cannot turn a changed Arc into a cache hit.
            let unchanged=matches!(&self.cached,Some((o,_,old)) if *o==owner && old.same_view(&next));
            let id=if unchanged {self.view_id} else {allocate_read_view_id(ids)?};
            // Failed load, readiness or ID allocation leaves the old cache intact.
            self.cached=Some((owner,epoch,next));
            self.view_id=id;
        } else {
            live(&self.cached.as_ref().ok_or(Status::Invalid)?.2)?;
        }
        self.active=true;
        Ok(())
    }
    fn get(&self) -> Result<&T,Status> {
        if !self.active {return Err(Status::Invalid);}
        self.cached.as_ref().map(|(_,_,value)|value).ok_or(Status::Invalid)
    }
    fn id(&self) -> Result<u64,Status> {
        self.get()?;
        Ok(self.view_id)
    }
    fn end(&mut self) {self.active=false;}
}
thread_local! {
    // One allocation per worker, not per transaction. Only cache misses clone
    // Node/snapshot Arcs. RefCell is thread-local borrowing, never a mutex.
    static READ: RefCell<Box<ReadCache<CapturedRead>>> = RefCell::new(Box::new(ReadCache::new()));
}
fn read_pointer(cache:&ReadCache<CapturedRead>) -> *const MakoShardRead {
    (cache as *const ReadCache<CapturedRead>).cast()
}
fn with_read<T>(read:*const MakoShardRead, call:impl FnOnce(&CapturedRead)->Result<T,Status>) -> Result<T,Status> {
    READ.with(|slot| {
        let cache=slot.try_borrow().map_err(|_|Status::Busy)?;
        if read.is_null() || read!=read_pointer(&cache) {return Err(Status::Invalid);}
        let captured=cache.get()?;
        call(captured)
    })
}
/// SAFETY: out is writable. Caller has already entered nonzero STO epoch and
/// keeps it pinned through actual engine cleanup and read_end. No current-epoch
/// commit recheck is performed. Nested operations reuse the existing handle.
/// Readiness, stop and metadata-fence errors reject new contexts here, including
/// cache hits; admitted work retains its immutable view through cleanup.
#[no_mangle]
pub unsafe extern "C" fn mako_sharding_read_begin(owner:u32, epoch:u64, out:*mut *const MakoShardRead) -> u32 {
    code((|| {
        if out.is_null() {return Err(Status::Invalid);}
        unsafe {out.write(std::ptr::null())};
        READ.with(|slot| {
            let mut cache=slot.try_borrow_mut().map_err(|_|Status::Busy)?;
            cache.begin(owner,epoch,|| {
                let node=runtime().node(owner)?;
                node.admission_ready()?;
                let view=node.admission_view()?;
                Ok(CapturedRead {node,view})
            },|captured|captured.node.admission_ready())?;
            unsafe {out.write(read_pointer(&cache))};
            Ok(())
        })
    })())
}
/// SAFETY: engine effects, unlocks and cleanup precede this same-thread call;
/// no read/scan callback may overlap it. The caller leaves STO only afterward.
#[no_mangle]
pub unsafe extern "C" fn mako_sharding_read_end(read:*const MakoShardRead) {
    READ.with(|slot| {
        if let Ok(mut cache)=slot.try_borrow_mut() {
            if !read.is_null() && read==read_pointer(&cache) {cache.end();}
        }
    });
}
/// SAFETY: out is writable. The ID describes this protected view, not permission
/// to perform effects. Failed calls clear out; active same-thread checks apply.
#[no_mangle]
pub unsafe extern "C" fn mako_sharding_read_view_id(read:*const MakoShardRead, out:*mut u64) -> u32 {
    code((|| {
        if out.is_null() {return Err(Status::Invalid);}
        unsafe {out.write(0)};
        READ.with(|slot| {
            let cache=slot.try_borrow().map_err(|_|Status::Busy)?;
            if read.is_null() || read!=read_pointer(&cache) {return Err(Status::Invalid);}
            let id=cache.id()?;
            unsafe {out.write(id)};
            Ok(())
        })
    })())
}
/// SAFETY: coordinate and output storage are valid for the synchronous call.
#[no_mangle]
pub unsafe extern "C" fn mako_sharding_read_route(read:*const MakoShardRead, table:u64, coordinate:Bytes, out:*mut RoutedGrant) -> u32 {
    code((|| {
        if out.is_null() {return Err(Status::Invalid);}
        let coordinate=unsafe {coordinate.slice()?};
        with_read(read,|captured|unsafe {output(out,captured.view.route(table,coordinate)?)})
    })())
}
/// SAFETY: coordinate bytes remain borrowed for this synchronous call.
#[no_mangle]
pub unsafe extern "C" fn mako_sharding_read_point(read:*const MakoShardRead, table:u64, coordinate:Bytes, route:RoutedGrant) -> u32 {
    code((|| {let coordinate=unsafe {coordinate.slice()?};
        with_read(read,|captured|checked(captured.view.point(table,coordinate,route))) })())
}
/// SAFETY: both spans remain borrowed for this synchronous call.
#[no_mangle]
pub unsafe extern "C" fn mako_sharding_read_range(read:*const MakoShardRead, table:u64, lo:Bytes, has_hi:u32, hi:Bytes, route:RoutedGrant) -> u32 {
    code((|| {let lo=unsafe {lo.slice()?};let hi=unsafe {upper(has_hi,hi)?};
        with_read(read,|captured|checked(captured.view.range(table,lo,hi,route))) })())
}
#[no_mangle]
pub extern "C" fn mako_sharding_catalog_tpcc(micro: u32) -> u32 {
    code(flag(micro).and_then(|micro|runtime().catalog_tpcc(micro)))
}
#[no_mangle]
pub unsafe extern "C" fn mako_sharding_catalog_register(table: u64, name: Bytes, kind: u32, initial_owner: u32) -> u32 {
    code((|| {
        let kind = match kind {0=>TableKind::Governed,1=>TableKind::Replicated,2=>TableKind::Static,_=>return Err(Status::Invalid)};
        checked(runtime().catalog_register(table,unsafe {name.slice()?},kind,Coordinates::Raw,initial_owner))
    })())
}
#[no_mangle]
pub unsafe extern "C" fn mako_sharding_table_id(name: Bytes, table: *mut u64) -> u32 {
    code((|| {let catalog = runtime().catalog.read();
        unsafe {output(table,catalog.by_name(name.slice()?).ok_or(Status::NotFound)?.id)} })())
}
#[no_mangle]
pub unsafe extern "C" fn mako_sharding_table_kind(name: Bytes, table: *mut u64, kind: *mut u32) -> u32 {
    code((|| {
        if table.is_null() || kind.is_null() {return Err(Status::Invalid);}
        let catalog = runtime().catalog.read();
        let entry = catalog.by_name(unsafe {name.slice()?}).ok_or(Status::NotFound)?;
        unsafe {output(table,entry.id)?;output(kind,entry.kind as u32)}
    })())
}
#[no_mangle]
pub unsafe extern "C" fn mako_sharding_table_coordinate(table: u64, coordinates: *mut u32) -> u32 {
    code((|| {let catalog = runtime().catalog.read();
        let entry = catalog.by_id(table).ok_or(Status::NotFound)?;
        unsafe {output(coordinates,if entry.coordinates == Coordinates::Raw {0} else {1})} })())
}
#[no_mangle]
pub extern "C" fn mako_sharding_warehouse_count() -> u32 {
    runtime().warehouses.read().as_ref().map_or(0,|d|d.dimensions().1)
}
#[no_mangle]
pub extern "C" fn mako_sharding_warehouse_init(per_shard: u32, total: u32) -> u32 {
    code(runtime().warehouse_init(per_shard,total))
}
#[no_mangle]
pub unsafe extern "C" fn mako_sharding_warehouse_opener(owner: u32, context: *mut c_void, open: Option<WarehouseOpen>) -> u32 {
    code((|| runtime().warehouse_opener(owner,unsafe {Opener::new(context,open.ok_or(Status::Invalid)?)}))())
}
#[no_mangle]
pub extern "C" fn mako_sharding_warehouse_register(table: u64, warehouse: u32, owner: u32, proxy: u32, handle: usize) -> u32 {
    code((|| {
        let key = HandleKey {table,warehouse,owner,proxy:flag(proxy)?};
        checked(runtime().warehouses.write().as_mut().ok_or(Status::NotFound)?.register(key,handle))
    })())
}
#[no_mangle]
pub unsafe extern "C" fn mako_sharding_warehouse_resolve(table: u64, warehouse: u32, owner: u32, handle: *mut usize, grant: *mut Grant) -> u32 {
    code((|| {
        if handle.is_null() || grant.is_null() {return Err(Status::Invalid);}
        let (h,g) = runtime().warehouse_resolve(table,warehouse,owner)?;
        unsafe {output(handle,h)?;output(grant,g)}
    })())
}
#[no_mangle]
pub unsafe extern "C" fn mako_sharding_warehouse_select(table: u64, warehouse: u32, owner: u32, grant: Grant, handle: *mut usize) -> u32 {
    code((|| {
        if handle.is_null() { return Err(Status::Invalid); }
        let (h,_) = runtime().warehouse_select(table,warehouse,owner,grant)?;
        unsafe { output(handle,h) }
    })())
}
#[no_mangle]
pub unsafe extern "C" fn mako_sharding_warehouse_local(table: u64, warehouse: u32, owner: u32, handle: *mut usize) -> u32 {
    code((|| {if handle.is_null() {return Err(Status::Invalid);}
        unsafe {output(handle,runtime().warehouse_local(table,warehouse,owner)?)} })())
}
#[no_mangle]
pub unsafe extern "C" fn mako_sharding_local_table(table: u64, coordinate: Bytes, fixed: u32, owner: u32, handle: *mut usize) -> u32 {
    code((|| {if handle.is_null() {return Err(Status::Invalid);}
        let handle_value = runtime().local_table(table,unsafe {coordinate.slice()?},flag(fixed)?,owner)?;
        unsafe {output(handle,handle_value)} })())
}
#[no_mangle]
pub unsafe extern "C" fn mako_sharding_raw_proxy(table: u64, owner: u32, handle: *mut usize) -> u32 {
    code((|| {if handle.is_null() {return Err(Status::Invalid);}
        unsafe {output(handle,runtime().raw_proxy(table,owner)?)} })())
}
#[no_mangle]
pub extern "C" fn mako_sharding_raw_register(table: u64, owner: u32, proxy: u32, handle: usize) -> u32 {
    code(flag(proxy).and_then(|proxy|runtime().raw_register(table,owner,proxy,handle)))
}
#[no_mangle]
pub unsafe extern "C" fn mako_sharding_start_node(owner: u32, nodes: u32, allow_migration: u32, abi: *const HostAbi) -> u32 {
    code((|| {if abi.is_null() {return Err(Status::Invalid);}
        let host = unsafe {Host::from_abi(&*abi)?};
        runtime().start(owner,nodes,flag(allow_migration)?,host) })())
}
#[no_mangle]
pub extern "C" fn mako_sharding_stop_node(owner: u32) {runtime().stop(owner)}
#[no_mangle]
pub unsafe extern "C" fn mako_sharding_dispatch(owner: u32, operation: u32, payload: Bytes, reply: Option<ReplySink>, context: *mut c_void) -> u32 {
    code((|| {
        let completion = unsafe {Completion::new(reply.ok_or(Status::Invalid)?,context)};
        runtime().dispatch(owner,operation,unsafe {payload.slice()?}.to_vec(),completion)
    })())
}
type SegmentCallback = unsafe extern "C" fn(*mut c_void,Bytes,u32,Bytes,RoutedGrant) -> u32;
#[no_mangle]
pub unsafe extern "C" fn mako_sharding_scan_segments(table: u64, lo: Bytes, has_hi: u32, hi: Bytes, reverse: u32,
                                                     callback: Option<SegmentCallback>, context: *mut c_void) -> u32 {
    code((|| {
        let callback = callback.ok_or(Status::Invalid)?;
        let lo = unsafe {lo.slice()?}; let hi = unsafe {upper(has_hi,hi)?};
        let reverse = flag(reverse)?;
        if let Some(hi) = hi {if lo == hi {return Ok(());}}
        let snapshot = runtime().snapshot()?;
        unsafe {scan_snapshot(&snapshot,table,lo,hi,reverse,callback,context)}
    })())
}
/// SAFETY: bytes/callback context remain live until all synchronous callbacks
/// return. Callbacks may read the same context but cannot end or replace it.
#[no_mangle]
pub unsafe extern "C" fn mako_sharding_read_scan_segments(read:*const MakoShardRead, table:u64,
    lo:Bytes, has_hi:u32, hi:Bytes, reverse:u32, callback:Option<SegmentCallback>, context:*mut c_void) -> u32 {
    code((|| {
        let callback=callback.ok_or(Status::Invalid)?;
        let lo=unsafe {lo.slice()?};let hi=unsafe {upper(has_hi,hi)?};
        let reverse=flag(reverse)?;
        with_read(read,|captured|unsafe {
            scan_snapshot(captured.view.routing(),table,lo,hi,reverse,callback,context)
        })
    })())
}
unsafe fn scan_snapshot(snapshot:&crate::routing::Snapshot, table:u64, lo:&[u8], hi:Option<&[u8]>,
    reverse:bool, callback:SegmentCallback, context:*mut c_void) -> Result<(),Status> {
    if hi==Some(lo) {return Ok(());}
    let visit=|segment:crate::routing::ScanSegment<'_>| -> Result<bool,Status> {
        match unsafe {callback(context,Bytes::from_slice(segment.lo),u32::from(segment.hi.is_some()),
            Bytes::from_slice(segment.hi.unwrap_or(&[])),
            RoutedGrant {grant:segment.grant,snapshot_version:snapshot.version})} {
            0=>Ok(true),1=>Ok(false),_=>Err(Status::Retry),
        }
    };
    if reverse {
        let mut segments=snapshot.reverse_segments(table,lo,hi)?;
        while let Some(segment)=segments.next() {if !visit(segment)? {break;}}
    } else {
        let mut segments=snapshot.segments(table,lo,hi)?;
        while let Some(segment)=segments.next() {if !visit(segment)? {break;}}
    }
    Ok(())
}
#[repr(C)]
pub struct AdminResult {generation: u64, outcome: u32}
unsafe fn admin(operation: u32, payload: &[u8], context: *mut c_void, rpc: Option<AdminRpc>, result: *mut AdminResult) -> Result<(),Status> {
    if result.is_null() {return Err(Status::Invalid);}
    let reply = unsafe {host::admin_call(context,rpc.ok_or(Status::Invalid)?,operation,payload)?};
    let (generation,outcome) = wire::decode_outcome(&reply)?;
    unsafe {output(result,AdminResult {generation,outcome})}
}
#[no_mangle]
pub unsafe extern "C" fn mako_sharding_admin_begin(nonce: TxnId, table_name: Bytes, lo: Bytes, has_hi: u32, hi: Bytes,
    source: u32, destination: u32, context: *mut c_void, rpc: Option<AdminRpc>, result: *mut AdminResult) -> u32 {
    code((|| {
        let request = wire::Begin {nonce,table_name:unsafe {table_name.slice()?}.to_vec(),
            lo:unsafe {lo.slice()?}.to_vec(),hi:unsafe {upper(has_hi,hi)?}.map(<[u8]>::to_vec),source,destination};
        unsafe {admin(wire::ADMIN_BEGIN,&wire::encode_begin(&request)?,context,rpc,result)}
    })())
}
#[no_mangle]
pub unsafe extern "C" fn mako_sharding_admin_poll(nonce: TxnId, context: *mut c_void, rpc: Option<AdminRpc>, result: *mut AdminResult) -> u32 {
    code((|| unsafe {admin(wire::ADMIN_POLL,&wire::encode_nonce(nonce)?,context,rpc,result)})())
}
#[no_mangle]
pub unsafe extern "C" fn mako_sharding_admin_abort(nonce: TxnId, context: *mut c_void, rpc: Option<AdminRpc>, result: *mut AdminResult) -> u32 {
    code((|| unsafe {admin(wire::ADMIN_ABORT,&wire::encode_nonce(nonce)?,context,rpc,result)})())
}

#[cfg(test)]
mod read_cache_tests {
    use super::*;
    use crate::directory::RouteTable;
    use crate::participant::Participant;
    use crate::routing::Snapshot;
    use crate::types::{Boundary, Command, KeyRange, MigrationPlan};

    impl ReadIdentity for Arc<AdmissionSnapshot> {
        fn same_view(&self, other:&Self) -> bool {Arc::ptr_eq(self,other)}
    }

    fn tables() -> Vec<RouteTable> {
        vec![RouteTable {table:1,boundaries:vec![Boundary {start:vec![],
            grant:Grant {owner:0,ownership_version:0}}]}]
    }
    fn capture(participant:&Participant) -> Arc<AdmissionSnapshot> {
        let mut routes=tables();
        routes[0].boundaries[0].grant.owner=participant.owner();
        Arc::new(AdmissionSnapshot::capture(participant,Arc::new(Snapshot {version:0,tables:routes})))
    }
    fn freeze(participant:&mut Participant) {
        let plan=MigrationPlan {generation:1,nonce:TxnId {client:9,sequence:1},source:0,destination:1,
            range:KeyRange {table:1,lo:b"b".to_vec(),hi:Some(b"d".to_vec())},
            old:tables().remove(0).boundaries};
        assert_eq!(participant.deliver(&plan,Command::Freeze).status,Status::Ok);
    }

    #[test]
    fn retained_and_same_epoch_views_survive_freeze_until_refresh() {
        let mut participant=Participant::new(0,tables()).unwrap();
        let old=capture(&participant);
        let mut cache=ReadCache::new();
        cache.begin(0,7,||Ok(Arc::clone(&old)),|_|Ok(())).unwrap();
        let route=cache.get().unwrap().route(1,b"c").unwrap();
        freeze(&mut participant);
        let frozen=capture(&participant);
        assert_eq!(cache.get().unwrap().point(1,b"c",route),Status::Ok);
        // A nested begin must not replace the active snapshot, even if the
        // owner or epoch changed. The C++ nested operation reuses its handle.
        assert_eq!(cache.begin(1,8,||panic!("active context refreshed"),|_|Ok(())),Err(Status::Busy));
        assert_eq!(cache.get().unwrap().point(1,b"c",route),Status::Ok);
        cache.end();
        assert!(matches!(cache.get(),Err(Status::Invalid)));
        cache.begin(0,7,||panic!("cache hit refreshed"),|_|Ok(())).unwrap();
        assert_eq!(cache.get().unwrap().point(1,b"c",route),Status::Ok);
        cache.end();
        cache.begin(0,8,||Ok(Arc::clone(&frozen)),|_|Ok(())).unwrap();
        assert_eq!(cache.get().unwrap().point(1,b"c",route),Status::Retry);
        cache.end();
    }

    #[test]
    fn cache_refreshes_on_owner_change_and_fails_closed_on_invalid_epoch_or_stop() {
        let participant=Participant::new(0,tables()).unwrap();
        let view=capture(&participant);
        let mut next_tables=tables();
        next_tables[0].boundaries[0].grant.owner=1;
        let next=capture(&Participant::new(1,next_tables).unwrap());
        let mut cache=ReadCache::new();
        assert_eq!(cache.begin(0,0,||panic!("zero epoch loaded"),|_:&Arc<AdmissionSnapshot>|Ok(())),Err(Status::Invalid));
        cache.begin(0,3,||Ok(Arc::clone(&view)),|_|Ok(())).unwrap();
        let old_route=cache.get().unwrap().route(1,b"c").unwrap();
        assert_eq!(cache.get().unwrap().point(1,b"c",old_route),Status::Ok);
        cache.end();
        cache.begin(1,3,||Ok(Arc::clone(&next)),|_|Ok(())).unwrap();
        let new_route=cache.get().unwrap().route(1,b"c").unwrap();
        assert_eq!(cache.get().unwrap().point(1,b"c",new_route),Status::Ok);
        assert_eq!(cache.get().unwrap().point(1,b"c",old_route),Status::Retry);
        cache.end();
        assert_eq!(cache.begin(1,3,||panic!("same owner/epoch refreshed"),|_|Err(Status::Retry)),Err(Status::Retry));
        assert!(matches!(cache.get(),Err(Status::Invalid)));
        assert_eq!(cache.begin(1,4,||Err(Status::Io),|_|Ok(())),Err(Status::Io));
        assert!(matches!(cache.get(),Err(Status::Invalid)));
        cache.begin(1,4,||Ok(Arc::clone(&next)),|_|Ok(())).unwrap();
        assert_eq!(cache.get().unwrap().point(1,b"c",new_route),Status::Ok);
        cache.end();
        cache.end();
        assert!(matches!(cache.get(),Err(Status::Invalid)));
    }

    #[test]
    fn identity_tracks_full_views_not_epochs_or_routing_labels() {
        let ids=AtomicU64::new(0);
        let mut participant=Participant::new(0,tables()).unwrap();
        let old=capture(&participant);
        let mut cache=ReadCache::new();
        assert_eq!(cache.id(),Err(Status::Invalid));
        cache.begin_with_ids(0,1,||Ok(Arc::clone(&old)),|_|Ok(()),&ids).unwrap();
        let first=cache.id().unwrap();
        assert_ne!(first,0);
        let route=cache.get().unwrap().route(1,b"c").unwrap();
        cache.end();
        assert_eq!(cache.id(),Err(Status::Invalid));
        for epoch in [1,2,3] {
            cache.begin_with_ids(0,epoch,||Ok(Arc::clone(&old)),|_|Ok(()),&ids).unwrap();
            assert_eq!(cache.id(),Ok(first));
            assert_eq!(cache.get().unwrap().point(1,b"c",route),Status::Ok);
            assert_eq!(ids.load(Ordering::Relaxed),first);
            cache.end();
        }
        freeze(&mut participant);
        let frozen=capture(&participant);
        // Identical route labels do not imply identical admission behavior.
        assert!(frozen.route(1,b"c").unwrap()==route);
        cache.begin_with_ids(0,4,||Ok(Arc::clone(&frozen)),|_|Ok(()),&ids).unwrap();
        let second=cache.id().unwrap();
        assert_ne!(first,second);
        assert_eq!(cache.get().unwrap().point(1,b"c",route),Status::Retry);
        cache.end();
        cache.begin_with_ids(0,5,||Ok(Arc::clone(&old)),|_|Ok(()),&ids).unwrap();
        let third=cache.id().unwrap();
        assert_ne!(third,first);
        assert_ne!(third,second);
        assert_eq!(cache.get().unwrap().point(1,b"c",route),Status::Ok);
        cache.end();
        cache.begin_with_ids(1,5,||Ok(Arc::clone(&old)),|_|Ok(()),&ids).unwrap();
        assert_ne!(cache.id().unwrap(),third);
        let mut worker=ReadCache::new();
        worker.begin_with_ids(1,5,||Ok(Arc::clone(&old)),|_|Ok(()),&ids).unwrap();
        assert_ne!(worker.id().unwrap(),cache.id().unwrap());
    }

    #[test]
    fn admitted_views_survive_stop_or_fence_but_new_admissions_fail() {
        use std::sync::atomic::AtomicU32;
        for unavailable in [Status::Retry,Status::Io] {
            let view=capture(&Participant::new(0,tables()).unwrap());
            let node=Arc::new(AtomicU32::new(Status::Ok as u32));
            let replacement=Arc::new(AtomicU32::new(Status::Ok as u32));
            let ready=|read:&CapturedRead<AtomicU32>|checked(wire::status(read.node.load(Ordering::Relaxed)));
            let ids=AtomicU64::new(0);
            let mut cache=ReadCache::new();
            cache.begin_with_ids(0,1,||Ok(CapturedRead {node:Arc::clone(&node),view:Arc::clone(&view)}),ready,&ids).unwrap();
            let first=cache.id().unwrap();
            let route=cache.get().unwrap().view.route(1,b"c").unwrap();
            // Stop yields Retry; a metadata fence yields Io. Neither revokes
            // the immutable view already admitted for this logical context.
            node.store(unavailable as u32,Ordering::Relaxed);
            assert_eq!(cache.id(),Ok(first));
            assert!(cache.get().unwrap().view.route(1,b"c").unwrap()==route);
            assert_eq!(cache.get().unwrap().view.point(1,b"c",route),Status::Ok);
            assert_eq!(cache.get().unwrap().view.range(1,b"b",Some(b"d"),route),Status::Ok);
            cache.end();
            assert!(matches!(cache.get(),Err(Status::Invalid)));
            assert_eq!(cache.id(),Err(Status::Invalid));
            assert_eq!(cache.begin_with_ids(0,1,||panic!("same epoch reload"),ready,&ids),Err(unavailable));
            assert_eq!(cache.id(),Err(Status::Invalid));
            assert_eq!(cache.begin_with_ids(0,2,||Ok(CapturedRead {node:Arc::clone(&node),view:Arc::clone(&view)}),ready,&ids),Err(unavailable));
            assert!(matches!(cache.get(),Err(Status::Invalid)));
            assert_eq!(cache.id(),Err(Status::Invalid));
            assert_eq!(ids.load(Ordering::Relaxed),first);
            // A replacement node changes identity even with the same snapshot.
            cache.begin_with_ids(0,2,||Ok(CapturedRead {node:replacement,view:Arc::clone(&view)}),ready,&ids).unwrap();
            assert_ne!(cache.id().unwrap(),first);
            assert_eq!(cache.get().unwrap().view.point(1,b"c",route),Status::Ok);
        }
    }

    #[test]
    fn exhausted_ids_fail_closed_and_failed_refresh_preserves_cached_view() {
        let view=capture(&Participant::new(0,tables()).unwrap());
        let changed=capture(&Participant::new(0,tables()).unwrap());
        let ids=AtomicU64::new(u64::MAX-1);
        let mut cache=ReadCache::new();
        cache.begin_with_ids(0,1,||Ok(Arc::clone(&view)),|_|Ok(()),&ids).unwrap();
        assert_eq!(cache.id(),Ok(u64::MAX));
        cache.end();
        // Even an exhausted allocator permits same-Arc epoch refreshes.
        cache.begin_with_ids(0,2,||Ok(Arc::clone(&view)),|_|Ok(()),&ids).unwrap();
        assert_eq!(cache.id(),Ok(u64::MAX));
        cache.end();
        assert_eq!(cache.begin_with_ids(0,3,||Ok(Arc::clone(&changed)),|_|Err(Status::Retry),&ids),Err(Status::Retry));
        assert_eq!(cache.begin_with_ids(0,3,||Err(Status::Io),|_|Ok(()),&ids),Err(Status::Io));
        assert_eq!(cache.begin_with_ids(0,3,||Ok(Arc::clone(&changed)),|_|Ok(()),&ids),Err(Status::Exhausted));
        assert_eq!(cache.id(),Err(Status::Invalid));
        assert_eq!(ids.load(Ordering::Relaxed),u64::MAX);
        cache.begin_with_ids(0,2,||panic!("failed refresh replaced old cache"),|_|Ok(()),&ids).unwrap();
        assert_eq!(cache.id(),Ok(u64::MAX));
        let route=cache.get().unwrap().route(1,b"c").unwrap();
        assert_eq!(cache.get().unwrap().point(1,b"c",route),Status::Ok);
        let mut empty=ReadCache::new();
        assert_eq!(empty.begin_with_ids(0,1,||Ok(view),|_|Ok(()),&ids),Err(Status::Exhausted));
        assert_eq!(empty.id(),Err(Status::Invalid));
    }

    #[test]
    fn exported_reads_reject_invalid_closed_and_foreign_handles() {
        let mut id=99;
        assert_eq!(unsafe {mako_sharding_read_view_id(std::ptr::null(),&mut id)},Status::Invalid as u32);
        assert_eq!(id,0);
        assert_eq!(with_read(std::ptr::null(),|_|Ok(())),Err(Status::Invalid));
        READ.with(|slot| {
            let pointer=read_pointer(&slot.borrow());
            id=99;
            assert_eq!(unsafe {mako_sharding_read_view_id(pointer,&mut id)},Status::Invalid as u32);
            assert_eq!(id,0);
            assert_eq!(unsafe {mako_sharding_read_view_id(pointer,std::ptr::null_mut())},Status::Invalid as u32);
            assert_eq!(with_read(pointer,|_|Ok(())),Err(Status::Invalid));
            let foreign=pointer as usize;
            std::thread::spawn(move || {
                let pointer=foreign as *const MakoShardRead;
                let mut id=99;
                assert_eq!(unsafe {mako_sharding_read_view_id(pointer,&mut id)},Status::Invalid as u32);
                assert_eq!(id,0);
                assert_eq!(with_read(pointer,|_|Ok(())),Err(Status::Invalid));
            }).join().unwrap();
        });
    }
}
