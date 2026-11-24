//! Runnable CRUD and concurrent transaction example. Every operation uses the
//! production coordinator, including writes to remote shards and explicit abort.
use crate::{
    client::{Client, Transaction},
    configuration::select_nodes,
    migration::{Kind, TableSpec},
    node::{Config as NodeConfig, Handle},
    storage::engine,
    wire::*,
};
use serde::{Deserialize, Serialize};
use std::{
    sync::{
        atomic::{AtomicBool, Ordering},
        Arc,
    },
    time::{Duration, Instant},
};
use sto_tpcc_ffi::native::Worker;

pub fn tables() -> Vec<TableSpec> {
    vec![TableSpec {
        id: 1,
        name: "records".into(),
        kind: Kind::Governed,
        warehouse: true,
        initial_owner: 0,
    }]
}

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(default, deny_unknown_fields)]
pub struct Settings {
    pub rounds_per_worker: u64,
    /// When set, keep incrementing until the duration elapses instead of using
    /// a fixed number of commits. Signals still stop and join the workers.
    pub seconds: Option<u64>,
    pub timeout_seconds: u64,
}
impl Default for Settings {
    fn default() -> Self {
        Self {
            rounds_per_worker: 100,
            seconds: None,
            timeout_seconds: 120,
        }
    }
}
#[derive(Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct File {
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub node: Option<NodeConfig>,
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub nodes: Vec<NodeConfig>,
    #[serde(default)]
    pub transactions: Settings,
}
impl File {
    pub fn into_parts(self) -> Result<(Vec<NodeConfig>, Settings)> {
        let nodes = select_nodes(self.node, self.nodes)?;
        for node in &nodes {
            self.transactions.validate(node)?;
        }
        Ok((nodes, self.transactions))
    }
}
impl Settings {
    pub fn validate(&self, config: &NodeConfig) -> Result<()> {
        config.schema()?;
        if config.tables != tables()
            || config.warehouses_per_shard != 1
            || !config.initial.is_empty()
            || self.rounds_per_worker == 0
            || !(1..=86400).contains(&self.timeout_seconds)
            || self
                .seconds
                .is_some_and(|seconds| !(1..=86400).contains(&seconds))
            || (0..config.nodes).any(|owner| {
                owner != config.owner
                    && !config.peers.contains_key(&owner)
                    && !config.replica_groups.contains_key(&owner)
            })
        {
            return Err(Error::new(
                ErrorCode::Invalid,
                "invalid transaction example configuration",
            ));
        }
        Ok(())
    }
}
#[derive(Debug, Serialize, Deserialize)]
pub struct Stats {
    pub shards: u32,
    pub workers: u32,
    pub committed: u64,
    pub aborted: u64,
    pub initial_counter: u64,
    pub final_counter: u64,
    pub elapsed_seconds: f64,
}
fn ensure(condition: bool, message: &str) -> Result<()> {
    if condition {
        Ok(())
    } else {
        Err(Error::new(ErrorCode::Fatal, message))
    }
}
fn cancelled(stop: &AtomicBool) -> Result<()> {
    if stop.load(Ordering::Relaxed) {
        Err(Error::new(ErrorCode::Closed, "transaction example stopped"))
    } else {
        Ok(())
    }
}
fn retryable(error: &Error) -> bool {
    matches!(
        error.code,
        ErrorCode::Conflict | ErrorCode::Busy | ErrorCode::RouteChanged
    )
}
fn client(host: &Handle, config: &NodeConfig, worker: u32) -> Result<Client> {
    let mut client = Client::new(
        host.network.clone(),
        config.peers.clone(),
        Stream {
            node: config.owner,
            worker,
        },
        0,
        Duration::from_secs(10),
    )?;
    for (owner, replicas) in &config.replica_groups {
        client.set_replicas(*owner, replicas.clone())?;
    }
    Ok(client)
}
struct Session<'a> {
    host: &'a Handle,
    client: Client,
    worker: Worker,
    stop: &'a AtomicBool,
    timeout: Duration,
}
impl Session<'_> {
    fn run<T>(
        &mut self,
        mut body: impl FnMut(&mut Transaction<'_, '_>) -> Result<T>,
    ) -> Result<(T, u64)> {
        let deadline = Instant::now() + self.timeout;
        let mut aborted = 0;
        loop {
            cancelled(self.stop)?;
            match self
                .client
                .run(self.host.store.clone(), &mut self.worker, &mut body)
            {
                Ok(result) => return Ok((result, aborted)),
                Err(error) if retryable(&error) && Instant::now() < deadline => {
                    aborted += 1;
                    std::thread::sleep(Duration::from_micros(100));
                }
                Err(error) => return Err(error),
            }
        }
    }
}
fn session<'a>(
    host: &'a Handle,
    config: &NodeConfig,
    settings: &Settings,
    index: u32,
    stop: &'a AtomicBool,
) -> Result<Session<'a>> {
    Ok(Session {
        host,
        client: client(host, config, index)?,
        worker: host.store.database.worker().map_err(engine)?,
        stop,
        timeout: Duration::from_secs(settings.timeout_seconds),
    })
}
fn target(host: &Handle, owner: u32, key: &[u8]) -> Result<Target> {
    host.schema.target(
        TableKey {
            table: 1,
            warehouse: owner + 1,
        },
        key,
        host.store.owner,
        host.store.migration,
    )
}
fn counter(tx: &mut Transaction<'_, '_>, host: &Handle, nodes: u32) -> Result<u64> {
    let mut first = None;
    for owner in 0..nodes {
        let value = tx.get(
            &target(host, owner, b"example/counter")?,
            b"example/counter",
        )?;
        let value = match value {
            None => 0,
            Some(value) => u64::from_le_bytes(
                value
                    .as_slice()
                    .try_into()
                    .map_err(|_| Error::new(ErrorCode::Fatal, "invalid example counter"))?,
            ),
        };
        ensure(
            first.is_none_or(|first| first == value),
            "cross-shard counter differs",
        )?;
        first = Some(value);
    }
    Ok(first.unwrap_or(0))
}
fn wait_for_peers(
    host: &Handle,
    config: &NodeConfig,
    settings: &Settings,
    stop: &AtomicBool,
) -> Result<()> {
    let deadline = Instant::now() + Duration::from_secs(settings.timeout_seconds);
    loop {
        cancelled(stop)?;
        host.store.log.health()?;
        let mut ready = true;
        for owner in 0..config.nodes {
            if owner == config.owner {
                continue;
            }
            let endpoint = match config.replica_groups.get(&owner) {
                Some(replicas) => {
                    host.network
                        .find_leader(owner, replicas, Duration::from_millis(500))
                }
                None => Ok(config.peers[&owner].clone()),
            };
            let status = endpoint.and_then(|endpoint| {
                host.network.call(
                    &endpoint,
                    Message::ReplicaStatus,
                    Duration::from_millis(500),
                )
            });
            if !matches!(status, Ok(Reply::ReplicaStatus(status)) if status.owner == owner && status.healthy && status.leader)
            {
                ready = false;
                break;
            }
        }
        if ready {
            return Ok(());
        }
        if Instant::now() >= deadline {
            return Err(Error::new(
                ErrorCode::Busy,
                "waiting for transaction peers timed out",
            ));
        }
        std::thread::sleep(Duration::from_millis(20));
    }
}
fn crud(host: &Handle, config: &NodeConfig, session: &mut Session<'_>) -> Result<()> {
    session.run(|tx| {
        for owner in 0..config.nodes {
            for row in 0..5 {
                let key = format!("example/row/{row}");
                tx.mutate(
                    &target(host, owner, key.as_bytes())?,
                    Mutation::Put,
                    key.as_bytes(),
                    format!("value/{row}").as_bytes(),
                )?;
            }
        }
        Ok(())
    })?;
    session.run(|tx| {
        for owner in 0..config.nodes {
            let table = TableKey {
                table: 1,
                warehouse: owner + 1,
            };
            for reverse in [false, true] {
                let rows = tx.scan_routed(
                    &host.schema,
                    table,
                    Bound::Included(b"example/row/".to_vec()),
                    Bound::Excluded(b"example/row0".to_vec()),
                    reverse,
                    10,
                )?;
                ensure(rows.len() == 5, "example scan row count")?;
                for (index, row) in rows.iter().enumerate() {
                    let index = if reverse { 4 - index } else { index };
                    ensure(
                        row.key == format!("example/row/{index}").as_bytes()
                            && row.value == format!("value/{index}").as_bytes(),
                        "example scan contents",
                    )?;
                    ensure(
                        tx.get(&target(host, owner, &row.key)?, &row.key)?
                            == Some(row.value.clone()),
                        "example point read",
                    )?;
                }
            }
        }
        Ok(())
    })?;
    for size in [1, 10, 50, 200, 5, 500] {
        let value = vec![b'X'; size];
        session.run(|tx| {
            for owner in 0..config.nodes {
                tx.mutate(
                    &target(host, owner, b"example/overwrite")?,
                    Mutation::Put,
                    b"example/overwrite",
                    &value,
                )?;
            }
            Ok(())
        })?;
        session.run(|tx| {
            for owner in 0..config.nodes {
                ensure(
                    tx.get(
                        &target(host, owner, b"example/overwrite")?,
                        b"example/overwrite",
                    )? == Some(value.clone()),
                    "growing/shrinking overwrite",
                )?;
            }
            Ok(())
        })?;
    }
    // Mutations reach remote participants before abort. The following reads
    // check their physical cleanup as well as the coordinator's local rollback.
    {
        let mut tx = session
            .client
            .begin(host.store.clone(), &mut session.worker)?;
        for owner in 0..config.nodes {
            tx.mutate(
                &target(host, owner, b"example/overwrite")?,
                Mutation::Put,
                b"example/overwrite",
                b"aborted",
            )?;
        }
        tx.abort()?;
    }
    session.run(|tx| {
        for owner in 0..config.nodes {
            let target = target(host, owner, b"example/overwrite")?;
            ensure(
                tx.get(&target, b"example/overwrite")? == Some(vec![b'X'; 500]),
                "abort changed committed data",
            )?;
            tx.mutate(&target, Mutation::Remove, b"example/overwrite", b"")?;
        }
        Ok(())
    })?;
    session.run(|tx| {
        for owner in 0..config.nodes {
            let target = target(host, owner, b"example/overwrite")?;
            ensure(
                tx.get(&target, b"example/overwrite")?.is_none(),
                "delete retained row",
            )?;
            tx.mutate(
                &target,
                Mutation::Insert,
                b"example/overwrite",
                b"reinserted",
            )?;
        }
        Ok(())
    })?;
    session.run(|tx| {
        for owner in 0..config.nodes {
            ensure(
                tx.get(
                    &target(host, owner, b"example/overwrite")?,
                    b"example/overwrite",
                )? == Some(b"reinserted".to_vec()),
                "reinsert value",
            )?;
        }
        Ok(())
    })?;
    Ok(())
}

pub fn run(
    host: &Handle,
    config: &NodeConfig,
    settings: &Settings,
    stop: Arc<AtomicBool>,
) -> Result<Stats> {
    settings.validate(config)?;
    ensure(
        config.owner == 0,
        "transaction example must run on shard zero",
    )?;
    wait_for_peers(host, config, settings, &stop)?;
    let started = Instant::now();
    let initial = {
        let mut session = session(host, config, settings, 0, &stop)?;
        crud(host, config, &mut session)?;
        session.run(|tx| counter(tx, host, config.nodes))?.0
    };
    let end = settings
        .seconds
        .map(|seconds| Instant::now() + Duration::from_secs(seconds));
    let (committed, aborted) = std::thread::scope(|scope| -> Result<(u64, u64)> {
        let mut threads = Vec::new();
        for index in 0..config.workers {
            let stop = &stop;
            threads.push(scope.spawn(move || -> Result<(u64, u64)> {
                let work = (|| {
                    let mut session = session(host, config, settings, index, stop)?;
                    let (mut committed, mut aborted) = (0, 0);
                    while end.map_or(committed < settings.rounds_per_worker, |end| {
                        Instant::now() < end
                    }) {
                        let (_, retries) = session.run(|tx| {
                            let next = counter(tx, host, config.nodes)?.checked_add(1).ok_or_else(
                                || Error::new(ErrorCode::Capacity, "example counter overflow"),
                            )?;
                            for owner in 0..config.nodes {
                                tx.mutate(
                                    &target(host, owner, b"example/counter")?,
                                    Mutation::Put,
                                    b"example/counter",
                                    &next.to_le_bytes(),
                                )?;
                            }
                            Ok(())
                        })?;
                        committed += 1;
                        aborted += retries;
                    }
                    Ok((committed, aborted))
                })();
                if work.is_err() {
                    stop.store(true, Ordering::Relaxed);
                }
                work
            }));
        }
        let (mut committed, mut aborted, mut failure) = (0, 0, None);
        for thread in threads {
            match thread
                .join()
                .unwrap_or_else(|_| Err(Error::new(ErrorCode::Fatal, "example worker panicked")))
            {
                Ok((c, a)) => {
                    committed += c;
                    aborted += a;
                }
                Err(error) => {
                    failure.get_or_insert(error);
                }
            }
        }
        if let Some(error) = failure {
            Err(error)
        } else {
            Ok((committed, aborted))
        }
    })?;
    let final_counter = session(host, config, settings, 0, &stop)?
        .run(|tx| counter(tx, host, config.nodes))?
        .0;
    ensure(
        initial.checked_add(committed) == Some(final_counter),
        "concurrent committed total differs from counter",
    )?;
    Ok(Stats {
        shards: config.nodes,
        workers: config.workers,
        committed,
        aborted,
        initial_counter: initial,
        final_counter,
        elapsed_seconds: started.elapsed().as_secs_f64(),
    })
}
