//! Owned benchmark workers. Each OS thread owns its STO worker and SRPC stream.
//! An unknown outcome ends the run; it is never counted as an ordinary abort.
use crate::{
    client::Client,
    node::Config as NodeConfig,
    storage::engine,
    tpcc::{self, Context, Generator, LoadProgress, Mix, Scale},
    wire::*,
};
use serde::{Deserialize, Serialize};

pub use crate::node::Handle as Host;
use std::{
    sync::{
        atomic::{AtomicBool, Ordering},
        Arc,
    },
    time::{Duration, Instant},
};

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(default, deny_unknown_fields)]
pub struct Config {
    pub scale: Scale,
    pub mix: Mix,
    pub micro: Option<tpcc::Micro>,
    pub seconds: u64,
    pub transactions_per_worker: Option<u64>,
    pub retry_aborted: bool,
    pub startup_timeout_seconds: u64,
    pub cpu_limit_percent: f64,
    pub throttle_cycle_ms: u64,
}
impl Default for Config {
    fn default() -> Self {
        Self {
            scale: Scale::default(),
            mix: Mix::default(),
            micro: None,
            seconds: 30,
            transactions_per_worker: None,
            retry_aborted: true,
            startup_timeout_seconds: 300,
            cpu_limit_percent: 0.0,
            throttle_cycle_ms: 100,
        }
    }
}
impl Config {
    pub fn validate(&self, node: &NodeConfig) -> Result<()> {
        if !self.cpu_limit_percent.is_finite()
            || !(0.0..=100.0).contains(&self.cpu_limit_percent)
            || !(1..=10_000).contains(&self.throttle_cycle_ms)
        {
            return Err(invalid("invalid CPU duty cycle"));
        }
        self.scale.validate()?;
        if let Some(micro) = &self.micro {
            micro.validate()?;
        } else {
            self.mix.validate()?;
        }
        node.schema()?;
        if node.tables != tpcc::tables()
            || node.warehouses_per_shard == 0
            || !node.initial.is_empty()
        {
            return Err(invalid(
                "benchmark requires the native TPC-C schema and transactional loader",
            ));
        }
        if !(1..=86400).contains(&self.seconds)
            || !(1..=86400).contains(&self.startup_timeout_seconds)
            || self.transactions_per_worker == Some(0)
        {
            return Err(invalid("invalid benchmark duration or transaction count"));
        }
        if (0..node.nodes).any(|owner| {
            owner != node.owner
                && !node.peers.contains_key(&owner)
                && !node.replica_groups.contains_key(&owner)
        }) {
            return Err(invalid("benchmark requires endpoints for all shards"));
        }
        Ok(())
    }
}
fn invalid(message: &str) -> Error {
    Error::new(ErrorCode::Invalid, message)
}
fn cancelled() -> Error {
    Error::new(ErrorCode::Closed, "benchmark stopped")
}
fn client(node: &Host, config: &NodeConfig, worker: u32) -> Result<Client> {
    let mut client = Client::new(
        node.network.clone(),
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
fn context<'a>(node: &'a Host, config: &NodeConfig, settings: &'a Config) -> Context<'a> {
    Context {
        schema: &node.schema,
        owner: config.owner,
        migration: config.migration,
        scale: &settings.scale,
    }
}

/// Load the local leader, then wait for every shard's committed completion
/// marker. Readiness probes do not allocate transaction identities or locks.
pub fn prepare(
    node: &Host,
    config: &NodeConfig,
    settings: &Config,
    stop: &AtomicBool,
    progress: impl FnMut(LoadProgress) -> Result<()>,
) -> Result<LoadProgress> {
    settings.validate(config)?;
    let context = context(node, config, settings);
    let mut client = client(node, config, 0)?;
    let mut progress = progress;
    let progress = |step| {
        if stop.load(Ordering::Relaxed) {
            return Err(cancelled());
        }
        progress(step)
    };
    let mut worker = node.store.database.worker().map_err(engine)?;
    let loaded = if let Some(micro) = &settings.micro {
        context.load_micro(
            &mut client,
            node.store.clone(),
            &mut worker,
            micro.keys,
            progress,
        )?
    } else {
        context.load(&mut client, node.store.clone(), &mut worker, progress)?
    };
    let deadline = Instant::now() + Duration::from_secs(settings.startup_timeout_seconds);
    loop {
        if stop.load(Ordering::Relaxed) {
            return Err(cancelled());
        }
        node.store.log.health()?;
        if Instant::now() >= deadline {
            return Err(Error::new(
                ErrorCode::Busy,
                "waiting for all workload loaders timed out",
            ));
        }
        let mut ready = true;
        for owner in 0..config.nodes {
            if owner == config.owner {
                continue;
            }
            let endpoint = if let Some(replicas) = config.replica_groups.get(&owner) {
                match node
                    .network
                    .find_leader(owner, replicas, Duration::from_millis(500))
                {
                    Ok(endpoint) => endpoint,
                    Err(_) => {
                        ready = false;
                        break;
                    }
                }
            } else {
                config.peers[&owner].clone()
            };
            match node.network.call(
                &endpoint,
                Message::ReplicaStatus,
                Duration::from_millis(500),
            ) {
                Ok(Reply::ReplicaStatus(status))
                    if status.owner == owner && status.healthy && status.leader => {}
                _ => {
                    ready = false;
                    break;
                }
            }
        }
        if ready {
            match client.run(node.store.clone(), &mut worker, |tx| {
                if let Some(micro) = &settings.micro {
                    context.all_micro_loaded(tx, micro.keys)
                } else {
                    context.all_loaded(tx)
                }
            }) {
                Ok(true) => return Ok(loaded),
                Ok(false) => (),
                Err(error) if retryable(&error) => (),
                Err(error) => return Err(error),
            }
        }
        std::thread::sleep(Duration::from_millis(20));
    }
}

#[derive(Clone, Debug, Default, Serialize, Deserialize)]
pub struct Stats {
    #[serde(default)]
    pub workload: String,
    #[serde(default)]
    pub owner: u32,
    pub committed: [u64; 5],
    pub aborted: [u64; 5],
    pub elapsed_seconds: f64,
    pub committed_latency_microseconds: u64,
}
impl Stats {
    pub fn transactions_per_second(&self) -> f64 {
        self.committed.iter().sum::<u64>() as f64 / self.elapsed_seconds.max(f64::MIN_POSITIVE)
    }
}
fn retryable(error: &Error) -> bool {
    matches!(
        error.code,
        ErrorCode::Conflict | ErrorCode::Busy | ErrorCode::RouteChanged
    )
}

pub fn run(
    node: &Host,
    config: &NodeConfig,
    settings: &Config,
    stop: Arc<AtomicBool>,
) -> Result<Stats> {
    settings.validate(config)?;
    let started = Instant::now();
    let deadline = started + Duration::from_secs(settings.seconds);
    std::thread::scope(|scope| {
        let mut workers = Vec::new();
        for index in 0..config.workers {
            let stop = stop.clone();
            workers.push(scope.spawn(move || {
                let result = run_worker(node, config, settings, index, deadline, &stop);
                if result.is_err() {
                    stop.store(true, Ordering::Relaxed);
                }
                result
            }));
        }
        let mut total = Stats {
            owner: config.owner,
            workload: if settings.micro.is_some() {
                "micro"
            } else {
                "tpcc"
            }
            .into(),
            ..Stats::default()
        };
        let mut failure = None;
        for worker in workers {
            match worker
                .join()
                .unwrap_or_else(|_| Err(Error::new(ErrorCode::Fatal, "benchmark worker panicked")))
            {
                Ok(stats) => {
                    for i in 0..5 {
                        total.committed[i] += stats.committed[i];
                        total.aborted[i] += stats.aborted[i];
                    }
                    total.committed_latency_microseconds += stats.committed_latency_microseconds;
                }
                Err(error) => {
                    failure.get_or_insert(error);
                }
            }
        }
        total.elapsed_seconds = started.elapsed().as_secs_f64();
        if let Some(error) = failure {
            Err(error)
        } else {
            Ok(total)
        }
    })
}
enum Work {
    Tpcc(tpcc::WorkloadRequest),
    Micro(tpcc::MicroRequest),
}
impl Work {
    fn index(&self) -> usize {
        match self {
            Self::Tpcc(request) => request.index(),
            Self::Micro(request) => request.index(),
        }
    }
    fn execute(
        &self,
        context: &Context<'_>,
        tx: &mut crate::client::Transaction<'_, '_>,
    ) -> Result<()> {
        match self {
            Self::Tpcc(request) => request.execute(context, tx),
            Self::Micro(request) => request.execute(context, tx),
        }
    }
}

fn run_worker(
    node: &Host,
    config: &NodeConfig,
    settings: &Config,
    index: u32,
    deadline: Instant,
    stop: &AtomicBool,
) -> Result<Stats> {
    let context = context(node, config, settings);
    let mut client = client(node, config, index)?;
    let mut worker = node.store.database.worker().map_err(engine)?;
    let seed = settings
        .scale
        .seed
        .wrapping_add(u64::from(index))
        .wrapping_add(u64::from(config.owner) << 32);
    let mut generator = settings
        .micro
        .is_none()
        .then(|| Generator::new(&context, settings.mix.clone(), seed))
        .transpose()?;
    let mut micro = settings
        .micro
        .clone()
        .map(|micro| tpcc::MicroGenerator::new(micro, config.owner, config.nodes, seed))
        .transpose()?;
    let mut dates = 0..0;
    let mut stats = Stats::default();
    let mut attempts = 0;
    let mut pending = None;
    let cycle = Duration::from_millis(settings.throttle_cycle_ms);
    let duty = settings.cpu_limit_percent / 100.0;
    let mut cycle_start = Instant::now();
    let mut work_time = Duration::ZERO;
    while !stop.load(Ordering::Relaxed)
        && Instant::now() < deadline
        && settings
            .transactions_per_worker
            .is_none_or(|limit| attempts < limit)
    {
        node.store.log.health()?;
        if pending.is_none() {
            if micro.is_none() && dates.is_empty() {
                match client.run(node.store.clone(), &mut worker, |tx| {
                    context.reserve_dates(tx, 65_536)
                }) {
                    Ok(range) => dates = range,
                    Err(error) if retryable(&error) => {
                        std::thread::sleep(Duration::from_millis(1));
                        continue;
                    }
                    Err(error) => return Err(error),
                }
            }
            pending = Some(if let Some(micro) = &mut micro {
                Work::Micro(micro.generate())
            } else {
                Work::Tpcc(generator.as_mut().unwrap().next(dates.next().unwrap()))
            });
        }
        let request = pending.as_ref().unwrap();
        let kind = request.index();
        let start = Instant::now();
        let result = client.run(node.store.clone(), &mut worker, |tx| {
            request.execute(&context, tx)
        });
        attempts += 1;
        match result {
            Ok(()) => {
                stats.committed[kind] += 1;
                stats.committed_latency_microseconds = stats
                    .committed_latency_microseconds
                    .saturating_add(start.elapsed().as_micros() as u64);
                pending = None;
            }
            Err(error) if retryable(&error) => {
                stats.aborted[kind] += 1;
                if !settings.retry_aborted {
                    pending = None;
                }
                std::thread::yield_now();
            }
            Err(error) => return Err(error),
        }
        if duty > 0.0 && duty < 1.0 {
            work_time += start.elapsed();
            if work_time >= cycle.mul_f64(duty) {
                let remaining = cycle.saturating_sub(cycle_start.elapsed());
                // Short sleeps keep process cancellation responsive.
                let wake = Instant::now() + remaining;
                while !stop.load(Ordering::Relaxed) && Instant::now() < wake.min(deadline) {
                    std::thread::sleep(
                        wake.min(deadline)
                            .saturating_duration_since(Instant::now())
                            .min(Duration::from_millis(20)),
                    );
                }
                cycle_start = Instant::now();
                work_time = Duration::ZERO;
            } else if cycle_start.elapsed() >= cycle {
                cycle_start = Instant::now();
                work_time = Duration::ZERO;
            }
        }
    }
    Ok(stats)
}
