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

namespace janus {
namespace raft {

// @safe - storage-state predicates over copied counters/flags. The map,
// mutex, and log-entry ownership remain in InMemoryLogStorage methods.
#if RUSTYCPP_RUST
pub fn memory_log_storage_is_usable(is_open: bool) -> bool {
    is_open
}

pub fn memory_log_storage_range_valid(start: u64, end: u64) -> bool {
    start < end
}

pub fn memory_log_storage_empty_from_size(size: usize) -> bool {
    size == 0
}

pub fn memory_log_storage_index_or_zero(has_entry: bool, index: u64) -> u64 {
    if has_entry {
        index
    } else {
        0
    }
}

pub fn memory_log_storage_metadata_found(found: bool) -> bool {
    found
}
#endif
/*RUSTYCPP:GEN-BEGIN id=memory_log_storage.helpers version=1 rust_sha256=d98f0d679c6e6740c2d5a52d66f279042e8e5d85fc7a7bdb86086cbec18d6f5d*/
inline bool memory_log_storage_is_usable(bool is_open);
inline bool memory_log_storage_range_valid(uint64_t start, uint64_t end);
inline bool memory_log_storage_empty_from_size(size_t size);
inline uint64_t memory_log_storage_index_or_zero(bool has_entry, uint64_t index);
inline bool memory_log_storage_metadata_found(bool found);

inline bool memory_log_storage_is_usable(bool is_open) {
    return is_open;
}

inline bool memory_log_storage_range_valid(uint64_t start, uint64_t end) {
    return start < end;
}

inline bool memory_log_storage_empty_from_size(size_t size) {
    return size == 0;
}

inline uint64_t memory_log_storage_index_or_zero(bool has_entry, uint64_t index) {
    if (has_entry) {
        return std::move(index);
    } else {
        return static_cast<uint64_t>(0);
    }
}

inline bool memory_log_storage_metadata_found(bool found) {
    return found;
}
/*RUSTYCPP:GEN-END id=memory_log_storage.helpers*/

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
// @safe - Thread-safe get from the log map.
inline rusty::Option<LogEntry> memory_log_storage_get_cpp(
    const rusty::Mutex<std::map<slotid_t, LogEntry>>* logs,
    bool is_open,
    slotid_t slot_id) {
    if (!memory_log_storage_is_usable(is_open)) {
        return rusty::None;
    }
    auto guard = logs->lock().unwrap();
    auto it = guard->find(slot_id);
    if (it == guard->end()) {
        return rusty::None;
    }
    return rusty::Some(it->second);
}

// @safe - Thread-safe put into the log map.
inline bool memory_log_storage_put_cpp(
    rusty::Mutex<std::map<slotid_t, LogEntry>>* logs,
    bool is_open,
    const LogEntry& entry) {
    if (!memory_log_storage_is_usable(is_open)) {
        return false;
    }
    auto guard = logs->lock().unwrap();
    (*guard)[entry.slot_id] = entry;
    return true;
}

// @safe - Thread-safe remove from the log map.
inline bool memory_log_storage_remove_cpp(
    rusty::Mutex<std::map<slotid_t, LogEntry>>* logs,
    bool is_open,
    slotid_t slot_id) {
    if (!memory_log_storage_is_usable(is_open)) {
        return false;
    }
    auto guard = logs->lock().unwrap();
    return guard->erase(slot_id) > 0;
}

// @safe - Thread-safe range get from the log map.
inline std::vector<LogEntry> memory_log_storage_get_range_cpp(
    const rusty::Mutex<std::map<slotid_t, LogEntry>>* logs,
    bool is_open,
    slotid_t start,
    slotid_t end) {
    std::vector<LogEntry> result;
    if (!memory_log_storage_is_usable(is_open) ||
        !memory_log_storage_range_valid(start, end)) {
        return result;
    }

    auto guard = logs->lock().unwrap();
    auto it_start = guard->lower_bound(start);
    auto it_end = guard->lower_bound(end);

    for (auto it = it_start; it != it_end; ++it) {
        result.push_back(it->second);
    }
    return result;
}

// @safe - Thread-safe batch put into the log map.
inline bool memory_log_storage_put_batch_cpp(
    rusty::Mutex<std::map<slotid_t, LogEntry>>* logs,
    bool is_open,
    const std::vector<LogEntry>& entries) {
    if (!memory_log_storage_is_usable(is_open)) {
        return false;
    }
    auto guard = logs->lock().unwrap();
    for (const auto& entry : entries) {
        (*guard)[entry.slot_id] = entry;
    }
    return true;
}

// @safe - Thread-safe range remove from the log map.
inline bool memory_log_storage_remove_range_cpp(
    rusty::Mutex<std::map<slotid_t, LogEntry>>* logs,
    bool is_open,
    slotid_t start,
    slotid_t end) {
    if (!memory_log_storage_is_usable(is_open) ||
        !memory_log_storage_range_valid(start, end)) {
        return false;
    }

    auto guard = logs->lock().unwrap();
    auto it = guard->lower_bound(start);
    while (it != guard->end() && it->first < end) {
        it = guard->erase(it);
    }
    return true;
}

// @safe - Thread-safe first index query.
inline slotid_t memory_log_storage_first_index_cpp(
    const rusty::Mutex<std::map<slotid_t, LogEntry>>* logs,
    bool is_open) {
    if (!memory_log_storage_is_usable(is_open)) {
        return 0;
    }
    auto guard = logs->lock().unwrap();
    return memory_log_storage_index_or_zero(!guard->empty(),
                                            guard->empty() ? 0 : guard->begin()->first);
}

// @safe - Thread-safe last index query.
inline slotid_t memory_log_storage_last_index_cpp(
    const rusty::Mutex<std::map<slotid_t, LogEntry>>* logs,
    bool is_open) {
    if (!memory_log_storage_is_usable(is_open)) {
        return 0;
    }
    auto guard = logs->lock().unwrap();
    return memory_log_storage_index_or_zero(!guard->empty(),
                                            guard->empty() ? 0 : guard->rbegin()->first);
}

// @safe - Thread-safe term query.
inline rusty::Option<ballot_t> memory_log_storage_term_cpp(
    const rusty::Mutex<std::map<slotid_t, LogEntry>>* logs,
    bool is_open,
    slotid_t slot_id) {
    auto entry_opt = memory_log_storage_get_cpp(logs, is_open, slot_id);
    if (entry_opt.is_none()) {
        return rusty::None;
    }
    return rusty::Some(entry_opt.unwrap().term);
}

// @safe - Thread-safe size query.
inline size_t memory_log_storage_size_cpp(
    const rusty::Mutex<std::map<slotid_t, LogEntry>>* logs,
    bool is_open) {
    if (!memory_log_storage_is_usable(is_open)) {
        return 0;
    }
    auto guard = logs->lock().unwrap();
    return guard->size();
}

// @safe - Thread-safe metadata set.
inline bool memory_log_storage_set_metadata_cpp(
    rusty::Mutex<std::map<std::string, std::string>>* metadata,
    bool is_open,
    const std::string& key,
    const std::string& value) {
    if (!memory_log_storage_is_usable(is_open)) {
        return false;
    }
    auto guard = metadata->lock().unwrap();
    (*guard)[key] = value;
    return true;
}

// @safe - Thread-safe metadata get.
inline rusty::Option<std::string> memory_log_storage_get_metadata_cpp(
    const rusty::Mutex<std::map<std::string, std::string>>* metadata,
    bool is_open,
    const std::string& key) {
    if (!memory_log_storage_is_usable(is_open)) {
        return rusty::None;
    }
    auto guard = metadata->lock().unwrap();
    auto it = guard->find(key);
    if (!memory_log_storage_metadata_found(it != guard->end())) {
        return rusty::None;
    }
    return rusty::Some(it->second);
}

// @safe - Close storage and clear all maps.
inline bool memory_log_storage_close_cpp(
    rusty::Mutex<std::map<slotid_t, LogEntry>>* logs,
    rusty::Mutex<std::map<std::string, std::string>>* metadata,
    rusty::Cell<bool>* is_open) {
    if (!memory_log_storage_is_usable(is_open->get())) {
        return false;
    }
    is_open->set(false);
    {
        auto guard = logs->lock().unwrap();
        guard->clear();
    }
    {
        auto guard = metadata->lock().unwrap();
        guard->clear();
    }
    return true;
}

// @safe - Clear all maps while leaving storage open.
inline bool memory_log_storage_clear_cpp(
    rusty::Mutex<std::map<slotid_t, LogEntry>>* logs,
    rusty::Mutex<std::map<std::string, std::string>>* metadata,
    bool is_open) {
    if (!memory_log_storage_is_usable(is_open)) {
        return false;
    }
    {
        auto guard = logs->lock().unwrap();
        guard->clear();
    }
    {
        auto guard = metadata->lock().unwrap();
        guard->clear();
    }
    return true;
}

// @safe - Thread-safe copy of all entries.
inline std::vector<LogEntry> memory_log_storage_get_all_cpp(
    const rusty::Mutex<std::map<slotid_t, LogEntry>>* logs,
    bool is_open) {
    std::vector<LogEntry> result;
    if (!memory_log_storage_is_usable(is_open)) {
        return result;
    }
    auto guard = logs->lock().unwrap();
    result.reserve(guard->size());
    for (const auto& pair : *guard) {
        result.push_back(pair.second);
    }
    return result;
}

#if RUSTYCPP_RUST
pub struct InMemoryLogStorageCore {
    logs_: rusty::Mutex<std::map<u64, LogEntry>>,
    metadata_: rusty::Mutex<std::map<std::string, std::string>>,
    is_open_: rusty::Cell<bool>,
}

impl InMemoryLogStorageCore {
    // @safe
    fn new() -> InMemoryLogStorageCore {
        InMemoryLogStorageCore {
            logs_: rusty::Mutex::<std::map<u64, LogEntry>>::default_(),
            metadata_: rusty::Mutex::<std::map<std::string, std::string>>::default_(),
            is_open_: rusty::Cell::<bool>::new_(true),
        }
    }

    // @safe
    fn get(&self, slot_id: u64) -> rusty::Option<LogEntry> {
        memory_log_storage_get_cpp(&self.logs_, self.is_open_.get(), slot_id)
    }

    // @safe
    fn put(&mut self, entry: &LogEntry) -> bool {
        memory_log_storage_put_cpp(&mut self.logs_, self.is_open_.get(), entry)
    }

    // @safe
    fn remove(&mut self, slot_id: u64) -> bool {
        memory_log_storage_remove_cpp(&mut self.logs_, self.is_open_.get(), slot_id)
    }

    // @safe
    fn get_range(&self, start: u64, end: u64) -> std::vector<LogEntry> {
        memory_log_storage_get_range_cpp(&self.logs_, self.is_open_.get(), start, end)
    }

    // @safe
    fn put_batch(&mut self, entries: &std::vector<LogEntry>) -> bool {
        memory_log_storage_put_batch_cpp(&mut self.logs_, self.is_open_.get(), entries)
    }

    // @safe
    fn remove_range(&mut self, start: u64, end: u64) -> bool {
        memory_log_storage_remove_range_cpp(&mut self.logs_, self.is_open_.get(), start, end)
    }

    // @safe
    fn get_first_index(&self) -> u64 {
        memory_log_storage_first_index_cpp(&self.logs_, self.is_open_.get())
    }

    // @safe
    fn get_last_index(&self) -> u64 {
        memory_log_storage_last_index_cpp(&self.logs_, self.is_open_.get())
    }

    // @safe
    fn get_term(&self, slot_id: u64) -> rusty::Option<i64> {
        memory_log_storage_term_cpp(&self.logs_, self.is_open_.get(), slot_id)
    }

    // @safe
    fn size(&self) -> usize {
        memory_log_storage_size_cpp(&self.logs_, self.is_open_.get())
    }

    // @safe
    fn empty(&self) -> bool {
        memory_log_storage_empty_from_size(self.size())
    }

    // @safe
    fn set_metadata(&mut self, key: &std::string,
                    value: &std::string) -> bool {
        memory_log_storage_set_metadata_cpp(&mut self.metadata_,
                                            self.is_open_.get(),
                                            key,
                                            value)
    }

    // @safe
    fn get_metadata(&self, key: &std::string) -> rusty::Option<std::string> {
        memory_log_storage_get_metadata_cpp(&self.metadata_,
                                            self.is_open_.get(),
                                            key)
    }

    // @safe
    fn sync(&self) -> bool {
        memory_log_storage_is_usable(self.is_open_.get())
    }

    // @safe
    fn close(&mut self) -> bool {
        memory_log_storage_close_cpp(&mut self.logs_,
                                     &mut self.metadata_,
                                     &mut self.is_open_)
    }

    // @safe
    fn is_open(&self) -> bool {
        memory_log_storage_is_usable(self.is_open_.get())
    }

    // @safe
    fn clear(&mut self) -> bool {
        memory_log_storage_clear_cpp(&mut self.logs_,
                                     &mut self.metadata_,
                                     self.is_open_.get())
    }

    // @safe
    fn reopen(&mut self) {
        self.is_open_.set(true);
    }

    // @safe
    fn get_all(&self) -> std::vector<LogEntry> {
        memory_log_storage_get_all_cpp(&self.logs_, self.is_open_.get())
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=memory_log_storage.core version=1 rust_sha256=d3349026e7bb2c2506f68de9438356f6ff42a3c668bb6406080eea3e53da09c5*/
struct InMemoryLogStorageCore;

struct InMemoryLogStorageCore {
    rusty::Mutex<std::map<uint64_t, LogEntry>> logs_;
    rusty::Mutex<std::map<std::string, std::string>> metadata_;
    rusty::Cell<bool> is_open_;

    static InMemoryLogStorageCore new_();
    rusty::Option<LogEntry> get(uint64_t slot_id) const;
    bool put(const LogEntry& entry);
    bool remove(uint64_t slot_id);
    std::vector<LogEntry> get_range(uint64_t start, uint64_t end) const;
    bool put_batch(const std::vector<LogEntry>& entries);
    bool remove_range(uint64_t start, uint64_t end);
    uint64_t get_first_index() const;
    uint64_t get_last_index() const;
    rusty::Option<int64_t> get_term(uint64_t slot_id) const;
    size_t size() const;
    bool empty() const;
    bool set_metadata(const std::string& key, const std::string& value);
    rusty::Option<std::string> get_metadata(const std::string& key) const;
    bool sync() const;
    bool close();
    bool is_open() const;
    bool clear();
    void reopen();
    std::vector<LogEntry> get_all() const;
};


inline InMemoryLogStorageCore InMemoryLogStorageCore::new_() {
    return InMemoryLogStorageCore{.logs_ = rusty::Mutex<std::map<uint64_t, LogEntry>>::default_(), .metadata_ = rusty::Mutex<std::map<std::string, std::string>>::default_(), .is_open_ = rusty::Cell<bool>::new_(true)};
}

inline rusty::Option<LogEntry> InMemoryLogStorageCore::get(uint64_t slot_id) const {
    return memory_log_storage_get_cpp(&this->logs_, this->is_open_.get(), std::move(slot_id));
}

inline bool InMemoryLogStorageCore::put(const LogEntry& entry) {
    return memory_log_storage_put_cpp(&this->logs_, this->is_open_.get(), entry);
}

inline bool InMemoryLogStorageCore::remove(uint64_t slot_id) {
    return memory_log_storage_remove_cpp(&this->logs_, this->is_open_.get(), std::move(slot_id));
}

inline std::vector<LogEntry> InMemoryLogStorageCore::get_range(uint64_t start, uint64_t end) const {
    return memory_log_storage_get_range_cpp(&this->logs_, this->is_open_.get(), std::move(start), std::move(end));
}

inline bool InMemoryLogStorageCore::put_batch(const std::vector<LogEntry>& entries) {
    return memory_log_storage_put_batch_cpp(&this->logs_, this->is_open_.get(), entries);
}

inline bool InMemoryLogStorageCore::remove_range(uint64_t start, uint64_t end) {
    return memory_log_storage_remove_range_cpp(&this->logs_, this->is_open_.get(), std::move(start), std::move(end));
}

inline uint64_t InMemoryLogStorageCore::get_first_index() const {
    return memory_log_storage_first_index_cpp(&this->logs_, this->is_open_.get());
}

inline uint64_t InMemoryLogStorageCore::get_last_index() const {
    return memory_log_storage_last_index_cpp(&this->logs_, this->is_open_.get());
}

inline rusty::Option<int64_t> InMemoryLogStorageCore::get_term(uint64_t slot_id) const {
    return memory_log_storage_term_cpp(&this->logs_, this->is_open_.get(), std::move(slot_id));
}

inline size_t InMemoryLogStorageCore::size() const {
    return memory_log_storage_size_cpp(&this->logs_, this->is_open_.get());
}

inline bool InMemoryLogStorageCore::empty() const {
    return memory_log_storage_empty_from_size(this->size());
}

inline bool InMemoryLogStorageCore::set_metadata(const std::string& key, const std::string& value) {
    return memory_log_storage_set_metadata_cpp(&this->metadata_, this->is_open_.get(), key, value);
}

inline rusty::Option<std::string> InMemoryLogStorageCore::get_metadata(const std::string& key) const {
    return memory_log_storage_get_metadata_cpp(&this->metadata_, this->is_open_.get(), key);
}

inline bool InMemoryLogStorageCore::sync() const {
    return memory_log_storage_is_usable(this->is_open_.get());
}

inline bool InMemoryLogStorageCore::close() {
    return memory_log_storage_close_cpp(&this->logs_, &this->metadata_, &this->is_open_);
}

inline bool InMemoryLogStorageCore::is_open() const {
    return memory_log_storage_is_usable(this->is_open_.get());
}

inline bool InMemoryLogStorageCore::clear() {
    return memory_log_storage_clear_cpp(&this->logs_, &this->metadata_, this->is_open_.get());
}

inline void InMemoryLogStorageCore::reopen() {
    this->is_open_.set(true);
}

inline std::vector<LogEntry> InMemoryLogStorageCore::get_all() const {
    return memory_log_storage_get_all_cpp(&this->logs_, this->is_open_.get());
}
/*RUSTYCPP:GEN-END id=memory_log_storage.core*/

class InMemoryLogStorage : public LogStorage {
private:
    InMemoryLogStorageCore core_;

public:
    // @safe - Default constructor
    InMemoryLogStorage() : core_(InMemoryLogStorageCore::new_()) {}

    // @safe - Destructor
    ~InMemoryLogStorage() noexcept override {
        close();
    }

    // ========================================================================
    // Single Entry Operations
    // ========================================================================

    // @safe - Thread-safe get
    rusty::Option<LogEntry> get(slotid_t slot_id) const override {
        return core_.get(slot_id);
    }

    // @safe - Thread-safe put
    bool put(const LogEntry& entry) override {
        return core_.put(entry);
    }

    // @safe - Thread-safe remove
    bool remove(slotid_t slot_id) override {
        return core_.remove(slot_id);
    }

    // ========================================================================
    // Batch Operations
    // ========================================================================

    // @safe - Thread-safe range get
    std::vector<LogEntry> get_range(slotid_t start, slotid_t end) const override {
        return core_.get_range(start, end);
    }

    // @safe - Thread-safe batch put
    bool put_batch(const std::vector<LogEntry>& entries) override {
        return core_.put_batch(entries);
    }

    // @safe - Thread-safe range remove
    bool remove_range(slotid_t start, slotid_t end) override {
        return core_.remove_range(start, end);
    }

    // ========================================================================
    // Index Queries
    // ========================================================================

    // @safe - Thread-safe first index query
    slotid_t get_first_index() const override {
        return core_.get_first_index();
    }

    // @safe - Thread-safe last index query
    slotid_t get_last_index() const override {
        return core_.get_last_index();
    }

    // @safe - Thread-safe term query
    rusty::Option<int64_t> get_term(uint64_t slot_id) const override {
        return core_.get_term(slot_id);
    }

    // @safe - Thread-safe size query
    size_t size() const override {
        return core_.size();
    }

    // @safe - Thread-safe empty check
    bool empty() const override {
        return core_.empty();
    }

    // ========================================================================
    // Metadata Operations
    // ========================================================================

    // @safe - Thread-safe metadata set
    bool set_metadata(const std::string& key, const std::string& value) override {
        return core_.set_metadata(key, value);
    }

    // @safe - Thread-safe metadata get
    rusty::Option<std::string> get_metadata(const std::string& key) const override {
        return core_.get_metadata(key);
    }

    // ========================================================================
    // Lifecycle Operations
    // ========================================================================

    // @safe - No-op for in-memory storage
    bool sync() override {
        return core_.sync();
    }

    // @safe - Close storage
    bool close() override {
        return core_.close();
    }

    // @safe - Check if open
    bool is_open() const override {
        return core_.is_open();
    }

    // @safe - Clear all data
    bool clear() override {
        return core_.clear();
    }

    // ========================================================================
    // Additional Utility Methods (not in interface)
    // ========================================================================

    /**
     * Reopen storage after close (for testing).
     */
    // @safe - Simple state change
    void reopen() {
        core_.reopen();
    }

    /**
     * Get all entries as a vector (for testing/debugging).
     */
    // @safe - Thread-safe copy
    std::vector<LogEntry> get_all() const {
        return core_.get_all();
    }
};

}  // namespace raft
}  // namespace janus
