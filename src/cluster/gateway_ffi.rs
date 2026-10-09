//! Native ABI for gateway sequencing. Pointer validity and successful legacy
//! engine atomicity are explicit boundary contracts, not handler refinement.
#![allow(unsafe_code)]
use crate::gateway::{self, Admission, Client, Gateway, Reply, Request};
use std::collections::BTreeMap;
use std::ffi::c_void;
use std::sync::LazyLock;
use parking_lot::Mutex;

#[repr(C)]
pub struct WireRequest {
    version: u32, kind: u32, client: u64, sequence: u64, session: u64,
    table: u64, epoch: u64, owner: u32, physical_table: u32,
    route_known: u32, fixed_coordinate: u32, coordinate_length: u32,
    key_length: u32, value_length: u32, name_length: u32,
    coordinate: [u8; 64], key: [u8; 64], name: [u8; 128], value: [u8; 8000],
}
#[repr(C)]
pub struct WireResponse {
    client: u64, sequence: u64, table: u64, epoch: u64,
    owner: u32, physical_table: u32, fixed_coordinate: u32, coordinate_length: u32,
    status: i32, outcome: u32, op_result: u32, value_length: u32,
    coordinate: [u8; 64], value: [u8; 8000],
}
const _: () = assert!(std::mem::offset_of!(WireRequest, value) == 336);
const _: () = assert!(std::mem::offset_of!(WireResponse, value) == 128);
impl WireRequest {
    fn decode(&self) -> Option<Request> {
        if self.version != 1 || self.coordinate_length > 64 || self.key_length > 64
            || self.name_length > 128 || self.value_length > 8000
            || self.route_known > 1 || self.fixed_coordinate > 1 { return None; }
        Some(Request {
            kind: self.kind, client: self.client, sequence: self.sequence,
            session: self.session, table: self.table, epoch: self.epoch,
            owner: self.owner, physical_table: self.physical_table,
            route_known: self.route_known != 0, fixed_coordinate: self.fixed_coordinate != 0,
            coordinate: self.coordinate[..self.coordinate_length as usize].to_vec(),
            key: self.key[..self.key_length as usize].to_vec(),
            name: self.name[..self.name_length as usize].to_vec(),
            value: self.value[..self.value_length as usize].to_vec(),
        })
    }
    fn encode(request: &Request) -> Self {
        let mut wire = Self { version: 1, kind: request.kind, client: request.client,
            sequence: request.sequence, session: request.session, table: request.table,
            epoch: request.epoch, owner: request.owner, physical_table: request.physical_table,
            route_known: u32::from(request.route_known), fixed_coordinate: u32::from(request.fixed_coordinate),
            coordinate_length: request.coordinate.len() as u32, key_length: request.key.len() as u32,
            value_length: request.value.len() as u32, name_length: request.name.len() as u32,
            coordinate: [0; 64], key: [0; 64], name: [0; 128], value: [0; 8000] };
        wire.coordinate[..request.coordinate.len()].copy_from_slice(&request.coordinate);
        wire.key[..request.key.len()].copy_from_slice(&request.key);
        wire.name[..request.name.len()].copy_from_slice(&request.name);
        wire.value[..request.value.len()].copy_from_slice(&request.value);
        wire
    }
}
impl WireResponse {
    fn reply(client: u64, sequence: u64, reply: &Reply) -> Self {
        let mut wire = Self { client, sequence, table: 0, epoch: 0, owner: 0, physical_table: 0,
            fixed_coordinate: 0, coordinate_length: 0, status: reply.status, outcome: reply.outcome,
            op_result: u32::from(reply.op_result), value_length: reply.value.len() as u32,
            coordinate: [0; 64], value: [0; 8000] };
        wire.value[..reply.value.len()].copy_from_slice(&reply.value);
        wire
    }
}
static GATEWAYS: LazyLock<Mutex<BTreeMap<u32, Gateway>>> =
    LazyLock::new(|| Mutex::new(BTreeMap::new()));
fn gateways() -> &'static Mutex<BTreeMap<u32, Gateway>> {
    &GATEWAYS
}

#[unsafe(no_mangle)]
pub extern "C" fn mako_gateway_client_new(client: u64, first_sequence: u64) -> *mut Client {
    Client::new(client, first_sequence).map_or(std::ptr::null_mut(), |c| Box::into_raw(Box::new(c)))
}
/// # Safety
/// `client` is null or a unique allocation returned by mako_gateway_client_new.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mako_gateway_client_free(client: *mut Client) {
    if !client.is_null() { drop(unsafe { Box::from_raw(client) }); }
}
/// # Safety
/// Inputs and output are valid, nonaliasing ABI objects; client is exclusively borrowed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mako_gateway_prepare(client: *mut Client, intent: *const WireRequest,
                                              request: *mut WireRequest) -> u32 {
    let (Some(client), Some(intent), Some(request)) =
        (unsafe { client.as_mut() }, unsafe { intent.as_ref() }, unsafe { request.as_mut() })
        else { return 3; };
    let Some(intent) = intent.decode() else { return 3; };
    match client.prepare(intent) {
        Ok(pending) => { *request = WireRequest::encode(pending); 0 }
        Err(()) => 3,
    }
}
/// # Safety
/// Valid nonaliasing objects; route is a response to this client's pending discovery.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mako_gateway_set_route(client: *mut Client, route: *const WireResponse,
                                                request: *mut WireRequest) -> u32 {
    let (Some(client), Some(route), Some(request)) =
        (unsafe { client.as_mut() }, unsafe { route.as_ref() }, unsafe { request.as_mut() })
        else { return 3; };
    if route.status != 0 || route.coordinate_length > 64 || route.fixed_coordinate > 1 {
        return 3;
    }
    match client.set_route(route.client, route.sequence, route.table, route.epoch,
                          route.owner, route.physical_table, route.fixed_coordinate != 0,
                          route.coordinate[..route.coordinate_length as usize].to_vec()) {
        Ok(pending) => { *request = WireRequest::encode(pending); 0 }
        Err(()) => 3,
    }
}
/// # Safety
/// Valid objects; client is exclusively borrowed. Transport failures do not call this.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mako_gateway_complete(client: *mut Client,
                                               response: *const WireResponse) -> u32 {
    let (Some(client), Some(response)) = (unsafe { client.as_mut() }, unsafe { response.as_ref() })
        else { return 3; };
    if client.complete(response.client, response.sequence, response.outcome) { 0 } else { 4 }
}
/// # Safety
/// The request/output/context live across the call. `execute` is the actual
/// ShardReceiver bridge; it must not unwind, escape pointers, or report committed
/// before the engine's successful atomic commit and lifetime completion.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mako_gateway_execute(participant: u32, request: *const WireRequest,
    context: *mut c_void,
    execute: unsafe extern "C" fn(*mut c_void, *const WireRequest, *mut WireResponse),
    response: *mut WireResponse) {
    let (Some(wire), Some(response)) = (unsafe { request.as_ref() }, unsafe { response.as_mut() })
        else { return; };
    *response = WireResponse::reply(wire.client, wire.sequence, &Reply::rejected());
    let Some(decoded) = wire.decode() else { return; };
    let admission = {
        let mut gateways = gateways().lock();
        gateways.entry(participant).or_insert_with(|| Gateway::new(participant)).admit(decoded)
    };
    let reply = match admission {
        Admission::Reply(reply) => reply,
        Admission::Execute => {
            response.outcome = gateway::UNKNOWN;
            unsafe { execute(context, request, response) };
            let reply = if response.value_length > 8000
                || !matches!(response.outcome, gateway::UNKNOWN | gateway::COMMITTED | gateway::ABORTED) {
                Reply { status: 2, outcome: gateway::UNKNOWN, op_result: false, value: Vec::new() }
            } else {
                Reply { status: response.status, outcome: response.outcome,
                    op_result: response.op_result != 0,
                    value: response.value[..response.value_length as usize].to_vec() }
            };
            // Save the real response before any transport send. A delayed retry
            // cannot enter the callback, even while this completion is pending.
            let mut gateways = gateways().lock();
            if let Some(gateway) = gateways.get_mut(&participant) {
                gateway.finish(wire.client, wire.sequence, reply.clone());
            }
            reply
        }
    };
    *response = WireResponse::reply(wire.client, wire.sequence, &reply);
}
