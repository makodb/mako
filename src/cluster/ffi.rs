//! ABI lifetimes only. Non-null foreign pointers must designate valid aligned
//! objects; spans remain borrowed for the call. Callbacks cannot unwind. Engine
//! handles/contexts outlive stop_node; dispatch transfers its callback only on OK.
#![allow(unsafe_code)]
use std::ffi::c_void;
use crate::catalog::{Coordinates, TableKind};
use crate::host::{self, AdminRpc, Bytes, Completion, Host, HostAbi, Opener, ReplySink, WarehouseOpen};
use crate::runtime::runtime;
use crate::types::{Grant, Status, TxnId};
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
pub unsafe extern "C" fn mako_sharding_route(table: u64, coordinate: Bytes, grant: *mut Grant) -> u32 {
    code((|| {if grant.is_null() {return Err(Status::Invalid);}
        let value = runtime().route(table,unsafe {coordinate.slice()?})?;
        unsafe {output(grant,value)} })())
}
#[no_mangle]
pub extern "C" fn mako_sharding_lease_begin(owner: u32, id: TxnId) -> u32 {
    code(runtime().node(owner).and_then(|node|checked(node.lease_begin(id))))
}
#[no_mangle]
pub unsafe extern "C" fn mako_sharding_lease_acquire(owner: u32, id: TxnId, table: u64, coordinate: Bytes, grant: Grant) -> u32 {
    code((|| checked(runtime().node(owner)?.lease_acquire(id,table,unsafe {coordinate.slice()?},grant)))())
}
#[no_mangle]
pub unsafe extern "C" fn mako_sharding_lease_acquire_range(owner: u32, id: TxnId, table: u64, lo: Bytes, has_hi: u32, hi: Bytes, grant: Grant) -> u32 {
    code((|| checked(runtime().node(owner)?.lease_range(id,table,unsafe {lo.slice()?},unsafe {upper(has_hi,hi)?},grant)))())
}
#[no_mangle]
pub extern "C" fn mako_sharding_lease_finish(owner: u32, id: TxnId) -> u32 {
    code(runtime().node(owner).and_then(|node|checked(node.lease_finish(id))))
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
    runtime().warehouses.lock().as_ref().map_or(0,|d|d.dimensions().1)
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
        checked(runtime().warehouses.lock().as_mut().ok_or(Status::NotFound)?.register(key,handle))
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
type SegmentCallback = unsafe extern "C" fn(*mut c_void,Bytes,u32,Bytes,Grant) -> u32;
#[no_mangle]
pub unsafe extern "C" fn mako_sharding_scan_segments(table: u64, lo: Bytes, has_hi: u32, hi: Bytes, reverse: u32,
                                                     callback: Option<SegmentCallback>, context: *mut c_void) -> u32 {
    code((|| {
        let callback = callback.ok_or(Status::Invalid)?;
        let lo = unsafe {lo.slice()?}; let hi = unsafe {upper(has_hi,hi)?};
        let reverse = flag(reverse)?;
        if let Some(hi) = hi {if lo == hi {return Ok(());}}
        let snapshot = runtime().snapshot()?;
        let visit = |segment: crate::routing::ScanSegment<'_>| -> Result<bool,Status> {
            match unsafe {callback(context,Bytes::from_slice(segment.lo),u32::from(segment.hi.is_some()),
                Bytes::from_slice(segment.hi.unwrap_or(&[])),segment.grant)} {
                0=>Ok(true),1=>Ok(false),_=>Err(Status::Retry),
            }
        };
        // Arc pins the entire observed version; no runtime lock crosses a
        // callback, so network waits and recursive routing cannot deadlock it.
        if reverse {
            let mut segments = snapshot.reverse_segments(table,lo,hi)?;
            while let Some(segment) = segments.next() {if !visit(segment)? {break;}}
        } else {
            let mut segments = snapshot.segments(table,lo,hi)?;
            while let Some(segment) = segments.next() {if !visit(segment)? {break;}}
        }
        Ok(())
    })())
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
