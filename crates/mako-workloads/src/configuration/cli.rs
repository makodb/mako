use super::{invalid, read, topology, BenchmarkFile};
use crate::{benchmark, tpcc::Micro, wire::*};
use std::path::PathBuf;

#[derive(Default)]
pub struct Args {
    native: Option<PathBuf>,
    topology: Option<PathBuf>,
    owners: Option<String>,
    site: Option<String>,
    workers: Option<u32>,
    seconds: Option<u64>,
    startup: Option<u64>,
    cpu: Option<f64>,
    cycle: Option<u64>,
    replicated: bool,
    micro: bool,
    migration: bool,
    volatile: bool,
    raft: Vec<(Option<u32>, PathBuf)>,
    data: Option<PathBuf>,
    create: bool,
    pub load_only: bool,
    pub serve_only: bool,
    pub exit_after_run: bool,
    pub stdin_close: bool,
    pub print_config: bool,
    pub help: bool,
}
impl Args {
    pub fn parse(arguments: impl IntoIterator<Item = String>) -> Result<Self> {
        let mut result = Self::default();
        let mut arguments = arguments.into_iter();
        while let Some(argument) = arguments.next() {
            let (flag, mut inline) = if argument.starts_with("--") {
                match argument.split_once('=') {
                    Some((flag, value)) => (flag.to_owned(), Some(value.to_owned())),
                    None => (argument, None),
                }
            } else if argument.starts_with('-') && argument.len() > 2 && argument.is_ascii() {
                (argument[..2].to_owned(), Some(argument[2..].to_owned()))
            } else {
                (argument, None)
            };
            let mut value = || {
                inline
                    .take()
                    .or_else(|| arguments.next())
                    .ok_or_else(|| invalid(format!("{flag} requires a value")))
            };
            match flag.as_str() {
                "--config" => result.native = Some(value()?.into()),
                "-q" | "--shard-config" => result.topology = Some(value()?.into()),
                "-g" | "--shard-index" | "-L" | "--local-shards" => {
                    if result.owners.is_some() {
                        return Err(invalid("choose shard-index or local-shards once"));
                    }
                    result.owners = Some(value()?);
                }
                "-N" | "--site-name" | "-P" | "--replica-name" => result.site = Some(value()?),
                "-t" | "--num-threads" => result.workers = Some(number(&value()?, &flag)?),
                "-u" | "--runtime" => result.seconds = Some(number(&value()?, &flag)?),
                "-T" | "--startup-timeout-sec" => result.startup = Some(number(&value()?, &flag)?),
                "-C" | "--cpu-limit" => result.cpu = Some(number(&value()?, &flag)?),
                "-Y" | "--throttle-cycle" => result.cycle = Some(number(&value()?, &flag)?),
                "-E" | "--storage-engine" => {
                    if value()? != "rust" {
                        return Err(invalid("the storage engine is native Rust"));
                    }
                }
                "-R" | "--replication" => {
                    if value()? != "raft" {
                        return Err(invalid("native replication supports only Raft"));
                    }
                    result.replicated = true;
                }
                "-F" | "--raft-config" => {
                    let path = value()?;
                    let entry = if let Some((owner, file)) = path.split_once('=') {
                        (
                            Some(number(owner, "Raft shard qualifier")?),
                            PathBuf::from(file),
                        )
                    } else {
                        (None, PathBuf::from(path))
                    };
                    result.raft.push(entry);
                }
                "--data-dir" => result.data = Some(value()?.into()),
                "--create" => result.create = true,
                "--migration" => result.migration = true,
                "--volatile" => result.volatile = true,
                "--is-replicated" => result.replicated = true,
                "--is-micro" => result.micro = true,
                "--load-only" => result.load_only = true,
                "--serve-only" => result.serve_only = true,
                "--exit-after-run" => result.exit_after_run = true,
                "--exit-on-stdin-close" => result.stdin_close = true,
                "--print-config" => result.print_config = true,
                "-x" | "--slow-exit" => (), // Every native exit joins workers.
                "--help" | "-h" => result.help = true,
                _ => return Err(invalid(format!("unknown argument: {flag}"))),
            }
            if inline.is_some() {
                return Err(invalid(format!("{flag} does not accept a value")));
            }
        }
        if result.load_only && result.serve_only {
            return Err(invalid("choose load-only or serve-only"));
        }
        if result.serve_only && result.exit_after_run {
            return Err(invalid("serve-only has no run to exit after"));
        }
        Ok(result)
    }
    pub fn load(&self) -> Result<(BenchmarkFile, Vec<String>)> {
        let (mut file, notices) = match (&self.native, &self.topology) {
            (Some(path), None) => {
                if self.owners.is_some()
                    || self.site.is_some()
                    || self.data.is_some()
                    || !self.raft.is_empty()
                    || self.replicated
                    || self.volatile
                {
                    return Err(invalid("topology selection/replication flags belong with --shard-config; native --config already declares them"));
                }
                (read::<BenchmarkFile>(path)?, Vec::new())
            }
            (None, Some(path)) => topology::convert(
                topology::Options {
                    topology: path.clone(),
                    owners: self.owners.clone(),
                    site: self.site.clone(),
                    workers: self.workers.unwrap_or(1),
                    replicated: self.replicated,
                    raft_files: self.raft.clone(),
                    data_root: self.data.clone(),
                    create: self.create,
                    migration: self.migration,
                    volatile: self.volatile,
                },
                benchmark::Config::default(),
            )?,
            _ => return Err(invalid("provide --config or --shard-config")),
        };
        if let Some(seconds) = self.seconds {
            file.benchmark.seconds = seconds;
        }
        if let Some(seconds) = self.startup {
            file.benchmark.startup_timeout_seconds = seconds;
        }
        if let Some(percent) = self.cpu {
            file.benchmark.cpu_limit_percent = percent;
        }
        if let Some(cycle) = self.cycle {
            file.benchmark.throttle_cycle_ms = cycle;
        }
        if self.micro {
            file.benchmark.micro.get_or_insert_with(Micro::default);
        }
        for node in file.node.iter_mut().chain(file.nodes.iter_mut()) {
            if let Some(workers) = self.workers {
                node.workers = workers;
            }
            if self.migration {
                node.migration = true;
            }
            if self.create {
                if let Some(persistence) = &mut node.persistence {
                    persistence.create = true;
                }
                if let Some(replication) = &mut node.replication {
                    replication.create = true;
                }
            }
        }
        file.clone().into_parts()?;
        Ok((file, notices))
    }
}
fn number<T: std::str::FromStr>(value: &str, flag: &str) -> Result<T> {
    value
        .parse()
        .map_err(|_| invalid(format!("invalid value for {flag}: {value}")))
}
pub const HELP:&str="Usage: mako_bench --config FILE [options]\n       mako_bench --shard-config FILE [-g SHARD | -L all|LIST] [-N SITE] [options]\n\nNative --config accepts JSON/YAML, one node or a nodes list.\n--shard-config accepts the existing role-based and sites/shard_map topologies.\n-t, --num-threads N         Worker threads per shard\n-u, --runtime SECONDS       Workload duration\n-C, --cpu-limit PERCENT     Work/sleep duty cycle; 0 or 100 disables it\n-Y, --throttle-cycle MS     Duty-cycle period, default 100 ms\n-T, --startup-timeout-sec N Readiness timeout\n--is-micro                 Run the item read/update workload\n-R, --replication raft      Enable native Raft for a topology file\n-F, --raft-config [N=]FILE  Raft site file or mode fragment; qualify colocated shard files\n--data-dir DIRECTORY       Required durable storage root for local/Raft topology mode\n--create                   Allow creation of new native durable databases\n--volatile                 Explicit in-memory development mode\n--migration                Enable the supported volatile migration mode\n--print-config             Print the validated native configuration and exit\n--load-only | --serve-only  Load without workload, or serve without loading\n--exit-after-run            Exit when all unreplicated colocated shards finish\n--exit-on-stdin-close       Stop when stdin closes or receives 'stop'\n\nBy default servers remain available after the workload. SIGINT/SIGTERM joins all workers.\nNative loading uses committed completion markers; the old --sync-dir barrier is retired.";
