use super::*;

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(default, deny_unknown_fields)]
pub struct Mix {
    /// NewOrder, Payment, Delivery, OrderStatus, StockLevel percentages.
    pub weights: [u32; 5],
    pub new_order_remote_percent: u32,
    pub payment_remote_percent: u32,
    pub cross_warehouse: bool,
    pub uniform_items: bool,
}
impl Default for Mix {
    fn default() -> Self {
        Self {
            weights: [45, 43, 4, 4, 4],
            new_order_remote_percent: 1,
            payment_remote_percent: 15,
            cross_warehouse: true,
            uniform_items: false,
        }
    }
}
impl Mix {
    pub fn validate(&self) -> Result<()> {
        if self.weights.iter().map(|n| u64::from(*n)).sum::<u64>() != 100
            || self.new_order_remote_percent > 100
            || self.payment_remote_percent > 100
        {
            return Err(invalid(
                "TPC-C mix must total 100 and percentages must be within 0..100",
            ));
        }
        Ok(())
    }
}

#[derive(Clone, Debug)]
pub enum Request {
    NewOrder(NewOrderRequest),
    Payment(PaymentRequest),
    Delivery {
        warehouse: u32,
        carrier: u32,
        date: u32,
    },
    OrderStatus {
        warehouse: u32,
        district: u32,
        customer: CustomerSelector,
    },
    StockLevel {
        warehouse: u32,
        district: u32,
        threshold: u32,
    },
}
impl Request {
    pub fn index(&self) -> usize {
        match self {
            Self::NewOrder(_) => 0,
            Self::Payment(_) => 1,
            Self::Delivery { .. } => 2,
            Self::OrderStatus { .. } => 3,
            Self::StockLevel { .. } => 4,
        }
    }
    pub fn execute(&self, context: &Context<'_>, tx: &mut Transaction<'_, '_>) -> Result<()> {
        match self {
            Self::NewOrder(request) => context.new_order(tx, request).map(|_| ()),
            Self::Payment(request) => context.payment(tx, request).map(|_| ()),
            Self::Delivery {
                warehouse,
                carrier,
                date,
            } => context
                .delivery(tx, *warehouse, *carrier, *date)
                .map(|_| ()),
            Self::OrderStatus {
                warehouse,
                district,
                customer,
            } => context
                .order_status(tx, *warehouse, *district, customer)
                .map(|_| ()),
            Self::StockLevel {
                warehouse,
                district,
                threshold,
            } => context
                .stock_level(tx, *warehouse, *district, *threshold)
                .map(|_| ()),
        }
    }
}

pub struct Generator {
    random: Random,
    scale: Scale,
    mix: Mix,
    owner: u32,
    per_shard: u32,
    total: u32,
}
impl Generator {
    pub fn new(context: &Context<'_>, mix: Mix, seed: u64) -> Result<Self> {
        context.scale.validate()?;
        mix.validate()?;
        if context.schema.per_shard == 0 || context.owner >= context.schema.nodes {
            return Err(invalid("TPC-C workload ownership"));
        }
        Ok(Self {
            random: Random::new(seed),
            scale: context.scale.clone(),
            mix,
            owner: context.owner,
            per_shard: context.schema.per_shard,
            total: context.schema.total,
        })
    }
    fn customer(&mut self) -> CustomerSelector {
        if self.random.between(1, 100) <= 60 {
            // The ordinary scale has all 1000 last names. Small integration
            // scales choose among names actually loaded, with the same access.
            let id = self
                .random
                .nonuniform(255, 223, 0, self.scale.customers.min(1000) - 1);
            CustomerSelector::LastName(last_name(id))
        } else {
            CustomerSelector::Id(self.random.nonuniform(1023, 259, 1, self.scale.customers))
        }
    }
    fn supplier(&mut self, home: u32, percent: u32) -> u32 {
        if self.mix.cross_warehouse && self.total > 1 && self.random.between(1, 100) <= percent {
            let id = self.random.between(1, self.total - 1);
            if id >= home {
                id + 1
            } else {
                id
            }
        } else {
            home
        }
    }
    pub fn next(&mut self, date: u32) -> Request {
        let choice = self.random.between(1, 100);
        let mut cumulative = 0;
        let kind = self
            .mix
            .weights
            .iter()
            .position(|weight| {
                cumulative += weight;
                choice <= cumulative
            })
            .unwrap();
        let warehouse = self.owner * self.per_shard + self.random.between(1, self.per_shard);
        let district = self.random.between(1, self.scale.districts);
        match kind {
            0 => {
                let count = self.random.between(5, 15);
                let customer = self.random.nonuniform(1023, 259, 1, self.scale.customers);
                let items = (0..count)
                    .map(|_| {
                        let item = if self.mix.uniform_items {
                            self.random.between(1, self.scale.items)
                        } else {
                            self.random.nonuniform(8191, 7911, 1, self.scale.items)
                        };
                        let supplier = self.supplier(warehouse, self.mix.new_order_remote_percent);
                        OrderItem {
                            item,
                            supplier,
                            quantity: self.random.between(1, 10) as u8,
                        }
                    })
                    .collect();
                Request::NewOrder(NewOrderRequest {
                    warehouse,
                    district,
                    customer,
                    date,
                    items,
                })
            }
            1 => {
                let customer_warehouse = self.supplier(warehouse, self.mix.payment_remote_percent);
                let customer_district = if customer_warehouse == warehouse {
                    district
                } else {
                    self.random.between(1, self.scale.districts)
                };
                Request::Payment(PaymentRequest {
                    warehouse,
                    district,
                    customer_warehouse,
                    customer_district,
                    customer: self.customer(),
                    amount: self.random.between(100, 500_000) as f32 / 100.0,
                    date,
                })
            }
            2 => Request::Delivery {
                warehouse,
                carrier: self.random.between(1, 10),
                date,
            },
            3 => Request::OrderStatus {
                warehouse,
                district,
                customer: self.customer(),
            },
            _ => Request::StockLevel {
                warehouse,
                district,
                threshold: self.random.between(10, 20),
            },
        }
    }
}
