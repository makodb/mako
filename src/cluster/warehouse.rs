//! Warehouse engine-handle selection, shared by the production FFI and Verus.
//!
//! Handles are non-owning tokens supplied by the legacy engine opener. The FFI
//! holds one directory mutex across lookup/open/register; indexes live until
//! engine shutdown. Local bytes are keyed by the physical participant, not just
//! the warehouse. Proxies belong to the originating participant and are never
//! entered in the engine's reverse physical-binding index. A proxy can follow
//! any destination; its RPC captures the full grant again under transaction
//! admission. This cache never caches a route or authorizes an engine access.
use std::collections::HashMap;
use vstd::prelude::*;
use crate::types::{Grant, Status};

verus! {

#[derive(Clone, Copy)]
pub struct HandleKey {
    pub table: u64,
    pub warehouse: u32,
    pub owner: u32,
    pub proxy: bool,
}

#[derive(Clone, Copy)]
pub struct Selection {
    pub key: HandleKey,
    pub grant: Grant,
    /// Zero asks the FFI to call the registered engine opener, not a fallback.
    pub handle: usize,
}

pub open spec fn code(key: HandleKey) -> u128 {
    ((key.table as u128) << 64) | ((key.owner as u128) << 32)
        | key.warehouse as u128
}

fn encode(key: HandleKey) -> (out: u128)
    ensures out == code(key),
{
    ((key.table as u128) << 64) | ((key.owner as u128) << 32)
        | key.warehouse as u128
}

pub proof fn code_injective(a: HandleKey, b: HandleKey)
    requires code(a) == code(b),
    ensures a.table == b.table, a.owner == b.owner, a.warehouse == b.warehouse,
{
    let at = a.table; let ao = a.owner; let aw = a.warehouse;
    let bt = b.table; let bo = b.owner; let bw = b.warehouse;
    assert((((at as u128) << 64) | ((ao as u128) << 32) | aw as u128)
        == (((bt as u128) << 64) | ((bo as u128) << 32) | bw as u128)
        ==> at == bt && ao == bo && aw == bw) by (bit_vector);
}

pub struct WarehouseDirectory {
    warehouses_per_shard: u32,
    total: u32,
    tables: HashMap<u64, ()>,
    local: HashMap<u128, usize>,
    proxy: HashMap<u128, usize>,
    // Also reject an opener accidentally returning source bytes as destination
    // bytes (including two participants in a single process).
    identities: HashMap<usize, HandleKey>,
}

pub struct WarehouseView {
    pub warehouses_per_shard: u32,
    pub total: u32,
    pub tables: Map<u64, ()>,
    pub local: Map<u128, usize>,
    pub proxy: Map<u128, usize>,
    pub identities: Map<usize, HandleKey>,
}

impl View for WarehouseDirectory {
    type V = WarehouseView;
    closed spec fn view(&self) -> WarehouseView {
        WarehouseView { warehouses_per_shard: self.warehouses_per_shard,
            total: self.total, tables: self.tables@, local: self.local@,
            proxy: self.proxy@, identities: self.identities@ }
    }
}

impl WarehouseDirectory {
    pub open spec fn valid_key(&self, key: HandleKey) -> bool {
        self@.warehouses_per_shard > 0 && key.warehouse > 0
            && key.warehouse <= self@.total
            && key.owner < self@.total / self@.warehouses_per_shard
            && self@.tables.contains_key(key.table)
    }

    pub open spec fn handles(&self, proxy: bool) -> Map<u128, usize> {
        if proxy { self@.proxy } else { self@.local }
    }

    pub open spec fn cached_spec(&self, key: HandleKey) -> usize {
        if self.handles(key.proxy).contains_key(code(key)) {
            self.handles(key.proxy)[code(key)]
        } else { 0 }
    }

    pub fn new(warehouses_per_shard: u32, total: u32) -> (out: Result<Self, Status>)
        ensures match out {
            Ok(d) => d@.warehouses_per_shard == warehouses_per_shard
                && d@.total == total && warehouses_per_shard > 0
                && total > 0 && total % warehouses_per_shard == 0
                && d@.tables == Map::<u64, ()>::empty()
                && d@.local == Map::<u128, usize>::empty()
                && d@.proxy == Map::<u128, usize>::empty()
                && d@.identities == Map::<usize, HandleKey>::empty(),
            Err(s) => s == Status::Invalid,
        },
    {
        if warehouses_per_shard == 0 || total == 0 || total % warehouses_per_shard != 0 {
            return Err(Status::Invalid);
        }
        Ok(Self { warehouses_per_shard, total, tables: HashMap::new(),
            local: HashMap::new(), proxy: HashMap::new(), identities: HashMap::new() })
    }

    pub fn dimensions(&self) -> (out: (u32, u32))
        ensures out == (self@.warehouses_per_shard, self@.total),
    {
        (self.warehouses_per_shard, self.total)
    }

    /// Only the bootstrap catalog supplies canonical table identities.
    pub fn add_table(&mut self, table: u64) -> (out: Status)
        ensures out == Status::Ok,
            final(self)@.tables == old(self)@.tables.insert(table, ()),
            final(self)@.local == old(self)@.local, final(self)@.proxy == old(self)@.proxy,
            final(self)@.identities == old(self)@.identities,
            final(self)@.total == old(self)@.total,
            final(self)@.warehouses_per_shard == old(self)@.warehouses_per_shard,
    {
        self.tables.insert(table, ());
        Status::Ok
    }

    fn valid(&self, key: HandleKey) -> (out: bool)
        ensures out == self.valid_key(key),
    {
        self.warehouses_per_shard > 0 && key.warehouse > 0
            && key.warehouse <= self.total
            && key.owner < self.total / self.warehouses_per_shard
            && self.tables.contains_key(&key.table)
    }

    pub fn cached(&self, key: HandleKey) -> (out: Result<usize, Status>)
        ensures match out {
            Ok(h) => self.valid_key(key) && h == self.cached_spec(key),
            Err(s) => !self.valid_key(key) && s == Status::Invalid,
        },
    {
        if !self.valid(key) { return Err(Status::Invalid); }
        let index = encode(key);
        let found = if key.proxy { self.proxy.get(&index) } else { self.local.get(&index) };
        Ok(match found { Some(handle) => *handle, None => 0 })
    }

    /// Select using the caller's current immutable route snapshot. The grant is
    /// returned unchanged, including epoch; no owner-only cache is consulted.
    pub fn select(&self, table: u64, warehouse: u32, participant: u32,
                  grant: Grant) -> (out: Result<Selection, Status>)
        ensures match out {
            Ok(s) => s.grant == grant && s.key.table == table
                && s.key.warehouse == warehouse && s.key.owner == participant
                && s.key.proxy == (grant.owner != participant)
                && self.valid_key(s.key) && s.handle == self.cached_spec(s.key),
            Err(s) => s == Status::Invalid,
        },
    {
        let key = HandleKey { table, warehouse, owner: participant,
            proxy: grant.owner != participant };
        if self.warehouses_per_shard == 0
            || grant.owner >= self.total / self.warehouses_per_shard {
            return Err(Status::Invalid);
        }
        match self.cached(key) {
            Ok(handle) => Ok(Selection { key, grant, handle }),
            Err(status) => Err(status),
        }
    }

    /// Copy/cleanup explicitly names physical bytes even before route commit.
    /// A partial return therefore finds that owner's existing index, never the
    /// index currently selected for another owner.
    pub fn local(&self, table: u64, warehouse: u32, owner: u32)
        -> (out: Result<HandleKey, Status>)
        ensures match out {
            Ok(k) => self.valid_key(k) && k.table == table
                && k.warehouse == warehouse && k.owner == owner && !k.proxy,
            Err(s) => s == Status::Invalid,
        },
    {
        let key = HandleKey { table, warehouse, owner, proxy: false };
        if self.valid(key) { Ok(key) } else { Err(Status::Invalid) }
    }

    /// Registration is immutable and idempotent. Pointer uniqueness is checked
    /// across both kinds of handle, not assumed from a source numeric window.
    pub fn register(&mut self, key: HandleKey, handle: usize) -> (out: Status)
        ensures final(self)@.total == old(self)@.total,
            final(self)@.warehouses_per_shard == old(self)@.warehouses_per_shard,
            final(self)@.tables == old(self)@.tables,
            out == Status::Ok ==> handle != 0 && final(self).valid_key(key)
                && final(self).cached_spec(key) == handle,
            out == Status::Ok ==> final(self).handles(key.proxy)
                == old(self).handles(key.proxy).insert(code(key), handle),
            out == Status::Ok ==> final(self)@.identities
                == old(self)@.identities.insert(handle, key),
            out == Status::Ok && old(self).handles(key.proxy).contains_key(code(key))
                ==> old(self).handles(key.proxy)[code(key)] == handle,
            out == Status::Ok && old(self)@.identities.contains_key(handle)
                ==> old(self)@.identities[handle] == key,
            out != Status::Ok ==> final(self)@.local == old(self)@.local
                && final(self)@.proxy == old(self)@.proxy
                && final(self)@.identities == old(self)@.identities,
            out == Status::Ok ==> final(self)@.identities.contains_key(handle)
                && final(self)@.identities[handle].table == key.table
                && final(self)@.identities[handle].warehouse == key.warehouse
                && final(self)@.identities[handle].owner == key.owner
                && final(self)@.identities[handle].proxy == key.proxy,
            key.proxy ==> final(self)@.local == old(self)@.local,
            !key.proxy ==> final(self)@.proxy == old(self)@.proxy,
    {
        if handle == 0 || !self.valid(key) { return Status::Invalid; }
        let index = encode(key);
        let previous = if key.proxy { self.proxy.get(&index) } else { self.local.get(&index) };
        if let Some(previous) = previous {
            if *previous != handle { return Status::Invalid; }
        }
        if let Some(previous) = self.identities.get(&handle) {
            if previous.table != key.table || previous.warehouse != key.warehouse
                || previous.owner != key.owner || previous.proxy != key.proxy {
                return Status::Invalid;
            }
        }
        self.identities.insert(handle, key);
        if key.proxy { self.proxy.insert(index, handle); }
        else { self.local.insert(index, handle); }
        Status::Ok
    }
}

} // verus!
