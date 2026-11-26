//! The same range-lock provider serves both RocksDB and Masstree transactions.
//! Sharding owns plan transitions; this adapter owns callback authorization.
use crate::{
    migration::status,
    storage::Store,
    wire::{Error, ErrorCode, Result},
};
use mako_sharding::{
    range_lock as contract,
    types::{MigrationPlan, Status},
    wire,
};
use mako_storage_api::range_lock::{RangeGuard, RangeIo, Request};
use std::{
    cell::RefCell,
    marker::PhantomData,
    rc::Rc,
    sync::{Arc, Weak},
};

struct Provider {
    store: Weak<Store>,
}
struct Guard {
    store: Weak<Store>,
    inner: RangeGuard,
}
struct Active {
    store: Arc<Store>,
    io: RangeIo,
}
thread_local! {
    static ACTIVE: RefCell<Option<Active>> = const { RefCell::new(None) };
}
struct Scope {
    _affine: PhantomData<Rc<()>>,
}
impl contract::RangeAccess for Scope {}
impl Drop for Scope {
    fn drop(&mut self) {
        ACTIVE.with(|slot| {
            slot.borrow_mut().take();
        });
    }
}
fn store(weak: &Weak<Store>) -> std::result::Result<Arc<Store>, Status> {
    let store = weak.upgrade().ok_or(Status::Retry)?;
    store.health().map_err(status)?;
    Ok(store)
}
pub(crate) fn provider(store: &Arc<Store>) -> Arc<dyn contract::RangeLocks> {
    Arc::new(Provider {
        store: Arc::downgrade(store),
    })
}
impl contract::RangeLocks for Provider {
    fn acquire(
        &self,
        plan: &MigrationPlan,
    ) -> std::result::Result<Arc<dyn contract::RangeGuard>, Status> {
        let store = store(&self.store)?;
        if store.owner != plan.source && store.owner != plan.destination {
            return Err(Status::Invalid);
        }
        let inner = store
            .ranges
            .acquire(Request {
                table: plan.range.table,
                lower: plan.range.lo.clone(),
                upper: plan.range.hi.clone(),
                generation: plan.generation,
                identity: wire::encode_plan(plan)?,
            })
            .map_err(status)?;
        Ok(Arc::new(Guard {
            store: self.store.clone(),
            inner,
        }))
    }
    fn reset(&self) -> std::result::Result<(), Status> {
        store(&self.store)?.ranges.reset().map_err(status)
    }
}
impl contract::RangeGuard for Guard {
    fn drained(&self) -> std::result::Result<bool, Status> {
        store(&self.store)?;
        self.inner.drained().map_err(status)
    }
    fn enter(&self) -> std::result::Result<Box<dyn contract::RangeAccess>, Status> {
        let store = store(&self.store)?;
        ACTIVE.with(|slot| {
            let mut slot = slot.borrow_mut();
            if slot.is_some() {
                return Err(Status::Busy);
            }
            let io = self.inner.enter().map_err(status)?;
            *slot = Some(Active { store, io });
            Ok(Box::new(Scope {
                _affine: PhantomData,
            }) as Box<dyn contract::RangeAccess>)
        })
    }
    fn finish(&self) -> std::result::Result<(), Status> {
        store(&self.store)?;
        self.inner.finish().map_err(status)
    }
}
pub(crate) fn point(store: &Arc<Store>, table: u64, coordinate: &[u8]) -> Result<()> {
    ACTIVE.with(|slot| {
        let slot = slot.borrow();
        let active = slot
            .as_ref()
            .ok_or_else(|| Error::new(ErrorCode::Invalid, "migration write has no range guard"))?;
        if !Arc::ptr_eq(store, &active.store) {
            return Err(Error::new(
                ErrorCode::Invalid,
                "migration range guard belongs to another shard",
            ));
        }
        active.io.point(table, coordinate)
    })
}
pub(crate) fn scan(
    store: &Arc<Store>,
    table: u64,
    lower: &[u8],
    upper: Option<&[u8]>,
) -> Result<()> {
    ACTIVE.with(|slot| {
        let slot = slot.borrow();
        let Some(active) = slot.as_ref() else {
            // Unguarded source precopy is advisory. It cannot certify final EOF.
            return Ok(());
        };
        if !Arc::ptr_eq(store, &active.store) {
            return Err(Error::new(
                ErrorCode::Invalid,
                "migration scan belongs to another shard",
            ));
        }
        active.io.range(table, lower, upper)
    })
}

#[cfg(all(test, feature = "rocksdb-engine"))]
mod tests {
    use super::*;
    use crate::storage::MemoryLog;
    use mako_sharding::types::{Boundary, Grant, KeyRange, TxnId};

    #[test]
    fn callback_scope_binds_shard_range_and_lifetime() {
        let directory = tempfile::tempdir().unwrap();
        let make = |owner| {
            Store::with_engine(
                owner,
                true,
                Arc::new(MemoryLog::new(owner)),
                mako_storage_rocksdb::database(
                    directory.path().join(format!("shard-{owner}")),
                    u64::from(owner),
                    false,
                )
                .unwrap(),
            )
            .unwrap()
        };
        let source = make(0);
        let destination = make(1);
        let plan = MigrationPlan {
            generation: 1,
            nonce: TxnId {
                client: 9,
                sequence: 1,
            },
            source: 0,
            destination: 1,
            range: KeyRange {
                table: 7,
                lo: b"b".to_vec(),
                hi: Some(b"f".to_vec()),
            },
            old: vec![Boundary {
                start: b"b".to_vec(),
                grant: Grant {
                    owner: 0,
                    ownership_version: 0,
                },
            }],
        };
        let provider = provider(&source);
        let guard = provider.acquire(&plan).unwrap();
        assert!(point(&source, 7, b"c").is_err());
        let scope = guard.enter().unwrap();
        point(&source, 7, b"c").unwrap();
        scan(&source, 7, b"b", Some(b"f")).unwrap();
        assert!(point(&destination, 7, b"c").is_err());
        assert!(point(&source, 8, b"c").is_err());
        assert!(point(&source, 7, b"f").is_err());
        assert!(scan(&source, 7, b"a", Some(b"f")).is_err());
        assert!(guard.enter().is_err());
        assert!(guard.finish().is_err());
        assert!(provider.reset().is_err());
        drop(scope);
        assert!(point(&source, 7, b"c").is_err());
        guard.finish().unwrap();
        assert!(guard.enter().is_err());
        assert!(provider.acquire(&plan).is_err());
        provider.reset().unwrap();
        let recovered = provider.acquire(&plan).unwrap();
        assert!(guard.finish().is_err());
        recovered.enter().unwrap();
    }
}
