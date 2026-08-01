module;

#include <rusty/rusty.hpp>
#include <rusty/function.hpp>

export module rrr.callbacks;

import std;
import rusty;
import rrr.errors;
import rrr.threading;

// @safe - Callback registry/dispatch. All operations go through rusty
// primitives (rusty::Mutex / Vec / Arc / Function). No raw pointers,
// syscalls, or operator-overload chains.
//
// `ConnectionCallbacks` (the payload struct behind the rusty::Mutex) and
// `CallbackManager` (the public facade) are authored as inline Rust
// DSL; the transpiler regenerates the matching
// `/*RUSTYCPP:GEN-BEGIN ... END*/` block below.
//
// One thing stays outside the DSL:
//   * The `invoke_callback_safely<...>` free template helper. The DSL
//     doesn't support C++ parameter packs or `try { } catch (…) { }`
//     blocks; the invoke_on_* methods just delegate to this helper for
//     the per-callback try/catch swallow.
export namespace rrr {

using ConnectionCallback = rusty::Arc<rusty::Function<void() const>>;
using ErrorCallback = rusty::Arc<rusty::Function<void(RpcError, const std::string&) const>>;
using ReconnectCallback = rusty::Arc<rusty::Function<void(bool) const>>;

// @safe - try/catch protects the dispatcher from a user callback that
// throws. The catch swallows everything per the original behaviour.
// Outside the DSL because the DSL has no `try/catch` and no parameter
// packs.
template<typename Callback, typename... Args>
inline void invoke_callback_safely(const Callback& cb, Args&&... args) {
    // @unsafe { user-supplied callback bodies aren't borrow-checked }
    try {
        (*cb)(std::forward<Args>(args)...);
    } catch (...) {
    }
}

#if RUSTYCPP_RUST
struct ConnectionCallbacks {
    on_connected: Vec<ConnectionCallback>,
    on_disconnected: Vec<ConnectionCallback>,
    on_error: Vec<ErrorCallback>,
    on_reconnecting: Vec<ConnectionCallback>,
    on_reconnected: Vec<ReconnectCallback>,
}

impl ConnectionCallbacks {
    fn new() -> ConnectionCallbacks {
        ConnectionCallbacks {
            on_connected: Vec::<ConnectionCallback>::new(),
            on_disconnected: Vec::<ConnectionCallback>::new(),
            on_error: Vec::<ErrorCallback>::new(),
            on_reconnecting: Vec::<ConnectionCallback>::new(),
            on_reconnected: Vec::<ReconnectCallback>::new(),
        }
    }

    fn total_count(&self) -> usize {
        self.on_connected.size() + self.on_disconnected.size() +
            self.on_error.size() + self.on_reconnecting.size() +
            self.on_reconnected.size()
    }

    fn clear(&mut self) {
        self.on_connected.clear();
        self.on_disconnected.clear();
        self.on_error.clear();
        self.on_reconnecting.clear();
        self.on_reconnected.clear();
    }
}

struct CallbackManager {
    callbacks_field: rusty::Mutex<ConnectionCallbacks>,
}

impl CallbackManager {
    fn new() -> CallbackManager {
        CallbackManager {
            callbacks_field: rusty::Mutex::<ConnectionCallbacks>::new(ConnectionCallbacks {}),
        }
    }

    // NOTE: field access through a `rusty::MutexGuard<T>` lowers to `.`
    // instead of `->` in the current transpiler (smart-pointer-guard
    // auto-deref handles method calls but not field access). We
    // dereference the guard explicitly with `(*guard).field` so the
    // emitted C++ becomes `(*guard).field.method(...)` — which
    // compiles because `rusty::MutexGuard<T>::operator*()` returns
    // `T&`. Method calls on the guard itself (`guard.unwrap()`,
    // `guard.lock()`, etc.) work unchanged.
    fn add_on_connected(&self, cb: rusty::Function<dyn Fn()>) {
        let arc_cb: ConnectionCallback = ConnectionCallback::make(cb);
        let guard = self.callbacks_field.lock().unwrap();
        (*guard).on_connected.push(arc_cb);
    }

    fn add_on_disconnected(&self, cb: rusty::Function<dyn Fn()>) {
        let arc_cb: ConnectionCallback = ConnectionCallback::make(cb);
        let guard = self.callbacks_field.lock().unwrap();
        (*guard).on_disconnected.push(arc_cb);
    }

    fn add_on_error(&self, cb: rusty::Function<dyn Fn(RpcError, &std::string)>) {
        let arc_cb: ErrorCallback = ErrorCallback::make(cb);
        let guard = self.callbacks_field.lock().unwrap();
        (*guard).on_error.push(arc_cb);
    }

    fn add_on_reconnecting(&self, cb: rusty::Function<dyn Fn()>) {
        let arc_cb: ConnectionCallback = ConnectionCallback::make(cb);
        let guard = self.callbacks_field.lock().unwrap();
        (*guard).on_reconnecting.push(arc_cb);
    }

    fn add_on_reconnected(&self, cb: rusty::Function<dyn Fn(bool)>) {
        let arc_cb: ReconnectCallback = ReconnectCallback::make(cb);
        let guard = self.callbacks_field.lock().unwrap();
        (*guard).on_reconnected.push(arc_cb);
    }

    fn invoke_on_connected(&self) {
        let callbacks_copy: Vec<ConnectionCallback> = {
            let guard = self.callbacks_field.lock().unwrap();
            (*guard).on_connected.clone()
        };
        let n: usize = callbacks_copy.size();
        let mut i: usize = 0usize;
        while i < n {
            invoke_callback_safely(callbacks_copy[i]);
            i += 1usize;
        }
    }

    fn invoke_on_disconnected(&self) {
        let callbacks_copy: Vec<ConnectionCallback> = {
            let guard = self.callbacks_field.lock().unwrap();
            (*guard).on_disconnected.clone()
        };
        let n: usize = callbacks_copy.size();
        let mut i: usize = 0usize;
        while i < n {
            invoke_callback_safely(callbacks_copy[i]);
            i += 1usize;
        }
    }

    fn invoke_on_error(&self, error: RpcError, message: &std::string) {
        let callbacks_copy: Vec<ErrorCallback> = {
            let guard = self.callbacks_field.lock().unwrap();
            (*guard).on_error.clone()
        };
        let n: usize = callbacks_copy.size();
        let mut i: usize = 0usize;
        while i < n {
            invoke_callback_safely(callbacks_copy[i], error, message);
            i += 1usize;
        }
    }

    fn invoke_on_reconnecting(&self) {
        let callbacks_copy: Vec<ConnectionCallback> = {
            let guard = self.callbacks_field.lock().unwrap();
            (*guard).on_reconnecting.clone()
        };
        let n: usize = callbacks_copy.size();
        let mut i: usize = 0usize;
        while i < n {
            invoke_callback_safely(callbacks_copy[i]);
            i += 1usize;
        }
    }

    fn invoke_on_reconnected(&self, success: bool) {
        let callbacks_copy: Vec<ReconnectCallback> = {
            let guard = self.callbacks_field.lock().unwrap();
            (*guard).on_reconnected.clone()
        };
        let n: usize = callbacks_copy.size();
        let mut i: usize = 0usize;
        while i < n {
            invoke_callback_safely(callbacks_copy[i], success);
            i += 1usize;
        }
    }

    fn clear_all(&self) {
        let guard = self.callbacks_field.lock().unwrap();
        guard.clear();
    }

    fn callback_count(&self) -> usize {
        let guard = self.callbacks_field.lock().unwrap();
        guard.total_count()
    }

    fn has_callbacks(&self) -> bool {
        self.callback_count() > 0usize
    }

    fn on_connected_count(&self) -> usize {
        let guard = self.callbacks_field.lock().unwrap();
        (*guard).on_connected.size()
    }

    fn on_disconnected_count(&self) -> usize {
        let guard = self.callbacks_field.lock().unwrap();
        (*guard).on_disconnected.size()
    }

    fn on_error_count(&self) -> usize {
        let guard = self.callbacks_field.lock().unwrap();
        (*guard).on_error.size()
    }

    fn on_reconnecting_count(&self) -> usize {
        let guard = self.callbacks_field.lock().unwrap();
        (*guard).on_reconnecting.size()
    }

    fn on_reconnected_count(&self) -> usize {
        let guard = self.callbacks_field.lock().unwrap();
        (*guard).on_reconnected.size()
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=callbacks.1 version=1 rust_sha256=8199f02f23e7188544e6247ffb4b0475a06c70d3e3b496b641250540750a351f*/
struct ConnectionCallbacks;
struct CallbackManager;

struct ConnectionCallbacks {
    rusty::Vec<ConnectionCallback> on_connected;
    rusty::Vec<ConnectionCallback> on_disconnected;
    rusty::Vec<ErrorCallback> on_error;
    rusty::Vec<ConnectionCallback> on_reconnecting;
    rusty::Vec<ReconnectCallback> on_reconnected;

    static ConnectionCallbacks new_();
    size_t total_count() const;
    void clear();
};

struct CallbackManager {
    rusty::Mutex<ConnectionCallbacks> callbacks_field;

    static CallbackManager new_();
    void add_on_connected(rusty::Function<void() const> cb) const;
    void add_on_disconnected(rusty::Function<void() const> cb) const;
    void add_on_error(rusty::Function<void(RpcError, const std::string&) const> cb) const;
    void add_on_reconnecting(rusty::Function<void() const> cb) const;
    void add_on_reconnected(rusty::Function<void(bool) const> cb) const;
    void invoke_on_connected() const;
    void invoke_on_disconnected() const;
    void invoke_on_error(RpcError error, const std::string& message) const;
    void invoke_on_reconnecting() const;
    void invoke_on_reconnected(bool success) const;
    void clear_all() const;
    size_t callback_count() const;
    bool has_callbacks() const;
    size_t on_connected_count() const;
    size_t on_disconnected_count() const;
    size_t on_error_count() const;
    size_t on_reconnecting_count() const;
    size_t on_reconnected_count() const;
};


ConnectionCallbacks ConnectionCallbacks::new_() {
    return ConnectionCallbacks{.on_connected = rusty::Vec<ConnectionCallback>::new_(), .on_disconnected = rusty::Vec<ConnectionCallback>::new_(), .on_error = rusty::Vec<ErrorCallback>::new_(), .on_reconnecting = rusty::Vec<ConnectionCallback>::new_(), .on_reconnected = rusty::Vec<ReconnectCallback>::new_()};
}

size_t ConnectionCallbacks::total_count() const {
    return (((this->on_connected.size() + this->on_disconnected.size()) + this->on_error.size()) + this->on_reconnecting.size()) + this->on_reconnected.size();
}

void ConnectionCallbacks::clear() {
    this->on_connected.clear();
    this->on_disconnected.clear();
    this->on_error.clear();
    this->on_reconnecting.clear();
    this->on_reconnected.clear();
}

CallbackManager CallbackManager::new_() {
    return CallbackManager{.callbacks_field = rusty::Mutex<ConnectionCallbacks>::new_(ConnectionCallbacks{})};
}

void CallbackManager::add_on_connected(rusty::Function<void() const> cb) const {
    ConnectionCallback arc_cb = ConnectionCallback::make(std::move(cb));
    auto guard = this->callbacks_field.lock().unwrap();
    (*guard).on_connected.push(std::move(arc_cb));
}

void CallbackManager::add_on_disconnected(rusty::Function<void() const> cb) const {
    ConnectionCallback arc_cb = ConnectionCallback::make(std::move(cb));
    auto guard = this->callbacks_field.lock().unwrap();
    (*guard).on_disconnected.push(std::move(arc_cb));
}

void CallbackManager::add_on_error(rusty::Function<void(RpcError, const std::string&) const> cb) const {
    ErrorCallback arc_cb = ErrorCallback::make(std::move(cb));
    auto guard = this->callbacks_field.lock().unwrap();
    (*guard).on_error.push(std::move(arc_cb));
}

void CallbackManager::add_on_reconnecting(rusty::Function<void() const> cb) const {
    ConnectionCallback arc_cb = ConnectionCallback::make(std::move(cb));
    auto guard = this->callbacks_field.lock().unwrap();
    (*guard).on_reconnecting.push(std::move(arc_cb));
}

void CallbackManager::add_on_reconnected(rusty::Function<void(bool) const> cb) const {
    ReconnectCallback arc_cb = ReconnectCallback::make(std::move(cb));
    auto guard = this->callbacks_field.lock().unwrap();
    (*guard).on_reconnected.push(std::move(arc_cb));
}

void CallbackManager::invoke_on_connected() const {
    const rusty::Vec<ConnectionCallback> callbacks_copy = [&]() -> rusty::Vec<ConnectionCallback> { auto guard = this->callbacks_field.lock().unwrap();
return rusty::clone((*guard).on_connected); }();
    const size_t n = callbacks_copy.size();
    size_t i = static_cast<size_t>(0);
    while (rusty::detail::deref_if_pointer_like(i) < rusty::detail::deref_if_pointer_like(n)) {
        invoke_callback_safely(callbacks_copy[i]);
        i += static_cast<size_t>(1);
    }
}

void CallbackManager::invoke_on_disconnected() const {
    const rusty::Vec<ConnectionCallback> callbacks_copy = [&]() -> rusty::Vec<ConnectionCallback> { auto guard = this->callbacks_field.lock().unwrap();
return rusty::clone((*guard).on_disconnected); }();
    const size_t n = callbacks_copy.size();
    size_t i = static_cast<size_t>(0);
    while (rusty::detail::deref_if_pointer_like(i) < rusty::detail::deref_if_pointer_like(n)) {
        invoke_callback_safely(callbacks_copy[i]);
        i += static_cast<size_t>(1);
    }
}

void CallbackManager::invoke_on_error(RpcError error, const std::string& message) const {
    const rusty::Vec<ErrorCallback> callbacks_copy = [&]() -> rusty::Vec<ErrorCallback> { auto guard = this->callbacks_field.lock().unwrap();
return rusty::clone((*guard).on_error); }();
    const size_t n = callbacks_copy.size();
    size_t i = static_cast<size_t>(0);
    while (rusty::detail::deref_if_pointer_like(i) < rusty::detail::deref_if_pointer_like(n)) {
        invoke_callback_safely(callbacks_copy[i], std::move(error), message);
        i += static_cast<size_t>(1);
    }
}

void CallbackManager::invoke_on_reconnecting() const {
    const rusty::Vec<ConnectionCallback> callbacks_copy = [&]() -> rusty::Vec<ConnectionCallback> { auto guard = this->callbacks_field.lock().unwrap();
return rusty::clone((*guard).on_reconnecting); }();
    const size_t n = callbacks_copy.size();
    size_t i = static_cast<size_t>(0);
    while (rusty::detail::deref_if_pointer_like(i) < rusty::detail::deref_if_pointer_like(n)) {
        invoke_callback_safely(callbacks_copy[i]);
        i += static_cast<size_t>(1);
    }
}

void CallbackManager::invoke_on_reconnected(bool success) const {
    const rusty::Vec<ReconnectCallback> callbacks_copy = [&]() -> rusty::Vec<ReconnectCallback> { auto guard = this->callbacks_field.lock().unwrap();
return rusty::clone((*guard).on_reconnected); }();
    const size_t n = callbacks_copy.size();
    size_t i = static_cast<size_t>(0);
    while (rusty::detail::deref_if_pointer_like(i) < rusty::detail::deref_if_pointer_like(n)) {
        invoke_callback_safely(callbacks_copy[i], std::move(success));
        i += static_cast<size_t>(1);
    }
}

void CallbackManager::clear_all() const {
    auto guard = this->callbacks_field.lock().unwrap();
    (*guard).clear();
}

size_t CallbackManager::callback_count() const {
    auto guard = this->callbacks_field.lock().unwrap();
    return (*guard).total_count();
}

bool CallbackManager::has_callbacks() const {
    return this->callback_count() > static_cast<size_t>(0);
}

size_t CallbackManager::on_connected_count() const {
    auto guard = this->callbacks_field.lock().unwrap();
    return (*guard).on_connected.size();
}

size_t CallbackManager::on_disconnected_count() const {
    auto guard = this->callbacks_field.lock().unwrap();
    return (*guard).on_disconnected.size();
}

size_t CallbackManager::on_error_count() const {
    auto guard = this->callbacks_field.lock().unwrap();
    return (*guard).on_error.size();
}

size_t CallbackManager::on_reconnecting_count() const {
    auto guard = this->callbacks_field.lock().unwrap();
    return (*guard).on_reconnecting.size();
}

size_t CallbackManager::on_reconnected_count() const {
    auto guard = this->callbacks_field.lock().unwrap();
    return (*guard).on_reconnected.size();
}
/*RUSTYCPP:GEN-END id=callbacks.1*/

} // export namespace rrr
