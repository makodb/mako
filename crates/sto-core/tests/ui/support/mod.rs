#![allow(dead_code)]

use std::rc::Rc;

use sto_core::{
    AcquireContext, AcquireError, CheckError, ExecutionCheckContext, CleanupContext,
    CleanupDisposition, CleanupItem, InstallContext, InstallItem, ItemInitError, LockDisposition,
    LockIdentity, NoPredicate, ObservationOrder, OpacityToken, PredicateContext, LockContext,
    PreflightItem, PrepareError, ReleaseContext, TransactionLock, TObject,
    ValidationContext,
};

pub struct Adapter;

#[derive(Clone, Copy)]
pub struct Observation;

impl OpacityToken for Observation {
    fn observation_order(&self) -> ObservationOrder {
        ObservationOrder::Unordered
    }
}

impl TObject for Adapter {
    type Key = ();
    type Local = ();
    type Observation = Observation;
    type Predicate = NoPredicate;
    type Intent = ();
    type Prepared = ();

    fn new_local(&self, _key: &Self::Key) -> Result<Self::Local, ItemInitError> {
        Ok(())
    }

    fn lock(
        &self,
        _key: &Self::Key,
        _item: PreflightItem<'_, Self>,
        _cx: &mut LockContext<'_>,
    ) -> Result<Self::Prepared, PrepareError> {
        Ok(())
    }

    fn revalidate_read(
        &self,
        _key: &Self::Key,
        _observation: &Self::Observation,
        _cx: &ExecutionCheckContext<'_>,
    ) -> Result<(), CheckError> {
        Ok(())
    }

    fn revalidate_predicate(
        &self,
        _key: &Self::Key,
        predicate: &Self::Predicate,
        _cx: &ExecutionCheckContext<'_>,
    ) -> Result<ObservationOrder, CheckError> {
        match *predicate {}
    }

    fn check_predicate(
        &self,
        _key: &Self::Key,
        predicate: &Self::Predicate,
        _prepared: &Self::Prepared,
        _cx: &PredicateContext<'_>,
    ) -> Result<Self::Observation, CheckError> {
        match *predicate {}
    }

    fn check(
        &self,
        _key: &Self::Key,
        _observation: &Self::Observation,
        _prepared: &Self::Prepared,
        _cx: &ValidationContext<'_>,
    ) -> Result<(), CheckError> {
        Ok(())
    }

    fn install(
        &self,
        _key: &Self::Key,
        _item: InstallItem<'_, Self>,
        _prepared: &mut Self::Prepared,
        _cx: &mut InstallContext<'_>,
    ) {
    }

    fn cleanup(
        &self,
        _key: &Self::Key,
        _item: CleanupItem<'_, Self>,
        _prepared: Option<&mut Self::Prepared>,
        _disposition: CleanupDisposition,
        _cx: &mut CleanupContext<'_>,
    ) {
    }
}

pub struct LocalLock;

pub struct LocalGuard(pub Rc<()>);

impl TransactionLock for LocalLock {
    type Guard = LocalGuard;

    fn try_acquire(
        &self,
        _identity: &LockIdentity,
        _cx: &AcquireContext<'_>,
    ) -> Result<Self::Guard, AcquireError> {
        Ok(LocalGuard(Rc::new(())))
    }

    fn release(
        &self,
        _guard: &mut Self::Guard,
        _disposition: LockDisposition,
        _cx: &ReleaseContext<'_>,
    ) {
    }
}
