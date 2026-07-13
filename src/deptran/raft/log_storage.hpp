#pragma once

/**
 * Log Storage Interface for Raft/Paxos Consensus Protocols
 *
 * This header defines:
 * - LogEntry: A unified log entry structure for both Raft and Paxos
 * - LogStorage: Abstract interface for pluggable storage backends
 *
 * RustyCpp Compliance: Uses rusty::Option, rusty::Mutex, rusty::Cell
 */

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <rusty/option.hpp>
#include <rusty/mutex.hpp>
#include <rusty/cell.hpp>

#include "rrr/rrr.hpp"
#include "../mako_commands.h"  // janus::Command (SerializableEnvelope<MakoCommands>)

namespace janus {
namespace raft {

// Bring rrr:: marshalling types into janus::raft:: scope (they live in rrr::).
using ::rrr::Marshal;
using ::rrr::BinaryWriteArchive;
using ::rrr::BinaryReadArchive;
using ::rrr::i8;
using ::janus::Command;

// Type aliases matching existing codebase
// Use preprocessor guards to avoid conflict with macro definitions in constants.h
#ifndef slotid_t
using slotid_t = uint64_t;
#endif
#ifndef ballot_t
using ballot_t = uint64_t;
#endif

/**
 * Unified log entry structure for Raft and Paxos consensus protocols.
 *
 * This structure captures the common elements needed by both protocols:
 * - Raft: term, log index, command, committed flag
 * - Paxos: ballot, slot, accepted command, committed flag
 */
struct LogEntry;

inline LogEntry log_entry_defaults();
inline LogEntry log_entry_with_slot_term(slotid_t slot, ballot_t term);
inline LogEntry log_entry_with_command(slotid_t slot,
                                       ballot_t term,
                                       Command cmd,
                                       bool commit);
inline bool log_entry_less_than(const LogEntry& lhs, const LogEntry& rhs);
inline bool log_entry_equals(const LogEntry& lhs, const LogEntry& rhs);
inline void log_entry_save(const LogEntry& entry, BinaryWriteArchive& ar);
inline void log_entry_load(LogEntry& entry, BinaryReadArchive& ar);

#if RUSTYCPP_RUST
pub struct LogEntry {
    slot_id: u64,
    term: u64,
    max_ballot_seen: u64,
    max_ballot_accepted: u64,
    command: Command,
    committed: bool,
    is_no_op: bool,
}

impl LogEntry {
    fn defaults() -> LogEntry {
        log_entry_defaults()
    }

    fn with_slot_term(slot: u64, term: u64) -> LogEntry {
        log_entry_with_slot_term(slot, term)
    }

    fn with_command(slot: u64, term: u64, cmd: Command, commit: bool) -> LogEntry {
        log_entry_with_command(slot, term, cmd, commit)
    }

    fn save(&self, ar: &mut BinaryWriteArchive) {
        log_entry_save(self, ar);
    }

    fn load(&mut self, ar: &mut BinaryReadArchive) {
        log_entry_load(self, ar);
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=log_storage.log_entry version=1 rust_sha256=4f817e60d3bcd92f8a9f23b2760a0793a7bfcf2d1ff4c8c05e9395b3eb3b6225*/
struct LogEntry;

struct LogEntry {
    uint64_t slot_id;
    uint64_t term;
    uint64_t max_ballot_seen;
    uint64_t max_ballot_accepted;
    Command command;
    bool committed;
    bool is_no_op;

    static LogEntry defaults();
    static LogEntry with_slot_term(uint64_t slot, uint64_t term);
    static LogEntry with_command(uint64_t slot, uint64_t term, Command cmd, bool commit);
    void save(BinaryWriteArchive& ar) const;
    void load(BinaryReadArchive& ar);
};


inline LogEntry LogEntry::defaults() {
    return log_entry_defaults();
}

inline LogEntry LogEntry::with_slot_term(uint64_t slot, uint64_t term) {
    return log_entry_with_slot_term(std::move(slot), std::move(term));
}

inline LogEntry LogEntry::with_command(uint64_t slot, uint64_t term, Command cmd, bool commit) {
    return log_entry_with_command(std::move(slot), std::move(term), std::move(cmd), std::move(commit));
}

inline void LogEntry::save(BinaryWriteArchive& ar) const {
    log_entry_save((*this), ar);
}

inline void LogEntry::load(BinaryReadArchive& ar) {
    log_entry_load((*this), ar);
}
/*RUSTYCPP:GEN-END id=log_storage.log_entry*/

inline LogEntry log_entry_defaults() {
    LogEntry entry{};
    entry.slot_id = 0;
    entry.term = 0;
    entry.max_ballot_seen = 0;
    entry.max_ballot_accepted = 0;
    entry.command = Command{};
    entry.committed = false;
    entry.is_no_op = false;
    return entry;
}

inline LogEntry log_entry_with_slot_term(slotid_t slot, ballot_t term) {
    LogEntry entry = LogEntry::defaults();
    entry.slot_id = slot;
    entry.term = term;
    return entry;
}

inline LogEntry log_entry_with_command(slotid_t slot,
                                       ballot_t term,
                                       Command cmd,
                                       bool commit) {
    LogEntry entry = LogEntry::with_slot_term(slot, term);
    entry.command = std::move(cmd);
    entry.committed = commit;
    return entry;
}

inline bool log_entry_less_than(const LogEntry& lhs, const LogEntry& rhs) {
    return lhs.slot_id < rhs.slot_id;
}

inline bool log_entry_equals(const LogEntry& lhs, const LogEntry& rhs) {
    return lhs.slot_id == rhs.slot_id &&
           lhs.term == rhs.term &&
           lhs.max_ballot_seen == rhs.max_ballot_seen &&
           lhs.max_ballot_accepted == rhs.max_ballot_accepted &&
           lhs.committed == rhs.committed &&
           lhs.is_no_op == rhs.is_no_op;
    // Note: command comparison requires deep equality
}

inline bool operator<(const LogEntry& lhs, const LogEntry& rhs) {
    return log_entry_less_than(lhs, rhs);
}

inline bool operator==(const LogEntry& lhs, const LogEntry& rhs) {
    return log_entry_equals(lhs, rhs);
}

/**
 * Serialize the log entry to a `BinaryWriteArchive`.
 * Format: slot_id, term, max_ballot_seen, max_ballot_accepted,
 *         committed, is_no_op, has_command, [command]
 * Note: bools are serialized as i8 since the wire format doesn't
 * carry a native bool primitive.
 *
 * This remains hand-written C++ after the struct-shape DSL migration
 * so the persistent storage format stays byte-for-byte unchanged.
 */
// @unsafe - delegates to BinaryWriteArchive primitive operators
inline void log_entry_save(const LogEntry& entry, BinaryWriteArchive& ar) {
    ar << entry.slot_id;
    ar << entry.term;
    ar << entry.max_ballot_seen;
    ar << entry.max_ballot_accepted;
    ar << static_cast<i8>(entry.committed ? 1 : 0);
    ar << static_cast<i8>(entry.is_no_op ? 1 : 0);

    // Drive the polymorphic command through Command's own archive operator.
    // Wire format is identical (Command emits `[v32 kind][payload]`).
    i8 has_command = entry.command.has_value() ? 1 : 0;
    ar << has_command;
    if (has_command) {
        ar << entry.command;
    }
}

/**
 * Deserialize a log entry from a `BinaryReadArchive`.
 * Required: archive's source must be a `MarshalSource` (the
 * prep `operator>>(BinaryReadArchive&, MarshallDeputy&)`
 * needs a backing `Marshal` to drain into the legacy decode path).
 */
// @unsafe - delegates to BinaryReadArchive primitive operators
inline void log_entry_load(LogEntry& entry, BinaryReadArchive& ar) {
    ar >> entry.slot_id;
    ar >> entry.term;
    ar >> entry.max_ballot_seen;
    ar >> entry.max_ballot_accepted;

    i8 committed_byte = 0;
    ar >> committed_byte;
    entry.committed = (committed_byte != 0);

    i8 is_no_op_byte = 0;
    ar >> is_no_op_byte;
    entry.is_no_op = (is_no_op_byte != 0);

    i8 has_command = 0;
    ar >> has_command;
    if (has_command) {
        ar >> entry.command;
    } else {
        entry.command = Command{};
    }
}

/**
 * Abstract interface for log storage backends.
 *
 * Implementations can provide:
 * - In-memory storage (for testing)
 * - RocksDB storage (for durability)
 * - Custom backends
 *
 * All methods are thread-safe in implementations.
 */
class LogStorage {
public:
    // @safe - Virtual destructor
    virtual ~LogStorage() = default;

    // ========================================================================
    // Single Entry Operations
    // ========================================================================

    /**
     * Get a log entry by slot ID.
     * @param slot_id The slot/index to retrieve
     * @return Some(entry) if found, None if not found
     */
    // @safe - Abstract method, implementations must be safe
    virtual rusty::Option<LogEntry> get(slotid_t slot_id) const = 0;

    /**
     * Store a log entry.
     * @param entry The entry to store (slot_id is the key)
     * @return true on success, false on failure
     */
    // @safe - Abstract method
    virtual bool put(const LogEntry& entry) = 0;

    /**
     * Remove a log entry by slot ID.
     * @param slot_id The slot to remove
     * @return true if removed, false if not found
     */
    // @safe - Abstract method
    virtual bool remove(slotid_t slot_id) = 0;

    // ========================================================================
    // Batch Operations
    // ========================================================================

    /**
     * Get a range of log entries [start, end).
     * @param start Start slot (inclusive)
     * @param end End slot (exclusive)
     * @return Vector of entries in the range (may be sparse)
     */
    // @safe - Abstract method
    virtual std::vector<LogEntry> get_range(slotid_t start, slotid_t end) const = 0;

    /**
     * Store multiple log entries atomically.
     * @param entries Vector of entries to store
     * @return true if all stored, false on failure
     */
    // @safe - Abstract method
    virtual bool put_batch(const std::vector<LogEntry>& entries) = 0;

    /**
     * Remove a range of log entries [start, end).
     * @param start Start slot (inclusive)
     * @param end End slot (exclusive)
     * @return true on success
     */
    // @safe - Abstract method
    virtual bool remove_range(slotid_t start, slotid_t end) = 0;

    // ========================================================================
    // Index Queries
    // ========================================================================

    /**
     * Get the first (lowest) slot ID in the log.
     * @return First slot ID, or 0 if empty
     */
    // @safe - Abstract method
    virtual slotid_t get_first_index() const = 0;

    /**
     * Get the last (highest) slot ID in the log.
     * @return Last slot ID, or 0 if empty
     */
    // @safe - Abstract method
    virtual slotid_t get_last_index() const = 0;

    /**
     * Get the term/ballot for a specific slot.
     * @param slot_id The slot to query
     * @return Some(term) if found, None if not found
     */
    // @safe - Abstract method
    virtual rusty::Option<ballot_t> get_term(slotid_t slot_id) const = 0;

    /**
     * Get the number of entries in the log.
     * @return Number of stored entries
     */
    // @safe - Abstract method
    virtual size_t size() const = 0;

    /**
     * Check if the log is empty.
     * @return true if no entries stored
     */
    // @safe - Abstract method
    virtual bool empty() const = 0;

    // ========================================================================
    // Metadata Operations
    // ========================================================================

    /**
     * Store metadata (term, vote, commit index, etc.).
     * @param key Metadata key
     * @param value Metadata value
     * @return true on success
     */
    // @safe - Abstract method
    virtual bool set_metadata(const std::string& key, const std::string& value) = 0;

    /**
     * Retrieve metadata.
     * @param key Metadata key
     * @return Some(value) if found, None if not found
     */
    // @safe - Abstract method
    virtual rusty::Option<std::string> get_metadata(const std::string& key) const = 0;

    // ========================================================================
    // Lifecycle Operations
    // ========================================================================

    /**
     * Force sync all pending writes to durable storage.
     * @return true on success
     */
    // @safe - Abstract method
    virtual bool sync() = 0;

    /**
     * Close the storage, releasing resources.
     * @return true on success
     */
    // @safe - Abstract method
    virtual bool close() = 0;

    /**
     * Check if storage is open and ready.
     * @return true if open
     */
    // @safe - Abstract method
    virtual bool is_open() const = 0;

    /**
     * Clear all entries and metadata.
     * @return true on success
     */
    // @safe - Abstract method
    virtual bool clear() = 0;
};

}  // namespace raft
}  // namespace janus
