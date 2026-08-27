#![allow(dead_code)]

use std::rc::Rc;

use sto_core::{
    AcquireContext, AcquireError, CheckError, ExecutionCheckContext, FinishContext,
    FinishDisposition, FinishItem, InstallContext, InstallItem, ItemInitError, LockDisposition,
    NoPredicate, ObservationOrder, OpacityToken, PredicateContext, PreflightContext, PreflightItem,
    PrepareError, ReleaseContext, TransactionLock, TransactionalResource, ValidationContext,
};

pub struct Adapter;

#[derive(Clone, Copy)]
pub struct Observation;

impl OpacityToken for Observation {
    fn observation_order(&self) -> ObservationOrder {
        ObservationOrder::Unordered
    }
}

impl TransactionalResource for Adapter {
    type Key = ();
    type Local = ();
    type Observation = Observation;
    type Predicate = NoPredicate;
    type Intent = ();
    type Prepared = ();

    fn new_local(&self, _key: &Self::Key) -> Result<Self::Local, ItemInitError> {
        Ok(())
    }

    fn preflight(
        &self,
        _key: &Self::Key,
        _item: PreflightItem<'_, Self>,
        _cx: &mut PreflightContext<'_>,
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

    fn upgrade_predicate(
        &self,
        _key: &Self::Key,
        predicate: &Self::Predicate,
        _prepared: &Self::Prepared,
        _cx: &PredicateContext<'_>,
    ) -> Result<Self::Observation, CheckError> {
        match *predicate {}
    }

    fn validate_read(
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

    fn finish(
        &self,
        _key: &Self::Key,
        _item: FinishItem<'_, Self>,
        _prepared: Option<&mut Self::Prepared>,
        _disposition: FinishDisposition,
        _cx: &mut FinishContext<'_>,
    ) {
    }
}

pub struct LocalLock;

pub struct LocalGuard(pub Rc<()>);

impl TransactionLock for LocalLock {
    type Guard = LocalGuard;

    fn try_acquire(&self, _cx: &AcquireContext<'_>) -> Result<Self::Guard, AcquireError> {
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
