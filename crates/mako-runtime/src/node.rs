//! Production process ownership for the native shard runtime. Configuration is
//! validated before workers or listeners start; stop joins all affine workers.
use crate::{
    gateway,
    migration::{RunningMigration, Schema, TableSpec},
    receiver::RunningShard,
    rpc::Network,
    storage::{engine, CommitLog, Store, VolatileLog},
    wire::*,
};
use serde::{Deserialize, Serialize};
use std::{collections::BTreeMap, path::PathBuf, sync::Arc};

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct ReplicaPeer {
    pub id: u16,
    pub address: String,
}
#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct ReplicaConfig {
    pub group: u64,
    pub id: u16,
    pub listen: String,
    pub peers: Vec<ReplicaPeer>,
    pub directory: PathBuf,
    #[serde(default)]
    pub create: bool,
    #[serde(default = "durable")]
    pub sync_writes: bool,
}
#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct PersistenceConfig {
    pub directory: PathBuf,
    #[serde(default)]
    pub create: bool,
    #[serde(default = "durable")]
    pub sync_writes: bool,
}
fn durable() -> bool {
    true
}

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct InitialRow {
    pub table: u64,
    #[serde(default)]
    pub warehouse: u32,
    pub key: Vec<u8>,
    pub value: Vec<u8>,
}
fn one() -> u32 {
    1
}
fn capacity() -> usize {
    64
}
#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Config {
    pub owner: u32,
    pub nodes: u32,
    pub listen: String,
    pub tables: Vec<TableSpec>,
    #[serde(default = "one")]
    pub workers: u32,
    #[serde(default = "one")]
    pub gateway_workers: u32,
    #[serde(default = "capacity")]
    pub queue_capacity: usize,
    #[serde(default)]
    pub warehouses_per_shard: u32,
    #[serde(default)]
    pub peers: BTreeMap<u32, String>,
    #[serde(default)]
    pub migration: bool,
    #[serde(default)]
    pub initial: Vec<InitialRow>,
    #[serde(default)]
    pub replication: Option<ReplicaConfig>,
    #[serde(default)]
    pub persistence: Option<PersistenceConfig>,
    /// Explicit development/test mode. Production requires a local or Raft log.
    #[serde(default)]
    pub volatile: bool,
    #[serde(default)]
    pub replica_groups: BTreeMap<u32, ReplicaSet>,
}
impl Config {
    pub fn schema(&self) -> Result<Arc<Schema>> {
        if self
            .persistence
            .as_ref()
            .is_some_and(|cfg| cfg.directory.as_os_str().is_empty())
            || self
                .replication
                .as_ref()
                .is_some_and(|cfg| cfg.directory.as_os_str().is_empty())
        {
            return Err(Error::new(
                ErrorCode::Invalid,
                "log directory must not be empty",
            ));
        }
        let persistent = self.persistence.is_some() || self.replication.is_some();
        if persistent == self.volatile {
            return Err(Error::new(ErrorCode::Invalid,
                "configure a local persistence directory or Raft; volatile=true is an exclusive development/test mode"));
        }

        if self.replica_groups.iter().any(|(owner, group)| {
            *owner >= self.nodes
                || group.endpoints.is_empty()
                || group.endpoints.iter().any(String::is_empty)
        }) {
            return Err(Error::new(
                ErrorCode::Invalid,
                "invalid shard replica group",
            ));
        }
        if self.persistence.is_some() && self.replication.is_some() {
            return Err(Error::new(
                ErrorCode::Invalid,
                "configure local persistence or Raft, not both",
            ));
        }
        if self.persistence.is_some() && self.migration {
            return Err(Error::new(
                ErrorCode::Invalid,
                "durable restart of live migration is unsupported by the sharding runtime",
            ));
        }
        if self.replication.is_some() && self.migration {
            return Err(Error::new(
                ErrorCode::Invalid,
                "live migration with replication replay is unsupported by the sharding runtime",
            ));
        }
        if (self.replication.is_some() || self.persistence.is_some()) && !self.initial.is_empty() {
            return Err(Error::new(
                ErrorCode::Invalid,
                "load replicated rows through transactions after leader readiness",
            ));
        }
        if self.owner >= self.nodes
            || self.workers == 0
            || self.gateway_workers == 0
            || self.gateway_workers > 64
            || self.queue_capacity == 0
            || self.listen.is_empty()
        {
            return Err(Error::new(
                ErrorCode::Invalid,
                "invalid shard configuration",
            ));
        }
        if self
            .nodes
            .checked_mul(self.workers)
            .is_none_or(|lanes| lanes > 256)
        {
            return Err(Error::new(
                ErrorCode::Capacity,
                "too many participant lanes",
            ));
        }
        if self.peers.keys().any(|owner| *owner >= self.nodes) {
            return Err(Error::new(
                ErrorCode::Invalid,
                "peer outside shard topology",
            ));
        }
        if self.migration
            && (0..self.nodes).any(|owner| owner != self.owner && !self.peers.contains_key(&owner))
        {
            return Err(Error::new(
                ErrorCode::Invalid,
                "migration requires every peer endpoint",
            ));
        }
        let schema = Schema::new(self.tables.clone(), self.nodes, self.warehouses_per_shard)?;
        for row in &self.initial {
            if row.key.len() > MAX_KEY || row.value.len() > MAX_VALUE {
                return Err(Error::new(
                    ErrorCode::Capacity,
                    "initial row exceeds storage limits",
                ));
            }
            let target = schema.target(
                TableKey {
                    table: row.table,
                    warehouse: row.warehouse,
                },
                &row.key,
                self.owner,
                false,
            )?;
            if target.owner != self.owner {
                return Err(Error::new(
                    ErrorCode::Invalid,
                    "initial row belongs to another shard",
                ));
            }
        }
        Ok(schema)
    }
}

/// Shareable application services. Each calling thread creates its own affine
/// storage worker; the process still owns shutdown through RunningNode.
#[derive(Clone)]
pub struct Handle {
    pub store: Arc<Store>,
    pub schema: Arc<Schema>,
    pub network: Arc<Network>,
}
impl From<&RunningNode> for Handle {
    fn from(node: &RunningNode) -> Self {
        Self {
            store: node.store.clone(),
            schema: node.schema.clone(),
            network: node.network.clone(),
        }
    }
}

pub struct RunningNode {
    server: Option<RunningShard>,
    migration: Option<RunningMigration>,
    pub store: Arc<Store>,
    pub schema: Arc<Schema>,
    pub network: Arc<Network>,
    #[cfg(feature = "replication")]
    log: Option<Arc<crate::log::Log>>,
}
impl RunningNode {
    pub fn start(mut config: Config) -> Result<Self> {
        let schema = config.schema()?;
        #[cfg(not(feature = "replication"))]
        if config.replication.is_some() || config.persistence.is_some() {
            return Err(Error::new(
                ErrorCode::Invalid,
                "this executable was built without native Raft support",
            ));
        }
        #[cfg(feature = "replication")]
        let durable_log = if config.replication.is_some() || config.persistence.is_some() {
            Some(crate::log::Log::new(std::time::Duration::from_secs(10))?)
        } else {
            None
        };
        let log: Arc<dyn CommitLog> = {
            #[cfg(feature = "replication")]
            {
                durable_log.as_ref().map_or_else(
                    || Arc::new(VolatileLog) as Arc<dyn CommitLog>,
                    |log| log.clone(),
                )
            }
            #[cfg(not(feature = "replication"))]
            {
                Arc::new(VolatileLog)
            }
        };
        let store = Store::new(config.owner, config.migration, log)?;
        {
            let mut loader = store.database.worker().map_err(engine)?;
            for spec in &config.tables {
                let warehouses: Box<dyn Iterator<Item = u32>> = if spec.warehouse {
                    Box::new(
                        config.owner * config.warehouses_per_shard + 1
                            ..=(config.owner + 1) * config.warehouses_per_shard,
                    )
                } else {
                    Box::new(std::iter::once(0))
                };
                for warehouse in warehouses {
                    let key = TableKey {
                        table: spec.id,
                        warehouse,
                    };
                    store.open(&mut loader, key, schema.log_id(key)?)?;
                }
            }
            for chunk in config.initial.chunks(128) {
                let mut tx = loader.begin().map_err(engine)?;
                for row in chunk {
                    let table = store.table(TableKey {
                        table: row.table,
                        warehouse: row.warehouse,
                    })?;
                    tx.put(&table.tree, &row.key, &row.value).map_err(engine)?;
                }
                tx.commit().map_err(engine)?;
            }
        }
        if config.migration {
            schema.install()?;
        }
        #[cfg(feature = "replication")]
        if let Some(log) = &durable_log {
            let (directory, group, sync_writes, create, mode) =
                if let Some(settings) = &config.replication {
                    let mode = crate::log::Mode::Raft(raft_host::Config {
                        group: settings.group,
                        node: settings.id,
                        listen: settings.listen.clone(),
                        peers: settings
                            .peers
                            .iter()
                            .map(|peer| raft_host::Peer {
                                id: peer.id,
                                address: peer.address.clone(),
                            })
                            .collect(),
                        directory: settings.directory.join("log"),
                        create: settings.create,
                        sync_writes: settings.sync_writes,
                        ..raft_host::Config::default()
                    });
                    (
                        &settings.directory,
                        settings.group,
                        settings.sync_writes,
                        settings.create,
                        mode,
                    )
                } else {
                    let settings = config.persistence.as_ref().expect("configured durable log");
                    let mode = crate::log::Mode::Local(raft_host::LocalConfig {
                        group: u64::from(config.owner),
                        directory: settings.directory.join("log"),
                        create: settings.create,
                        sync_writes: settings.sync_writes,
                    });
                    (
                        &settings.directory,
                        u64::from(config.owner),
                        settings.sync_writes,
                        settings.create,
                        mode,
                    )
                };
            if !create
                && (!directory.join("rows/CURRENT").is_file()
                    || !directory.join("log/FORMAT").is_file())
            {
                return Err(Error::new(ErrorCode::Invalid,"durable rows/log missing or legacy format; create must be explicit and existing stores require offline conversion"));
            }
            if directory.join("raft").exists() {
                return Err(Error::new(
                    ErrorCode::Invalid,
                    "legacy Raft directory requires offline format conversion",
                ));
            }
            let mut specs = config.tables.clone();
            specs.sort_by_key(|spec| spec.id);
            let identity = crate::wire::encode(&(
                config.owner,
                config.nodes,
                config.warehouses_per_shard,
                specs,
            ))?;
            let rows = crate::durable::DurableRows::open(
                &directory.join("rows"),
                group,
                sync_writes,
                &identity,
            )?;
            log.start(mode, &store, schema.clone(), rows)?;
        }
        let network = Arc::new(Network::default());
        let server = RunningShard::start_with_gateway(
            store.clone(),
            network.clone(),
            &config.listen,
            config.nodes,
            config.workers,
            config.queue_capacity,
            gateway::Config {
                schema: schema.clone(),
                endpoints: config.peers.clone(),
                replicas: config.replica_groups.clone(),
                workers: config.gateway_workers as usize,
            },
        )?;
        config
            .peers
            .insert(config.owner, server.address().to_owned());
        let mut node = Self {
            server: Some(server),
            migration: None,
            store,
            schema,
            network,
            #[cfg(feature = "replication")]
            log: durable_log,
        };
        if config.migration {
            node.migration = Some(RunningMigration::start(
                node.store.clone(),
                node.schema.clone(),
                node.network.clone(),
                config.peers,
            )?);
        }
        Ok(node)
    }
    pub fn address(&self) -> &str {
        self.server.as_ref().expect("running node").address()
    }
    pub fn ready(&self) -> bool {
        #[cfg(feature = "replication")]
        if let Some(log) = &self.log {
            return log.health().is_ok()
                && log.status().is_ok_and(|status| {
                    status.leader.is_some()
                        && status.applied.index > 0
                        && status.applied.term == status.term
                });
        }
        self.migration.as_ref().is_none_or(RunningMigration::ready)
    }
    pub fn health(&self) -> Result<()> {
        self.store.health()
    }
    pub fn shutdown(&mut self) {
        self.server.take();
        self.store.shutdown();
        self.migration.take();
        #[cfg(feature = "replication")]
        if let Some(log) = self.log.take() {
            log.shutdown();
        }
    }
}
impl Drop for RunningNode {
    fn drop(&mut self) {
        self.shutdown();
    }
}
