use std::{marker::PhantomData, mem, mem::MaybeUninit, ptr::NonNull};

use sto_core::{Active, BeginError, InvalidUse, Participant, Transaction, WorkerContext};

type ActiveTransaction = Transaction<'static, Active>;

/// Stable ownership for a worker and its lifetime-erased active transaction.
///
/// Only the borrowed worker needs a stable address. The transaction itself
/// is an ordinary movable value, so keeping its slot inline avoids a pointer
/// chase on every operation. The worker pointer and transaction slot remain
/// private to this child module: outer handle borrows cannot recursively
/// retag the exclusive worker reference, and callers cannot reach the worker.
pub(super) struct StoWorkerOwner {
    transaction: MaybeUninit<ActiveTransaction>,
    participant: Option<Participant<'static>>,
    worker: NonNull<WorkerContext>,
    active: bool,
    owns_worker: PhantomData<Box<WorkerContext>>,
}

impl StoWorkerOwner {
    pub(super) fn new(worker: WorkerContext) -> Self {
        fn assert_unpin<T: Unpin>() {}

        // Moving the private owner while active moves the transaction slot.
        // Keep that assumption compile-time checked if Transaction changes.
        assert_unpin::<ActiveTransaction>();
        let worker = NonNull::new(Box::into_raw(Box::new(worker)))
            .expect("Box::into_raw never returns null");
        Self {
            transaction: MaybeUninit::uninit(),
            participant: None,
            worker,
            active: false,
            owns_worker: PhantomData,
        }
    }

    pub(super) const fn is_active(&self) -> bool {
        self.active || self.participant.is_some()
    }

    pub(super) fn has_participant(&self) -> bool {
        self.participant.is_some()
    }

    /// Begins and stores a transaction whose worker borrow stays private.
    pub(super) fn begin_erased(&mut self) -> Result<(), BeginError> {
        if self.is_active() {
            return Err(InvalidUse::WorkerBusy.into());
        }
        // SAFETY: The inactive state proves that no stored transaction
        // borrows the worker. Its private allocation is stable and owned by
        // `self`; no outer code can dereference the pointer while active.
        let transaction = unsafe { (*self.worker.as_ptr()).begin()? };
        // SAFETY: The worker allocation remains stable until Drop, and this
        // module consumes the transaction before reclaiming that worker.
        let transaction =
            unsafe { mem::transmute::<Transaction<'_, Active>, ActiveTransaction>(transaction) };
        // SAFETY: `active == false` means the slot is uninitialized.
        self.transaction.write(transaction);
        self.active = true;
        Ok(())
    }

    /// Runs one operation with a lifetime-branded borrow of the transaction.
    ///
    /// The higher-ranked closure prevents the transaction or its borrow
    /// from escaping, and prevents exchanging transactions borrowed from
    /// different finite-lived owners through safe parent-module code.
    #[inline(always)]
    pub(super) fn with_active_transaction<R>(
        &mut self,
        operation: impl for<'transaction> FnOnce(
            &'transaction mut Transaction<'transaction, Active>,
        ) -> R,
    ) -> Option<R> {
        if !self.active {
            return None;
        }
        // SAFETY: `active == true` means begin initialized the slot. The
        // private helper brands both lifetimes with this exclusive borrow.
        let transaction = unsafe { self.borrow_active() };
        Some(operation(transaction))
    }

    /// Consumes the active transaction inside a lifetime-branded closure.
    #[inline(always)]
    pub(super) fn finish_active<R>(
        &mut self,
        operation: impl for<'transaction> FnOnce(Transaction<'transaction, Active>) -> R,
    ) -> Option<R> {
        if !self.active {
            return None;
        }
        self.active = false;
        // SAFETY: The slot is initialized exactly while `active` is true.
        let transaction = unsafe { self.take_active() };
        Some(operation(transaction))
    }

    /// Transfers the existing worker borrow; never creates a second borrow.
    pub(super) fn promote(&mut self) -> bool {
        if !self.active || self.participant.is_some() {
            return false;
        }
        self.active = false;
        // SAFETY: The initialized transaction is consumed exactly once,
        // and the retained participant has the identical worker lifetime.
        let transaction = unsafe { self.transaction.assume_init_read() };
        self.participant = Some(transaction.into_participant());
        true
    }

    pub(super) fn with_participant<R>(
        &mut self,
        operation: impl for<'borrow> FnOnce(&'borrow mut Participant<'borrow>) -> R,
    ) -> Option<R> {
        let participant = self.participant.as_mut()?;
        // SAFETY: As with borrow_active, both lifetimes are shortened to
        // this exclusive owner borrow. HRTB prevents either from escaping.
        let participant = unsafe {
            mem::transmute::<&mut Participant<'static>, &mut Participant<'_>>(participant)
        };
        Some(operation(participant))
    }

    pub(super) fn finish_participant<R>(
        &mut self,
        operation: impl for<'borrow> FnOnce(Participant<'borrow>) -> R,
    ) -> Option<R> {
        self.participant.take().map(operation)
    }

    /// Borrows the initialized slot while tying both transaction lifetimes
    /// to the exclusive owner borrow.
    unsafe fn borrow_active<'transaction>(
        &'transaction mut self,
    ) -> &'transaction mut Transaction<'transaction, Active> {
        // SAFETY: The caller proved the inline slot initialized, and no
        // reference to the stored value is live.
        let transaction: &'transaction mut ActiveTransaction =
            unsafe { self.transaction.assume_init_mut() };
        // SAFETY: Lifetimes do not affect layout. The exclusive owner borrow
        // bounds both shortened lifetimes and prevents the value from moving.
        unsafe {
            mem::transmute::<
                &'transaction mut ActiveTransaction,
                &'transaction mut Transaction<'transaction, Active>,
            >(transaction)
        }
    }

    /// Moves the initialized slot out with its worker lifetime shortened to
    /// the exclusive owner borrow retained across the consuming closure.
    unsafe fn take_active<'transaction>(
        &'transaction mut self,
    ) -> Transaction<'transaction, Active> {
        // SAFETY: The caller changed `active` from true to false immediately
        // before this call, so the slot contains exactly one live value.
        unsafe { self.transaction.assume_init_read() }
    }
}

impl Drop for StoWorkerOwner {
    fn drop(&mut self) {
        let _ = self.finish_active(|transaction| {
            let _ = transaction.abort();
        });
        let _ = self.finish_participant(|participant| participant.abort());
        // SAFETY: `new` obtained this pointer from exactly one Box. The
        // transaction slot is uninitialized and no worker borrow remains.
        drop(unsafe { Box::from_raw(self.worker.as_ptr()) });
    }
}
