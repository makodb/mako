module;

#include <rusty/rusty.hpp>
#include <rusty/function.hpp>

export module rrr.callbacks;

import std;
import rrr.errors;
import rrr.threading;

// @safe - Callback registry/dispatch. All operations go through rusty
// primitives (SpinMutex / Vec / Arc / Function). No raw pointers,
// syscalls, or operator-overload chains.
export namespace rrr {

using ConnectionCallback = rusty::Arc<rusty::Function<void() const>>;
using ErrorCallback = rusty::Arc<rusty::Function<void(RpcError, const std::string&) const>>;
using ReconnectCallback = rusty::Arc<rusty::Function<void(bool) const>>;

struct ConnectionCallbacks {
    rusty::Vec<ConnectionCallback> on_connected;
    rusty::Vec<ConnectionCallback> on_disconnected;
    rusty::Vec<ErrorCallback> on_error;
    rusty::Vec<ConnectionCallback> on_reconnecting;
    rusty::Vec<ReconnectCallback> on_reconnected;

    size_t total_count() const {
        return on_connected.size() + on_disconnected.size() +
               on_error.size() + on_reconnecting.size() +
               on_reconnected.size();
    }

    void clear() {
        on_connected.clear();
        on_disconnected.clear();
        on_error.clear();
        on_reconnecting.clear();
        on_reconnected.clear();
    }
};

class CallbackManager {
private:
    mutable SpinMutex<ConnectionCallbacks> callbacks_;

public:
    CallbackManager() = default;

    void add_on_connected(rusty::Function<void() const> cb) const {
        auto arc_cb = ConnectionCallback::make(std::move(cb));
        auto guard = callbacks_.lock().unwrap();
        guard->on_connected.push(std::move(arc_cb));
    }

    void add_on_disconnected(rusty::Function<void() const> cb) const {
        auto arc_cb = ConnectionCallback::make(std::move(cb));
        auto guard = callbacks_.lock().unwrap();
        guard->on_disconnected.push(std::move(arc_cb));
    }

    void add_on_error(rusty::Function<void(RpcError, const std::string&) const> cb) const {
        auto arc_cb = ErrorCallback::make(std::move(cb));
        auto guard = callbacks_.lock().unwrap();
        guard->on_error.push(std::move(arc_cb));
    }

    void add_on_reconnecting(rusty::Function<void() const> cb) const {
        auto arc_cb = ConnectionCallback::make(std::move(cb));
        auto guard = callbacks_.lock().unwrap();
        guard->on_reconnecting.push(std::move(arc_cb));
    }

    void add_on_reconnected(rusty::Function<void(bool) const> cb) const {
        auto arc_cb = ReconnectCallback::make(std::move(cb));
        auto guard = callbacks_.lock().unwrap();
        guard->on_reconnected.push(std::move(arc_cb));
    }

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

    void clear_all() const {
        auto guard = callbacks_.lock().unwrap();
        guard->clear();
    }

    size_t callback_count() const {
        auto guard = callbacks_.lock().unwrap();
        return guard->total_count();
    }

    bool has_callbacks() const {
        return callback_count() > 0;
    }

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
    template<typename Callback, typename... Args>
    void invoke_safely(const Callback& cb, Args&&... args) const {
        try {
            (*cb)(std::forward<Args>(args)...);
        } catch (...) {
        }
    }
};

} // export namespace rrr
