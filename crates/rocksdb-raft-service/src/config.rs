use crate::wire::{Error, ErrorCode, Operation, Result, MAX_WIRE_BYTES};
use mako_sharding::{directory::RouteTable, routing::Snapshot, types::{Boundary, Grant}};
use serde::{Deserialize, Serialize};
use std::{collections::BTreeSet, path::{Path, PathBuf}, sync::Arc};

#[derive(Clone, Copy, Debug, Default, Deserialize, Serialize, Eq, PartialEq)]
#[serde(rename_all = "lowercase")]
pub enum DurabilityMode { #[default] Sync, Async }
impl DurabilityMode {
    pub fn sync_writes(self) -> bool { self == Self::Sync }
    pub fn warning(self) -> Option<&'static str> {
        (self == Self::Async).then_some("ASYNC EXPERIMENT: WAL enabled, no foreground commit/log sync; acknowledged commits can be lost on power or machine crash. This is not durable quorum.")
    }
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(default, deny_unknown_fields)]
pub struct Limits {
    pub rpc_bytes: usize, pub key_bytes: usize, pub value_bytes: usize,
    pub transaction_bytes: usize, pub transaction_operations: usize, pub scan_rows: usize,
    pub idle_ms: u64, pub commit_ms: u64,
}
impl Default for Limits {
    fn default() -> Self { Self { rpc_bytes: MAX_WIRE_BYTES, key_bytes: 64 * 1024,
        value_bytes: 1024 * 1024, transaction_bytes: raft_host::MAX_COMMAND_BYTES / 2,
        transaction_operations: 10_000, scan_rows: 4096, idle_ms: 30_000, commit_ms: 10_000 } }
}
impl Limits {
    pub fn operation(&self, operation: &Operation) -> Result<()> {
        let (key, value, end, rows) = match operation {
            Operation::Get { key } | Operation::Exists { key } | Operation::Delete { key } => (key, None, None, None),
            Operation::Put { key, value } | Operation::Insert { key, value } => (key, Some(value), None, None),
            Operation::Scan { start, end, limit } | Operation::ReverseScan { start, end, limit } => (start, None, end.as_ref(), Some(*limit)),
        };
        if key.len() > self.key_bytes || value.is_some_and(|v| v.len() > self.value_bytes)
            || end.is_some_and(|v| v.len() > self.key_bytes) || rows.is_some_and(|n| n > self.scan_rows) {
            return Err(Error::new(ErrorCode::TooLarge, "operation exceeds configured key/value/scan limit"));
        }
        if end.is_some_and(|end| end.as_slice() <= key.as_slice()) || rows == Some(0) {
            return Err(Error::new(ErrorCode::Invalid, "scan requires nonempty half-open range and positive limit"));
        }
        Ok(())
    }
}
#[derive(Clone, Debug, Deserialize, Serialize, PartialEq, Eq)]
#[serde(deny_unknown_fields)]
pub struct Peer { pub id: u16, pub raft: String, pub client: String }
#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct GroupConfig {
    pub id: u64, pub data_path: PathBuf, pub raft_path: PathBuf,
    pub raft_listen: String, pub peers: Vec<Peer>,
}
#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct RouteBoundary { pub start: Vec<u8>, pub owner: u32, pub ownership_version: u64 }
#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct Route { pub id: u64, pub name: String, pub boundaries: Vec<RouteBoundary> }
#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct RoutingConfig { pub version: u64, pub shards: Vec<Placement>, pub tables: Vec<Route> }
#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct Placement { pub id: u32, pub peers: Vec<Peer> }

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(default, deny_unknown_fields)]
pub struct ClientLimits { pub rpc_bytes: usize, pub request_timeout_ms: u64 }
impl Default for ClientLimits {
    fn default() -> Self { Self { rpc_bytes: MAX_WIRE_BYTES, request_timeout_ms: 12_000 } }
}
/// Connection-only options. No local replica identity, storage paths or server
/// worker configuration are accepted by the client JSON/API boundary.
#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct ClientConfig {
    pub routing: RoutingConfig,
    #[serde(default)] pub limits: ClientLimits,
    #[serde(default = "default_context_capacity")] pub context_capacity: usize,
}
fn default_context_capacity() -> usize { 128 }
impl ClientConfig {
    pub fn read(path: impl AsRef<Path>) -> Result<Self> {
        let file = std::fs::File::open(path).map_err(|e| Error::new(ErrorCode::Invalid, e.to_string()))?;
        let config: Self = serde_json::from_reader(file).map_err(|e| Error::new(ErrorCode::Invalid, e.to_string()))?;
        config.validate()?;
        Ok(config)
    }
    pub fn validate(&self) -> Result<()> {
        if self.context_capacity == 0 || self.context_capacity > 65536
            || self.limits.rpc_bytes < 1024 || self.limits.rpc_bytes > MAX_WIRE_BYTES
            || self.limits.request_timeout_ms == 0 || self.limits.request_timeout_ms > u64::from(u32::MAX) {
            return Err(Error::new(ErrorCode::Invalid, "invalid client context capacity, RPC limit or timeout"));
        }
        self.routing.validate()
    }
    pub fn snapshot(&self) -> Arc<Snapshot> { self.routing.snapshot() }
    pub fn route_id(&self, name: &str) -> Result<u64> { self.routing.route_id(name) }
}
impl RoutingConfig {
    pub fn validate(&self) -> Result<()> {
        let invalid = |s| Error::new(ErrorCode::Invalid, s);
        let mut global_ids = BTreeSet::new();
        for shard in &self.shards {
            if !global_ids.insert(shard.id) || shard.peers.is_empty() { return Err(invalid("duplicate/empty global shard placement")); }
            let mut ids = BTreeSet::new();
            let mut addresses = BTreeSet::new();
            for peer in &shard.peers {
                if !ids.insert(peer.id) || !addresses.insert(&peer.raft) || peer.raft.is_empty() || peer.client.is_empty() {
                    return Err(invalid("invalid global shard replica membership"));
                }
            }
        }
        if global_ids.is_empty() { return Err(invalid("global shard catalog is empty")); }
        let mut names = BTreeSet::new();
        for table in &self.tables {
            if table.name.is_empty() || table.name.len() > 4096 || !names.insert(&table.name) {
                return Err(invalid("empty, duplicate or overlong routing table name"));
            }
        }
        let nodes: Vec<_> = global_ids.into_iter().collect();
        if !self.snapshot().validate(&nodes) { return Err(invalid("invalid fixed routing snapshot")); }
        Ok(())
    }
    pub fn snapshot(&self) -> Arc<Snapshot> {
        Arc::new(Snapshot { version: self.version, tables: self.tables.iter().map(|t| RouteTable {
            table: t.id, boundaries: t.boundaries.iter().map(|b| Boundary { start: b.start.clone(),
                grant: Grant { owner: b.owner, ownership_version: b.ownership_version } }).collect(),
        }).collect() })
    }
    pub fn route_id(&self, name: &str) -> Result<u64> {
        self.tables.iter().find(|t| t.name == name).map(|t| t.id)
            .ok_or_else(|| Error::new(ErrorCode::UnknownTable, "table not in immutable routing catalog"))
    }
}
#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct Config {
    pub node: u16, pub listen: String, pub groups: Vec<GroupConfig>, pub routing: RoutingConfig,
    #[serde(default)] pub durability: DurabilityMode,
    #[serde(default = "default_workers")] pub workers_per_group: usize,
    #[serde(default = "default_capacity")] pub lane_capacity: usize,
    #[serde(default = "default_admin_workers")] pub admin_workers: usize,
    #[serde(default = "default_capacity")] pub admin_capacity: usize,
    #[serde(default)] pub limits: Limits,
}
fn default_workers() -> usize { 64 }
fn default_admin_workers() -> usize { 4 }
fn default_capacity() -> usize { 64 }
impl Config {
    pub fn read(path: impl AsRef<Path>) -> Result<Self> {
        let file = std::fs::File::open(path).map_err(|e| Error::new(ErrorCode::Invalid, e.to_string()))?;
        let config: Self = serde_json::from_reader(file).map_err(|e| Error::new(ErrorCode::Invalid, e.to_string()))?;
        config.validate()?;
        Ok(config)
    }
    pub fn validate(&self) -> Result<()> {
        let invalid = |s| Error::new(ErrorCode::Invalid, s);
        if self.groups.is_empty() || self.workers_per_group == 0 || self.workers_per_group > 4096
            || self.admin_workers == 0 || self.admin_workers > 256 || self.lane_capacity == 0
            || self.lane_capacity > 4096 || self.admin_capacity == 0 || self.admin_capacity > 65536 {
            return Err(invalid("nonempty groups and bounded positive workers/queue capacities required"));
        }
        let l = &self.limits;
        if l.rpc_bytes < 1024 || l.rpc_bytes > MAX_WIRE_BYTES || l.transaction_bytes > raft_host::MAX_COMMAND_BYTES
            || l.key_bytes > 1024 * 1024 || l.value_bytes > MAX_WIRE_BYTES || l.scan_rows > 100_000
            || l.transaction_operations == 0 || l.transaction_operations > 100_000
            || l.idle_ms == 0 || l.commit_ms == 0 {
            return Err(invalid("invalid RPC/transaction limits"));
        }
        self.routing.validate()?;
        let mut groups = BTreeSet::new();
        let mut paths = BTreeSet::new();
        let mut raft_addresses = BTreeSet::new();
        for g in &self.groups {
            if u32::try_from(g.id).is_err() || !groups.insert(g.id) || g.peers.is_empty()
                || !paths.insert(g.data_path.clone()) || !paths.insert(g.raft_path.clone())
                || !raft_addresses.insert(g.raft_listen.clone()) || g.raft_listen == self.listen {
                return Err(invalid("duplicate/invalid group, persistent path or endpoint"));
            }
            let placement = self.routing.shards.iter().find(|p| u64::from(p.id) == g.id)
                .ok_or_else(|| invalid("hosted group missing from global placement"))?;
            if g.peers != placement.peers { return Err(invalid("hosted peers differ from immutable global placement")); }
            let mut ids = BTreeSet::new();
            let mut addresses = BTreeSet::new();
            for peer in &g.peers {
                if !ids.insert(peer.id) || !addresses.insert(&peer.raft) || peer.raft.is_empty() || peer.client.is_empty() {
                    return Err(invalid("invalid duplicate Raft peer"));
                }
            }
            let own = g.peers.iter().find(|peer| peer.id == self.node).ok_or_else(|| invalid("node absent from group peers"))?;
            if own.raft != g.raft_listen || own.client != self.listen { return Err(invalid("local endpoints must match peer membership")); }
        }
        Ok(())
    }
    pub fn snapshot(&self) -> Arc<Snapshot> { self.routing.snapshot() }
    pub fn route_id(&self, name: &str) -> Result<u64> { self.routing.route_id(name) }
    pub fn client_config(&self) -> ClientConfig {
        ClientConfig { routing: self.routing.clone(),
            limits: ClientLimits { rpc_bytes: self.limits.rpc_bytes, request_timeout_ms: self.limits.commit_ms.saturating_add(2000) },
            context_capacity: self.workers_per_group }
    }
}

/// Reuses the native routing iterator, including unbounded ranges; a scan cannot
/// silently trim a different shard's interval, even if its row limit is small.
pub fn check_scope(snapshot: &Snapshot, table: u64, group: u64, operation: &Operation) -> Result<()> {
    let valid = match operation {
        Operation::Get { key } | Operation::Exists { key } | Operation::Put { key, .. }
        | Operation::Insert { key, .. } | Operation::Delete { key } =>
            snapshot.capture(table, key).is_some_and(|r| u64::from(r.grant.owner) == group),
        Operation::Scan { start, end, .. } | Operation::ReverseScan { start, end, .. } => {
            let mut segments = snapshot.segments(table, start, end.as_deref())
                .map_err(|_| Error::new(ErrorCode::Invalid, "invalid scan routing range"))?;
            let mut valid = true;
            while let Some(segment) = segments.next() {
                if u64::from(segment.grant.owner) != group { valid = false; break; }
            }
            valid
        }
    };
    if valid { Ok(()) } else { Err(Error::new(ErrorCode::CrossShard, "operation crosses the transaction's bound user DB/shard")) }
}
