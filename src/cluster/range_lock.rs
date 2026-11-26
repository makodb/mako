//! Transactional-storage contract for migration range exclusion.
//!
//! The controller publishes non-serving admission before acquire. The provider
//! blocks new ordinary accesses to the range and waits for earlier admitted
//! transactions, including prepared/uncertain work. A granted callback scope
//! authorizes only this plan's range and physical host. Guard drop never unlocks.
//! Explicit finish follows terminal ownership publication and all cleanup;
//! retired owners remain fenced by that published admission. Recovery resets
//! guards with admission closed and old physical effects settled, reconstructing
//! reservations before serving again.
use crate::types::{MigrationPlan, Status};
use std::sync::Arc;

/// Affine callback scope. Drop ends authorization for migration I/O.
pub trait RangeAccess {}
pub trait RangeGuard: Send + Sync {
    fn drained(&self) -> Result<bool, Status>;
    fn enter(&self) -> Result<Box<dyn RangeAccess>, Status>;
    fn finish(&self) -> Result<(), Status>;
}
pub trait RangeLocks: Send + Sync {
    fn acquire(&self, plan: &MigrationPlan) -> Result<Arc<dyn RangeGuard>, Status>;
    fn reset(&self) -> Result<(), Status>;
}
