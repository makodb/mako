//! Native driver for the repository's TPC-C workload and storage layout.
pub mod codec;
mod load;
mod micro;
mod operations;
mod random;
mod workload;
pub use load::*;
pub use micro::{Micro, MicroGenerator, MicroRequest};
pub use operations::*;
pub use random::{last_name, Random};
pub use workload::{Generator, Mix, Request as WorkloadRequest};

use crate::{
    client::Transaction,
    migration::{Kind, Schema, TableSpec},
    wire::*,
};
use codec::Record;
use serde::{Deserialize, Serialize};

pub const CUSTOMER: u64 = 1;
pub const CUSTOMER_NAME: u64 = 2;
pub const DISTRICT: u64 = 3;
pub const HISTORY: u64 = 4;
pub const NEW_ORDER: u64 = 5;
pub const ORDER: u64 = 6;
pub const ORDER_INDEX: u64 = 7;
pub const ORDER_LINE: u64 = 8;
pub const STOCK: u64 = 9;
pub const STOCK_DATA: u64 = 10;
pub const WAREHOUSE: u64 = 11;
pub const ITEM: u64 = 12;
pub const LOAD_STATE: u64 = 13;

pub fn tables() -> Vec<TableSpec> {
    [
        "customer",
        "customer_name_idx",
        "district",
        "history",
        "new_order",
        "oorder",
        "oorder_c_id_idx",
        "order_line",
        "stock",
        "stock_data",
        "warehouse",
        "item",
        "tpcc_load_state",
    ]
    .into_iter()
    .enumerate()
    .map(|(i, name)| TableSpec {
        id: i as u64 + 1,
        name: name.into(),
        kind: if i < 11 {
            Kind::Governed
        } else {
            Kind::Replicated
        },
        warehouse: i < 11,
        initial_owner: 0,
    })
    .collect()
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Scale {
    pub districts: u32,
    pub customers: u32,
    pub items: u32,
    pub seed: u64,
}
impl Default for Scale {
    fn default() -> Self {
        Self {
            districts: 10,
            customers: 3000,
            items: 100_000,
            seed: 0x1729,
        }
    }
}
impl Scale {
    pub fn validate(&self) -> Result<()> {
        if !(1..=10).contains(&self.districts)
            || !(1..=3000).contains(&self.customers)
            || !(1..=100_000).contains(&self.items)
        {
            return Err(invalid("invalid TPC-C scale"));
        }
        Ok(())
    }
}
pub(super) fn invalid(message: &str) -> Error {
    Error::new(ErrorCode::Invalid, message)
}

pub struct Context<'a> {
    pub schema: &'a Schema,
    pub owner: u32,
    pub migration: bool,
    pub scale: &'a Scale,
}
impl Context<'_> {
    pub fn local_warehouse(&self, warehouse: u32) -> Result<u32> {
        if self.schema.per_shard == 0 || warehouse == 0 || warehouse > self.schema.total {
            return Err(invalid("warehouse outside workload"));
        }
        Ok((warehouse - 1) % self.schema.per_shard + 1)
    }
    pub(super) fn district(&self, warehouse: u32, district: u32) -> Result<u32> {
        if district == 0 || district > self.scale.districts {
            return Err(invalid("district outside workload"));
        }
        self.local_warehouse(warehouse)
    }
    pub fn target(&self, table: u64, warehouse: u32, key: &[u8]) -> Result<Target> {
        self.schema.target(
            TableKey {
                table,
                warehouse: if table <= WAREHOUSE { warehouse } else { 0 },
            },
            key,
            self.owner,
            self.migration,
        )
    }
    pub fn read<R: Record>(
        &self,
        tx: &mut Transaction<'_, '_>,
        table: u64,
        warehouse: u32,
        key: &[u8],
    ) -> Result<R> {
        let bytes = tx
            .get(&self.target(table, warehouse, key)?, key)?
            .ok_or_else(|| invalid("required TPC-C row is missing"))?;
        R::decode(&bytes)
    }
    pub fn put<R: Record>(
        &self,
        tx: &mut Transaction<'_, '_>,
        table: u64,
        warehouse: u32,
        key: &[u8],
        value: &R,
    ) -> Result<()> {
        tx.mutate(
            &self.target(table, warehouse, key)?,
            Mutation::Put,
            key,
            &value.encode()?,
        )
    }
    pub fn insert<R: Record>(
        &self,
        tx: &mut Transaction<'_, '_>,
        table: u64,
        warehouse: u32,
        key: &[u8],
        value: &R,
    ) -> Result<()> {
        let target = self.target(table, warehouse, key)?;
        if tx.get(&target, key)?.is_some() {
            // NewOrder derives these keys from an optimistic district read.
            // Another order can commit before this attempt certifies that read,
            // including between two line insertions. Abort the whole attempt.
            let code = match table {
                NEW_ORDER | ORDER | ORDER_INDEX | ORDER_LINE => ErrorCode::Conflict,
                _ => ErrorCode::Invalid,
            };
            return Err(Error::new(
                code,
                format!(
                    "duplicate TPC-C insertion: table={table} warehouse={warehouse} key={key:02x?}"
                ),
            ));
        }
        tx.mutate(&target, Mutation::Insert, key, &value.encode()?)
    }
    pub(super) fn rows(
        &self,
        tx: &mut Transaction<'_, '_>,
        table: u64,
        warehouse: u32,
        range: std::ops::Range<Vec<u8>>,
        reverse: bool,
        limit: usize,
    ) -> Result<Vec<Row>> {
        tx.scan_routed(
            self.schema,
            TableKey { table, warehouse },
            Bound::Included(range.start),
            Bound::Excluded(range.end),
            reverse,
            limit,
        )
    }
}
