//! A batch of writes, shaped like RocksDB's `WriteBatch`.

/// One buffered operation.
pub(crate) enum Op {
    Put(Vec<u8>, Vec<u8>),
    Delete(Vec<u8>),
}

/// A sequence of writes to apply together.
///
/// **This is not atomic**, and the name is kept only because callers
/// recognise it. RocksDB's `WriteBatch` is applied as a unit; this one is
/// replayed through the ordinary write path, so each operation becomes
/// visible as it lands and a reader can see a partially applied batch.
///
/// It buys call-overhead amortisation and ordering within the batch,
/// nothing more. Anything needing isolation needs a commit protocol above
/// this layer.
#[derive(Default)]
pub struct WriteBatch {
    ops: Vec<Op>,
}

impl WriteBatch {
    /// An empty batch.
    pub fn new() -> Self {
        Self::default()
    }

    /// Queue a write.
    pub fn put(&mut self, key: &[u8], value: &[u8]) -> &mut Self {
        self.ops.push(Op::Put(key.to_vec(), value.to_vec()));
        self
    }

    /// Queue a delete.
    pub fn delete(&mut self, key: &[u8]) -> &mut Self {
        self.ops.push(Op::Delete(key.to_vec()));
        self
    }

    /// How many operations are queued.
    pub fn len(&self) -> usize {
        self.ops.len()
    }

    /// Whether the batch is empty.
    pub fn is_empty(&self) -> bool {
        self.ops.is_empty()
    }

    pub(crate) fn into_ops(self) -> Vec<Op> {
        self.ops
    }
}
