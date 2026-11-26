//! Migration exclusion shared by all transactional storage implementations.
//!
//! Reservations block new ordinary access to a logical half-open range. Earlier
//! admission pins may finish, but migration I/O cannot start until they drain.
//! The initial implementation conservatively drains all earlier shard pins.
//! Dropping a reservation never unlocks it. The controller must publish the
//! terminal ownership/admission state and finish cleanup before explicit release.
use crate::epoch::{EpochDomain, Pin};
use crate::wire::{Error, ErrorCode, Result};
use parking_lot::Mutex;
use std::{collections::BTreeMap, sync::Arc};

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Request {
    pub table: u64,
    pub lower: Vec<u8>,
    pub upper: Option<Vec<u8>>,
    pub generation: u64,
    /// Canonical migration identity, including source, destination and old grants.
    pub identity: Vec<u8>,
}
impl Request {
    fn overlaps(&self, table: u64, lower: &[u8], upper: Option<&[u8]>) -> bool {
        self.table == table
            && self.upper.as_deref().is_none_or(|end| lower < end)
            && upper.is_none_or(|end| self.lower.as_slice() < end)
    }
    fn contains(&self, table: u64, key: &[u8]) -> bool {
        self.table == table
            && self.lower.as_slice() <= key
            && self.upper.as_deref().is_none_or(|end| key < end)
    }
}
struct Reservation {
    request: Request,
    cut: u64,
    io: bool,
}
struct State {
    incarnation: u64,
    active: BTreeMap<u64, Reservation>,
    // Retain identities so a delayed acquire cannot reopen a completed migration.
    // The controller already retains the corresponding plans/terminal receipts.
    finished: BTreeMap<u64, Request>,
}
pub struct RangeLocks {
    epochs: Arc<EpochDomain>,
    state: Mutex<State>,
}
#[derive(Clone)]
pub struct RangeGuard {
    locks: Arc<RangeLocks>,
    incarnation: u64,
    generation: u64,
}
/// An authorized migration operation. Release/reset cannot invalidate live I/O.
pub struct RangeIo {
    guard: RangeGuard,
}
fn stale() -> Error {
    Error::new(ErrorCode::RouteChanged, "migration range guard is stale")
}
fn busy() -> Error {
    Error::new(ErrorCode::Busy, "migration range is not exclusive yet")
}
fn valid_range(lower: &[u8], upper: Option<&[u8]>) -> Result<()> {
    if upper.is_some_and(|end| end <= lower) {
        Err(Error::new(ErrorCode::Invalid, "invalid migration range"))
    } else {
        Ok(())
    }
}
impl RangeLocks {
    pub fn new(epochs: Arc<EpochDomain>) -> Arc<Self> {
        Arc::new(Self {
            epochs,
            state: Mutex::new(State {
                incarnation: 1,
                active: BTreeMap::new(),
                finished: BTreeMap::new(),
            }),
        })
    }
    /// Acquisition is nonblocking. Poll/enter the returned guard to wait for
    /// earlier work. Publication of frozen/staging admission precedes this call.
    pub fn acquire(self: &Arc<Self>, request: Request) -> Result<RangeGuard> {
        valid_range(&request.lower, request.upper.as_deref())?;
        if request.generation == 0 || request.identity.is_empty() {
            return Err(Error::new(ErrorCode::Invalid, "missing migration identity"));
        }
        let mut state = self.state.lock();
        if let Some(existing) = state.active.get(&request.generation) {
            if existing.request != request {
                return Err(Error::new(ErrorCode::Invalid, "migration identity changed"));
            }
        } else {
            if state.finished.contains_key(&request.generation)
                || state.finished.values().any(|old| {
                    old.generation > request.generation
                        && old.overlaps(request.table, &request.lower, request.upper.as_deref())
                })
            {
                return Err(stale());
            }
            if state.active.values().any(|old| {
                old.request
                    .overlaps(request.table, &request.lower, request.upper.as_deref())
            }) {
                return Err(busy());
            }
            let cut = self.epochs.cut()?;
            state.active.insert(
                request.generation,
                Reservation {
                    request: request.clone(),
                    cut,
                    io: false,
                },
            );
        }
        Ok(RangeGuard {
            locks: self.clone(),
            incarnation: state.incarnation,
            generation: request.generation,
        })
    }
    fn check_pin(&self, pin: &Pin) -> Result<()> {
        if !self.epochs.owns(pin) {
            Err(Error::new(
                ErrorCode::Invalid,
                "foreign transaction admission pin",
            ))
        } else {
            Ok(())
        }
    }
    pub fn point(&self, pin: &Pin, table: u64, key: &[u8]) -> Result<()> {
        self.check_pin(pin)?;
        if self
            .state
            .lock()
            .active
            .values()
            .any(|r| pin.epoch() > r.cut && r.request.contains(table, key))
        {
            Err(busy())
        } else {
            Ok(())
        }
    }
    pub fn range(&self, pin: &Pin, table: u64, lower: &[u8], upper: Option<&[u8]>) -> Result<()> {
        self.check_pin(pin)?;
        valid_range(lower, upper)?;
        if self
            .state
            .lock()
            .active
            .values()
            .any(|r| pin.epoch() > r.cut && r.request.overlaps(table, lower, upper))
        {
            Err(busy())
        } else {
            Ok(())
        }
    }
    /// Recovery only: ordinary admission must be closed and previous physical
    /// effects settled. Reconstruct reservations before reopening admission.
    pub fn reset(&self) -> Result<()> {
        let mut state = self.state.lock();
        if state.active.values().any(|r| r.io) {
            return Err(busy());
        }
        state.incarnation = state
            .incarnation
            .checked_add(1)
            .ok_or_else(|| Error::new(ErrorCode::Capacity, "range guard incarnation exhausted"))?;
        state.active.clear();
        state.finished.clear();
        Ok(())
    }
}
impl RangeGuard {
    fn reservation<'a>(&self, state: &'a State) -> Result<&'a Reservation> {
        if state.incarnation != self.incarnation {
            return Err(stale());
        }
        state.active.get(&self.generation).ok_or_else(stale)
    }
    pub fn drained(&self) -> Result<bool> {
        let state = self.locks.state.lock();
        self.locks.epochs.drained(self.reservation(&state)?.cut)
    }
    pub fn enter(&self) -> Result<RangeIo> {
        let mut state = self.locks.state.lock();
        let reservation = self.reservation(&state)?;
        if reservation.io || !self.locks.epochs.drained(reservation.cut)? {
            return Err(busy());
        }
        state.active.get_mut(&self.generation).unwrap().io = true;
        Ok(RangeIo {
            guard: self.clone(),
        })
    }
    /// Explicit terminal completion, after ownership publication and cleanup.
    /// Repeating this exact completion is harmless; old handles never release
    /// another incarnation's reservation, even if it has the same generation.
    pub fn finish(&self) -> Result<()> {
        let mut state = self.locks.state.lock();
        if state.incarnation != self.incarnation {
            return Err(stale());
        }
        if state.finished.contains_key(&self.generation) {
            return Ok(());
        }
        if self.reservation(&state)?.io {
            return Err(busy());
        }
        let reservation = state.active.remove(&self.generation).unwrap();
        state.finished.insert(self.generation, reservation.request);
        Ok(())
    }
}
impl RangeIo {
    pub fn point(&self, table: u64, key: &[u8]) -> Result<()> {
        let state = self.guard.locks.state.lock();
        if self.guard.reservation(&state)?.request.contains(table, key) {
            Ok(())
        } else {
            Err(Error::new(
                ErrorCode::Invalid,
                "migration effect outside guarded range",
            ))
        }
    }
    pub fn range(&self, table: u64, lower: &[u8], upper: Option<&[u8]>) -> Result<()> {
        valid_range(lower, upper)?;
        let state = self.guard.locks.state.lock();
        let request = &self.guard.reservation(&state)?.request;
        if request.table == table
            && request.lower.as_slice() <= lower
            && match (request.upper.as_deref(), upper) {
                (None, _) => true,
                (Some(end), Some(upper)) => upper <= end,
                (Some(_), None) => false,
            }
        {
            Ok(())
        } else {
            Err(Error::new(
                ErrorCode::Invalid,
                "migration scan outside guarded range",
            ))
        }
    }
}
impl Drop for RangeIo {
    fn drop(&mut self) {
        let mut state = self.guard.locks.state.lock();
        // reset/finish refuse live I/O, so this reservation remains current.
        assert_eq!(state.incarnation, self.guard.incarnation);
        state
            .active
            .get_mut(&self.guard.generation)
            .expect("live range I/O")
            .io = false;
    }
}
