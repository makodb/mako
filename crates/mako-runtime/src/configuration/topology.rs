//! Read the two existing topology formats into native application settings.
//! Old per-worker Raft groups become one ordered application log per shard;
//! conversion reports that change and retains the first group's peer ports.
use super::{invalid, read, BenchmarkFile};
use crate::{
    benchmark,
    node::{Config, PersistenceConfig, ReplicaConfig, ReplicaPeer},
    tpcc,
    wire::*,
};
use serde::Deserialize;
use std::{
    collections::{BTreeMap, BTreeSet},
    path::{Path, PathBuf},
};

#[derive(Clone, Debug)]
pub struct Endpoint {
    pub name: String,
    pub address: String,
}
#[derive(Clone, Debug)]
pub struct Topology {
    pub warehouses: u32,
    pub shards: Vec<Vec<Endpoint>>,
    roles: Vec<String>,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct OldEndpoint {
    name: String,
    index: u32,
    ip: String,
    port: u16,
}
#[derive(Deserialize)]
struct OldTopology {
    shards: u32,
    replicas: u32,
    warehouses: u32,
    #[serde(flatten)]
    sections: BTreeMap<String, serde_json::Value>,
}
#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct Site {
    name: String,
    #[serde(default)]
    id: Option<u32>,
    ip: String,
    port: u16,
}
#[derive(Deserialize)]
struct SiteTopology {
    sites: Vec<Site>,
    shard_map: Vec<Vec<String>>,
    #[serde(default = "one")]
    warehouses: u32,
    #[serde(flatten)]
    extra: BTreeMap<String, serde_json::Value>,
}
fn one() -> u32 {
    1
}
fn address(host: &str, port: u16) -> Result<String> {
    if host.is_empty() || host.contains('\0') || port == 0 {
        return Err(invalid("empty host or zero topology port"));
    }
    Ok(if host.contains(':') && !host.starts_with('[') {
        format!("[{host}]:{port}")
    } else {
        format!("{host}:{port}")
    })
}
fn memory_port(name: &str, value: &serde_json::Value) -> bool {
    name.starts_with("mem")
        && value
            .as_u64()
            .is_some_and(|port| port > 0 && port <= u64::from(u16::MAX))
}
impl Topology {
    pub fn load(path: impl AsRef<Path>) -> Result<Self> {
        let value: serde_json::Value = read(path)?;
        let topology = if value.get("sites").is_some() || value.get("shard_map").is_some() {
            let parsed: SiteTopology =
                serde_json::from_value(value).map_err(|error| invalid(error.to_string()))?;
            if parsed
                .extra
                .iter()
                .any(|(name, value)| !memory_port(name, value))
            {
                return Err(invalid("unknown site topology field"));
            }
            let mut sites = BTreeMap::new();
            let mut ids = BTreeSet::new();
            for site in parsed.sites {
                if site.name.is_empty() || site.id.is_some_and(|id| !ids.insert(id)) {
                    return Err(invalid("empty site name or duplicate site ID"));
                }
                let endpoint = Endpoint {
                    name: site.name.clone(),
                    address: address(&site.ip, site.port)?,
                };
                if sites.insert(site.name, endpoint).is_some() {
                    return Err(invalid("duplicate site name"));
                }
            }
            let mut assigned = BTreeSet::new();
            let mut shards = Vec::new();
            for group in parsed.shard_map {
                let mut endpoints = Vec::new();
                for name in group {
                    if !assigned.insert(name.clone()) {
                        return Err(invalid("site assigned more than once"));
                    }
                    endpoints.push(
                        sites
                            .get(&name)
                            .ok_or_else(|| invalid("shard_map refers to absent site"))?
                            .clone(),
                    );
                }
                shards.push(endpoints);
            }
            Self {
                warehouses: parsed.warehouses,
                shards,
                roles: Vec::new(),
            }
        } else {
            let parsed: OldTopology =
                serde_json::from_value(value).map_err(|error| invalid(error.to_string()))?;
            if parsed.shards == 0 || parsed.shards > 256 || parsed.replicas == 0 {
                return Err(invalid("invalid topology shard/replica count"));
            }
            let mut sections = BTreeMap::new();
            for (name, value) in parsed.sections {
                if memory_port(&name, &value) {
                    continue;
                }
                let entries: Vec<OldEndpoint> = serde_json::from_value(value)
                    .map_err(|error| invalid(format!("site {name}: {error}")))?;
                let mut endpoints = BTreeMap::new();
                for entry in entries {
                    if entry.name.is_empty() || entry.index >= parsed.shards {
                        return Err(invalid("invalid topology endpoint index/name"));
                    }
                    let endpoint = Endpoint {
                        name: format!("{name}/{}", entry.name),
                        address: address(&entry.ip, entry.port)?,
                    };
                    if endpoints.insert(entry.index, endpoint).is_some() {
                        return Err(invalid("duplicate shard index in site"));
                    }
                }
                if endpoints.len() != parsed.shards as usize {
                    return Err(invalid("site must describe every shard"));
                }
                sections.insert(name, endpoints);
            }
            let mut roles = Vec::new();
            for role in ["localhost", "p1", "p2", "learner"] {
                if sections.contains_key(role) {
                    roles.push(role.to_owned());
                }
            }
            for role in sections.keys() {
                if !roles.contains(role) {
                    roles.push(role.clone());
                }
            }
            if roles.len() < parsed.replicas as usize {
                return Err(invalid("replica count exceeds available sites"));
            }
            let shards = (0..parsed.shards)
                .map(|owner| {
                    roles
                        .iter()
                        .map(|role| sections[role][&owner].clone())
                        .collect()
                })
                .collect();
            Self {
                warehouses: parsed.warehouses,
                shards,
                roles,
            }
        };
        if topology.warehouses == 0
            || topology.shards.is_empty()
            || topology.shards.len() > 256
            || topology.shards.iter().any(Vec::is_empty)
        {
            return Err(invalid("empty topology or warehouse count"));
        }
        let mut addresses = BTreeSet::new();
        if topology
            .shards
            .iter()
            .flatten()
            .any(|site| !addresses.insert(&site.address))
        {
            return Err(invalid("duplicate topology listener"));
        }
        Ok(topology)
    }
    pub fn nodes(&self) -> u32 {
        self.shards.len() as u32
    }
    pub fn owners(&self, selection: Option<&str>, site: Option<&str>) -> Result<Vec<u32>> {
        let owners = match selection {
            Some("all") => (0..self.nodes()).collect(),
            Some(list) => list
                .split(',')
                .map(|id| {
                    id.parse::<u32>()
                        .map_err(|_| invalid("invalid local shard list"))
                })
                .collect::<Result<Vec<_>>>()?,
            None => {
                let owner = site
                    .and_then(|site| {
                        self.shards
                            .iter()
                            .position(|group| group.iter().any(|endpoint| endpoint.name == site))
                    })
                    .unwrap_or(0);
                vec![owner as u32]
            }
        };
        let mut unique = BTreeSet::new();
        if owners.is_empty()
            || owners
                .iter()
                .any(|owner| *owner >= self.nodes() || !unique.insert(*owner))
        {
            return Err(invalid(
                "local shard selection is empty, duplicated or out of range",
            ));
        }
        Ok(owners)
    }
    fn replica(&self, site: Option<&str>) -> Result<usize> {
        let Some(site) = site else {
            return Ok(0);
        };
        if let Some(index) = self.roles.iter().position(|role| role == site) {
            return Ok(index);
        }
        self.shards
            .iter()
            .find_map(|group| group.iter().position(|endpoint| endpoint.name == site))
            .ok_or_else(|| invalid("unknown topology site"))
    }
}

pub struct Options {
    pub topology: PathBuf,
    pub owners: Option<String>,
    pub site: Option<String>,
    pub workers: u32,
    pub replicated: bool,
    /// Optional owner qualifier; unqualified site files select the sole local
    /// shard. Replication mode-only fragments may be shared by all local shards.
    pub raft_files: Vec<(Option<u32>, PathBuf)>,
    pub data_root: Option<PathBuf>,
    pub create: bool,
    pub migration: bool,
    pub volatile: bool,
}
#[derive(Default, Deserialize)]
#[serde(default, deny_unknown_fields)]
struct RaftSites {
    server: Vec<Vec<String>>,
}
#[derive(Default, Deserialize)]
#[serde(default, deny_unknown_fields)]
struct Mode {
    ab: String,
    batch: bool,
    retry: u32,
}
#[derive(Default, Deserialize)]
#[serde(default, deny_unknown_fields)]
struct RaftFile {
    site: RaftSites,
    process: BTreeMap<String, String>,
    host: BTreeMap<String, String>,
    mode: Option<Mode>,
}
fn merge(into: &mut BTreeMap<String, String>, other: BTreeMap<String, String>) -> Result<()> {
    for (key, value) in other {
        if into.get(&key).is_some_and(|old| old != &value) {
            return Err(invalid("conflicting Raft configuration fragments"));
        }
        into.insert(key, value);
    }
    Ok(())
}

pub fn convert(
    options: Options,
    benchmark: benchmark::Config,
) -> Result<(BenchmarkFile, Vec<String>)> {
    let topology = Topology::load(&options.topology)?;
    let owners = topology.owners(options.owners.as_deref(), options.site.as_deref())?;
    let selected = topology.replica(options.site.as_deref())?;
    let mut fragments: BTreeMap<u32, RaftFile> = BTreeMap::new();
    let mut notices = Vec::new();
    for (owner, path) in options.raft_files {
        let document: RaftFile = read(&path)?;
        if let Some(mode) = document.mode {
            if mode.ab != "raft" {
                return Err(invalid("native replication supports only Raft"));
            }
            if mode.batch || mode.retry != 0 {
                notices.push("Native Raft uses its own bounded batching and retry policy; the old mode.batch/mode.retry settings are retired.".into());
            }
        }
        if document.site.server.is_empty() {
            if !document.host.is_empty() || !document.process.is_empty() {
                return Err(invalid(
                    "Raft mapping fragment must include its site groups",
                ));
            }
            continue;
        }
        let owner = owner
            .or_else(|| (owners.len() == 1).then_some(owners[0]))
            .ok_or_else(|| invalid("qualify colocated Raft site files as SHARD=FILE"))?;
        if !owners.contains(&owner) {
            return Err(invalid("Raft file belongs to a nonlocal shard"));
        }
        let fragment = fragments.entry(owner).or_default();
        fragment.site.server.extend(document.site.server);
        merge(&mut fragment.process, document.process)?;
        merge(&mut fragment.host, document.host)?;
    }
    if !options.replicated && !fragments.is_empty() {
        return Err(invalid("Raft files require replication mode"));
    }
    let mut raft = BTreeMap::new();
    for owner in &owners {
        if !options.replicated {
            continue;
        }
        let fragment = fragments
            .remove(owner)
            .ok_or_else(|| invalid("replicated shard requires a Raft site configuration"))?;
        let mut membership = None;
        let mut peers = None;
        for group in &fragment.site.server {
            if group.is_empty() || group.len() >= u16::MAX as usize {
                return Err(invalid("invalid Raft group size"));
            }
            let mut roles = Vec::new();
            let mut addresses = BTreeSet::new();
            let mut current = Vec::new();
            for (index, entry) in group.iter().enumerate() {
                let (name, port) = entry
                    .rsplit_once(':')
                    .ok_or_else(|| invalid("Raft site needs NAME:PORT"))?;
                let port = port
                    .parse::<u16>()
                    .map_err(|_| invalid("invalid Raft port"))?;
                let process = fragment
                    .process
                    .get(name)
                    .ok_or_else(|| invalid("Raft site has no process mapping"))?;
                let host = fragment
                    .host
                    .get(process)
                    .ok_or_else(|| invalid("Raft process has no host mapping"))?;
                let address = address(host, port)?;
                if !addresses.insert(address.clone()) || roles.contains(process) {
                    return Err(invalid("duplicate Raft endpoint/process"));
                }
                roles.push(process.clone());
                current.push(ReplicaPeer {
                    id: index as u16 + 1,
                    address,
                });
            }
            if membership.as_ref().is_some_and(|first| first != &roles) {
                return Err(invalid(
                    "per-worker Raft groups have different replica membership",
                ));
            }
            if membership.is_none() {
                membership = Some(roles);
                peers = Some(current);
            }
        }
        let peers = peers.ok_or_else(|| invalid("Raft file has no groups"))?;
        if selected >= peers.len() || peers.len() > topology.shards[*owner as usize].len() {
            return Err(invalid("selected topology replica is not a Raft voter"));
        }
        if !topology.roles.is_empty()
            && membership.as_ref().unwrap() != &topology.roles[..peers.len()]
        {
            return Err(invalid(
                "Raft replica order differs from application site order",
            ));
        }
        if fragment.site.server.len() > 1 {
            notices.push(format!("Shard {owner}: {} per-worker Raft logs become one native shard log using the first group's ports.",fragment.site.server.len()));
        }
        raft.insert(*owner, peers);
    }
    let peer_map: BTreeMap<_, _> = topology
        .shards
        .iter()
        .enumerate()
        .map(|(owner, sites)| (owner as u32, sites[0].address.clone()))
        .collect();
    let replica_count = raft.values().next().map(Vec::len);
    if raft
        .values()
        .any(|peers| Some(peers.len()) != replica_count)
    {
        return Err(invalid(
            "colocated Raft groups have different replica counts",
        ));
    }
    let replica_groups = if let Some(count) = replica_count {
        let mut groups = BTreeMap::new();
        for (owner, sites) in topology.shards.iter().enumerate() {
            if sites.len() < count {
                return Err(invalid(
                    "application topology is missing Raft replica endpoints",
                ));
            }
            groups.insert(
                owner as u32,
                ReplicaSet {
                    group: owner as u64 + 1,
                    endpoints: sites[..count]
                        .iter()
                        .map(|site| site.address.clone())
                        .collect(),
                },
            );
        }
        groups
    } else {
        BTreeMap::new()
    };
    let mut nodes = Vec::new();
    for owner in owners {
        let endpoint = topology.shards[owner as usize]
            .get(selected)
            .ok_or_else(|| invalid("selected replica missing from shard"))?;
        let directory = options.data_root.as_ref().map(|root| {
            root.join(format!("replica-{selected}"))
                .join(format!("shard-{owner}"))
        });
        let replication = if let Some(peers) = raft.remove(&owner) {
            Some(ReplicaConfig {
                group: u64::from(owner) + 1,
                id: selected as u16 + 1,
                listen: peers[selected].address.clone(),
                peers,
                directory: directory
                    .clone()
                    .ok_or_else(|| invalid("Raft requires an explicit --data-dir"))?,
                create: options.create,
                sync_writes: true,
            })
        } else {
            None
        };
        let persistence = if replication.is_none() {
            directory.map(|directory| PersistenceConfig {
                directory,
                create: options.create,
                sync_writes: true,
            })
        } else {
            None
        };
        nodes.push(Config {
            owner,
            nodes: topology.nodes(),
            listen: endpoint.address.clone(),
            tables: tpcc::tables(),
            workers: options.workers,
            gateway_workers: 1,
            queue_capacity: 64,
            warehouses_per_shard: topology.warehouses,
            peers: peer_map.clone(),
            migration: options.migration,
            initial: vec![],
            volatile: options.volatile || options.migration,
            replication,
            persistence,
            replica_groups: replica_groups.clone(),
        });
    }
    let file = BenchmarkFile {
        node: None,
        nodes,
        benchmark,
    };
    file.clone().into_parts()?;
    Ok((file, notices))
}
