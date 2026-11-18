//! A transactional FIFO queue backed by one snapshot item.
//!
//! The entire queue is one logical conflict unit. This deliberately simple
//! policy makes the type useful as a reference adapter: every operation sees
//! one transaction-local queue snapshot, and any committed queue mutation
//! invalidates an older observation of any part of that queue. Values enter at
//! the back and leave from the front.

use std::{collections::VecDeque, fmt, sync::Arc};

use sto_core::{
    AccessError, Active, CapacityError, ObjectId, RegistrationError, Runtime, Transaction,
};

use crate::snapshot::Snapshot;

/// A transactionally composable first-in, first-out queue.
///
/// `TxnQueue` favors a small, auditable adapter over fine-grained concurrency.
/// Reads and writes operate on a transaction-local clone of the complete
/// `VecDeque`, so two transactions that touch different positions still
/// conflict if one commits a mutation before the other commits.
pub struct TxnQueue<T: Clone + Send + Sync + 'static> {
    snapshot: Snapshot<VecDeque<T>>,
}

impl<T: Clone + Send + Sync + 'static> TxnQueue<T> {
    /// Registers an empty queue in `runtime`.
    pub fn new(runtime: &Arc<Runtime>) -> Result<Self, RegistrationError> {
        Self::from_iter(runtime, std::iter::empty())
    }

    /// Registers a queue containing `values` in iteration order.
    ///
    /// The first yielded value becomes the front of the queue.
    #[allow(
        clippy::should_implement_trait,
        reason = "construction also requires the STO runtime that owns the queue"
    )]
    pub fn from_iter(
        runtime: &Arc<Runtime>,
        values: impl IntoIterator<Item = T>,
    ) -> Result<Self, RegistrationError> {
        let iterator = values.into_iter();
        let mut values = VecDeque::new();
        values
            .try_reserve(iterator.size_hint().0)
            .map_err(|_| CapacityError::BufferLimit)?;
        for value in iterator {
            if values.len() == values.capacity() {
                values
                    .try_reserve(1)
                    .map_err(|_| CapacityError::BufferLimit)?;
            }
            values.push_back(value);
        }
        let snapshot = Snapshot::new(runtime, values)?;
        Ok(Self { snapshot })
    }

    /// Returns the stable STO object identity of this queue.
    pub fn object_id(&self) -> ObjectId {
        self.snapshot.object_id()
    }

    /// Returns the transaction-local number of queued values.
    ///
    /// Queue length is dynamic, so this operation records an observation and
    /// can fail if the transaction is already unusable.
    pub fn len(&self, txn: &mut Transaction<'_, Active>) -> Result<usize, AccessError> {
        self.snapshot.inspect(txn, VecDeque::len)
    }

    /// Returns whether the transaction-local queue is empty.
    ///
    /// This operation observes the whole queue, like every other queue read.
    pub fn is_empty(&self, txn: &mut Transaction<'_, Active>) -> Result<bool, AccessError> {
        self.snapshot.inspect(txn, VecDeque::is_empty)
    }

    /// Clones the transaction-local front value without removing it.
    pub fn front(&self, txn: &mut Transaction<'_, Active>) -> Result<Option<T>, AccessError> {
        self.snapshot.inspect(txn, |queue| queue.front().cloned())
    }

    /// Clones the transaction-local back value without removing it.
    pub fn back(&self, txn: &mut Transaction<'_, Active>) -> Result<Option<T>, AccessError> {
        self.snapshot.inspect(txn, |queue| queue.back().cloned())
    }

    /// Stages `value` at the back of the transaction-local queue.
    pub fn push_back(
        &self,
        txn: &mut Transaction<'_, Active>,
        value: T,
    ) -> Result<(), AccessError> {
        self.snapshot.update(txn, move |queue| {
            queue.push_back(value);
        })
    }

    /// Removes and returns the transaction-local front value, if any.
    pub fn pop_front(&self, txn: &mut Transaction<'_, Active>) -> Result<Option<T>, AccessError> {
        self.snapshot.update(txn, VecDeque::pop_front)
    }

    /// Clones the transaction-local queue into front-to-back order.
    pub fn to_vec(&self, txn: &mut Transaction<'_, Active>) -> Result<Vec<T>, AccessError> {
        self.snapshot
            .inspect(txn, |queue| queue.iter().cloned().collect())
    }
}

impl<T: Clone + Send + Sync + 'static> Clone for TxnQueue<T> {
    fn clone(&self) -> Self {
        Self {
            snapshot: self.snapshot.clone(),
        }
    }
}

impl<T: Clone + Send + Sync + 'static> fmt::Debug for TxnQueue<T> {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("TxnQueue")
            .field("object_id", &self.object_id())
            .field("conflict_granularity", &"whole queue")
            .finish_non_exhaustive()
    }
}

#[cfg(test)]
mod tests {
    use std::{sync::Arc, thread};

    use sto_core::{AbortReason, CommitOutcome, RuntimeConfig};

    use super::*;

    fn assert_committed(outcome: CommitOutcome) {
        assert!(matches!(outcome, CommitOutcome::Committed(_)));
    }

    #[test]
    fn empty_queue_reports_no_values() {
        let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
        let mut worker = runtime.attach().unwrap();
        let queue = TxnQueue::<u64>::new(&runtime).unwrap();

        let mut txn = worker.begin().unwrap();
        assert!(queue.is_empty(&mut txn).unwrap());
        assert_eq!(queue.len(&mut txn).unwrap(), 0);
        assert_eq!(queue.front(&mut txn).unwrap(), None);
        assert_eq!(queue.back(&mut txn).unwrap(), None);
        assert_eq!(queue.pop_front(&mut txn).unwrap(), None);
        assert_committed(txn.commit().unwrap());
    }

    #[test]
    fn queue_preserves_fifo_order_and_reads_its_writes() {
        let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
        let mut worker = runtime.attach().unwrap();
        let queue = TxnQueue::from_iter(&runtime, [1_u64, 2]).unwrap();

        let mut txn = worker.begin().unwrap();
        queue.push_back(&mut txn, 3).unwrap();
        assert_eq!(queue.front(&mut txn).unwrap(), Some(1));
        assert_eq!(queue.back(&mut txn).unwrap(), Some(3));
        assert_eq!(queue.pop_front(&mut txn).unwrap(), Some(1));
        assert_eq!(queue.to_vec(&mut txn).unwrap(), vec![2, 3]);
        assert_eq!(queue.len(&mut txn).unwrap(), 2);
        assert_committed(txn.commit().unwrap());

        let mut verify = worker.begin().unwrap();
        assert_eq!(queue.to_vec(&mut verify).unwrap(), vec![2, 3]);
        assert_committed(verify.commit().unwrap());
    }

    #[test]
    fn abort_discards_the_complete_transaction_local_queue() {
        let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
        let mut worker = runtime.attach().unwrap();
        let queue = TxnQueue::from_iter(&runtime, [4_u64, 5]).unwrap();

        let mut txn = worker.begin().unwrap();
        assert_eq!(queue.pop_front(&mut txn).unwrap(), Some(4));
        queue.push_back(&mut txn, 6).unwrap();
        assert_eq!(queue.to_vec(&mut txn).unwrap(), vec![5, 6]);
        assert_eq!(*txn.abort().reason(), AbortReason::Explicit);

        let mut verify = worker.begin().unwrap();
        assert_eq!(queue.to_vec(&mut verify).unwrap(), vec![4, 5]);
        assert_committed(verify.commit().unwrap());
    }

    #[test]
    fn cloned_handles_retain_one_object_identity() {
        let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
        let queue = TxnQueue::<String>::new(&runtime).unwrap();
        let clone = queue.clone();

        assert_eq!(queue.object_id(), clone.object_id());
        assert!(format!("{queue:?}").contains("whole queue"));
    }

    #[test]
    fn a_committed_mutation_invalidates_an_older_whole_queue_read() {
        let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
        let mut stale_worker = runtime.attach().unwrap();
        let queue = TxnQueue::from_iter(&runtime, [7_u64]).unwrap();

        let mut stale = stale_worker.begin().unwrap();
        assert_eq!(queue.front(&mut stale).unwrap(), Some(7));

        let writer_runtime = Arc::clone(&runtime);
        let writer_queue = queue.clone();
        thread::spawn(move || {
            let mut worker = writer_runtime.attach().unwrap();
            let mut txn = worker.begin().unwrap();
            writer_queue.push_back(&mut txn, 8).unwrap();
            assert_committed(txn.commit().unwrap());
        })
        .join()
        .unwrap();

        assert!(matches!(
            stale.commit().unwrap(),
            CommitOutcome::Aborted(AbortReason::Conflict(_))
        ));
    }
}
