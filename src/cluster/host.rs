//! Native ABI kernel: borrowed spans, synchronous engine/RPC callbacks and their
//! lifetime contract. No routing, role, generation or lease decision lives here.
#![allow(unsafe_code)]
use std::ffi::c_void;
use crate::types::{KeyRange, Row, Status};
use crate::wire::{checked, status};

#[repr(C)]
#[derive(Clone, Copy)]
pub struct Bytes { pub data: *const u8, pub len: usize }
impl Bytes {
    pub fn from_slice(bytes: &[u8]) -> Self { Self { data: bytes.as_ptr(), len: bytes.len() } }
    /// Caller keeps a valid immutable allocation borrowed for the entire call.
    pub unsafe fn slice<'a>(self) -> Result<&'a [u8], Status> {
        if self.len == 0 { return Ok(&[]); }
        if self.data.is_null() || self.len > isize::MAX as usize { return Err(Status::Invalid); }
        Ok(unsafe { std::slice::from_raw_parts(self.data,self.len) })
    }
}
pub type RowSink = unsafe extern "C" fn(*mut c_void, Bytes, Bytes, Bytes);
pub type ReplySink = unsafe extern "C" fn(*mut c_void, u32, Bytes);
pub type AdminRpc = unsafe extern "C" fn(*mut c_void, u32, Bytes, ReplySink, *mut c_void) -> u32;
type Enter = unsafe extern "C" fn(*mut c_void,u32) -> u32;
type Leave = unsafe extern "C" fn(*mut c_void);
type Open = unsafe extern "C" fn(*mut c_void,u64,Bytes,u32,u32,u32,*mut usize) -> u32;
type Scan = unsafe extern "C" fn(*mut c_void,u64,Bytes,u32,Bytes,u32,Bytes,Bytes,RowSink,*mut c_void) -> u32;
type Put = unsafe extern "C" fn(*mut c_void,u64,Bytes,Bytes,Bytes) -> u32;
type Remove = unsafe extern "C" fn(*mut c_void,u64,Bytes,Bytes) -> u32;
type Peer = unsafe extern "C" fn(*mut c_void,u32,u32,Bytes,ReplySink,*mut c_void) -> u32;
pub type WarehouseOpen = unsafe extern "C" fn(*mut c_void,u64,u32,u32,u32,*mut usize) -> u32;

#[repr(C)]
pub struct HostAbi {
    pub context: *mut c_void,
    pub thread_enter: Option<Enter>, pub thread_leave: Option<Leave>,
    pub open: Option<Open>, pub scan_next: Option<Scan>, pub put: Option<Put>,
    pub remove: Option<Remove>, pub peer_call: Option<Peer>,
}
/// The C++ owner keeps context/DB alive until stop_node joins every borrower.
/// Context is an address token, not dereferenced or owned by Rust.
#[derive(Clone, Copy)]
pub struct Host {
    context: usize, enter: Enter, leave: Leave, open: Open, scan: Scan,
    put: Put, remove: Remove, peer: Peer,
}
impl Host {
    pub unsafe fn from_abi(abi: &HostAbi) -> Result<Self, Status> {
        Ok(Self { context: abi.context as usize,
            enter: abi.thread_enter.ok_or(Status::Invalid)?, leave: abi.thread_leave.ok_or(Status::Invalid)?,
            open: abi.open.ok_or(Status::Invalid)?, scan: abi.scan_next.ok_or(Status::Invalid)?,
            put: abi.put.ok_or(Status::Invalid)?, remove: abi.remove.ok_or(Status::Invalid)?,
            peer: abi.peer_call.ok_or(Status::Invalid)? })
    }
    pub fn enter(&self, owner: u32) -> Result<(), Status> {
        checked(status(unsafe { (self.enter)(self.context as *mut c_void,owner) }))
    }
    pub fn leave(&self) { unsafe { (self.leave)(self.context as *mut c_void) } }
    pub fn open(&self, table: u64, name: &[u8], owner: u32, proxy: bool, kind: u32) -> Result<usize, Status> {
        let mut handle = 0usize;
        checked(status(unsafe { (self.open)(self.context as *mut c_void,table,
            Bytes::from_slice(name),owner,u32::from(proxy),kind,&mut handle) }))?;
        if handle == 0 { Err(Status::Invalid) } else { Ok(handle) }
    }
    pub fn scan(&self, range: &KeyRange, after: Option<&Row>) -> Result<Option<Row>, Status> {
        let mut output = RowOutput { calls: 0, row: Err(Status::Invalid) };
        let (coordinate,key) = after.map_or((&[][..],&[][..]),|r| (&r.coordinate[..],&r.key[..]));
        let result = status(unsafe { (self.scan)(self.context as *mut c_void,range.table,
            Bytes::from_slice(&range.lo),u32::from(range.hi.is_some()),
            Bytes::from_slice(range.hi.as_deref().unwrap_or(&[])),u32::from(after.is_some()),
            Bytes::from_slice(coordinate),Bytes::from_slice(key),row_sink,
            &mut output as *mut RowOutput as *mut c_void) });
        match result {
            Status::Ok if output.calls == 1 => {
                let row = output.row?;
                if !crate::bytes::contains(range,range.table,&row.coordinate)
                    || after.is_some_and(|a| crate::wire::compare_identity(a,&row) >= 0) {
                    return Err(Status::Invalid);
                }
                Ok(Some(row))
            },
            Status::NotFound if output.calls == 0 => Ok(None),
            Status::Ok | Status::NotFound => Err(Status::Invalid),
            error => Err(error),
        }
    }
    pub fn put(&self, table: u64, row: &Row) -> Status {
        status(unsafe { (self.put)(self.context as *mut c_void,table,Bytes::from_slice(&row.coordinate),
            Bytes::from_slice(&row.key),Bytes::from_slice(&row.value)) })
    }
    pub fn remove(&self, table: u64, row: &Row) -> Status {
        status(unsafe { (self.remove)(self.context as *mut c_void,table,
            Bytes::from_slice(&row.coordinate),Bytes::from_slice(&row.key)) })
    }
    pub fn peer(&self, owner: u32, operation: u32, payload: &[u8]) -> Result<Vec<u8>, Status> {
        let mut reply = ReplyOutput::new();
        checked(status(unsafe { (self.peer)(self.context as *mut c_void,owner,operation,
            Bytes::from_slice(payload),reply_sink,&mut reply as *mut ReplyOutput as *mut c_void) }))?;
        reply.take()
    }
}
struct RowOutput { calls: usize, row: Result<Row,Status> }
unsafe extern "C" fn row_sink(context: *mut c_void, coordinate: Bytes, key: Bytes, value: Bytes) {
    let output = unsafe { &mut *(context as *mut RowOutput) };
    output.calls += 1;
    output.row = if output.calls != 1 { Err(Status::Invalid) } else {
        (|| Ok(Row { coordinate: unsafe { coordinate.slice()? }.to_vec(),
            key: unsafe { key.slice()? }.to_vec(), value: unsafe { value.slice()? }.to_vec() }))()
    };
}
struct ReplyOutput { calls: usize, reply: Result<Vec<u8>,Status> }
impl ReplyOutput {
    fn new() -> Self { Self { calls: 0, reply: Err(Status::Invalid) } }
    fn take(self) -> Result<Vec<u8>,Status> {
        if self.calls == 1 { self.reply } else { Err(Status::Invalid) }
    }
}
unsafe extern "C" fn reply_sink(context: *mut c_void, code: u32, payload: Bytes) {
    let output = unsafe { &mut *(context as *mut ReplyOutput) };
    output.calls += 1;
    output.reply = if output.calls != 1 { Err(Status::Invalid) } else {
        (|| { checked(status(code))?; Ok(unsafe { payload.slice()? }.to_vec()) })()
    };
}
/// A dispatch success transfers this one completion to the worker queue.
/// A failed enqueue returns ownership to the foreign caller and never fires it.
pub struct Completion { sink: ReplySink, context: usize }
impl Completion {
    pub unsafe fn new(sink: ReplySink, context: *mut c_void) -> Self {
        Self { sink, context: context as usize }
    }
    pub fn complete(self, result: Result<Vec<u8>,Status>) {
        let (code,payload) = match result { Ok(bytes) => (Status::Ok,bytes), Err(s) => (s,Vec::new()) };
        unsafe { (self.sink)(self.context as *mut c_void,code as u32,Bytes::from_slice(&payload)) }
    }
}
#[derive(Clone, Copy)]
pub struct Opener { context: usize, open: WarehouseOpen }
impl Opener {
    pub unsafe fn new(context: *mut c_void, open: WarehouseOpen) -> Self { Self { context: context as usize,open } }
    pub fn same(&self, other: &Self) -> bool {
        self.context == other.context && std::ptr::fn_addr_eq(self.open,other.open)
    }
    pub fn open(&self, table: u64, warehouse: u32, owner: u32, proxy: bool) -> Result<usize,Status> {
        let mut handle = 0;
        checked(status(unsafe { (self.open)(self.context as *mut c_void,table,warehouse,
            owner,u32::from(proxy),&mut handle) }))?;
        if handle == 0 { Err(Status::Invalid) } else { Ok(handle) }
    }
}
pub unsafe fn admin_call(context: *mut c_void, rpc: AdminRpc, operation: u32, payload: &[u8]) -> Result<Vec<u8>,Status> {
    let mut reply = ReplyOutput::new();
    checked(status(unsafe { rpc(context,operation,Bytes::from_slice(payload),reply_sink,
        &mut reply as *mut ReplyOutput as *mut c_void) }))?;
    reply.take()
}
