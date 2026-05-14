#pragma once

// import std; replacement — see <std_compat.hpp> for rationale.
#include <std_compat.hpp>

// @c-compat-added
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <rusty/rusty.hpp>
#include <rusty/function.hpp>

#include "../base/threading.hpp"  // SpinMutex<T>
#include "errors.hpp"

namespace rrr {

/**
 * Callback function types for connection events.
 *
 * Stored as Arc<Function<...const>> so the manager can clone-out under the
 * lock and invoke without holding it (callbacks are user code that may take
 * arbitrary time and may itself register more callbacks). Arc clone is a
 * cheap atomic refcount bump; the Function inside is move-only and shared.
 *
 * The "const" qualifier on Function lets us call through `const Arc&`
 * without losing const-correctness — non-mutable lambdas (the common case)
 * satisfy the const-callable requirement.
 */
using ConnectionCallback = rusty::Arc<rusty::Function<void() const>>;
using ErrorCallback = rusty::Arc<rusty::Function<void(RpcError, const std::string&) const>>;
using ReconnectCallback = rusty::Arc<rusty::Function<void(bool) const>>;

/**
 * Container for all connection callbacks.
 */
struct ConnectionCallbacks {
    rusty::Vec<ConnectionCallback> on_connected;
    rusty::Vec<ConnectionCallback> on_disconnected;
    rusty::Vec<ErrorCallback> on_error;
    rusty::Vec<ConnectionCallback> on_reconnecting;
    rusty::Vec<ReconnectCallback> on_reconnected;

    // @safe - Get total callback count
    size_t total_count() const {
        return on_connected.size() + on_disconnected.size() +
               on_error.size() + on_reconnecting.size() +
               on_reconnected.size();
    }

    // @safe - Clear all callbacks
    void clear() {
        on_connected.clear();
        on_disconnected.clear();
        on_error.clear();
        on_reconnecting.clear();
        on_reconnected.clear();
    }
};

/**
 * Manager for connection lifecycle callbacks.
 *
 * Provides thread-safe registration and invocation of callbacks
 * for connection events. Callbacks are invoked synchronously but
 * exceptions are caught to prevent propagation.
 *
 * Usage:
 *   CallbackManager mgr;
 *   mgr.add_on_connected([]() { std::cout << "Connected!\n"; });
 *   mgr.add_on_error([](RpcError e, const std::string& msg) {
 *       std::cerr << "Error: " << msg << "\n";
 *   });
 *
 *   // Called by connection when state changes
 *   mgr.invoke_on_connected();
 *   mgr.invoke_on_error(RpcError::CONNECTION_RESET, "Connection lost");
 */
class CallbackManager {
private:
    // SpinMutex<T> owns its T (data-inside-the-mutex pattern).
    mutable SpinMutex<ConnectionCallbacks> callbacks_;

public:
    // @safe - Default constructor
    CallbackManager() = default;

    // === Registration Methods ===
    //
    // Each `add_*` takes the user callable by value as a `rusty::Function<...const>`
    // (auto-converts from a plain non-mutable lambda) and wraps it in an Arc
    // before pushing into the per-event Vec. Storing Arc<Function const> lets
    // `invoke_*` clone the per-event Vec under lock and invoke without it.

    // @safe - Add callback for connection established
    void add_on_connected(rusty::Function<void() const> cb) const {
        auto arc_cb = ConnectionCallback::make(std::move(cb));
        auto guard = callbacks_.lock().unwrap();
        guard->on_connected.push(std::move(arc_cb));
    }

    // @safe - Add callback for connection closed/lost
    void add_on_disconnected(rusty::Function<void() const> cb) const {
        auto arc_cb = ConnectionCallback::make(std::move(cb));
        auto guard = callbacks_.lock().unwrap();
        guard->on_disconnected.push(std::move(arc_cb));
    }

    // @safe - Add callback for errors
    void add_on_error(rusty::Function<void(RpcError, const std::string&) const> cb) const {
        auto arc_cb = ErrorCallback::make(std::move(cb));
        auto guard = callbacks_.lock().unwrap();
        guard->on_error.push(std::move(arc_cb));
    }

    // @safe - Add callback for reconnection started
    void add_on_reconnecting(rusty::Function<void() const> cb) const {
        auto arc_cb = ConnectionCallback::make(std::move(cb));
        auto guard = callbacks_.lock().unwrap();
        guard->on_reconnecting.push(std::move(arc_cb));
    }

    // @safe - Add callback for reconnection completed
    void add_on_reconnected(rusty::Function<void(bool) const> cb) const {
        auto arc_cb = ReconnectCallback::make(std::move(cb));
        auto guard = callbacks_.lock().unwrap();
        guard->on_reconnected.push(std::move(arc_cb));
    }

    // === Invocation Methods ===

    // @safe - Invoke all on_connected callbacks
    void invoke_on_connected() const {
        rusty::Vec<ConnectionCallback> callbacks_copy;
        {
            auto guard = callbacks_.lock().unwrap();
            callbacks_copy = guard->on_connected.clone();
        }
        for (const auto& cb : callbacks_copy) {
            invoke_safely(cb);
        }
    }

    // @safe - Invoke all on_disconnected callbacks
    void invoke_on_disconnected() const {
        rusty::Vec<ConnectionCallback> callbacks_copy;
        {
            auto guard = callbacks_.lock().unwrap();
            callbacks_copy = guard->on_disconnected.clone();
        }
        for (const auto& cb : callbacks_copy) {
            invoke_safely(cb);
        }
    }

    // @safe - Invoke all on_error callbacks
    void invoke_on_error(RpcError error, const std::string& message = "") const {
        rusty::Vec<ErrorCallback> callbacks_copy;
        {
            auto guard = callbacks_.lock().unwrap();
            callbacks_copy = guard->on_error.clone();
        }
        for (const auto& cb : callbacks_copy) {
            invoke_safely(cb, error, message);
        }
    }

    // @safe - Invoke all on_reconnecting callbacks
    void invoke_on_reconnecting() const {
        rusty::Vec<ConnectionCallback> callbacks_copy;
        {
            auto guard = callbacks_.lock().unwrap();
            callbacks_copy = guard->on_reconnecting.clone();
        }
        for (const auto& cb : callbacks_copy) {
            invoke_safely(cb);
        }
    }

    // @safe - Invoke all on_reconnected callbacks
    void invoke_on_reconnected(bool success) const {
        rusty::Vec<ReconnectCallback> callbacks_copy;
        {
            auto guard = callbacks_.lock().unwrap();
            callbacks_copy = guard->on_reconnected.clone();
        }
        for (const auto& cb : callbacks_copy) {
            invoke_safely(cb, success);
        }
    }

    // === Utility Methods ===

    // @safe - Clear all registered callbacks
    void clear_all() const {
        auto guard = callbacks_.lock().unwrap();
        guard->clear();
    }

    // @safe - Get total number of registered callbacks
    size_t callback_count() const {
        auto guard = callbacks_.lock().unwrap();
        return guard->total_count();
    }

    // @safe - Check if any callbacks are registered
    bool has_callbacks() const {
        return callback_count() > 0;
    }

    // @safe - Get count for specific event type
    size_t on_connected_count() const {
        auto guard = callbacks_.lock().unwrap();
        return guard->on_connected.size();
    }

    size_t on_disconnected_count() const {
        auto guard = callbacks_.lock().unwrap();
        return guard->on_disconnected.size();
    }

    size_t on_error_count() const {
        auto guard = callbacks_.lock().unwrap();
        return guard->on_error.size();
    }

    size_t on_reconnecting_count() const {
        auto guard = callbacks_.lock().unwrap();
        return guard->on_reconnecting.size();
    }

    size_t on_reconnected_count() const {
        auto guard = callbacks_.lock().unwrap();
        return guard->on_reconnected.size();
    }

private:
    // @safe - Invoke an Arc<Function const> with exception safety.
    // The first dereference goes Arc -> Function const&; the call operator
    // is the const variant of rusty::Function::operator().
    template<typename Callback, typename... Args>
    void invoke_safely(const Callback& cb, Args&&... args) const {
        // @unsafe { exception handling is not borrow-checked }
        try {
            (*cb)(std::forward<Args>(args)...);
        } catch (...) {
            // Silently ignore exceptions from callbacks
            // In production, this could log the error
        }
    }
};

} // namespace rrr
