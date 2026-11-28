use super::{codec::*, *};
use crate::{
    client::Client,
    storage::{engine, Store},
};
use mako_storage_api::Worker;
use std::{ops::Range, sync::Arc};

const LOAD_KEY: &[u8] = b"tpcc/v1";
const MICRO_LOAD_KEY: &[u8] = b"micro/v1";
const BATCH_ROWS: usize = 32;

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct State {
    version: u32,
    scale: Scale,
    #[serde(default)]
    micro_keys: Option<u32>,
    owner: u32,
    nodes: u32,
    warehouses: u32,
    rows: u64,
    complete: bool,
    next_date: u32,
}
impl State {
    fn new(context: &Context<'_>, micro_keys: Option<u32>) -> Self {
        Self {
            version: 1,
            micro_keys,
            scale: context.scale.clone(),
            owner: context.owner,
            nodes: context.schema.nodes,
            warehouses: context.schema.per_shard,
            rows: 0,
            complete: false,
            next_date: 2,
        }
    }
    fn encode(&self) -> Result<Vec<u8>> {
        serde_json::to_vec(self).map_err(|_| invalid("TPC-C load marker encoding"))
    }
    fn decode(bytes: &[u8], expected: &Self) -> Result<Self> {
        let state: Self =
            serde_json::from_slice(bytes).map_err(|_| invalid("TPC-C load marker encoding"))?;
        if state.version != expected.version
            || state.micro_keys != expected.micro_keys
            || state.scale != expected.scale
            || state.owner != expected.owner
            || state.nodes != expected.nodes
            || state.warehouses != expected.warehouses
            || state.next_date < 2
        {
            return Err(invalid("TPC-C load configuration differs from stored data"));
        }
        Ok(state)
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct LoadProgress {
    pub rows: u64,
    pub complete: bool,
    pub already_loaded: bool,
}
struct SeedRow {
    table: u64,
    warehouse: u32,
    key: Vec<u8>,
    value: Vec<u8>,
}
impl Context<'_> {
    fn load_target(&self, owner: u32) -> Result<Target> {
        let mut target = self.target(LOAD_STATE, 0, LOAD_KEY)?;
        // The reference/control table has a physical copy at each shard.
        target.owner = owner;
        Ok(target)
    }
    /// Load one shard in bounded committed batches. An incomplete load is
    /// regenerated and checked byte-for-byte. A complete load is never rewritten.
    /// Applications must wait for `all_loaded` before allowing workload writes.
    pub fn load(
        &self,
        client: &mut Client,
        store: Arc<Store>,
        worker: &mut Worker,
        progress: impl FnMut(LoadProgress) -> Result<()>,
    ) -> Result<LoadProgress> {
        self.load_dataset(client, store, worker, None, progress)
    }
    pub fn load_micro(
        &self,
        client: &mut Client,
        store: Arc<Store>,
        worker: &mut Worker,
        keys: u32,
        progress: impl FnMut(LoadProgress) -> Result<()>,
    ) -> Result<LoadProgress> {
        if keys == 0 || keys > i32::MAX as u32 {
            return Err(invalid("micro key count"));
        }
        self.load_dataset(client, store, worker, Some(keys), progress)
    }
    fn load_dataset(
        &self,
        client: &mut Client,
        store: Arc<Store>,
        worker: &mut Worker,
        micro_keys: Option<u32>,
        mut progress: impl FnMut(LoadProgress) -> Result<()>,
    ) -> Result<LoadProgress> {
        self.scale.validate()?;
        let load_key = if micro_keys.is_some() {
            MICRO_LOAD_KEY
        } else {
            LOAD_KEY
        };
        if self.owner >= self.schema.nodes
            || self.schema.per_shard == 0
            || store.owner != self.owner
        {
            return Err(invalid("invalid TPC-C loader ownership"));
        }
        let target = self.load_target(self.owner)?;
        let expected = State::new(self, micro_keys);
        let mut state = client.run(store.clone(), worker, |tx| {
            let other_key = if micro_keys.is_some() {
                LOAD_KEY
            } else {
                MICRO_LOAD_KEY
            };
            if tx.get(&target, other_key)?.is_some() {
                return Err(invalid("cannot mix TPC-C and micro data in one database"));
            }

            if let Some(bytes) = tx.get(&target, load_key)? {
                State::decode(&bytes, &expected)
            } else {
                tx.mutate(&target, Mutation::Insert, load_key, &expected.encode()?)?;
                Ok(expected.clone())
            }
        })?;
        if state.complete {
            return Ok(LoadProgress {
                rows: state.rows,
                complete: true,
                already_loaded: true,
            });
        }
        let previous_rows = state.rows;
        let mut ordinal = 0u64;
        let mut batch = Vec::with_capacity(BATCH_ROWS);
        let mut flush = |batch: &mut Vec<SeedRow>, ordinal: u64| -> Result<()> {
            if batch.is_empty() {
                return Ok(());
            }
            let next = ordinal.max(state.rows);
            client.run(store.clone(), worker, |tx| {
                let bytes = tx
                    .get(&target, load_key)?
                    .ok_or_else(|| invalid("load marker disappeared"))?;
                if State::decode(&bytes, &expected)? != state {
                    return Err(invalid("another loader changed the load marker"));
                }
                for (offset, row) in batch.iter().enumerate() {
                    let row_target = self.target(row.table, row.warehouse, &row.key)?;
                    if row_target.owner != self.owner {
                        return Err(invalid("warehouse moved during loading"));
                    }
                    match tx.get(&row_target, &row.key)? {
                        Some(bytes) if bytes == row.value => {}
                        Some(_) => {
                            return Err(invalid(
                                "existing row differs from deterministic TPC-C load",
                            ))
                        }
                        None if ordinal - batch.len() as u64 + (offset as u64) < previous_rows => {
                            return Err(invalid("committed load prefix has missing rows"));
                        }
                        None => tx.mutate(&row_target, Mutation::Insert, &row.key, &row.value)?,
                    }
                }
                let mut updated = state.clone();
                updated.rows = next;
                tx.mutate(&target, Mutation::Put, load_key, &updated.encode()?)
            })?;
            state.rows = next;
            batch.clear();
            progress(LoadProgress {
                rows: ordinal,
                complete: false,
                already_loaded: false,
            })
        };
        let emit = |row| {
            ordinal += 1;
            batch.push(row);
            if batch.len() == BATCH_ROWS {
                flush(&mut batch, ordinal)?;
            }
            Ok(())
        };
        if let Some(keys) = micro_keys {
            self.generate_micro(keys, emit)?;
        } else {
            self.generate(emit)?;
        }
        flush(&mut batch, ordinal)?;
        if state.rows != ordinal {
            return Err(invalid("stored load prefix exceeds generated rows"));
        }
        client.run(store, worker, |tx| {
            let bytes = tx
                .get(&target, load_key)?
                .ok_or_else(|| invalid("load marker disappeared"))?;
            if State::decode(&bytes, &expected)? != state {
                return Err(invalid("load marker changed"));
            }
            let mut completed = state.clone();
            completed.complete = true;
            tx.mutate(&target, Mutation::Put, load_key, &completed.encode()?)
        })?;
        let result = LoadProgress {
            rows: ordinal,
            complete: true,
            already_loaded: false,
        };
        progress(result)?;
        Ok(result)
    }
    pub fn all_loaded(&self, tx: &mut Transaction<'_, '_>) -> Result<bool> {
        self.all_loaded_dataset(tx, None)
    }
    pub fn all_micro_loaded(&self, tx: &mut Transaction<'_, '_>, keys: u32) -> Result<bool> {
        self.all_loaded_dataset(tx, Some(keys))
    }
    fn all_loaded_dataset(
        &self,
        tx: &mut Transaction<'_, '_>,
        micro_keys: Option<u32>,
    ) -> Result<bool> {
        let load_key = if micro_keys.is_some() {
            MICRO_LOAD_KEY
        } else {
            LOAD_KEY
        };
        for owner in 0..self.schema.nodes {
            let Some(bytes) = tx.get(&self.load_target(owner)?, load_key)? else {
                return Ok(false);
            };
            let mut expected = State::new(self, micro_keys);
            expected.owner = owner;
            if !State::decode(&bytes, &expected)?.complete {
                return Ok(false);
            }
        }
        Ok(true)
    }
    /// Reserve monotonic history dates once per workload worker or refill. The
    /// committed high-water mark survives restart, avoiding history-key reuse.
    pub fn reserve_dates(&self, tx: &mut Transaction<'_, '_>, count: u32) -> Result<Range<u32>> {
        if count == 0 {
            return Err(invalid("empty date reservation"));
        }
        let target = self.load_target(self.owner)?;
        let bytes = tx
            .get(&target, LOAD_KEY)?
            .ok_or_else(|| invalid("TPC-C is not loaded"))?;
        let mut state = State::decode(&bytes, &State::new(self, None))?;
        if !state.complete {
            return Err(invalid("TPC-C is still loading"));
        }
        let first = state.next_date;
        state.next_date = first
            .checked_add(count)
            .ok_or_else(|| invalid("TPC-C dates exhausted"))?;
        tx.mutate(&target, Mutation::Put, LOAD_KEY, &state.encode()?)?;
        Ok(first..state.next_date)
    }

    fn generate_micro(&self, keys: u32, mut emit: impl FnMut(SeedRow) -> Result<()>) -> Result<()> {
        let value = MicroItem {
            name: Inline(b"cccccccc".to_vec()),
        };
        for id in 0..keys {
            seed(&mut emit, ITEM, 0, key(&[id]), &value)?;
        }
        Ok(())
    }

    fn generate(&self, mut emit: impl FnMut(SeedRow) -> Result<()>) -> Result<()> {
        let mut r = Random::new(self.scale.seed);
        // Item reference data must be identical on every shard.
        for id in 1..=self.scale.items {
            let item = Item {
                name: Inline(r.text(14, 24)),
                price: r.between(100, 10_000) as f32 / 100.0,
                data: Inline(r.data()),
                image: r.between(1, 10_000) as i32,
            };
            seed(&mut emit, ITEM, 0, key(&[id]), &item)?;
        }
        for warehouse in
            self.owner * self.schema.per_shard + 1..=(self.owner + 1) * self.schema.per_shard
        {
            r = Random::new(self.scale.seed.wrapping_add(u64::from(warehouse) << 32));
            let w = self.local_warehouse(warehouse)?;
            let value = Warehouse {
                ytd: 300_000.0,
                tax: r.between(0, 2000) as f32 / 10_000.0,
                name: Inline(r.text(6, 10)),
                street1: Inline(r.text(10, 20)),
                street2: Inline(r.text(10, 20)),
                city: Inline(r.text(10, 20)),
                state: Fixed::new(&r.text(2, 2))?,
                zip: Fixed(*b"123456789"),
            };
            seed(&mut emit, WAREHOUSE, warehouse, key(&[w]), &value)?;
            for item in 1..=self.scale.items {
                let value = Stock {
                    quantity: r.between(10, 100) as i16,
                    ..Stock::default()
                };
                seed(&mut emit, STOCK, warehouse, key(&[w, item]), &value)?;
                let value = StockData {
                    data: Inline(r.data()),
                    district1: Fixed::new(&r.text(24, 24))?,
                    district2: Fixed::new(&r.text(24, 24))?,
                    district3: Fixed::new(&r.text(24, 24))?,
                    district4: Fixed::new(&r.text(24, 24))?,
                    district5: Fixed::new(&r.text(24, 24))?,
                    district6: Fixed::new(&r.text(24, 24))?,
                    district7: Fixed::new(&r.text(24, 24))?,
                    district8: Fixed::new(&r.text(24, 24))?,
                    district9: Fixed::new(&r.text(24, 24))?,
                    district10: Fixed::new(&r.text(24, 24))?,
                };
                seed(&mut emit, STOCK_DATA, warehouse, key(&[w, item]), &value)?;
            }
            for d in 1..=self.scale.districts {
                let district = District {
                    ytd: 30_000.0,
                    tax: r.between(0, 2000) as f32 / 10_000.0,
                    next_order: self.scale.customers as i32 + 1,
                    name: Inline(r.text(6, 10)),
                    street1: Inline(r.text(10, 20)),
                    street2: Inline(r.text(10, 20)),
                    city: Inline(r.text(10, 20)),
                    state: Fixed::new(&r.text(2, 2))?,
                    zip: Fixed(*b"123456789"),
                };
                seed(&mut emit, DISTRICT, warehouse, key(&[w, d]), &district)?;
                for customer in 1..=self.scale.customers {
                    let last = if customer <= 1000 {
                        customer - 1
                    } else {
                        r.nonuniform(255, 157, 0, 999)
                    };
                    // Include the customer ID in the first name so this index
                    // never loses a customer to a duplicate generated key.
                    let mut first = r.text(8, 12);
                    first.extend_from_slice(format!("{customer:04}").as_bytes());
                    let value = Customer {
                        discount: r.between(1, 5000) as f32 / 10_000.0,
                        credit: Fixed(if r.between(1, 100) <= 10 {
                            *b"BC"
                        } else {
                            *b"GC"
                        }),
                        last: Inline(last_name(last)),
                        first: Inline(first),
                        credit_limit: 50_000.0,
                        balance: -10.0,
                        ytd_payment: 10.0,
                        payment_count: 1,
                        delivery_count: 0,
                        street1: Inline(r.text(10, 20)),
                        street2: Inline(r.text(10, 20)),
                        city: Inline(r.text(10, 20)),
                        state: Fixed::new(&r.text(2, 2))?,
                        zip: Fixed(*b"123411111"),
                        phone: Fixed(*b"1234567890123456"),
                        since: 1,
                        middle: Fixed(*b"OE"),
                    };
                    seed(
                        &mut emit,
                        CUSTOMER,
                        warehouse,
                        key(&[w, d, customer]),
                        &value,
                    )?;
                    seed(
                        &mut emit,
                        CUSTOMER,
                        warehouse,
                        key(&[w, d + 200, customer]),
                        &Balance { balance: -10.0 },
                    )?;
                    seed(
                        &mut emit,
                        CUSTOMER,
                        warehouse,
                        key(&[w, d + 100, customer]),
                        &CustomerData {
                            data: Inline(r.text(100, 300)),
                        },
                    )?;
                    seed(
                        &mut emit,
                        CUSTOMER_NAME,
                        warehouse,
                        name_key(w, d, &value.last.0, &value.first.0)?,
                        &CustomerName {
                            customer: customer as i32,
                        },
                    )?;
                    seed(
                        &mut emit,
                        HISTORY,
                        warehouse,
                        key(&[customer, d, w, d, w, 1]),
                        &History {
                            amount: 10.0,
                            data: Inline(r.text(10, 24)),
                        },
                    )?;
                }
                let mut customers: Vec<_> = (1..=self.scale.customers).collect();
                for i in (1..customers.len()).rev() {
                    let j = r.between(0, i as u32) as usize;
                    customers.swap(i, j);
                }
                for order in 1..=self.scale.customers {
                    let delivered = order <= self.scale.customers * 7 / 10;
                    let value = Order {
                        customer: customers[order as usize - 1] as i32,
                        carrier: if delivered {
                            r.between(1, 10) as i32
                        } else {
                            0
                        },
                        lines: r.between(5, 15) as u8,
                        all_local: true,
                        date: 1,
                    };
                    seed(&mut emit, ORDER, warehouse, key(&[w, d, order]), &value)?;
                    seed(
                        &mut emit,
                        ORDER_INDEX,
                        warehouse,
                        key(&[w, d, value.customer as u32, order]),
                        &OrderIndex::default(),
                    )?;
                    if !delivered {
                        seed(
                            &mut emit,
                            NEW_ORDER,
                            warehouse,
                            key(&[w, d, order]),
                            &NewOrder::default(),
                        )?;
                    }
                    for line in 1..=value.lines {
                        let value = OrderLine {
                            item: r.between(1, self.scale.items) as i32,
                            delivery: u32::from(delivered),
                            amount: if delivered {
                                0.0
                            } else {
                                r.between(1, 999_999) as f32 / 100.0
                            },
                            supply: w as i32,
                            quantity: 5,
                        };
                        seed(
                            &mut emit,
                            ORDER_LINE,
                            warehouse,
                            key(&[w, d, order, u32::from(line)]),
                            &value,
                        )?;
                    }
                }
            }
        }
        Ok(())
    }
}
fn seed(
    emit: &mut impl FnMut(SeedRow) -> Result<()>,
    table: u64,
    warehouse: u32,
    key: Vec<u8>,
    value: &impl Record,
) -> Result<()> {
    emit(SeedRow {
        table,
        warehouse,
        key,
        value: value.encode()?,
    })
}

/// Attach a loader worker on its owning OS thread.
pub fn load_local(
    context: &Context<'_>,
    client: &mut Client,
    store: Arc<Store>,
    progress: impl FnMut(LoadProgress) -> Result<()>,
) -> Result<LoadProgress> {
    let mut worker = store.database.worker().map_err(engine)?;
    context.load(client, store, &mut worker, progress)
}
