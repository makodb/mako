//! The record queue: records numbered in step order (design §3, "Records,
//! under the lock").
//!
//! The step wrappers push under the server's `mtx_`, so the numbering is the
//! order in which steps changed memory. The flusher takes everything queued
//! at once: one batch, one sync, for every thread's records (group commit).

use std::sync::{Condvar, Mutex, MutexGuard};

use crate::record::Record;

struct Inner<P> {
    q: Vec<Record<P>>,
    /// The sequence number of `q[0]` (or of the next push, if `q` is empty).
    first_seq: u64,
    closed: bool,
    // When the oldest queued record was pushed (stats only).
    oldest_us: u64,
}

pub struct RecordQueue<P> {
    inner: Mutex<Inner<P>>,
    cv: Condvar,
}

impl<P> RecordQueue<P> {
    /// A queue whose first record will be number `next_seq`.
    pub fn new(next_seq: u64) -> Self {
        RecordQueue { inner: Mutex::new(Inner { q: Vec::new(), first_seq: next_seq, closed: false, oldest_us: 0 }), cv: Condvar::new() }
    }

    fn lock(&self) -> MutexGuard<'_, Inner<P>> {
        self.inner.lock().unwrap_or_else(|e| e.into_inner())
    }

    /// Queues a record; returns its sequence number.
    pub fn push(&self, rec: Record<P>) -> u64 {
        let mut g = self.lock();
        assert!(!g.closed, "record pushed after the store closed");
        if g.q.is_empty() {
            g.oldest_us = crate::stats::now_us();
        }
        g.q.push(rec);
        let seq = g.first_seq + g.q.len() as u64 - 1;
        drop(g);
        self.cv.notify_one();
        seq
    }

    /// The number of the last record queued (0 if none ever was): the tail
    /// an output produced now must wait for.
    pub fn last_seq(&self) -> u64 {
        let g = self.lock();
        g.first_seq + g.q.len() as u64 - 1
    }

    /// Waits for records and takes them all: `(first_seq, records)`. `None`
    /// once the queue is closed and empty.
    pub fn take_all(&self) -> Option<(u64, Vec<Record<P>>)> {
        let mut g = self.lock();
        while g.q.is_empty() && !g.closed {
            g = self.cv.wait(g).unwrap_or_else(|e| e.into_inner());
        }
        if g.q.is_empty() {
            return None;
        }
        let first = g.first_seq;
        if crate::stats::on() {
            crate::stats::add(crate::stats::QUEUE_WAIT, crate::stats::now_us().saturating_sub(g.oldest_us));
        }
        let batch = std::mem::take(&mut g.q);
        g.first_seq += batch.len() as u64;
        Some((first, batch))
    }

    /// No more pushes; the flusher drains what is queued, then stops.
    pub fn close(&self) {
        self.lock().closed = true;
        self.cv.notify_all();
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn numbers_in_push_order() {
        let q: RecordQueue<u8> = RecordQueue::new(5);
        assert_eq!(q.last_seq(), 4);
        assert_eq!(q.push(Record::default()), 5);
        assert_eq!(q.push(Record::default()), 6);
        assert_eq!(q.last_seq(), 6);
        let (first, b) = q.take_all().unwrap();
        assert_eq!((first, b.len()), (5, 2));
        assert_eq!(q.push(Record::default()), 7);
        q.close();
        assert_eq!(q.take_all().unwrap().0, 7);
        assert!(q.take_all().is_none());
    }
}
