#pragma once

/**
 * In-Memory Log Storage Implementation
 *
 * Thread-safe in-memory implementation of LogStorage for testing
 * and simple use cases. Uses rusty::Mutex for thread safety.
 *
 * RustyCpp Compliance: Uses rusty::Mutex, rusty::Cell, rusty::Option
 */

#include <map>
#include <string>

#include <rusty/mutex.hpp>
#include <rusty/cell.hpp>
#include <rusty/option.hpp>

#include "log_storage.hpp"

namespace rrr {

/**
 * In-memory implementation of LogStorage.
 *
 * Suitable for:
 * - Unit testing
 * - Development/debugging
 * - Non-persistent deployments
 *
 * Thread-safe: All operations are protected by rusty::Mutex.
 */
class InMemoryLogStorage : public LogStorage {
private:
    // @safe - Thread-safe log storage (initialized with empty map)
    mutable rusty::Mutex<std::map<slotid_t, LogEntry>> logs_{std::map<slotid_t, LogEntry>{}};

    // @safe - Thread-safe metadata storage (initialized with empty map)
    mutable rusty::Mutex<std::map<std::string, std::string>> metadata_{std::map<std::string, std::string>{}};

    // @safe - Open state tracking
    rusty::Cell<bool> is_open_{true};

public:
    // @safe - Default constructor
    InMemoryLogStorage() {}

    // @safe - Destructor
    ~InMemoryLogStorage() override {
        close();
    }

    // ========================================================================
    // Single Entry Operations
    // ========================================================================

    // @safe - Thread-safe get
    rusty::Option<LogEntry> get(slotid_t slot_id) const override {
        if (!is_open_.get()) {
            return rusty::None;
        }
        auto guard = logs_.lock().unwrap();
        auto it = guard->find(slot_id);
        if (it == guard->end()) {
            return rusty::None;
        }
        return rusty::Some(it->second);
    }

    // @safe - Thread-safe put
    bool put(const LogEntry& entry) override {
        if (!is_open_.get()) {
            return false;
        }
        auto guard = logs_.lock().unwrap();
        (*guard)[entry.slot_id] = entry;
        return true;
    }

    // @safe - Thread-safe remove
    bool remove(slotid_t slot_id) override {
        if (!is_open_.get()) {
            return false;
        }
        auto guard = logs_.lock().unwrap();
        return guard->erase(slot_id) > 0;
    }

    // ========================================================================
    // Batch Operations
    // ========================================================================

    // @safe - Thread-safe range get
    std::vector<LogEntry> get_range(slotid_t start, slotid_t end) const override {
        std::vector<LogEntry> result;
        if (!is_open_.get() || start >= end) {
            return result;
        }

        auto guard = logs_.lock().unwrap();
        auto it_start = guard->lower_bound(start);
        auto it_end = guard->lower_bound(end);

        for (auto it = it_start; it != it_end; ++it) {
            result.push_back(it->second);
        }
        return result;
    }

    // @safe - Thread-safe batch put
    bool put_batch(const std::vector<LogEntry>& entries) override {
        if (!is_open_.get()) {
            return false;
        }
        auto guard = logs_.lock().unwrap();
        for (const auto& entry : entries) {
            (*guard)[entry.slot_id] = entry;
        }
        return true;
    }

    // @safe - Thread-safe range remove
    bool remove_range(slotid_t start, slotid_t end) override {
        if (!is_open_.get() || start >= end) {
            return false;
        }

        auto guard = logs_.lock().unwrap();
        auto it = guard->lower_bound(start);
        while (it != guard->end() && it->first < end) {
            it = guard->erase(it);
        }
        return true;
    }

    // ========================================================================
    // Index Queries
    // ========================================================================

    // @safe - Thread-safe first index query
    slotid_t get_first_index() const override {
        if (!is_open_.get()) {
            return 0;
        }
        auto guard = logs_.lock().unwrap();
        if (guard->empty()) {
            return 0;
        }
        return guard->begin()->first;
    }

    // @safe - Thread-safe last index query
    slotid_t get_last_index() const override {
        if (!is_open_.get()) {
            return 0;
        }
        auto guard = logs_.lock().unwrap();
        if (guard->empty()) {
            return 0;
        }
        return guard->rbegin()->first;
    }

    // @safe - Thread-safe term query
    rusty::Option<ballot_t> get_term(slotid_t slot_id) const override {
        auto entry_opt = get(slot_id);
        if (entry_opt.is_none()) {
            return rusty::None;
        }
        return rusty::Some(entry_opt.unwrap().term);
    }

    // @safe - Thread-safe size query
    size_t size() const override {
        if (!is_open_.get()) {
            return 0;
        }
        auto guard = logs_.lock().unwrap();
        return guard->size();
    }

    // @safe - Thread-safe empty check
    bool empty() const override {
        return size() == 0;
    }

    // ========================================================================
    // Metadata Operations
    // ========================================================================

    // @safe - Thread-safe metadata set
    bool set_metadata(const std::string& key, const std::string& value) override {
        if (!is_open_.get()) {
            return false;
        }
        auto guard = metadata_.lock().unwrap();
        (*guard)[key] = value;
        return true;
    }

    // @safe - Thread-safe metadata get
    rusty::Option<std::string> get_metadata(const std::string& key) const override {
        if (!is_open_.get()) {
            return rusty::None;
        }
        auto guard = metadata_.lock().unwrap();
        auto it = guard->find(key);
        if (it == guard->end()) {
            return rusty::None;
        }
        return rusty::Some(it->second);
    }

    // ========================================================================
    // Lifecycle Operations
    // ========================================================================

    // @safe - No-op for in-memory storage
    bool sync() override {
        return is_open_.get();
    }

    // @safe - Close storage
    bool close() override {
        if (!is_open_.get()) {
            return false;
        }
        is_open_.set(false);
        // Clear data on close
        {
            auto guard = logs_.lock().unwrap();
            guard->clear();
        }
        {
            auto guard = metadata_.lock().unwrap();
            guard->clear();
        }
        return true;
    }

    // @safe - Check if open
    bool is_open() const override {
        return is_open_.get();
    }

    // @safe - Clear all data
    bool clear() override {
        if (!is_open_.get()) {
            return false;
        }
        {
            auto guard = logs_.lock().unwrap();
            guard->clear();
        }
        {
            auto guard = metadata_.lock().unwrap();
            guard->clear();
        }
        return true;
    }

    // ========================================================================
    // Additional Utility Methods (not in interface)
    // ========================================================================

    /**
     * Reopen storage after close (for testing).
     */
    // @safe - Simple state change
    void reopen() {
        is_open_.set(true);
    }

    /**
     * Get all entries as a vector (for testing/debugging).
     */
    // @safe - Thread-safe copy
    std::vector<LogEntry> get_all() const {
        std::vector<LogEntry> result;
        if (!is_open_.get()) {
            return result;
        }
        auto guard = logs_.lock().unwrap();
        result.reserve(guard->size());
        for (const auto& pair : *guard) {
            result.push_back(pair.second);
        }
        return result;
    }
};

}  // namespace rrr
