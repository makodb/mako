//! Native control messages use the checked snapshot codec primitives. SRPC
//! supplies request/response correlation and non-Byzantine sender provenance.
use crate::bytes::compare;
use crate::migration::Coordinator;
use crate::participant::{Cleanup, ControlResult};
use crate::routing_codec::{Reader, Writer, SNAPSHOT_TAG};
use crate::types::*;

pub const SNAPSHOT: u32 = 1;
pub const HELLO: u32 = 2;
pub const SHUTDOWN_TOKEN: u32 = 3;
pub const CONTROL_BASE: u32 = 10;
pub const DRAIN: u32 = 16;
pub const FINAL_COPY: u32 = 17;
pub const CAPTURE: u32 = 18;
pub const ADMIN_BEGIN: u32 = 100;
pub const ADMIN_POLL: u32 = 101;
pub const ADMIN_ABORT: u32 = 102;
const PLAN_TAG: u32 = 0x31504b4d;
pub const COPY_ROWS: usize = 512;
pub const COPY_BYTES: usize = 1 << 20;

pub fn checked(status: Status) -> Result<(), Status> {
    match status { Status::Ok => Ok(()), error => Err(error) }
}
pub fn status(code: u32) -> Status {
    match code { 0 => Status::Ok, 1 => Status::NotFound, 2 => Status::Retry,
        3 => Status::Invalid, 4 => Status::Busy, 5 => Status::Exhausted,
        _ => Status::Io }
}
pub fn command(operation: u32) -> Option<Command> {
    match operation { 10 => Some(Command::Start), 11 => Some(Command::Freeze),
        12 => Some(Command::Final), 13 => Some(Command::Retire),
        14 => Some(Command::Commit), 15 => Some(Command::Abort), _ => None }
}
pub fn command_operation(command: Command) -> u32 { CONTROL_BASE + command as u32 }

fn write_upper(w: &mut Writer, upper: Option<&[u8]>) -> Result<(), Status> {
    match upper {
        None => checked(w.write_u32(0)),
        Some(h) => { checked(w.write_u32(1))?; checked(w.write_bytes(h)) }
    }
}
fn read_upper(r: &mut Reader) -> Result<Option<Vec<u8>>, Status> {
    match r.read_u32()? { 0 => Ok(None), 1 => Ok(Some(r.read_bytes()?)), _ => Err(Status::Invalid) }
}
pub fn encode_plan(plan: &MigrationPlan) -> Result<Vec<u8>, Status> {
    let mut w = Writer::new();
    checked(w.write_u32(PLAN_TAG))?;
    checked(w.write_u64(plan.generation))?;
    checked(w.write_u64(plan.nonce.client))?;
    checked(w.write_u64(plan.nonce.sequence))?;
    checked(w.write_u32(plan.source))?;
    checked(w.write_u32(plan.destination))?;
    checked(w.write_u64(plan.range.table))?;
    checked(w.write_bytes(&plan.range.lo))?;
    write_upper(&mut w, plan.range.hi.as_deref())?;
    checked(w.write_u64(plan.old.len() as u64))?;
    for boundary in &plan.old {
        checked(w.write_bytes(&boundary.start))?;
        checked(w.write_u32(boundary.grant.owner))?;
        checked(w.write_u64(boundary.grant.epoch))?;
    }
    Ok(w.into_bytes())
}
pub fn decode_plan(data: &[u8], nodes: u32) -> Result<MigrationPlan, Status> {
    let mut r = Reader::new(data);
    if r.read_u32()? != PLAN_TAG { return Err(Status::Invalid); }
    let generation = r.read_u64()?;
    let nonce = TxnId { client: r.read_u64()?, sequence: r.read_u64()? };
    let source = r.read_u32()?;
    let destination = r.read_u32()?;
    let table = r.read_u64()?;
    let lo = r.read_bytes()?;
    let hi = read_upper(&mut r)?;
    if generation == 0 || source == destination || source >= nodes || destination >= nodes
        || hi.as_ref().is_some_and(|end| compare(&lo,end) >= 0) { return Err(Status::Invalid); }
    let count = usize::try_from(r.read_u64()?).map_err(|_| Status::Invalid)?;
    if count == 0 || count > r.remaining() / 20 { return Err(Status::Invalid); }
    let mut old: Vec<Boundary> = Vec::with_capacity(count);
    for _ in 0..count {
        let start = r.read_bytes()?;
        let grant = Grant { owner: r.read_u32()?, epoch: r.read_u64()? };
        if grant.owner >= nodes || grant.epoch >= generation { return Err(Status::Invalid); }
        if let Some(previous) = old.last() {
            if compare(&previous.start,&start) >= 0 || previous.grant == grant { return Err(Status::Invalid); }
        } else if !start.is_empty() { return Err(Status::Invalid); }
        old.push(Boundary { start, grant });
    }
    checked(r.finish())?;
    Ok(MigrationPlan { generation, nonce, source, destination, range: KeyRange { table, lo, hi }, old })
}
pub fn encode_control(generation: u64, result: &ControlResult) -> Result<Vec<u8>, Status> {
    let mut w = Writer::new();
    checked(w.write_u64(generation))?;
    checked(w.write_u32(match result.certificate { None => 0, Some(c) => c as u32 + 1 }))?;
    Ok(w.into_bytes())
}
pub fn decode_certificate(data: &[u8], generation: u64) -> Result<Option<Certificate>, Status> {
    let mut r = Reader::new(data);
    if r.read_u64()? != generation { return Err(Status::Invalid); }
    let certificate = match r.read_u32()? {
        0 => None, 1 => Some(Certificate::Drained), 2 => Some(Certificate::Ready),
        3 => Some(Certificate::Retired), 4 => Some(Certificate::SourceDone),
        5 => Some(Certificate::DestinationDone), _ => return Err(Status::Invalid),
    };
    checked(r.finish())?;
    Ok(certificate)
}
pub fn failure(status: Status) -> ControlResult {
    ControlResult { status, certificate: None, cleanup: Cleanup::None }
}
pub fn snapshot(coordinator: &Coordinator) -> Result<Vec<u8>, Status> {
    let mut w = Writer::new();
    checked(w.write_u64(SNAPSHOT_TAG))?;
    checked(w.write_u64(coordinator.published_version()))?;
    checked(w.write_u64(coordinator.table_count() as u64))?;
    for i in 0..coordinator.table_count() {
        checked(w.write_table(coordinator.table_at(i).ok_or(Status::Invalid)?))?;
    }
    Ok(w.into_bytes())
}

pub fn capture_request(plan: &[u8], after: Option<&Row>) -> Result<Vec<u8>, Status> {
    let mut w = Writer::new();
    checked(w.write_bytes(plan))?;
    checked(w.write_u32(u32::from(after.is_some())))?;
    if let Some(row) = after {
        checked(w.write_bytes(&row.coordinate))?;
        checked(w.write_bytes(&row.key))?;
    }
    Ok(w.into_bytes())
}
pub fn decode_capture(data: &[u8], nodes: u32) -> Result<(MigrationPlan, Option<Row>, &[u8]), Status> {
    let mut r = Reader::new(data);
    let size = usize::try_from(r.read_u64()?).map_err(|_| Status::Invalid)?;
    let encoded = r.read_slice(size)?;
    let plan = decode_plan(encoded,nodes)?;
    let after = match r.read_u32()? {
        0 => None,
        1 => Some(Row { coordinate: r.read_bytes()?, key: r.read_bytes()?, value: Vec::new() }),
        _ => return Err(Status::Invalid),
    };
    checked(r.finish())?;
    Ok((plan,after,encoded))
}
pub fn encode_rows(generation: u64, rows: &[Row], eof: bool) -> Result<Vec<u8>, Status> {
    let mut w = Writer::new();
    checked(w.write_u64(generation))?;
    checked(w.write_u32(u32::from(eof)))?;
    checked(w.write_u64(rows.len() as u64))?;
    for row in rows {
        checked(w.write_bytes(&row.coordinate))?;
        checked(w.write_bytes(&row.key))?;
        checked(w.write_bytes(&row.value))?;
    }
    Ok(w.into_bytes())
}
pub fn decode_rows(data: &[u8], plan: &MigrationPlan, after: Option<&Row>) -> Result<(Vec<Row>, bool), Status> {
    let mut r = Reader::new(data);
    if r.read_u64()? != plan.generation { return Err(Status::Invalid); }
    let eof = match r.read_u32()? { 0 => false, 1 => true, _ => return Err(Status::Invalid) };
    let count = usize::try_from(r.read_u64()?).map_err(|_| Status::Invalid)?;
    if count > COPY_ROWS || count > r.remaining() / 24 || (count == 0 && !eof) { return Err(Status::Invalid); }
    let mut rows: Vec<Row> = Vec::with_capacity(count);
    for _ in 0..count {
        let row = Row { coordinate: r.read_bytes()?, key: r.read_bytes()?, value: r.read_bytes()? };
        if !crate::bytes::contains(&plan.range,plan.range.table,&row.coordinate) { return Err(Status::Invalid); }
        if let Some(previous) = rows.last().or(after) {
            if compare_identity(previous,&row) >= 0 { return Err(Status::Invalid); }
        }
        rows.push(row);
    }
    checked(r.finish())?;
    Ok((rows,eof))
}
pub fn compare_identity(a: &Row,b: &Row) -> i32 {
    let c = compare(&a.coordinate,&b.coordinate);
    if c == 0 { compare(&a.key,&b.key) } else { c }
}

pub struct Begin {
    pub nonce: TxnId,
    pub table_name: Vec<u8>,
    pub lo: Vec<u8>,
    pub hi: Option<Vec<u8>>,
    pub source: u32,
    pub destination: u32,
}
pub fn encode_begin(request: &Begin) -> Result<Vec<u8>, Status> {
    let mut w = Writer::new();
    checked(w.write_u64(request.nonce.client))?;
    checked(w.write_u64(request.nonce.sequence))?;
    checked(w.write_bytes(&request.table_name))?;
    checked(w.write_bytes(&request.lo))?;
    write_upper(&mut w,request.hi.as_deref())?;
    checked(w.write_u32(request.source))?;
    checked(w.write_u32(request.destination))?;
    Ok(w.into_bytes())
}
pub fn decode_begin(data: &[u8]) -> Result<Begin, Status> {
    let mut r = Reader::new(data);
    let request = Begin { nonce: TxnId { client: r.read_u64()?, sequence: r.read_u64()? },
        table_name: r.read_bytes()?, lo: r.read_bytes()?, hi: read_upper(&mut r)?,
        source: r.read_u32()?, destination: r.read_u32()? };
    checked(r.finish())?;
    Ok(request)
}
pub fn encode_nonce(nonce: TxnId) -> Result<Vec<u8>, Status> {
    let mut w = Writer::new();
    checked(w.write_u64(nonce.client))?;
    checked(w.write_u64(nonce.sequence))?;
    Ok(w.into_bytes())
}
pub fn decode_nonce(data: &[u8]) -> Result<TxnId, Status> {
    let mut r = Reader::new(data);
    let nonce = TxnId { client: r.read_u64()?, sequence: r.read_u64()? };
    checked(r.finish())?;
    Ok(nonce)
}
pub fn encode_outcome(generation: u64, outcome: Option<Outcome>) -> Result<Vec<u8>, Status> {
    let mut w = Writer::new();
    checked(w.write_u64(generation))?;
    checked(w.write_u32(match outcome { None => 0, Some(o) => if o.committed { 1 } else { 2 } }))?;
    Ok(w.into_bytes())
}
pub fn decode_outcome(data: &[u8]) -> Result<(u64,u32), Status> {
    let mut r = Reader::new(data);
    let generation = r.read_u64()?;
    let outcome = r.read_u32()?;
    checked(r.finish())?;
    if generation == 0 || outcome > 2 { return Err(Status::Invalid); }
    Ok((generation,outcome))
}
