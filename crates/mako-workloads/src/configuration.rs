//! TPC-C benchmark configuration and topology conversion.
use crate::{benchmark, node::Config, wire::*};
pub use mako_runtime::configuration::{read, select_nodes};
use serde::{Deserialize, Serialize};
pub mod cli;
pub mod topology;
fn invalid(message: impl Into<String>) -> Error {
    Error::new(ErrorCode::Invalid, message)
}

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct BenchmarkFile {
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub node: Option<Config>,
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub nodes: Vec<Config>,
    #[serde(default)]
    pub benchmark: benchmark::Config,
}
impl BenchmarkFile {
    pub fn into_parts(self) -> Result<(Vec<Config>, benchmark::Config)> {
        let nodes = select_nodes(self.node, self.nodes)?;
        for node in &nodes {
            self.benchmark.validate(node)?;
        }
        Ok((nodes, self.benchmark))
    }
}
