use super::{codec::*, *};

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(default, deny_unknown_fields)]
pub struct Micro {
    pub keys: u32,
    pub operations: u32,
    pub write_percent: u32,
    pub remote_percent: u32,
}
impl Default for Micro {
    fn default() -> Self {
        Self {
            keys: 1_000_000,
            operations: 4,
            write_percent: 50,
            remote_percent: 50,
        }
    }
}
impl Micro {
    pub fn validate(&self) -> Result<()> {
        if self.keys == 0
            || self.keys > i32::MAX as u32
            || !(1..=64).contains(&self.operations)
            || self.write_percent > 100
            || self.remote_percent > 100
        {
            return Err(invalid("invalid micro workload bounds or percentages"));
        }
        Ok(())
    }
}
#[derive(Clone, Debug)]
pub struct MicroRequest {
    pub write: bool,
    /// Explicit shard and signed-positive item key, as in the original workload.
    pub keys: Vec<(u32, u32)>,
}
impl MicroRequest {
    pub fn index(&self) -> usize {
        usize::from(!self.write)
    }
    pub fn execute(&self, context: &Context<'_>, tx: &mut Transaction<'_, '_>) -> Result<()> {
        if self.keys.is_empty() || self.keys.len() > 64 {
            return Err(invalid("micro operation count"));
        }
        let update = MicroItem {
            name: Inline(b"aaaaaaaa".to_vec()),
        }
        .encode()?;
        for (owner, id) in &self.keys {
            if *owner >= context.schema.nodes || *id > i32::MAX as u32 {
                return Err(invalid("micro target"));
            }
            let key = key(&[*id]);
            let mut target = context.target(ITEM, 0, &key)?;
            target.owner = *owner;
            let value = tx
                .get(&target, &key)?
                .ok_or_else(|| invalid("micro row is missing"))?;
            MicroItem::decode(&value)?;
            if self.write {
                tx.mutate(&target, Mutation::Put, &key, &update)?;
            }
        }
        Ok(())
    }
}
pub struct MicroGenerator {
    random: Random,
    config: Micro,
    owner: u32,
    nodes: u32,
}
impl MicroGenerator {
    pub fn new(config: Micro, owner: u32, nodes: u32, seed: u64) -> Result<Self> {
        config.validate()?;
        if owner >= nodes {
            return Err(invalid("micro owner"));
        }
        Ok(Self {
            random: Random::new(seed),
            config,
            owner,
            nodes,
        })
    }
    pub fn generate(&mut self) -> MicroRequest {
        let remote = self.nodes > 1 && self.random.between(1, 100) <= self.config.remote_percent;
        let first_owner = if remote {
            let owner = self.random.between(0, self.nodes - 2);
            if owner >= self.owner {
                owner + 1
            } else {
                owner
            }
        } else {
            self.owner
        };
        let write = self.random.between(1, 100) <= self.config.write_percent;
        let keys = (0..self.config.operations)
            .map(|i| {
                (
                    if i == 0 { first_owner } else { self.owner },
                    self.random.between(0, self.config.keys - 1),
                )
            })
            .collect();
        MicroRequest { write, keys }
    }
}
