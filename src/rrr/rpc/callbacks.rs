//! Thread-safe connection lifecycle callback registration and dispatch.
//!
//! The public names, field order, and method signatures intentionally match
//! the legacy `rrr.callbacks` C++ module. Callback lists are cloned under the
//! mutex and invoked after releasing it, and `clear_all` drains dispatches that
//! already took a snapshot before returning.

use std::panic::{catch_unwind, AssertUnwindSafe};
use std::sync::{Arc, Condvar, Mutex};

// This otherwise-unused source-owned import keeps the exact `rrr.errors`
// provider visible to generated C++. The private alias below remains the real
// Rust enum for rustc callers; the checked type map preserves its public C++
// spelling as `::rrr::RpcError`.
#[allow(unused_imports)]
use cpp::rrr::errors as cpp_errors;
use rusty as cpp;

// Native Rust uses `String`. The SRPC consumer profile maps this private alias
// to `std::string`, preserving the legacy callback and method signatures rather
// than exposing rusty-cpp's distinct `rusty::String` type.
type LegacyStdString = String;
type LegacyRpcError = crate::errors::RpcError;

pub type ConnectionCallback = Arc<Box<dyn Fn() + Send + Sync>>;
pub type ErrorCallback = Arc<Box<dyn Fn(LegacyRpcError, &LegacyStdString) + Send + Sync>>;
pub type ReconnectCallback = Arc<Box<dyn Fn(bool) + Send + Sync>>;

fn invoke_connection_callback_safely(callback: &ConnectionCallback) {
    let _ = catch_unwind(AssertUnwindSafe(|| {
        // The explicit double dereference is also significant to the C++
        // consumer: `Arc<Box<dyn Fn>>` flattens to `Arc<Function>`, so this
        // emits exactly one pointer-like dereference before invocation.
        (**callback)();
    }));
}

fn invoke_error_callback_safely(
    callback: &ErrorCallback,
    error: LegacyRpcError,
    message: &LegacyStdString,
) {
    let _ = catch_unwind(AssertUnwindSafe(|| {
        (**callback)(error, message);
    }));
}

fn invoke_reconnect_callback_safely(callback: &ReconnectCallback, success: bool) {
    let _ = catch_unwind(AssertUnwindSafe(|| {
        (**callback)(success);
    }));
}

pub struct ConnectionCallbacks {
    pub on_connected: Vec<ConnectionCallback>,
    pub on_disconnected: Vec<ConnectionCallback>,
    pub on_error: Vec<ErrorCallback>,
    pub on_reconnecting: Vec<ConnectionCallback>,
    pub on_reconnected: Vec<ReconnectCallback>,
}

impl ConnectionCallbacks {
    pub fn new() -> ConnectionCallbacks {
        ConnectionCallbacks {
            on_connected: Vec::new(),
            on_disconnected: Vec::new(),
            on_error: Vec::new(),
            on_reconnecting: Vec::new(),
            on_reconnected: Vec::new(),
        }
    }

    pub fn total_count(&self) -> usize {
        self.on_connected.len()
            + self.on_disconnected.len()
            + self.on_error.len()
            + self.on_reconnecting.len()
            + self.on_reconnected.len()
    }

    pub fn clear(&mut self) {
        self.on_connected.clear();
        self.on_disconnected.clear();
        self.on_error.clear();
        self.on_reconnecting.clear();
        self.on_reconnected.clear();
    }
}

pub struct CallbackManager {
    pub callbacks_field: Mutex<ConnectionCallbacks>,
    pub inflight_field: Mutex<usize>,
    pub inflight_cv_field: Box<Condvar>,
}

impl CallbackManager {
    pub fn new() -> CallbackManager {
        CallbackManager {
            callbacks_field: Mutex::new(ConnectionCallbacks::new()),
            inflight_field: Mutex::new(0),
            inflight_cv_field: Box::new(Condvar::new()),
        }
    }

    pub fn inflight_enter(&self) {
        let mut guard = self.inflight_field.lock().unwrap();
        *guard += 1;
    }

    pub fn inflight_exit(&self) {
        {
            let mut guard = self.inflight_field.lock().unwrap();
            *guard -= 1;
        }
        self.inflight_cv_field.notify_all();
    }

    pub fn add_on_connected(&self, callback: Box<dyn Fn() + Send + Sync>) {
        let callback: ConnectionCallback = Arc::new(callback);
        let mut guard = self.callbacks_field.lock().unwrap();
        guard.on_connected.push(callback);
    }

    pub fn add_on_disconnected(&self, callback: Box<dyn Fn() + Send + Sync>) {
        let callback: ConnectionCallback = Arc::new(callback);
        let mut guard = self.callbacks_field.lock().unwrap();
        guard.on_disconnected.push(callback);
    }

    #[allow(clippy::type_complexity)]
    pub fn add_on_error(
        &self,
        callback: Box<dyn Fn(LegacyRpcError, &LegacyStdString) + Send + Sync>,
    ) {
        let callback: ErrorCallback = Arc::new(callback);
        let mut guard = self.callbacks_field.lock().unwrap();
        guard.on_error.push(callback);
    }

    pub fn add_on_reconnecting(&self, callback: Box<dyn Fn() + Send + Sync>) {
        let callback: ConnectionCallback = Arc::new(callback);
        let mut guard = self.callbacks_field.lock().unwrap();
        guard.on_reconnecting.push(callback);
    }

    pub fn add_on_reconnected(&self, callback: Box<dyn Fn(bool) + Send + Sync>) {
        let callback: ReconnectCallback = Arc::new(callback);
        let mut guard = self.callbacks_field.lock().unwrap();
        guard.on_reconnected.push(callback);
    }

    pub fn invoke_on_connected(&self) {
        self.inflight_enter();
        let callbacks = {
            let guard = self.callbacks_field.lock().unwrap();
            guard.on_connected.clone()
        };
        let count = callbacks.len();
        let mut index = 0;
        while index < count {
            invoke_connection_callback_safely(&callbacks[index]);
            index += 1;
        }
        self.inflight_exit();
    }

    pub fn invoke_on_disconnected(&self) {
        self.inflight_enter();
        let callbacks = {
            let guard = self.callbacks_field.lock().unwrap();
            guard.on_disconnected.clone()
        };
        let count = callbacks.len();
        let mut index = 0;
        while index < count {
            invoke_connection_callback_safely(&callbacks[index]);
            index += 1;
        }
        self.inflight_exit();
    }

    pub fn invoke_on_error(&self, error: LegacyRpcError, message: &LegacyStdString) {
        self.inflight_enter();
        let callbacks = {
            let guard = self.callbacks_field.lock().unwrap();
            guard.on_error.clone()
        };
        let count = callbacks.len();
        let mut index = 0;
        while index < count {
            invoke_error_callback_safely(&callbacks[index], error, message);
            index += 1;
        }
        self.inflight_exit();
    }

    pub fn invoke_on_reconnecting(&self) {
        self.inflight_enter();
        let callbacks = {
            let guard = self.callbacks_field.lock().unwrap();
            guard.on_reconnecting.clone()
        };
        let count = callbacks.len();
        let mut index = 0;
        while index < count {
            invoke_connection_callback_safely(&callbacks[index]);
            index += 1;
        }
        self.inflight_exit();
    }

    pub fn invoke_on_reconnected(&self, success: bool) {
        self.inflight_enter();
        let callbacks = {
            let guard = self.callbacks_field.lock().unwrap();
            guard.on_reconnected.clone()
        };
        let count = callbacks.len();
        let mut index = 0;
        while index < count {
            invoke_reconnect_callback_safely(&callbacks[index], success);
            index += 1;
        }
        self.inflight_exit();
    }

    /// Remove every registered callback and wait for dispatches that already
    /// took a callback snapshot. Must not be called from inside a callback.
    pub fn clear_all(&self) {
        {
            let mut guard = self.callbacks_field.lock().unwrap();
            guard.clear();
        }
        let guard = self.inflight_field.lock().unwrap();
        let _guard = self
            .inflight_cv_field
            .wait_while(guard, |inflight| *inflight != 0)
            .unwrap();
    }

    pub fn callback_count(&self) -> usize {
        let guard = self.callbacks_field.lock().unwrap();
        guard.total_count()
    }

    pub fn has_callbacks(&self) -> bool {
        self.callback_count() > 0
    }

    pub fn on_connected_count(&self) -> usize {
        let guard = self.callbacks_field.lock().unwrap();
        guard.on_connected.len()
    }

    pub fn on_disconnected_count(&self) -> usize {
        let guard = self.callbacks_field.lock().unwrap();
        guard.on_disconnected.len()
    }

    pub fn on_error_count(&self) -> usize {
        let guard = self.callbacks_field.lock().unwrap();
        guard.on_error.len()
    }

    pub fn on_reconnecting_count(&self) -> usize {
        let guard = self.callbacks_field.lock().unwrap();
        guard.on_reconnecting.len()
    }

    pub fn on_reconnected_count(&self) -> usize {
        let guard = self.callbacks_field.lock().unwrap();
        guard.on_reconnected.len()
    }
}
