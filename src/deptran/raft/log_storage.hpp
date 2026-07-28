#pragma once

/**
 * Log Storage Interface for Raft/Paxos Consensus Protocols
 *
 * This header defines:
 * - LogEntry: A unified log entry structure for both Raft and Paxos
 * - LogStorage: Abstract interface for pluggable storage backends
 *
 * RustyCpp migration notes:
 * - LogEntry and LogStorage are DSL-owned declaration surfaces.
 * - Serialization and backend I/O are intentionally delegated to helpers or
 *   concrete storage bridges.
 * - In-memory and RocksDB implementations decide their own synchronization and
 *   unsafe boundaries; the base trait only fixes the call shape.
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
 *
 * This trait is the stable virtual surface. Concrete implementations may use
 * DSL Core structs, but backend handles, RocksDB iterators, raw byte
 * serialization, and persistence side effects stay behind C++ helpers.
 */
#if RUSTYCPP_RUST
pub trait LogStorage {
    // @safe
    fn get(&self, slot_id: u64) -> rusty::Option<LogEntry>;
    // @safe
    fn put(&mut self, entry: &LogEntry) -> bool;
    // @safe
    fn remove(&mut self, slot_id: u64) -> bool;
    // @safe
    fn get_range(&self, start: u64, end: u64) -> std::vector<LogEntry>;
    // @safe
    fn put_batch(&mut self, entries: &std::vector<LogEntry>) -> bool;
    // @safe
    fn remove_range(&mut self, start: u64, end: u64) -> bool;
    // @safe
    fn get_first_index(&self) -> u64;
    // @safe
    fn get_last_index(&self) -> u64;
    // @safe
    fn get_term(&self, slot_id: u64) -> rusty::Option<i64>;
    // @safe
    fn size(&self) -> usize;
    // @safe
    fn empty(&self) -> bool;
    // @safe
    fn set_metadata(&mut self, key: &std::string,
                    value: &std::string) -> bool;
    // @safe
    fn get_metadata(&self, key: &std::string) -> rusty::Option<std::string>;
    // @safe
    fn sync(&mut self) -> bool;
    // @safe
    fn close(&mut self) -> bool;
    // @safe
    fn is_open(&self) -> bool;
    // @safe
    fn clear(&mut self) -> bool;
}
#endif
/*RUSTYCPP:GEN-BEGIN id=log_storage.interface version=1 rust_sha256=a186ac27949b2eaaf626139815015bdb754a1d743f936a1c4b9ce7bed486fc55*/
namespace {
class LogStorage {
public:
    virtual ~LogStorage() noexcept(false) {}
    virtual rusty::Option<LogEntry> get(uint64_t slot_id) const = 0;
    virtual bool put(const LogEntry& entry) = 0;
    virtual bool remove(uint64_t slot_id) = 0;
    virtual std::vector<LogEntry> get_range(uint64_t start, uint64_t end) const = 0;
    virtual bool put_batch(const std::vector<LogEntry>& entries) = 0;
    virtual bool remove_range(uint64_t start, uint64_t end) = 0;
    virtual uint64_t get_first_index() const = 0;
    virtual uint64_t get_last_index() const = 0;
    virtual rusty::Option<int64_t> get_term(uint64_t slot_id) const = 0;
    virtual size_t size() const = 0;
    virtual bool empty() const = 0;
    virtual bool set_metadata(const std::string& key, const std::string& value) = 0;
    virtual rusty::Option<std::string> get_metadata(const std::string& key) const = 0;
    virtual bool sync() = 0;
    virtual bool close() = 0;
    virtual bool is_open() const = 0;
    virtual bool clear() = 0;
    LogStorage(const LogStorage&) = delete;
    LogStorage& operator=(const LogStorage&) = delete;
    LogStorage(LogStorage&&) = delete;
    LogStorage& operator=(LogStorage&&) = delete;
protected:
    LogStorage() = default;
};
}

template <class U> class LogStorageAdapter;
template <class U> class LogStorageAdapterRef;
template <class U> class LogStorageAdapterRefMut;
/*RUSTYCPP:GEN-END id=log_storage.interface*/

}  // namespace raft
}  // namespace janus
