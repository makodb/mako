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
#include <vector>

#include <rusty/option.hpp>
#include <rusty/mutex.hpp>
#include <rusty/cell.hpp>

#include "srpc/srpc.hpp"
#include "../mako_commands.h"  // janus::Command (SerializableEnvelope<MakoCommands>)

namespace janus {
namespace raft {

// Bring srpc:: marshalling types into janus::raft:: scope (they live in srpc::).
using ::srpc::BinaryWriteArchive;
using ::srpc::BinaryReadArchive;
using ::srpc::i8;
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
// @safe - POD-like struct with Marshallable serialization
struct LogEntry {
    slotid_t slot_id{0};              // Primary key (log index / slot)
    ballot_t term{0};                 // Raft term or Paxos epoch
    ballot_t max_ballot_seen{0};      // Highest ballot seen (Paxos)
    ballot_t max_ballot_accepted{0};  // Highest accepted ballot (Paxos)
    // the persistent log's
    // polymorphic command field migrated from
    // `shared_ptr<Marshallable>` to `janus::Command`
    // (`SerializableEnvelope<MakoCommands>`).  Command's internal
    // storage is still a `shared_ptr<Marshallable>` (callers crossing
    // the boundary into APIs that still take
    // `shared_ptr<Marshallable>` use `command.inner_marshallable()`),
    // so wire format is byte-for-byte unchanged.  See
    // `docs/dev/l10-unblock-plan.md` for the broader migration plan.
    Command command{};                // The replicated command
    bool committed{false};            // Whether entry is committed
    bool is_no_op{false};             // No-op entry flag

    // @safe - Default constructor
    LogEntry() = default;

    // @safe - Constructor with slot and term
    LogEntry(slotid_t slot, ballot_t t)
        : slot_id(slot), term(t) {}

    // @safe - Full constructor.  `cmd` accepts any
    // `shared_ptr<T>` for T inheriting Marshallable (via Command's
    // templated ctor), so callers passing `shared_ptr<Marshallable>`
    // / `shared_ptr<TestCommand>` continue to compile unchanged.
    LogEntry(slotid_t slot, ballot_t t, Command cmd, bool commit = false)
        : slot_id(slot), term(t), command(std::move(cmd)), committed(commit) {}

    // @safe - Comparison for ordering
    bool operator<(const LogEntry& other) const {
        return slot_id < other.slot_id;
    }

    // @safe - Equality comparison
    bool operator==(const LogEntry& other) const {
        return slot_id == other.slot_id &&
               term == other.term &&
               max_ballot_seen == other.max_ballot_seen &&
               max_ballot_accepted == other.max_ballot_accepted &&
               committed == other.committed &&
               is_no_op == other.is_no_op;
        // Note: command comparison requires deep equality
    }

    /**
     * Serialize the log entry to a `BinaryWriteArchive`.
     * Format: slot_id, term, max_ballot_seen, max_ballot_accepted,
     *         committed, is_no_op, has_command, [command]
     * Note: bools are serialized as i8 since the wire format doesn't
     * carry a native bool primitive.
     *
     * migrated from the legacy
     * `Marshal& to_marshal(Marshal&) const` / `from_marshal` member
     * pair to `save(BinaryWriteArchive&)` / `load(BinaryReadArchive&)`.
     * Wire format byte-for-byte preserved (the new archive operators
     * for primitives + the Phase 3f-prep MarshallDeputy archive op
     * produce the same bytes as their legacy Marshal counterparts).
     * The lone production callers in `rocksdb_log_storage.hpp` were
     * updated; no other callers existed.
     */
    // @unsafe - delegates to BinaryWriteArchive primitive operators
    void save(BinaryWriteArchive& ar) const {
        srpc::Serialize_::serialize(slot_id, ar);
        srpc::Serialize_::serialize(term, ar);
        srpc::Serialize_::serialize(max_ballot_seen, ar);
        srpc::Serialize_::serialize(max_ballot_accepted, ar);
        srpc::Serialize_::serialize(static_cast<i8>(committed ? 1 : 0), ar);
        srpc::Serialize_::serialize(static_cast<i8>(is_no_op ? 1 : 0), ar);

        // drive the polymorphic command through Command's
        // own archive operator instead of wrapping it in a temporary
        // MarshallDeputy.  Wire format is identical (Command emits
        // `[v32 kind][payload]`, same as MarshallDeputy post-L9).
        i8 has_command = command.has_value() ? 1 : 0;
        srpc::Serialize_::serialize(has_command, ar);
        if (has_command) {
            srpc::Serialize_::serialize(command, ar);
        }
    }

    /**
     * Deserialize a log entry from a `BinaryReadArchive`.
     * Required: archive's source must be a `MarshalSource` (the
     * prep `operator>>(BinaryReadArchive&, MarshallDeputy&)`
     * needs a backing `Marshal` to drain into the legacy decode path).
     */
    // @unsafe - delegates to BinaryReadArchive primitive operators
    void load(BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(slot_id, ar);
        srpc::Deserialize_::deserialize(term, ar);
        srpc::Deserialize_::deserialize(max_ballot_seen, ar);
        srpc::Deserialize_::deserialize(max_ballot_accepted, ar);

        i8 committed_byte = 0;
        srpc::Deserialize_::deserialize(committed_byte, ar);
        committed = (committed_byte != 0);

        i8 is_no_op_byte = 0;
        srpc::Deserialize_::deserialize(is_no_op_byte, ar);
        is_no_op = (is_no_op_byte != 0);

        i8 has_command = 0;
        srpc::Deserialize_::deserialize(has_command, ar);
        if (has_command) {
            srpc::Deserialize_::deserialize(command, ar);
        } else {
            command = Command{};
        }
    }
};

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
