//! Checked byte-level durable journal, independent of a particular storage engine.
//!
//! IMPLEMENTOR CONTRACT: one serialized KV adapter owns submission and settlement.
//! There is one live journal owner per namespace; neither Journal nor Admission may
//! be used to manufacture a second writer. `&mut K` serializes the read/head check
//! and submission, NOT a distributed compare-and-swap. A replicated adapter must
//! enforce that serialization and old-writer fencing at application time as well.
//! Recovery's settlement includes accepted writes whose responses were lost and
//! guarantees that no pre-barrier submission can subsequently change the map.
//! The primitive knows bytes, not migration plans, receipts, actors or correctness.
//! The adapter reserves the versioned KEY_TAG key domain for these system
//! records; engine row keys must not alias it. Settlement may finish another
//! namespace's already accepted batch, but recovery itself performs no writes.
//! Batch idempotence derives old-or-whole settlement for both puts and deletes;
//! recovery retains that settlement relation through every prefix read.
//! No implementation of the primitive (in-memory or otherwise) is supplied here.
use vstd::prelude::*;
use crate::routing_codec::{Reader, Writer};
use crate::types::Status;
#[cfg(verus_keep_ghost)]
use crate::routing_codec_proofs::{word32, word64, blob, word32_injective, word64_injective};

verus! {
pub const KEY_TAG: u64 = 0x014b56444f4b414d;
pub const HEAD_TAG: u64 = 0x014448444f4b414d;
pub const RECORD_TAG: u64 = 0x01524a444f4b414d;

#[derive(Debug, PartialEq, Eq, Clone, Copy)]
pub struct Namespace { pub owner: u32, pub coordinator: bool }
#[derive(Debug, PartialEq, Eq, Clone, Copy)]
pub struct Admission { pub namespace: Namespace, pub incarnation: u64 }
#[derive(Debug, PartialEq, Eq, Clone, Copy)]
pub enum CommitResult { Committed, NotCommitted, Unknown }
#[derive(Debug, PartialEq, Eq, Clone, Copy)]
pub enum JournalError { Io, Corrupt, Exists, Missing, Inactive, Exhausted, NotCommitted, Unknown }
pub enum ReadResult { Found(Vec<u8>), Absent, Io }
/// None deletes a key. Batch order determines the last value of repeated keys.
pub struct WriteOp { pub key: Vec<u8>, pub value: Option<Vec<u8>> }

pub open spec fn apply_op(m: Map<Seq<u8>, Seq<u8>>, op: WriteOp) -> Map<Seq<u8>, Seq<u8>> {
    match op.value { Some(v) => m.insert(op.key@, v@), None => m.remove(op.key@) }
}
pub open spec fn apply_batch(m: Map<Seq<u8>, Seq<u8>>, ops: Seq<WriteOp>) -> Map<Seq<u8>, Seq<u8>>
    decreases ops.len(),
{
    if ops.len() == 0 { m } else { apply_batch(apply_op(m, ops[0]), ops.skip(1)) }
}
pub open spec fn settled_map(before: Map<Seq<u8>,Seq<u8>>, pending: Option<Seq<WriteOp>>, after: Map<Seq<u8>,Seq<u8>>) -> bool {
    match pending { None => after == before,
        Some(batch) => after == before || after == apply_batch(before,batch) }
}

/// Trusted primitive assumptions are exclusively byte-map and accepted-I/O facts.
/// `pending` survives response loss/crash in the backend's recovery mechanism; it
/// is not a caller's volatile queue. Unknown permits either whole-batch outcome.
/// A successful barrier resolves it and excludes ALL later old applications.
/// An unsuccessful barrier leaves admission blocked. Reads of unsettled state
/// cannot succeed. Implementations must supply these guarantees, not assume a
/// migration journal or actor invariant in `wf`.
pub trait DurableKv: Sized {
    spec fn wf(&self) -> bool;
    spec fn committed(&self) -> Map<Seq<u8>, Seq<u8>>;
    spec fn pending(&self) -> Option<Seq<WriteOp>>;
    fn read(&mut self, key: &[u8]) -> (out: ReadResult)
        requires old(self).wf(),
        ensures final(self).wf(), final(self).committed() == old(self).committed(),
            final(self).pending() == old(self).pending(),
            match out {
                ReadResult::Found(v) => old(self).pending() is None
                    && old(self).committed().contains_key(key@) && v@ == old(self).committed()[key@],
                ReadResult::Absent => old(self).pending() is None && !old(self).committed().contains_key(key@),
                ReadResult::Io => true,
            };
    fn submit(&mut self, batch: Vec<WriteOp>) -> (out: CommitResult)
        requires old(self).wf(),
        ensures final(self).wf(),
            old(self).pending() is Some ==> out == CommitResult::NotCommitted
                && final(self).committed() == old(self).committed() && final(self).pending() == old(self).pending(),
            old(self).pending() is None ==> match out {
                CommitResult::Committed => final(self).committed() == apply_batch(old(self).committed(),batch@)
                    && final(self).pending() is None,
                CommitResult::NotCommitted => final(self).committed() == old(self).committed() && final(self).pending() is None,
                CommitResult::Unknown => (final(self).committed() == old(self).committed()
                    || final(self).committed() == apply_batch(old(self).committed(),batch@))
                    && final(self).pending() == Some(batch@),
            };
    fn settle(&mut self) -> (ok: bool)
        requires old(self).wf(),
        ensures final(self).wf(), settled_map(old(self).committed(),old(self).pending(),final(self).committed()),
            ok ==> final(self).pending() is None,
            !ok ==> final(self).pending() == old(self).pending();
}

pub open spec fn key_wire(ns: Namespace, entry: bool, index: u64) -> Seq<u8> {
    word64(KEY_TAG) + word32(ns.owner) + word32(if ns.coordinator {1} else {0})
        + word64(if entry {1} else {0}) + word64(index)
}
pub open spec fn head_wire(incarnation: u64, count: u64) -> Seq<u8> {
    word64(HEAD_TAG) + word64(incarnation) + word64(count)
}
pub open spec fn record_wire(payload: Seq<u8>) -> Seq<u8> { word64(RECORD_TAG) + blob(payload) }
pub open spec fn namespace_key(ns: Namespace, key: Seq<u8>) -> bool {
    exists|entry: bool, index: u64| key == key_wire(ns,entry,index)
}
pub open spec fn frames(before: Map<Seq<u8>,Seq<u8>>, after: Map<Seq<u8>,Seq<u8>>, ns: Namespace) -> bool {
    forall|key: Seq<u8>| !namespace_key(ns,key) ==>
        (after.contains_key(key) == before.contains_key(key)
            && (before.contains_key(key) ==> after[key] == before[key]))
}
pub open spec fn bootstrap_map(m: Map<Seq<u8>,Seq<u8>>, ns: Namespace, bootstrap: Seq<u8>) -> Map<Seq<u8>,Seq<u8>> {
    m.insert(key_wire(ns,true,0),record_wire(bootstrap))
        .insert(key_wire(ns,false,0),head_wire(0,1))
}
pub open spec fn bootstrap_options<K: DurableKv>(kv: &K, before: Map<Seq<u8>,Seq<u8>>,
    pending: Option<Seq<WriteOp>>, ns: Namespace, bootstrap: Seq<u8>) -> bool {
    forall|candidate: Map<Seq<u8>,Seq<u8>>| settled_map(kv.committed(),kv.pending(),candidate) ==>
        exists|base: Map<Seq<u8>,Seq<u8>>| settled_map(before,pending,base)
            && (candidate == base || candidate == bootstrap_map(base,ns,bootstrap))
}
pub open spec fn append_map(m: Map<Seq<u8>,Seq<u8>>, ns: Namespace, incarnation: u64, records: Seq<Seq<u8>>, entry: Seq<u8>) -> Map<Seq<u8>,Seq<u8>> {
    m.insert(key_wire(ns,true,records.len() as u64),record_wire(entry))
        .insert(key_wire(ns,false,0),head_wire(incarnation,(records.len()+1) as u64))
}
pub open spec fn activation_map(m: Map<Seq<u8>,Seq<u8>>, ns: Namespace, incarnation: u64, records: Seq<Seq<u8>>) -> Map<Seq<u8>,Seq<u8>> {
    m.insert(key_wire(ns,false,0),head_wire(incarnation,records.len() as u64))
}
pub open spec fn settlement_options<K: DurableKv>(kv: &K, before: Map<Seq<u8>,Seq<u8>>, after: Map<Seq<u8>,Seq<u8>>) -> bool {
    forall|candidate: Map<Seq<u8>,Seq<u8>>| settled_map(kv.committed(),kv.pending(),candidate)
        ==> candidate == before || candidate == after
}
pub open spec fn recoverable_prefix<K: DurableKv>(kv: &K, ns: Namespace, incarnation: u64, records: Seq<Seq<u8>>) -> bool {
    forall|candidate: Map<Seq<u8>,Seq<u8>>| settled_map(kv.committed(),kv.pending(),candidate)
        ==> has_prefix(candidate,ns,incarnation,records)
}
pub open spec fn entry_matches(m:Map<Seq<u8>,Seq<u8>>,ns:Namespace,index:u64,payload:Seq<u8>)->bool {
    m.contains_key(key_wire(ns,true,index)) && m[key_wire(ns,true,index)]==record_wire(payload)
}
pub open spec fn entries_match(m:Map<Seq<u8>,Seq<u8>>,ns:Namespace,records:Seq<Seq<u8>>)->bool {
    forall|i:int| 0<=i<records.len() ==> #[trigger] entry_matches(m,ns,i as u64,records[i])
}
pub open spec fn has_prefix(m: Map<Seq<u8>,Seq<u8>>, ns: Namespace, incarnation: u64, records: Seq<Seq<u8>>) -> bool {
    m.contains_key(key_wire(ns,false,0))
    && m[key_wire(ns,false,0)] == head_wire(incarnation,records.len() as u64)
    && entries_match(m,ns,records)
}

pub proof fn prefix_entry(m:Map<Seq<u8>,Seq<u8>>,ns:Namespace,incarnation:u64,records:Seq<Seq<u8>>,index:int)
    requires has_prefix(m,ns,incarnation,records),0<=index<records.len(),
    ensures entry_matches(m,ns,index as u64,records[index]),
{
    hide(key_wire);hide(record_wire);hide(head_wire);
    assert(entry_matches(m,ns,index as u64,records[index]));
}

proof fn extend_entries(m:Map<Seq<u8>,Seq<u8>>,ns:Namespace,records:Seq<Seq<u8>>,payload:Seq<u8>)
    requires entries_match(m,ns,records),records.len()<=u64::MAX,
        entry_matches(m,ns,records.len() as u64,payload),
    ensures entries_match(m,ns,records.push(payload)),
{
    hide(key_wire);hide(record_wire);
    assert forall|i:int| 0<=i<records.push(payload).len() implies
        #[trigger] entry_matches(m,ns,i as u64,records.push(payload)[i]) by {
        if i<records.len() {
            assert(entry_matches(m,ns,i as u64,records[i]));
        } else {
            assert(i==records.len());
        }
    }
}

pub proof fn key_injective(a: Namespace, ae: bool, ai: u64, b: Namespace, be: bool, bi: u64)
    requires key_wire(a,ae,ai) == key_wire(b,be,bi),
    ensures a == b, ae == be, ai == bi,
{
    hide(word32);hide(word64);
    assert forall|v:u32|#[trigger] word32(v).len()==4 by{reveal(word32);}
    assert forall|v:u64|#[trigger] word64(v).len()==8 by{reveal(word64);}
    assert(word32(a.owner) =~= key_wire(a,ae,ai).subrange(8,12));
    assert(word32(b.owner) =~= key_wire(b,be,bi).subrange(8,12));
    word32_injective(a.owner,b.owner);
    assert(word32(if a.coordinator {1} else {0}) =~= key_wire(a,ae,ai).subrange(12,16));
    assert(word32(if b.coordinator {1} else {0}) =~= key_wire(b,be,bi).subrange(12,16));
    word32_injective(if a.coordinator {1} else {0},if b.coordinator {1} else {0});
    assert(word64(if ae {1} else {0}) =~= key_wire(a,ae,ai).subrange(16,24));
    assert(word64(if be {1} else {0}) =~= key_wire(b,be,bi).subrange(16,24));
    word64_injective(if ae {1} else {0},if be {1} else {0});
    assert(word64(ai) =~= key_wire(a,ae,ai).subrange(24,32));
    assert(word64(bi) =~= key_wire(b,be,bi).subrange(24,32));
    word64_injective(ai,bi);
}
pub proof fn head_injective(a: u64, ac: u64, b: u64, bc: u64)
    requires head_wire(a,ac) == head_wire(b,bc), ensures a == b, ac == bc,
{
    assert(word64(a) =~= head_wire(a,ac).subrange(8,16));
    assert(word64(b) =~= head_wire(b,bc).subrange(8,16));
    word64_injective(a,b);
    assert(word64(ac) =~= head_wire(a,ac).subrange(16,24));
    assert(word64(bc) =~= head_wire(b,bc).subrange(16,24));
    word64_injective(ac,bc);
}
pub proof fn record_injective(a: Seq<u8>, b: Seq<u8>)
    requires record_wire(a) == record_wire(b), ensures a == b,
{
    assert(a =~= record_wire(a).skip(16));
    assert(b =~= record_wire(b).skip(16));
}

pub fn journal_key(ns: Namespace, entry: bool, index: u64) -> (bytes: Vec<u8>)
    ensures bytes@ == key_wire(ns,entry,index),
{
    let mut w = Writer::new();
    w.write_u64(KEY_TAG); w.write_u32(ns.owner);
    w.write_u32(if ns.coordinator {1} else {0});
    w.write_u64(if entry {1} else {0}); w.write_u64(index);
    w.into_bytes()
}
fn encode_head(incarnation: u64, count: u64) -> (bytes: Vec<u8>)
    ensures bytes@ == head_wire(incarnation,count),
{
    let mut w = Writer::new();
    w.write_u64(HEAD_TAG); w.write_u64(incarnation); w.write_u64(count);
    w.into_bytes()
}
fn decode_head(bytes: &[u8]) -> (out: Result<(u64,u64),JournalError>)
    ensures out is Ok ==> bytes@ == head_wire(out->Ok_0.0,out->Ok_0.1) && out->Ok_0.1 > 0,
{
    let mut r = Reader::new(bytes);
    let tag = match r.read_u64() { Ok(v) => v, Err(_) => return Err(JournalError::Corrupt) };
    if tag != HEAD_TAG { return Err(JournalError::Corrupt); }
    let incarnation = match r.read_u64() { Ok(v) => v, Err(_) => return Err(JournalError::Corrupt) };
    let count = match r.read_u64() { Ok(v) => v, Err(_) => return Err(JournalError::Corrupt) };
    if count == 0 || !matches!(r.finish(), Status::Ok) { return Err(JournalError::Corrupt); }
    proof { assert(bytes@ =~= head_wire(incarnation,count)); }
    Ok((incarnation,count))
}
fn encode_record(payload: &[u8]) -> (out: Result<Vec<u8>,JournalError>)
    ensures out is Ok ==> out->Ok_0@ == record_wire(payload@),
{
    let mut w = Writer::new();
    w.write_u64(RECORD_TAG);
    if !matches!(w.write_bytes(payload), Status::Ok) { return Err(JournalError::Exhausted); }
    Ok(w.into_bytes())
}
fn decode_record(bytes: &[u8]) -> (out: Result<&[u8],JournalError>)
    ensures out is Ok ==> bytes@ == record_wire(out->Ok_0@),
{
    let mut r = Reader::new(bytes);
    let tag = match r.read_u64() { Ok(v) => v, Err(_) => return Err(JournalError::Corrupt) };
    if tag != RECORD_TAG { return Err(JournalError::Corrupt); }
    let length = match r.read_u64() { Ok(v) => v, Err(_) => return Err(JournalError::Corrupt) };
    if length > r.remaining() as u64 { return Err(JournalError::Corrupt); }
    let payload = match r.read_slice(length as usize) { Ok(v) => v, Err(_) => return Err(JournalError::Corrupt) };
    if !matches!(r.finish(), Status::Ok) { return Err(JournalError::Corrupt); }
    proof { assert(bytes@ =~= record_wire(payload@)); }
    Ok(payload)
}

/// Non-Clone volatile cache. Only bytes survive its destruction. `records` is
/// erased proof state, built by the SAME executable reads/decodes as recovery.
pub struct Journal {
    namespace: Namespace,
    incarnation: u64,
    count: u64,
    admitted: bool,
    can_activate: bool,
    records: Ghost<Seq<Seq<u8>>>,
}
impl Journal {
    pub closed spec fn wf(&self) -> bool {
        self.count > 0 && self.records@.len() == self.count
        && !(self.admitted && self.can_activate)
    }
    pub closed spec fn active(&self) -> bool { self.admitted }
    pub closed spec fn records(&self) -> Seq<Seq<u8>> { self.records@ }
    pub closed spec fn namespace_spec(&self) -> Namespace { self.namespace }
    pub closed spec fn incarnation_spec(&self) -> u64 { self.incarnation }
    pub closed spec fn consistent<K: DurableKv>(&self, kv: &K) -> bool {
        kv.pending() is None && has_prefix(kv.committed(),self.namespace,self.incarnation,self.records@)
    }
    pub proof fn committed_records<K: DurableKv>(&self, kv: &K)
        requires self.wf(), self.consistent(kv),
        ensures self.records().len() > 0, self.records().len() <= u64::MAX,
            kv.pending() is None,
            has_prefix(kv.committed(),self.namespace_spec(),self.incarnation_spec(),self.records()),
            recoverable_prefix(kv,self.namespace_spec(),self.incarnation_spec(),self.records()),
    {}
    /// Reestablishes cache consistency from the same settled committed bytes.
    pub proof fn prefix_consistent<K: DurableKv>(&self, kv: &K)
        requires kv.pending() is None,
            has_prefix(kv.committed(),self.namespace_spec(),self.incarnation_spec(),self.records()),
        ensures self.consistent(kv),
    {}
    pub fn namespace(&self) -> (ns: Namespace) ensures ns == self.namespace_spec(), { self.namespace }
    pub fn count(&self) -> (n: u64) requires self.wf(), ensures n == self.records().len(), { self.count }
    pub fn admission(&self) -> (out: Option<Admission>)
        ensures match out { Some(a) => self.active() && a.namespace == self.namespace_spec() && a.incarnation == self.incarnation_spec(),
            None => !self.active() },
    {
        if self.admitted { Some(Admission { namespace: self.namespace, incarnation: self.incarnation }) } else { None }
    }
    pub fn check(&self, captured: &Admission) -> (ok: bool)
        ensures ok == (self.active() && captured.namespace == self.namespace_spec() && captured.incarnation == self.incarnation_spec()),
    {
        self.admitted && captured.namespace.owner == self.namespace.owner
            && captured.namespace.coordinator == self.namespace.coordinator
            && captured.incarnation == self.incarnation
    }
    pub fn fence(&mut self)
        requires old(self).wf(),
        ensures final(self).wf(), !final(self).active(), final(self).records() == old(self).records(),
            final(self).namespace_spec() == old(self).namespace_spec(), final(self).incarnation_spec() == old(self).incarnation_spec(),
    { self.admitted = false; self.can_activate = false; }

    pub fn create<K: DurableKv>(kv: &mut K, namespace: Namespace, bootstrap: Vec<u8>) -> (out: Result<Self,JournalError>)
        requires old(kv).wf(),
        ensures final(kv).wf(),
            old(kv).pending() is None ==> frames(old(kv).committed(),final(kv).committed(),namespace),
            bootstrap_options(final(kv),old(kv).committed(),old(kv).pending(),namespace,bootstrap@),
            match out { Ok(j) => j.wf() && !j.active() && j.consistent(final(kv))
            && j.namespace_spec() == namespace && j.incarnation_spec() == 0 && j.records() == seq![bootstrap@], Err(_) => true },
    {
        let ghost initial = kv.committed();
        let ghost initial_pending = kv.pending();
        let settled = kv.settle();
        proof {
            settlement_transitive(initial,initial_pending,kv.committed(),kv.pending());
            assert forall|candidate: Map<Seq<u8>,Seq<u8>>| settled_map(kv.committed(),kv.pending(),candidate) implies
                exists|base: Map<Seq<u8>,Seq<u8>>| settled_map(initial,initial_pending,base)
                    && (candidate == base || candidate == bootstrap_map(base,namespace,bootstrap@)) by {
                assert(settled_map(initial,initial_pending,candidate));
            }
        }
        if !settled { return Err(JournalError::Io); }
        let head = journal_key(namespace,false,0);
        match kv.read(&head) {
            ReadResult::Found(_) => return Err(JournalError::Exists),
            ReadResult::Io => return Err(JournalError::Io), ReadResult::Absent => {},
        }
        let encoded = match encode_record(&bootstrap) { Ok(v) => v, Err(e) => return Err(e) };
        let mut batch = Vec::new();
        batch.push(WriteOp { key: journal_key(namespace,true,0), value: Some(encoded) });
        batch.push(WriteOp { key: head, value: Some(encode_head(0,1)) });
        let ghost before = kv.committed();
        let ghost ops = batch@;
        proof {
            reveal_with_fuel(apply_batch,3);
            batch_namespace_frames(before,namespace,ops);
            batch_idempotent(before,ops);
        }
        let result = kv.submit(batch);
        proof {
            reveal_with_fuel(apply_batch,3);
            batch_settlement(before,ops,kv.committed(),kv.pending());
            assert forall|candidate: Map<Seq<u8>,Seq<u8>>| settled_map(kv.committed(),kv.pending(),candidate) implies
                exists|base: Map<Seq<u8>,Seq<u8>>| settled_map(initial,initial_pending,base)
                    && (candidate == base || candidate == bootstrap_map(base,namespace,bootstrap@)) by {
                assert(settled_map(initial,initial_pending,before));
                assert(candidate == before || candidate == bootstrap_map(before,namespace,bootstrap@));
            }
        }
        match result {
            CommitResult::NotCommitted => Err(JournalError::NotCommitted),
            CommitResult::Unknown => Err(JournalError::Unknown),
            CommitResult::Committed => {
                proof {
                    reveal_with_fuel(apply_batch,3);
                    assert(kv.committed()==bootstrap_map(before,namespace,bootstrap@));
                    bootstrap_prefix(before,namespace,bootstrap@);
                }
                let ghost records = seq![bootstrap@];
                Ok(Self { namespace, incarnation: 0, count: 1, admitted: false, can_activate: true, records: Ghost(records) })
            }
        }
    }

    pub fn recover<K: DurableKv>(kv: &mut K, namespace: Namespace) -> (out: Result<Self,JournalError>)
        requires old(kv).wf(),
        ensures final(kv).wf(),
            settled_map(old(kv).committed(),old(kv).pending(),final(kv).committed()),
            forall|candidate: Map<Seq<u8>,Seq<u8>>| settled_map(final(kv).committed(),final(kv).pending(),candidate)
                ==> settled_map(old(kv).committed(),old(kv).pending(),candidate),
            match out { Ok(j) => j.wf() && !j.active() && j.consistent(final(kv))
            && j.namespace_spec() == namespace, Err(_) => true },
    {
        hide(key_wire); hide(record_wire); hide(head_wire);
        // This barrier precedes even the head read; Unknown is never a stutter.
        let ghost initial = kv.committed();
        let ghost initial_pending = kv.pending();
        let settled = kv.settle();
        proof { settlement_transitive(initial,initial_pending,kv.committed(),kv.pending()); }
        if !settled { return Err(JournalError::Io); }
        let key = journal_key(namespace,false,0);
        let bytes = match kv.read(&key) {
            ReadResult::Found(v) => v, ReadResult::Absent => return Err(JournalError::Missing), ReadResult::Io => return Err(JournalError::Io),
        };
        let (incarnation,count) = match decode_head(&bytes) { Ok(v) => v, Err(e) => return Err(e) };
        let ghost map = kv.committed();
        let ghost mut records = Seq::<Seq<u8>>::empty();
        let mut i = 0u64;
        while i < count
            invariant kv.wf(), kv.pending() is None, kv.committed() == map,
                initial == old(kv).committed(), initial_pending == old(kv).pending(),
                settled_map(initial,initial_pending,map),
                i <= count, count > 0, records.len() == i,
                map.contains_key(key_wire(namespace,false,0)), map[key_wire(namespace,false,0)] == head_wire(incarnation,count),
                entries_match(map,namespace,records),
            decreases count-i,
        {
            let key = journal_key(namespace,true,i);
            let bytes = match kv.read(&key) {
                ReadResult::Found(v) => v, ReadResult::Absent => return Err(JournalError::Corrupt), ReadResult::Io => return Err(JournalError::Io),
            };
            let payload = match decode_record(&bytes) { Ok(v) => v, Err(e) => return Err(e) };
            proof {
                assert(entry_matches(map,namespace,i,payload@));
                extend_entries(map,namespace,records,payload@);
                records = records.push(payload@);
            }
            i += 1;
        }
        proof { assert(has_prefix(map,namespace,incarnation,records)); }
        Ok(Self { namespace, incarnation, count, admitted: false, can_activate: true, records: Ghost(records) })
    }

    /// Startup-only reads do not admit an actor or expose any actor capability.
    pub fn read_record<K: DurableKv>(&mut self, kv: &mut K, index: u64) -> (out: Result<Vec<u8>,JournalError>)
        requires old(self).wf(), old(kv).wf(), old(self).consistent(old(kv)),
        ensures final(self).wf(), final(kv).wf(), final(kv).committed() == old(kv).committed(),
            final(kv).pending() == old(kv).pending(), final(self).records() == old(self).records(),
            final(self).namespace_spec() == old(self).namespace_spec(), final(self).incarnation_spec() == old(self).incarnation_spec(),
            final(self).consistent(final(kv)),
            out is Ok ==> *final(self) == *old(self),
            match out { Ok(bytes) => index < old(self).records().len() && bytes@ == old(self).records()[index as int],
                Err(_) => !final(self).active() },
    {
        if index >= self.count { self.fence(); return Err(JournalError::Missing); }
        proof { prefix_entry(kv.committed(),self.namespace,self.incarnation,self.records@,index as int); }
        let key = journal_key(self.namespace,true,index);
        let bytes = match kv.read(&key) {
            ReadResult::Found(v) => v,
            ReadResult::Absent => { self.fence(); return Err(JournalError::Corrupt); },
            ReadResult::Io => { self.fence(); return Err(JournalError::Io); },
        };
        match decode_record(&bytes) {
            Ok(v) => { proof { record_injective(v@,self.records@[index as int]); } Ok(crate::bytes::copy_bytes(v)) },
            Err(e) => { self.fence(); Err(e) },
        }
    }

    // The adapter must serialize this check together with the following submit.
    // It is a stale-handle check, NOT a newly invented KV CAS primitive.
    fn check_head<K: DurableKv>(&mut self, kv: &mut K) -> (out: Result<(),JournalError>)
        requires old(self).wf(), old(kv).wf(),
        ensures final(self).wf(), final(kv).wf(), final(kv).committed() == old(kv).committed(), final(kv).pending() == old(kv).pending(),
            final(self).records() == old(self).records(), final(self).namespace_spec() == old(self).namespace_spec(),
            final(self).incarnation_spec() == old(self).incarnation_spec(),
            out is Ok ==> *final(self) == *old(self) && final(kv).pending() is None,
            out is Err ==> !final(self).active(),
    {
        let key = journal_key(self.namespace,false,0);
        let bytes = match kv.read(&key) {
            ReadResult::Found(v) => v,
            ReadResult::Absent => { self.fence(); return Err(JournalError::Missing); },
            ReadResult::Io => { self.fence(); return Err(JournalError::Io); },
        };
        let (incarnation,count) = match decode_head(&bytes) { Ok(v) => v, Err(e) => { self.fence(); return Err(e); } };
        if incarnation != self.incarnation || count != self.count { self.fence(); return Err(JournalError::Inactive); }
        Ok(())
    }

    pub fn activate<K: DurableKv>(&mut self, kv: &mut K) -> (out: Result<Admission,JournalError>)
        requires old(self).wf(), old(kv).wf(), old(self).consistent(old(kv)),
        ensures final(self).wf(), final(kv).wf(), final(self).records() == old(self).records(),
            final(self).namespace_spec() == old(self).namespace_spec(),
            frames(old(kv).committed(),final(kv).committed(),old(self).namespace_spec()),
            settlement_options(final(kv),old(kv).committed(),activation_map(old(kv).committed(),
                old(self).namespace_spec(),(old(self).incarnation_spec()+1) as u64,old(self).records())),
            match out { Ok(a) => final(self).active() && final(self).consistent(final(kv))
                && a.namespace == final(self).namespace_spec() && a.incarnation == final(self).incarnation_spec()
                && a.incarnation == old(self).incarnation_spec()+1,
                Err(_) => !final(self).active() },
    {
        if !self.can_activate || self.admitted { self.fence(); return Err(JournalError::Inactive); }
        if self.incarnation == u64::MAX { self.fence(); return Err(JournalError::Exhausted); }
        if let Err(e) = self.check_head(kv) { return Err(e); }
        let next = self.incarnation+1;
        let mut batch = Vec::new();
        batch.push(WriteOp { key: journal_key(self.namespace,false,0), value: Some(encode_head(next,self.count)) });
        let ghost before = kv.committed();
        let ghost ops = batch@;
        proof {
            reveal_with_fuel(apply_batch,3);
            activate_prefix(before,self.namespace,self.incarnation,next,self.records@);
            batch_namespace_frames(before,self.namespace,ops);
            batch_idempotent(before,ops);
        }
        let result = kv.submit(batch);
        proof {
            reveal_with_fuel(apply_batch,3);
            batch_settlement(before,ops,kv.committed(),kv.pending());
        }
        match result {
            CommitResult::Committed => {
                proof { activate_prefix(before,self.namespace,self.incarnation,next,self.records@); }
                self.incarnation = next; self.admitted = true; self.can_activate = false;
                Ok(Admission { namespace: self.namespace, incarnation: next })
            },
            CommitResult::NotCommitted => { self.fence(); Err(JournalError::NotCommitted) },
            CommitResult::Unknown => { self.fence(); Err(JournalError::Unknown) },
        }
    }

    pub fn append<K: DurableKv>(&mut self, kv: &mut K, entry: Vec<u8>) -> (out: Result<(),JournalError>)
        requires old(self).wf(), old(kv).wf(), old(self).consistent(old(kv)),
        ensures final(self).wf(), final(kv).wf(), final(self).namespace_spec() == old(self).namespace_spec(),
            final(self).incarnation_spec() == old(self).incarnation_spec(),
            frames(old(kv).committed(),final(kv).committed(),old(self).namespace_spec()),
            settlement_options(final(kv),old(kv).committed(),append_map(old(kv).committed(),
                old(self).namespace_spec(),old(self).incarnation_spec(),old(self).records(),entry@)),
            has_prefix(final(kv).committed(),old(self).namespace_spec(),old(self).incarnation_spec(),old(self).records())
                || has_prefix(final(kv).committed(),old(self).namespace_spec(),old(self).incarnation_spec(),old(self).records().push(entry@)),
            match out { Ok(()) => final(self).active() && final(self).consistent(final(kv))
                && final(self).records() == old(self).records().push(entry@),
                Err(_) => !final(self).active() && final(self).records() == old(self).records() },
    {
        if !self.admitted { self.fence(); return Err(JournalError::Inactive); }
        if self.count == u64::MAX { self.fence(); return Err(JournalError::Exhausted); }
        let encoded = match encode_record(&entry) { Ok(v) => v, Err(e) => { self.fence(); return Err(e); } };
        if let Err(e) = self.check_head(kv) { return Err(e); }
        let mut batch = Vec::new();
        batch.push(WriteOp { key: journal_key(self.namespace,true,self.count), value: Some(encoded) });
        batch.push(WriteOp { key: journal_key(self.namespace,false,0), value: Some(encode_head(self.incarnation,self.count+1)) });
        let ghost before = kv.committed();
        let ghost ops = batch@;
        proof {
            reveal_with_fuel(apply_batch,3);
            append_prefix(before,self.namespace,self.incarnation,self.records@,entry@);
            batch_namespace_frames(before,self.namespace,ops);
            batch_idempotent(before,ops);
        }
        let result = kv.submit(batch);
        proof {
            reveal_with_fuel(apply_batch,3);
            batch_settlement(before,ops,kv.committed(),kv.pending());
        }
        match result {
            CommitResult::Committed => {
                proof { append_prefix(before,self.namespace,self.incarnation,self.records@,entry@); }
                self.count += 1;
                self.records = Ghost(self.records@.push(entry@));
                Ok(())
            },
            CommitResult::NotCommitted => { self.fence(); Err(JournalError::NotCommitted) },
            CommitResult::Unknown => { self.fence(); Err(JournalError::Unknown) },
        }
    }
}

/// Byte batches used by the journal are fixed puts. Reapplying such a batch
/// after an Unknown that already committed cannot create a third outcome.
pub proof fn batch_settlement(before: Map<Seq<u8>,Seq<u8>>, ops: Seq<WriteOp>,
    observed: Map<Seq<u8>,Seq<u8>>, pending: Option<Seq<WriteOp>>)
    requires observed == before || observed == apply_batch(before,ops),
        pending is None || pending == Some(ops),
        apply_batch(apply_batch(before,ops),ops) == apply_batch(before,ops),
    ensures forall|candidate: Map<Seq<u8>,Seq<u8>>| settled_map(observed,pending,candidate)
        ==> candidate == before || candidate == apply_batch(before,ops),
{
    assert forall|candidate: Map<Seq<u8>,Seq<u8>>| settled_map(observed,pending,candidate)
        implies candidate == before || candidate == apply_batch(before,ops) by {
        if observed == before {} else { assert(apply_batch(observed,ops) == observed); }
    }
}

pub proof fn activate_prefix(m: Map<Seq<u8>,Seq<u8>>, ns: Namespace, old_inc: u64, new_inc: u64, records: Seq<Seq<u8>>)
    requires has_prefix(m,ns,old_inc,records), records.len() <= u64::MAX,
    ensures has_prefix(m.insert(key_wire(ns,false,0),head_wire(new_inc,records.len() as u64)),ns,new_inc,records),
{
    hide(key_wire); hide(record_wire); hide(head_wire);
    let next = m.insert(key_wire(ns,false,0),head_wire(new_inc,records.len() as u64));
    assert forall|i:int| 0<=i<records.len() implies
        #[trigger] entry_matches(next,ns,i as u64,records[i]) by {
        let key = key_wire(ns,true,i as u64);
        prefix_entry(m,ns,old_inc,records,i);
        if key == key_wire(ns,false,0) { key_injective(ns,true,i as u64,ns,false,0); }
        assert(next.contains_key(key));
        assert(next[key] == record_wire(records[i]));
    }
}
pub proof fn append_prefix(m: Map<Seq<u8>,Seq<u8>>, ns: Namespace, incarnation: u64, records: Seq<Seq<u8>>, entry: Seq<u8>)
    requires has_prefix(m,ns,incarnation,records), records.len() < u64::MAX,
    ensures has_prefix(m.insert(key_wire(ns,true,records.len() as u64),record_wire(entry))
        .insert(key_wire(ns,false,0),head_wire(incarnation,(records.len()+1) as u64)),ns,incarnation,records.push(entry)),
{
    hide(key_wire); hide(record_wire); hide(head_wire);
    let next = m.insert(key_wire(ns,true,records.len() as u64),record_wire(entry))
        .insert(key_wire(ns,false,0),head_wire(incarnation,(records.len()+1) as u64));
    assert(records.push(entry).len()==records.len()+1);
    assert(next.contains_key(key_wire(ns,false,0)));
    assert(next[key_wire(ns,false,0)]==head_wire(incarnation,records.push(entry).len() as u64));
    assert forall|i:int| 0<=i<records.push(entry).len() implies
        #[trigger] entry_matches(next,ns,i as u64,records.push(entry)[i]) by {
        assert((i as u64) as int == i);
        if key_wire(ns,true,i as u64) == key_wire(ns,false,0) { key_injective(ns,true,i as u64,ns,false,0); }
        if i < records.len() {
            assert((records.len() as u64) as int == records.len());
            prefix_entry(m,ns,incarnation,records,i);
            assert(records.push(entry)[i] == records[i]);
            if key_wire(ns,true,i as u64) == key_wire(ns,true,records.len() as u64) { key_injective(ns,true,i as u64,ns,true,records.len() as u64); }
        } else {
            assert(i == records.len());
            assert(records.push(entry)[i] == entry);
        }
    }
}
/// General WriteBatch framing, including keys that are not journal keys.
pub proof fn batch_frames(m: Map<Seq<u8>,Seq<u8>>, ops: Seq<WriteOp>, key: Seq<u8>)
    requires forall|i: int| 0 <= i < ops.len() ==> ops[i].key@ != key,
    ensures apply_batch(m,ops).contains_key(key) == m.contains_key(key),
        m.contains_key(key) ==> apply_batch(m,ops)[key] == m[key],
    decreases ops.len(),
{
    if ops.len() > 0 {
        assert forall|i: int| 0 <= i < ops.skip(1).len() implies ops.skip(1)[i].key@ != key by { assert(ops.skip(1)[i] == ops[i+1]); }
        batch_frames(apply_op(m,ops[0]),ops.skip(1),key);
    }
}
pub open spec fn same_key(a: Map<Seq<u8>,Seq<u8>>, b: Map<Seq<u8>,Seq<u8>>, key: Seq<u8>) -> bool {
    a.contains_key(key) == b.contains_key(key) && (a.contains_key(key) ==> a[key] == b[key])
}
pub proof fn batch_preserves_same_key(a: Map<Seq<u8>,Seq<u8>>, b: Map<Seq<u8>,Seq<u8>>, ops: Seq<WriteOp>, key: Seq<u8>)
    requires same_key(a,b,key),
    ensures same_key(apply_batch(a,ops),apply_batch(b,ops),key),
    decreases ops.len(),
{
    if ops.len() > 0 { batch_preserves_same_key(apply_op(a,ops[0]),apply_op(b,ops[0]),ops.skip(1),key); }
}
pub proof fn batch_overwrites_key(a: Map<Seq<u8>,Seq<u8>>, b: Map<Seq<u8>,Seq<u8>>, ops: Seq<WriteOp>, key: Seq<u8>)
    requires exists|i: int| 0 <= i < ops.len() && ops[i].key@ == key,
    ensures same_key(apply_batch(a,ops),apply_batch(b,ops),key),
    decreases ops.len(),
{
    let i = choose|i: int| 0 <= i < ops.len() && ops[i].key@ == key;
    if ops[0].key@ == key {
        batch_preserves_same_key(apply_op(a,ops[0]),apply_op(b,ops[0]),ops.skip(1),key);
    } else {
        assert(i > 0);
        assert(ops.skip(1)[i-1].key@ == key);
        batch_overwrites_key(apply_op(a,ops[0]),apply_op(b,ops[0]),ops.skip(1),key);
    }
}
/// Puts and deletes are idempotent even with repeated keys within one batch.
pub proof fn batch_idempotent(m: Map<Seq<u8>,Seq<u8>>, ops: Seq<WriteOp>)
    ensures apply_batch(apply_batch(m,ops),ops) == apply_batch(m,ops),
{
    let once = apply_batch(m,ops);
    let twice = apply_batch(once,ops);
    assert forall|key: Seq<u8>| same_key(twice,once,key) by {
        if exists|i: int| 0 <= i < ops.len() && ops[i].key@ == key {
            batch_overwrites_key(once,m,ops,key);
        } else {
            assert forall|i: int| 0 <= i < ops.len() implies ops[i].key@ != key by {};
            batch_frames(once,ops,key);
        }
        assert(same_key(twice,once,key));
    }
    assert(twice.dom() =~= once.dom()) by {
        assert forall|key: Seq<u8>| #[trigger] twice.dom().contains(key)
            <==> once.dom().contains(key) by {
            assert(same_key(twice,once,key));
        }
    }
    assert forall|key: Seq<u8>| twice.dom().contains(key) implies
        #[trigger] twice[key] == once[key] by {
        assert(same_key(twice,once,key));
    }
    assert(twice =~= once);
}
/// An unsuccessful settlement may advance an accepted batch, but cannot grow
/// the set of possible future maps. This is derived from raw batch idempotency.
pub proof fn settlement_transitive(before: Map<Seq<u8>,Seq<u8>>, pending: Option<Seq<WriteOp>>,
    observed: Map<Seq<u8>,Seq<u8>>, remaining: Option<Seq<WriteOp>>)
    requires settled_map(before,pending,observed), remaining is None || remaining == pending,
    ensures forall|candidate: Map<Seq<u8>,Seq<u8>>| settled_map(observed,remaining,candidate)
        ==> settled_map(before,pending,candidate),
{
    if let Some(ops) = pending { batch_idempotent(before,ops); }
    assert forall|candidate: Map<Seq<u8>,Seq<u8>>| settled_map(observed,remaining,candidate)
        implies settled_map(before,pending,candidate) by {}
}

pub proof fn bootstrap_prefix(m: Map<Seq<u8>,Seq<u8>>, ns: Namespace, bootstrap: Seq<u8>)
    ensures has_prefix(bootstrap_map(m,ns,bootstrap),ns,0,seq![bootstrap]),
{
    if key_wire(ns,true,0) == key_wire(ns,false,0) { key_injective(ns,true,0,ns,false,0); }
    assert forall|i:int| 0<=i<seq![bootstrap].len() implies
        #[trigger] entry_matches(bootstrap_map(m,ns,bootstrap),ns,i as u64,seq![bootstrap][i]) by {
        assert(i == 0);
    }
}

/// Decoding the same committed namespace cannot recover a different incarnation,
/// length or payload sequence. No erased pre-crash actor is an input.
pub proof fn committed_prefix_unique(m: Map<Seq<u8>,Seq<u8>>, ns: Namespace,
    ai: u64, a: Seq<Seq<u8>>, bi: u64, b: Seq<Seq<u8>>)
    requires has_prefix(m,ns,ai,a), has_prefix(m,ns,bi,b),
        a.len() <= u64::MAX, b.len() <= u64::MAX,
    ensures ai == bi, a == b,
{
    head_injective(ai,a.len() as u64,bi,b.len() as u64);
    assert(a.len() == b.len());
    assert forall|i: int| 0 <= i < a.len() implies a[i] == b[i] by {
        prefix_entry(m,ns,ai,a,i);
        prefix_entry(m,ns,bi,b,i);
        record_injective(a[i],b[i]);
    }
    assert(a =~= b);
}

/// A credential captured before a successful admission cannot pass the current
/// namespace-and-incarnation check, even when the actor's record prefix stutters.
pub proof fn admission_fenced(old_admission: Admission, current: Admission)
    requires old_admission.namespace != current.namespace
        || old_admission.incarnation < current.incarnation,
    ensures old_admission.namespace != current.namespace
        || old_admission.incarnation != current.incarnation,
{}

/// Authority framing follows from concrete key injectivity, not a disjointness
/// promise supplied by a backend. Both possible Unknown outcomes obey it.
pub proof fn batch_namespace_frames(m: Map<Seq<u8>,Seq<u8>>, ns: Namespace, ops: Seq<WriteOp>)
    requires forall|i: int| 0 <= i < ops.len() ==> namespace_key(ns,ops[i].key@),
    ensures frames(m,apply_batch(m,ops),ns),
{
    assert forall|key: Seq<u8>| !namespace_key(ns,key) implies
        (apply_batch(m,ops).contains_key(key) == m.contains_key(key)
            && (m.contains_key(key) ==> apply_batch(m,ops)[key] == m[key])) by {
        assert forall|i: int| 0 <= i < ops.len() implies ops[i].key@ != key by {};
        batch_frames(m,ops,key);
    }
}

pub proof fn namespace_frames(m: Map<Seq<u8>,Seq<u8>>, ns: Namespace, other: Namespace, ops: Seq<WriteOp>, entry: bool, index: u64)
    requires ns != other,
        forall|i: int| #![trigger ops[i]] 0 <= i < ops.len() ==> exists|e: bool,n: u64| ops[i].key@ == key_wire(ns,e,n),
    ensures apply_batch(m,ops).contains_key(key_wire(other,entry,index)) == m.contains_key(key_wire(other,entry,index)),
        m.contains_key(key_wire(other,entry,index)) ==> apply_batch(m,ops)[key_wire(other,entry,index)] == m[key_wire(other,entry,index)],
{
    assert forall|i: int| 0 <= i < ops.len() implies ops[i].key@ != key_wire(other,entry,index) by {
        let (e,n) = choose|e: bool,n: u64| ops[i].key@ == key_wire(ns,e,n);
        if ops[i].key@ == key_wire(other,entry,index) { key_injective(ns,e,n,other,entry,index); }
    }
    batch_frames(m,ops,key_wire(other,entry,index));
}
} // verus!
