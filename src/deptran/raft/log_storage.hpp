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
#include <rusty/slice.hpp>

#include "rrr/rrr.hpp"
#include "../mako_commands.h"  // janus::Command (SerializableEnvelope<MakoCommands>)

namespace janus {
namespace raft {

// Bring rrr:: marshalling types into janus::raft:: scope (they live in rrr::).
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
#if RUSTYCPP_RUST
pub const fn log_entry_slot_precedes(slot_id: u64, other_slot_id: u64) -> bool {
    slot_id < other_slot_id
}

pub const fn log_entry_scalar_fields_equal(slot_equal: bool,
                                            term_equal: bool,
                                            max_seen_equal: bool,
                                            max_accepted_equal: bool,
                                            committed_equal: bool,
                                            no_op_equal: bool) -> bool {
    slot_equal && term_equal && max_seen_equal && max_accepted_equal &&
        committed_equal && no_op_equal
}

pub const fn log_entry_bool_to_i8(value: bool) -> i8 {
    if value { 1 } else { 0 }
}

pub const fn log_entry_i8_to_bool(value: i8) -> bool {
    value != 0
}
#endif
/*RUSTYCPP:GEN-BEGIN id=raft_log_entry.scalar_decisions version=1 rust_sha256=983d5a4010a6372e6e6c5cf7f136c3d1947e81a01a66c8e1688aba403d213d95*/
constexpr bool log_entry_slot_precedes(uint64_t slot_id, uint64_t other_slot_id);
constexpr bool log_entry_scalar_fields_equal(bool slot_equal, bool term_equal, bool max_seen_equal, bool max_accepted_equal, bool committed_equal, bool no_op_equal);
constexpr int8_t log_entry_bool_to_i8(bool value);
constexpr bool log_entry_i8_to_bool(int8_t value);
constexpr bool log_entry_slot_precedes(uint64_t slot_id, uint64_t other_slot_id) {
    return rusty::detail::deref_if_pointer_like(slot_id) < rusty::detail::deref_if_pointer_like(other_slot_id);
}
constexpr bool log_entry_scalar_fields_equal(bool slot_equal, bool term_equal, bool max_seen_equal, bool max_accepted_equal, bool committed_equal, bool no_op_equal) {
    return ((((rusty::detail::deref_if_pointer_like(slot_equal) && rusty::detail::deref_if_pointer_like(term_equal)) && rusty::detail::deref_if_pointer_like(max_seen_equal)) && rusty::detail::deref_if_pointer_like(max_accepted_equal)) && rusty::detail::deref_if_pointer_like(committed_equal)) && rusty::detail::deref_if_pointer_like(no_op_equal);
}
constexpr int8_t log_entry_bool_to_i8(bool value) {
    if (value) {
        return static_cast<int8_t>(1);
    } else {
        return static_cast<int8_t>(0);
    }
}
constexpr bool log_entry_i8_to_bool(int8_t value) {
    return rusty::detail::deref_if_pointer_like(value) != static_cast<int8_t>(0);
}
/*RUSTYCPP:GEN-END id=raft_log_entry.scalar_decisions*/

static_assert(log_entry_slot_precedes(1, 2));
static_assert(!log_entry_slot_precedes(2, 2));
static_assert(log_entry_scalar_fields_equal(
    true, true, true, true, true, true));
static_assert(!log_entry_scalar_fields_equal(
    false, true, true, true, true, true));
static_assert(!log_entry_scalar_fields_equal(
    true, false, true, true, true, true));
static_assert(!log_entry_scalar_fields_equal(
    true, true, false, true, true, true));
static_assert(!log_entry_scalar_fields_equal(
    true, true, true, false, true, true));
static_assert(!log_entry_scalar_fields_equal(
    true, true, true, true, false, true));
static_assert(!log_entry_scalar_fields_equal(
    true, true, true, true, true, false));
static_assert(log_entry_bool_to_i8(false) == 0);
static_assert(log_entry_bool_to_i8(true) == 1);
static_assert(!log_entry_i8_to_bool(0));
static_assert(log_entry_i8_to_bool(1));
static_assert(log_entry_i8_to_bool(-1));

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
        return log_entry_slot_precedes(slot_id, other.slot_id);
    }

    // @safe - Equality comparison
    bool operator==(const LogEntry& other) const {
        return log_entry_scalar_fields_equal(
            slot_id == other.slot_id,
            term == other.term,
            max_ballot_seen == other.max_ballot_seen,
            max_ballot_accepted == other.max_ballot_accepted,
            committed == other.committed,
            is_no_op == other.is_no_op);
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
        rrr::Serialize_::serialize(slot_id, ar);
        rrr::Serialize_::serialize(term, ar);
        rrr::Serialize_::serialize(max_ballot_seen, ar);
        rrr::Serialize_::serialize(max_ballot_accepted, ar);
        rrr::Serialize_::serialize(log_entry_bool_to_i8(committed), ar);
        rrr::Serialize_::serialize(log_entry_bool_to_i8(is_no_op), ar);

        // drive the polymorphic command through Command's
        // own archive operator instead of wrapping it in a temporary
        // MarshallDeputy.  Wire format is identical (Command emits
        // `[v32 kind][payload]`, same as MarshallDeputy post-L9).
        i8 has_command = log_entry_bool_to_i8(command.has_value());
        rrr::Serialize_::serialize(has_command, ar);
        if (log_entry_i8_to_bool(has_command)) {
            rrr::Serialize_::serialize(command, ar);
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
        rrr::Deserialize_::deserialize(slot_id, ar);
        rrr::Deserialize_::deserialize(term, ar);
        rrr::Deserialize_::deserialize(max_ballot_seen, ar);
        rrr::Deserialize_::deserialize(max_ballot_accepted, ar);

        i8 committed_byte = 0;
        rrr::Deserialize_::deserialize(committed_byte, ar);
        committed = log_entry_i8_to_bool(committed_byte);

        i8 is_no_op_byte = 0;
        rrr::Deserialize_::deserialize(is_no_op_byte, ar);
        is_no_op = log_entry_i8_to_bool(is_no_op_byte);

        i8 has_command = 0;
        rrr::Deserialize_::deserialize(has_command, ar);
        if (log_entry_i8_to_bool(has_command)) {
            rrr::Deserialize_::deserialize(command, ar);
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
     * Atomically store a group of metadata values.
     *
     * Raft's current term and vote are one persistent state transition.  A
     * backend must therefore expose a real all-or-nothing write instead of
     * emulating this operation with successive set_metadata() calls.
     */
    // @safe - Abstract method; implementations provide one atomic operation
    virtual bool set_metadata_batch(
        const std::vector<std::pair<std::string, std::string>>& entries) = 0;

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
