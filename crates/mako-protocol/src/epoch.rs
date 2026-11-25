//! Host admission epochs. A freeze publishes its immutable view before cutting
//! this domain. Work keeps a pin through storage cleanup and response retention.
use crate::wire::{Error, ErrorCode, Result};
use parking_lot::Mutex;
use std::{collections::BTreeMap, sync::Arc};

struct State {
    current: u64,
    active: BTreeMap<u64, usize>,
}
pub struct EpochDomain {
    state: Mutex<State>,
}
pub struct Pin {
    domain: Arc<EpochDomain>,
    epoch: u64,
}
impl Default for EpochDomain {
    fn default() -> Self {
        Self {
            state: Mutex::new(State {
                current: 1,
                active: BTreeMap::new(),
            }),
        }
    }
}
impl EpochDomain {
    pub fn enter(self: &Arc<Self>) -> Pin {
        let mut state = self.state.lock();
        let epoch = state.current;
        *state.active.entry(epoch).or_default() += 1;
        Pin {
            domain: self.clone(),
            epoch,
        }
    }
    pub fn cut(&self) -> Result<u64> {
        let mut state = self.state.lock();
        let cut = state.current;
        state.current = cut
            .checked_add(1)
            .ok_or_else(|| Error::new(ErrorCode::Capacity, "admission epoch exhausted"))?;
        Ok(cut)
    }
    pub fn drained(&self, cut: u64) -> Result<bool> {
        let state = self.state.lock();
        if cut == 0 || cut >= state.current {
            return Err(Error::new(ErrorCode::Invalid, "invalid epoch cut"));
        }
        Ok(state.active.range(..=cut).next().is_none())
    }
}
impl Pin {
    pub fn epoch(&self) -> u64 {
        self.epoch
    }
}
impl Drop for Pin {
    fn drop(&mut self) {
        let mut state = self.domain.state.lock();
        let count = state
            .active
            .get_mut(&self.epoch)
            .expect("live admission pin");
        *count -= 1;
        if *count == 0 {
            state.active.remove(&self.epoch);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn cut_waits_for_all_prior_pins_but_not_new_arrivals() {
        let domain = Arc::new(EpochDomain::default());
        let first = domain.enter();
        let nested = domain.enter();
        let cut = domain.cut().unwrap();
        let new = domain.enter();
        drop(first);
        assert!(!domain.drained(cut).unwrap());
        drop(nested);
        assert!(domain.drained(cut).unwrap());
        let next = domain.cut().unwrap();
        assert!(!domain.drained(next).unwrap());
        drop(new);
        assert!(domain.drained(next).unwrap());
    }
    #[test]
    fn cut_never_wraps_or_accepts_future_epochs() {
        let domain = EpochDomain {
            state: Mutex::new(State {
                current: u64::MAX,
                active: BTreeMap::new(),
            }),
        };
        assert!(domain.cut().is_err());
        assert!(domain.drained(0).is_err());
        assert!(domain.drained(u64::MAX).is_err());
    }
}
