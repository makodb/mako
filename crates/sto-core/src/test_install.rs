//! Test-only observation after individual writes have been installed.
//!
//! This module and every call to it are absent without `test-install-observer`.
//! The callback runs after each datatype's install-time publication decision.
//! Table locks remain held; early-publishing datatypes may already be visible.
//! Tests may park there to inspect or kill a partial install. A callback panic
//! is contained by the normal install boundary and leaves the runtime indeterminate.

use std::cell::Cell;

#[derive(Clone, Copy)]
struct Observer {
    callback: fn(usize),
    installed: usize,
}

thread_local! {
    static OBSERVER: Cell<Option<Observer>> = const { Cell::new(None) };
}

struct Restore(Option<Observer>);

impl Drop for Restore {
    fn drop(&mut self) {
        OBSERVER.with(|observer| observer.set(self.0));
    }
}

/// Observe each completed write install during `run` on this thread.
///
/// The callback receives the number of writes installed since this scope was
/// entered, starting at one. It must not start another transaction. Previous
/// observer state is restored even if `run` unwinds. No observation is emitted
/// for reads, validation failures, or an install callback that panics.
pub fn with_observer<R>(callback: fn(usize), run: impl FnOnce() -> R) -> R {
    let _restore = Restore(OBSERVER.with(|observer| {
        observer.replace(Some(Observer {
            callback,
            installed: 0,
        }))
    }));
    run()
}

pub(crate) fn write_installed() {
    let event = OBSERVER.with(|slot| {
        let mut observer = slot.get()?;
        observer.installed += 1;
        slot.set(Some(observer));
        Some(observer)
    });
    if let Some(observer) = event {
        (observer.callback)(observer.installed);
    }
}
