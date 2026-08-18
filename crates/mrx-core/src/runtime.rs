//! The background threads: one flusher, one sweeper.
//!
//! [`Store`] itself is passive — it exposes `flush_cycle` and
//! `sweep_chunk` and never starts a thread. That split is deliberate: it
//! is what lets every durability property be tested by driving the flusher
//! by hand, one cycle at a time, instead of sleeping and hoping. This
//! module is the thin part that a real deployment adds on top.

use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::thread::JoinHandle;
use std::time::Duration;

use crate::{Blobs, KeyIndex, Store};

/// A running cache: a store plus its background threads.
pub struct Runtime<K: KeyIndex + 'static, B: Blobs + 'static> {
    store: Arc<Store<K, B>>,
    stop: Arc<AtomicBool>,
    threads: Vec<JoinHandle<()>>,
}

impl<K: KeyIndex + 'static, B: Blobs + 'static> Runtime<K, B> {
    /// Start the flusher, and the sweeper if a capacity is configured.
    pub fn start(store: Arc<Store<K, B>>) -> Self {
        let stop = Arc::new(AtomicBool::new(false));
        let mut threads = Vec::new();

        {
            let s = Arc::clone(&store);
            let stop = Arc::clone(&stop);
            threads.push(
                std::thread::Builder::new()
                    .name("mrx-flusher".into())
                    .spawn(move || {
                        while !stop.load(Ordering::Acquire) {
                            if s.flush_cycle() == 0 {
                                std::thread::sleep(Duration::from_micros(200));
                            }
                        }
                    })
                    .expect("spawn flusher"),
            );
        }

        if store.has_capacity() {
            let s = Arc::clone(&store);
            let stop = Arc::clone(&stop);
            threads.push(
                std::thread::Builder::new()
                    .name("mrx-sweeper".into())
                    .spawn(move || {
                        let mut cursor = 0u32;
                        while !stop.load(Ordering::Acquire) {
                            if s.over_capacity() {
                                // A sweep that evicts nothing means every
                                // candidate is ineligible — typically
                                // because the watermark is pinned. Backing
                                // off is the correct response; spinning
                                // burns a core for the duration of an IO
                                // outage.
                                if s.sweep_chunk(&mut cursor) == 0 {
                                    std::thread::sleep(Duration::from_millis(1));
                                }
                            } else {
                                std::thread::sleep(Duration::from_millis(1));
                            }
                        }
                    })
                    .expect("spawn sweeper"),
            );
        }

        Self { store, stop, threads }
    }

    /// The store.
    pub fn store(&self) -> &Arc<Store<K, B>> {
        &self.store
    }

    /// Stop cleanly: make everything acked durable, *then* stop.
    ///
    /// This is the maintenance-exit path, and the ordering is the whole
    /// property: drain first, stop second. Returns `false` if the drain
    /// could not complete — the caller is then losing acked writes by
    /// exiting, and should say so rather than exit silently.
    pub fn shutdown(&mut self) -> bool {
        let drained = self.store.drain_fully();
        self.stop.store(true, Ordering::Release);
        self.store.stop();
        for t in self.threads.drain(..) {
            let _ = t.join();
        }
        drained
    }

    /// Stop *without* draining, as a crash would.
    ///
    /// Only for tests: it is how the "writes past the watermark may be
    /// lost, but nothing may be corrupted" property is exercised without
    /// actually killing a process.
    pub fn abort(&mut self) {
        self.stop.store(true, Ordering::Release);
        self.store.stop();
        for t in self.threads.drain(..) {
            let _ = t.join();
        }
    }
}

impl<K: KeyIndex + 'static, B: Blobs + 'static> Drop for Runtime<K, B> {
    fn drop(&mut self) {
        if !self.threads.is_empty() {
            self.abort();
        }
    }
}
