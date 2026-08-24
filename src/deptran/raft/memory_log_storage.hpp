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
#include <utility>

#include <rusty/mutex.hpp>
#include <rusty/cell.hpp>
#include <rusty/option.hpp>
#include <rusty/slice.hpp>

#include "log_storage.hpp"

namespace janus {
namespace raft {

#if RUSTYCPP_RUST
pub const fn memory_log_storage_is_usable(is_open: bool) -> bool {
    is_open
}

pub const fn memory_log_storage_range_valid(start: u64, end: u64) -> bool {
    start < end
}

pub const fn memory_log_storage_empty_from_size(size: usize) -> bool {
    size == 0
}

pub const fn memory_log_storage_index_or_zero(has_entry: bool,
                                              index: u64) -> u64 {
    if has_entry {
        index
    } else {
        0
    }
}

pub const fn memory_log_storage_metadata_found(found: bool) -> bool {
    found
}
#endif
/*RUSTYCPP:GEN-BEGIN id=raft_memory_log.scalar_decisions version=1 rust_sha256=809b036a24d4aa09b39dd1e1bfef805891450ec698190c4b69a8b2e31b75e477*/
constexpr bool memory_log_storage_is_usable(bool is_open);
constexpr bool memory_log_storage_range_valid(uint64_t start, uint64_t end);
constexpr bool memory_log_storage_empty_from_size(size_t size);
constexpr uint64_t memory_log_storage_index_or_zero(bool has_entry, uint64_t index);
constexpr bool memory_log_storage_metadata_found(bool found);
constexpr bool memory_log_storage_is_usable(bool is_open) {
    return std::move(is_open);
}
constexpr bool memory_log_storage_range_valid(uint64_t start, uint64_t end) {
    return rusty::detail::deref_if_pointer_like(start) < rusty::detail::deref_if_pointer_like(end);
}
constexpr bool memory_log_storage_empty_from_size(size_t size) {
    return rusty::detail::deref_if_pointer_like(size) == static_cast<size_t>(0);
}
constexpr uint64_t memory_log_storage_index_or_zero(bool has_entry, uint64_t index) {
    if (has_entry) {
        return std::move(index);
    } else {
        return static_cast<uint64_t>(0);
    }
}
constexpr bool memory_log_storage_metadata_found(bool found) {
    return std::move(found);
}
/*RUSTYCPP:GEN-END id=raft_memory_log.scalar_decisions*/

static_assert(memory_log_storage_is_usable(true));
static_assert(!memory_log_storage_is_usable(false));
static_assert(memory_log_storage_range_valid(1, 2));
static_assert(!memory_log_storage_range_valid(2, 2));
static_assert(memory_log_storage_empty_from_size(0));
static_assert(!memory_log_storage_empty_from_size(1));
static_assert(memory_log_storage_index_or_zero(false, 7) == 0);
static_assert(memory_log_storage_index_or_zero(true, 7) == 7);
static_assert(memory_log_storage_metadata_found(true));

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
        if (!memory_log_storage_is_usable(is_open_.get())) {
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
        if (!memory_log_storage_is_usable(is_open_.get())) {
            return false;
        }
        auto guard = logs_.lock().unwrap();
        (*guard)[entry.slot_id] = entry;
        return true;
    }

    // @safe - Thread-safe remove
    bool remove(slotid_t slot_id) override {
        if (!memory_log_storage_is_usable(is_open_.get())) {
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
        if (!memory_log_storage_is_usable(is_open_.get()) ||
            !memory_log_storage_range_valid(start, end)) {
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
        if (!memory_log_storage_is_usable(is_open_.get())) {
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
        if (!memory_log_storage_is_usable(is_open_.get()) ||
            !memory_log_storage_range_valid(start, end)) {
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
        if (!memory_log_storage_is_usable(is_open_.get())) {
            return 0;
        }
        auto guard = logs_.lock().unwrap();
        const bool has_entry = !guard->empty();
        return memory_log_storage_index_or_zero(
            has_entry, has_entry ? guard->begin()->first : 0);
    }

    // @safe - Thread-safe last index query
    slotid_t get_last_index() const override {
        if (!memory_log_storage_is_usable(is_open_.get())) {
            return 0;
        }
        auto guard = logs_.lock().unwrap();
        const bool has_entry = !guard->empty();
        return memory_log_storage_index_or_zero(
            has_entry, has_entry ? guard->rbegin()->first : 0);
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
        if (!memory_log_storage_is_usable(is_open_.get())) {
            return 0;
        }
        auto guard = logs_.lock().unwrap();
        return guard->size();
    }

    // @safe - Thread-safe empty check
    bool empty() const override {
        return memory_log_storage_empty_from_size(size());
    }

    // ========================================================================
    // Metadata Operations
    // ========================================================================

    // @safe - Thread-safe metadata set
    bool set_metadata(const std::string& key, const std::string& value) override {
        if (!memory_log_storage_is_usable(is_open_.get())) {
            return false;
        }
        auto guard = metadata_.lock().unwrap();
        (*guard)[key] = value;
        return true;
    }

    // @safe - Thread-safe metadata get
    rusty::Option<std::string> get_metadata(const std::string& key) const override {
        if (!memory_log_storage_is_usable(is_open_.get())) {
            return rusty::None;
        }
        auto guard = metadata_.lock().unwrap();
        auto it = guard->find(key);
        if (!memory_log_storage_metadata_found(it != guard->end())) {
            return rusty::None;
        }
        return rusty::Some(it->second);
    }

    // ========================================================================
    // Lifecycle Operations
    // ========================================================================

    // @safe - No-op for in-memory storage
    bool sync() override {
        return memory_log_storage_is_usable(is_open_.get());
    }

    // @safe - Close storage
    bool close() override {
        if (!memory_log_storage_is_usable(is_open_.get())) {
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
        return memory_log_storage_is_usable(is_open_.get());
    }

    // @safe - Clear all data
    bool clear() override {
        if (!memory_log_storage_is_usable(is_open_.get())) {
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
        if (!memory_log_storage_is_usable(is_open_.get())) {
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

}  // namespace raft
}  // namespace janus
