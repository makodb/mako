//! Native JSON/YAML configuration. Application validation happens before any
//! listener, database directory or worker is created.
use crate::{node::Config, wire::*};
use serde::de::DeserializeOwned;
use std::{collections::BTreeSet, io::Read, path::Path};

const MAX_CONFIG_BYTES: u64 = 4 * 1024 * 1024;
pub fn read<T: DeserializeOwned>(path: impl AsRef<Path>) -> Result<T> {
    let path = path.as_ref();
    let file = std::fs::File::open(path)
        .map_err(|error| invalid(format!("{}: {error}", path.display())))?;
    let mut bytes = Vec::new();
    file.take(MAX_CONFIG_BYTES + 1)
        .read_to_end(&mut bytes)
        .map_err(|error| invalid(error.to_string()))?;
    if bytes.len() as u64 > MAX_CONFIG_BYTES {
        return Err(invalid("configuration exceeds 4 MiB"));
    }
    if path
        .extension()
        .is_some_and(|extension| extension.eq_ignore_ascii_case("json"))
    {
        serde_json::from_slice(&bytes)
            .map_err(|error| invalid(format!("{}: {error}", path.display())))
    } else {
        serde_saphyr::from_slice(&bytes)
            .map_err(|error| invalid(format!("{}: {error}", path.display())))
    }
}
fn invalid(message: impl Into<String>) -> Error {
    Error::new(ErrorCode::Invalid, message)
}

pub fn select_nodes(node: Option<Config>, selected: Vec<Config>) -> Result<Vec<Config>> {
    let nodes = match (node, selected.is_empty()) {
        (Some(node), true) => vec![node],
        (None, false) => selected,
        _ => return Err(invalid("configure one node or a nonempty nodes list")),
    };
    let first = &nodes[0];
    let mut owners = BTreeSet::new();
    let mut listeners = BTreeSet::new();
    let mut directories = BTreeSet::new();
    for node in &nodes {
        node.schema()?;
        if node.nodes != first.nodes
            || node.warehouses_per_shard != first.warehouses_per_shard
            || node.tables != first.tables
            || node.workers != first.workers
        {
            return Err(invalid(
                "colocated shards must share schema, topology and worker count",
            ));
        }
        if !owners.insert(node.owner) {
            return Err(invalid("duplicate colocated shard owner"));
        }
        if nodes.len() > 1 && node.listen.ends_with(":0") {
            return Err(invalid(
                "colocated shards require explicit listener ports for peer routing",
            ));
        }
        if !listeners.insert(&node.listen) {
            return Err(invalid("duplicate listener"));
        }
        if let Some(replica) = &node.replication {
            if !listeners.insert(&replica.listen) {
                return Err(invalid("duplicate Raft listener"));
            }
            if !directories.insert(&replica.directory) {
                return Err(invalid("duplicate persistence directory"));
            }
        }
        if let Some(persistence) = &node.persistence {
            if !directories.insert(&persistence.directory) {
                return Err(invalid("duplicate persistence directory"));
            }
        }
    }
    Ok(nodes)
}
