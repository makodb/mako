#pragma once

#include <functional>
#include <string>
#include <vector>
#include <mutex>
#include "errors.hpp"

namespace rrr {

/**
 * Callback function types for connection events.
 */
using ConnectionCallback = std::function<void()>;
using ErrorCallback = std::function<void(RpcError, const std::string&)>;
using ReconnectCallback = std::function<void(bool)>;

/**
 * Container for all connection callbacks.
 */
struct ConnectionCallbacks {
    std::vector<ConnectionCallback> on_connected;
    std::vector<ConnectionCallback> on_disconnected;
    std::vector<ErrorCallback> on_error;
    std::vector<ConnectionCallback> on_reconnecting;
    std::vector<ReconnectCallback> on_reconnected;

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
    // @unsafe { std::mutex for thread-safe concurrent access }
    mutable std::mutex mutex_;
    ConnectionCallbacks callbacks_;

public:
    // @safe - Default constructor
    CallbackManager() = default;

    // === Registration Methods ===

    // @safe - Add callback for connection established
    void add_on_connected(ConnectionCallback cb) {
        // @unsafe { std::mutex lock }
        std::lock_guard<std::mutex> lock(mutex_);
        callbacks_.on_connected.push_back(std::move(cb));
    }

    // @safe - Add callback for connection closed/lost
    void add_on_disconnected(ConnectionCallback cb) {
        // @unsafe { std::mutex lock }
        std::lock_guard<std::mutex> lock(mutex_);
        callbacks_.on_disconnected.push_back(std::move(cb));
    }

    // @safe - Add callback for errors
    void add_on_error(ErrorCallback cb) {
        // @unsafe { std::mutex lock }
        std::lock_guard<std::mutex> lock(mutex_);
        callbacks_.on_error.push_back(std::move(cb));
    }

    // @safe - Add callback for reconnection started
    void add_on_reconnecting(ConnectionCallback cb) {
        // @unsafe { std::mutex lock }
        std::lock_guard<std::mutex> lock(mutex_);
        callbacks_.on_reconnecting.push_back(std::move(cb));
    }

    // @safe - Add callback for reconnection completed
    void add_on_reconnected(ReconnectCallback cb) {
        // @unsafe { std::mutex lock }
        std::lock_guard<std::mutex> lock(mutex_);
        callbacks_.on_reconnected.push_back(std::move(cb));
    }

    // === Invocation Methods ===

    // @safe - Invoke all on_connected callbacks
    void invoke_on_connected() {
        std::vector<ConnectionCallback> callbacks_copy;
        {
            // @unsafe { std::mutex lock }
            std::lock_guard<std::mutex> lock(mutex_);
            callbacks_copy = callbacks_.on_connected;
        }
        for (const auto& cb : callbacks_copy) {
            invoke_safely(cb);
        }
    }

    // @safe - Invoke all on_disconnected callbacks
    void invoke_on_disconnected() {
        std::vector<ConnectionCallback> callbacks_copy;
        {
            // @unsafe { std::mutex lock }
            std::lock_guard<std::mutex> lock(mutex_);
            callbacks_copy = callbacks_.on_disconnected;
        }
        for (const auto& cb : callbacks_copy) {
            invoke_safely(cb);
        }
    }

    // @safe - Invoke all on_error callbacks
    void invoke_on_error(RpcError error, const std::string& message = "") {
        std::vector<ErrorCallback> callbacks_copy;
        {
            // @unsafe { std::mutex lock }
            std::lock_guard<std::mutex> lock(mutex_);
            callbacks_copy = callbacks_.on_error;
        }
        for (const auto& cb : callbacks_copy) {
            invoke_safely(cb, error, message);
        }
    }

    // @safe - Invoke all on_reconnecting callbacks
    void invoke_on_reconnecting() {
        std::vector<ConnectionCallback> callbacks_copy;
        {
            // @unsafe { std::mutex lock }
            std::lock_guard<std::mutex> lock(mutex_);
            callbacks_copy = callbacks_.on_reconnecting;
        }
        for (const auto& cb : callbacks_copy) {
            invoke_safely(cb);
        }
    }

    // @safe - Invoke all on_reconnected callbacks
    void invoke_on_reconnected(bool success) {
        std::vector<ReconnectCallback> callbacks_copy;
        {
            // @unsafe { std::mutex lock }
            std::lock_guard<std::mutex> lock(mutex_);
            callbacks_copy = callbacks_.on_reconnected;
        }
        for (const auto& cb : callbacks_copy) {
            invoke_safely(cb, success);
        }
    }

    // === Utility Methods ===

    // @safe - Clear all registered callbacks
    void clear_all() {
        // @unsafe { std::mutex lock }
        std::lock_guard<std::mutex> lock(mutex_);
        callbacks_.clear();
    }

    // @safe - Get total number of registered callbacks
    size_t callback_count() const {
        // @unsafe { std::mutex lock }
        std::lock_guard<std::mutex> lock(mutex_);
        return callbacks_.total_count();
    }

    // @safe - Check if any callbacks are registered
    bool has_callbacks() const {
        return callback_count() > 0;
    }

    // @safe - Get count for specific event type
    size_t on_connected_count() const {
        // @unsafe { std::mutex lock }
        std::lock_guard<std::mutex> lock(mutex_);
        return callbacks_.on_connected.size();
    }

    size_t on_disconnected_count() const {
        // @unsafe { std::mutex lock }
        std::lock_guard<std::mutex> lock(mutex_);
        return callbacks_.on_disconnected.size();
    }

    size_t on_error_count() const {
        // @unsafe { std::mutex lock }
        std::lock_guard<std::mutex> lock(mutex_);
        return callbacks_.on_error.size();
    }

    size_t on_reconnecting_count() const {
        // @unsafe { std::mutex lock }
        std::lock_guard<std::mutex> lock(mutex_);
        return callbacks_.on_reconnecting.size();
    }

    size_t on_reconnected_count() const {
        // @unsafe { std::mutex lock }
        std::lock_guard<std::mutex> lock(mutex_);
        return callbacks_.on_reconnected.size();
    }

private:
    // @safe - Invoke callback with exception safety
    template<typename Callback, typename... Args>
    void invoke_safely(const Callback& cb, Args&&... args) {
        // @unsafe { exception handling is not borrow-checked }
        try {
            cb(std::forward<Args>(args)...);
        } catch (...) {
            // Silently ignore exceptions from callbacks
            // In production, this could log the error
        }
    }
};

} // namespace rrr
