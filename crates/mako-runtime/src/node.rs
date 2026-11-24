//! Production process ownership for the native shard runtime. Configuration is
//! validated before workers or listeners start; stop joins all affine workers.
use crate::{
    gateway,
    migration::{RunningMigration, Schema, TableSpec},
    receiver::RunningShard,
    rpc::Network,
    storage::{engine, Store, VolatileLog},
    wire::*,
};
use serde::{Deserialize, Serialize};
use std::{collections::BTreeMap, sync::Arc};

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
}
impl Config {
    pub fn schema(&self) -> Result<Arc<Schema>> {
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

pub struct RunningNode {
    server: Option<RunningShard>,
    migration: Option<RunningMigration>,
    pub store: Arc<Store>,
    pub schema: Arc<Schema>,
    pub network: Arc<Network>,
}
impl RunningNode {
    pub fn start(mut config: Config) -> Result<Self> {
        let schema = config.schema()?;
        let store = Store::new(config.owner, config.migration, Arc::new(VolatileLog))?;
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
        self.migration.as_ref().is_none_or(RunningMigration::ready)
    }
    pub fn shutdown(&mut self) {
        self.server.take();
        self.migration.take();
    }
}
impl Drop for RunningNode {
    fn drop(&mut self) {
        self.shutdown();
    }
}
