//! Correctness-first transactional vector backed by one immutable snapshot.

use std::{fmt, sync::Arc};

use sto_core::{
    AccessError, Active, CapacityError, ObjectId, RegistrationError, Runtime, Transaction,
};

use crate::snapshot::Snapshot;

/// A checked transactional vector-bounds outcome.
///
/// Bounds failures are abstract operation results. They do not doom an
/// otherwise healthy transaction.
#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
pub struct VecBoundsError {
    index: usize,
    len: usize,
}

impl VecBoundsError {
    const fn new(index: usize, len: usize) -> Self {
        Self { index, len }
    }

    /// Returns the rejected vector index.
    pub const fn index(&self) -> usize {
        self.index
    }

    /// Returns the vector length checked by the operation.
    pub const fn vector_len(&self) -> usize {
        self.len
    }
}

impl fmt::Display for VecBoundsError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            formatter,
            "vector index {} is out of bounds for length {}",
            self.index, self.len
        )
    }
}

impl std::error::Error for VecBoundsError {}

/// A dynamically sized transactional vector with whole-vector conflicts.
///
/// Every operation observes one immutable vector snapshot. A mutation clones
/// that snapshot during transaction execution and stages the complete
/// replacement. Consequently, operations on the same vector conservatively
/// conflict even when they address different indices.
pub struct TxnVec<T: Clone + Send + Sync + 'static> {
    snapshot: Snapshot<Vec<T>>,
}

impl<T: Clone + Send + Sync + 'static> TxnVec<T> {
    /// Registers an empty vector in `runtime`.
    pub fn new(runtime: &Arc<Runtime>) -> Result<Self, RegistrationError> {
        Self::from_iter(runtime, [])
    }

    /// Registers a vector containing `values` in iteration order.
    #[allow(
        clippy::should_implement_trait,
        reason = "construction also requires the STO runtime that owns the vector"
    )]
    pub fn from_iter(
        runtime: &Arc<Runtime>,
        values: impl IntoIterator<Item = T>,
    ) -> Result<Self, RegistrationError> {
        let iterator = values.into_iter();
        let mut collected = Vec::new();
        collected
            .try_reserve(iterator.size_hint().0)
            .map_err(|_| CapacityError::BufferLimit)?;
        for value in iterator {
            if collected.len() == collected.capacity() {
                collected
                    .try_reserve(1)
                    .map_err(|_| CapacityError::BufferLimit)?;
            }
            collected.push(value);
        }
        Ok(Self {
            snapshot: Snapshot::new(runtime, collected)?,
        })
    }

    /// Returns the transaction-local vector length.
    pub fn len(&self, txn: &mut Transaction<'_, Active>) -> Result<usize, AccessError> {
        self.snapshot.inspect(txn, Vec::len)
    }

    /// Returns whether the transaction-local vector is empty.
    pub fn is_empty(&self, txn: &mut Transaction<'_, Active>) -> Result<bool, AccessError> {
        self.snapshot.inspect(txn, Vec::is_empty)
    }

    /// Returns an owned clone of one transaction-local element.
    ///
    /// The inner result is the abstract bounds outcome. The outer result is
    /// reserved for transaction or runtime failure.
    pub fn get(
        &self,
        txn: &mut Transaction<'_, Active>,
        index: usize,
    ) -> Result<Result<T, VecBoundsError>, AccessError> {
        self.snapshot.inspect(txn, |values| {
            values
                .get(index)
                .cloned()
                .ok_or_else(|| VecBoundsError::new(index, values.len()))
        })
    }

    /// Replaces one transaction-local element.
    ///
    /// A rejected index does not stage a write or doom the transaction.
    pub fn set(
        &self,
        txn: &mut Transaction<'_, Active>,
        index: usize,
        value: T,
    ) -> Result<Result<T, VecBoundsError>, AccessError> {
        self.snapshot.update_checked(txn, move |values| {
            if index >= values.len() {
                Err(VecBoundsError::new(index, values.len()))
            } else {
                Ok(std::mem::replace(&mut values[index], value))
            }
        })
    }

    /// Appends one element to the transaction-local vector.
    pub fn push(&self, txn: &mut Transaction<'_, Active>, value: T) -> Result<(), AccessError> {
        self.snapshot.update(txn, move |values| values.push(value))
    }

    /// Removes and returns the last transaction-local element, if any.
    pub fn pop(&self, txn: &mut Transaction<'_, Active>) -> Result<Option<T>, AccessError> {
        self.snapshot.update(txn, Vec::pop)
    }

    /// Inserts `value` before `index` in the transaction-local vector.
    ///
    /// Insertion at the current length appends. A larger index is an abstract
    /// bounds failure and does not stage a write.
    pub fn insert(
        &self,
        txn: &mut Transaction<'_, Active>,
        index: usize,
        value: T,
    ) -> Result<Result<(), VecBoundsError>, AccessError> {
        self.snapshot.update_checked(txn, move |values| {
            if index > values.len() {
                Err(VecBoundsError::new(index, values.len()))
            } else {
                values.insert(index, value);
                Ok(())
            }
        })
    }

    /// Removes and returns the transaction-local element at `index`.
    ///
    /// A rejected index does not stage a write or doom the transaction.
    pub fn remove(
        &self,
        txn: &mut Transaction<'_, Active>,
        index: usize,
    ) -> Result<Result<T, VecBoundsError>, AccessError> {
        self.snapshot.update_checked(txn, move |values| {
            if index >= values.len() {
                Err(VecBoundsError::new(index, values.len()))
            } else {
                Ok(values.remove(index))
            }
        })
    }

    /// Returns an owned copy of the transaction-local vector.
    pub fn to_vec(&self, txn: &mut Transaction<'_, Active>) -> Result<Vec<T>, AccessError> {
        self.snapshot.inspect(txn, Clone::clone)
    }

    /// Returns this vector's stable STO object identity.
    pub fn object_id(&self) -> ObjectId {
        self.snapshot.object_id()
    }
}

impl<T: Clone + Send + Sync + 'static> Clone for TxnVec<T> {
    fn clone(&self) -> Self {
        Self {
            snapshot: self.snapshot.clone(),
        }
    }
}

impl<T: Clone + Send + Sync + 'static> fmt::Debug for TxnVec<T> {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("TxnVec")
            .field("object_id", &self.object_id())
            .field("conflict_granularity", &"whole vector")
            .finish_non_exhaustive()
    }
}
