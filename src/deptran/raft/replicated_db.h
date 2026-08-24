#pragma once
#include <rusty/arc.hpp>
#include <rusty/num.hpp>
#include <rusty/option.hpp>
#include <rusty/slice.hpp>
#include "__dep__.h"
#include "rrr/rrr.hpp"
#include "../mako_commands.h"
#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>
#include <atomic>
#include <functional>
#include <rocksdb/c.h>
#include "lz4.h"

namespace janus {

class RaftServer;  // Forward declaration

// @safe - Trivially copyable enum for operation type
#if RUSTYCPP_RUST
#[allow(non_camel_case_types)]
#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Eq, PartialEq))]
#[repr(u8)]
pub enum ReplicatedDBOp {
    PUT = 1,
    DELETE = 2,
    BATCH = 3,
}
#endif
/*RUSTYCPP:GEN-BEGIN id=raft_replicated_db.operation version=1 rust_sha256=e6e33890ec5a6fa6633445ded015e012caca637c69b920dd9ed4cd6d17bacd62*/
enum class ReplicatedDBOp : uint8_t;
constexpr ReplicatedDBOp ReplicatedDBOp_PUT();
constexpr ReplicatedDBOp ReplicatedDBOp_DELETE();
constexpr ReplicatedDBOp ReplicatedDBOp_BATCH();

enum class ReplicatedDBOp : uint8_t {
    PUT = 1,
    DELETE = 2,
    BATCH = 3
};
inline constexpr ReplicatedDBOp ReplicatedDBOp_PUT() { return ReplicatedDBOp::PUT; }
inline constexpr ReplicatedDBOp ReplicatedDBOp_DELETE() { return ReplicatedDBOp::DELETE; }
inline constexpr ReplicatedDBOp ReplicatedDBOp_BATCH() { return ReplicatedDBOp::BATCH; }
/*RUSTYCPP:GEN-END id=raft_replicated_db.operation*/

static_assert(std::is_same_v<
              std::underlying_type_t<ReplicatedDBOp>, uint8_t>);
static_assert(std::is_trivially_copyable_v<ReplicatedDBOp>);
static_assert(sizeof(ReplicatedDBOp) == sizeof(uint8_t));
static_assert(alignof(ReplicatedDBOp) == alignof(uint8_t));
static_assert(static_cast<uint8_t>(ReplicatedDBOp::PUT) == 1);
static_assert(static_cast<uint8_t>(ReplicatedDBOp::DELETE) == 2);
static_assert(static_cast<uint8_t>(ReplicatedDBOp::BATCH) == 3);
// The emitted C++ Stage-1 provider deliberately preserves all raw byte
// values accepted by legacy disk/wire decoding. Native Rust promotion must
// validate bytes or use a transparent newtype before consuming these states.
static_assert(static_cast<uint8_t>(static_cast<ReplicatedDBOp>(0)) == 0);
static_assert(static_cast<uint8_t>(static_cast<ReplicatedDBOp>(0xff)) == 0xff);

// These helpers own scalar decisions only. Raw operation bytes remain valid
// inputs on the generated-C++ path, including unnamed values read from the
// existing wire format.
#if RUSTYCPP_RUST
pub const fn replicated_db_op_is_batch(op: u8) -> bool {
    op == ReplicatedDBOp::BATCH as u8
}

pub const fn replicated_db_has_command_payload(has_value: bool) -> bool {
    has_value
}

pub const fn replicated_db_should_skip_applied(index: u64,
                                               last_applied_index: u64) -> bool {
    index <= last_applied_index
}

pub const fn replicated_db_command_kind_matches(kind: i32,
                                                expected_kind: i32) -> bool {
    kind == expected_kind
}

pub const fn replicated_db_required_value_missing(has_value: bool) -> bool {
    !has_value
}

pub const fn replicated_db_commit_succeeded(commit_state: i32) -> bool {
    commit_state > 0
}

pub const fn replicated_db_commit_pending(commit_state: i32) -> bool {
    commit_state == 0
}

pub const fn replicated_db_commit_callback_state(rolled_back: bool) -> i32 {
    if rolled_back {
        -1
    } else {
        1
    }
}

pub const fn replicated_db_read_found(has_value_ptr: bool) -> bool {
    has_value_ptr
}

pub const fn replicated_db_is_leader(is_leader: bool) -> bool {
    is_leader
}

pub const fn replicated_db_snapshot_has_header(size: usize) -> bool {
    size >= 1
}

pub const fn replicated_db_snapshot_is_lz4(compression: u8,
                                           lz4_tag: u8) -> bool {
    compression == lz4_tag
}

pub const fn replicated_db_snapshot_is_uncompressed(compression: u8,
                                                    uncompressed_tag: u8) -> bool {
    compression == uncompressed_tag
}

pub const fn replicated_db_snapshot_has_bytes(offset: usize,
                                              needed: usize,
                                              total: usize) -> bool {
    offset.wrapping_add(needed) <= total
}

pub const fn replicated_db_snapshot_has_u64_bytes(offset: u64,
                                                  needed: u64,
                                                  total: u64) -> bool {
    offset.wrapping_add(needed) <= total
}
#endif
/*RUSTYCPP:GEN-BEGIN id=raft_replicated_db.scalar_decisions version=1 rust_sha256=d319e421c490d07526222c902ab111822dfecd9c41b9801303d4d2b9f3c83ed1*/
constexpr bool replicated_db_op_is_batch(uint8_t op);
constexpr bool replicated_db_has_command_payload(bool has_value);
constexpr bool replicated_db_should_skip_applied(uint64_t index, uint64_t last_applied_index);
constexpr bool replicated_db_command_kind_matches(int32_t kind, int32_t expected_kind);
constexpr bool replicated_db_required_value_missing(bool has_value);
constexpr bool replicated_db_commit_succeeded(int32_t commit_state);
constexpr bool replicated_db_commit_pending(int32_t commit_state);
constexpr int32_t replicated_db_commit_callback_state(bool rolled_back);
constexpr bool replicated_db_read_found(bool has_value_ptr);
constexpr bool replicated_db_is_leader(bool is_leader);
constexpr bool replicated_db_snapshot_has_header(size_t size);
constexpr bool replicated_db_snapshot_is_lz4(uint8_t compression, uint8_t lz4_tag);
constexpr bool replicated_db_snapshot_is_uncompressed(uint8_t compression, uint8_t uncompressed_tag);
constexpr bool replicated_db_snapshot_has_bytes(size_t offset, size_t needed, size_t total);
constexpr bool replicated_db_snapshot_has_u64_bytes(uint64_t offset, uint64_t needed, uint64_t total);
constexpr bool replicated_db_op_is_batch(uint8_t op) {
    return rusty::detail::deref_if_pointer_like(op) == (static_cast<uint8_t>(ReplicatedDBOp_BATCH()));
}
constexpr bool replicated_db_has_command_payload(bool has_value) {
    return std::move(has_value);
}
constexpr bool replicated_db_should_skip_applied(uint64_t index, uint64_t last_applied_index) {
    return rusty::detail::deref_if_pointer_like(index) <= rusty::detail::deref_if_pointer_like(last_applied_index);
}
constexpr bool replicated_db_command_kind_matches(int32_t kind, int32_t expected_kind) {
    return rusty::detail::deref_if_pointer_like(kind) == rusty::detail::deref_if_pointer_like(expected_kind);
}
constexpr bool replicated_db_required_value_missing(bool has_value) {
    return !has_value;
}
constexpr bool replicated_db_commit_succeeded(int32_t commit_state) {
    return rusty::detail::deref_if_pointer_like(commit_state) > 0;
}
constexpr bool replicated_db_commit_pending(int32_t commit_state) {
    return rusty::detail::deref_if_pointer_like(commit_state) == static_cast<int32_t>(0);
}
constexpr int32_t replicated_db_commit_callback_state(bool rolled_back) {
    if (rolled_back) {
        return -1;
    } else {
        return static_cast<int32_t>(1);
    }
}
constexpr bool replicated_db_read_found(bool has_value_ptr) {
    return std::move(has_value_ptr);
}
constexpr bool replicated_db_is_leader(bool is_leader) {
    return std::move(is_leader);
}
constexpr bool replicated_db_snapshot_has_header(size_t size) {
    return rusty::detail::deref_if_pointer_like(size) >= 1;
}
constexpr bool replicated_db_snapshot_is_lz4(uint8_t compression, uint8_t lz4_tag) {
    return rusty::detail::deref_if_pointer_like(compression) == rusty::detail::deref_if_pointer_like(lz4_tag);
}
constexpr bool replicated_db_snapshot_is_uncompressed(uint8_t compression, uint8_t uncompressed_tag) {
    return rusty::detail::deref_if_pointer_like(compression) == rusty::detail::deref_if_pointer_like(uncompressed_tag);
}
constexpr bool replicated_db_snapshot_has_bytes(size_t offset, size_t needed, size_t total) {
    return rusty::wrapping_add(offset, static_cast<std::remove_cvref_t<decltype(offset)>>(std::move(needed))) <= rusty::detail::deref_if_pointer_like(total);
}
constexpr bool replicated_db_snapshot_has_u64_bytes(uint64_t offset, uint64_t needed, uint64_t total) {
    return rusty::wrapping_add(offset, static_cast<std::remove_cvref_t<decltype(offset)>>(std::move(needed))) <= rusty::detail::deref_if_pointer_like(total);
}
/*RUSTYCPP:GEN-END id=raft_replicated_db.scalar_decisions*/

static_assert(replicated_db_op_is_batch(3));
static_assert(!replicated_db_op_is_batch(0));
static_assert(replicated_db_has_command_payload(true));
static_assert(replicated_db_should_skip_applied(7, 7));
static_assert(!replicated_db_should_skip_applied(8, 7));
static_assert(replicated_db_command_kind_matches(19, 19));
static_assert(!replicated_db_required_value_missing(true));
static_assert(replicated_db_required_value_missing(false));
static_assert(replicated_db_commit_succeeded(1));
static_assert(replicated_db_commit_pending(0));
static_assert(replicated_db_commit_callback_state(true) == -1);
static_assert(replicated_db_commit_callback_state(false) == 1);
static_assert(replicated_db_read_found(true));
static_assert(replicated_db_is_leader(true));
static_assert(!replicated_db_snapshot_has_header(0));
static_assert(replicated_db_snapshot_has_header(1));
static_assert(replicated_db_snapshot_is_lz4(1, 1));
static_assert(replicated_db_snapshot_is_uncompressed(0, 0));
static_assert(replicated_db_snapshot_has_bytes(4, 6, 10));
// Pin the incumbent unsigned-wrap boundary rather than silently hardening the
// malformed-input path as PR #79 did. Any hardening belongs in a separate fix.
static_assert(replicated_db_snapshot_has_bytes(
    static_cast<size_t>(-1), 1, 0));
static_assert(replicated_db_snapshot_has_u64_bytes(UINT64_MAX, 1, 0));

// @safe - Plain data struct for batch operations
struct KVOperation {
    ReplicatedDBOp op;
    std::string key;
    std::string value;  // empty for DELETE
};

// Explicit registered kind. Wire payload preserved
// byte-for-byte:
//   uint8_t op | std::string key | std::string value
//   [if op == BATCH] uint32_t count
//   for each batch op: uint8_t op | std::string key | std::string value
// Construction sites use `wrap_typed_marshallable` (still works through
// the Phase 4d-prep bridge dispatch) and `marshallable_cast<T>` (also
// dispatched through the bridge for any non-Marshallable T). The
// legacy `to_marshal` / `from_marshal` member functions are gone as
// thin wrappers that build a BinaryWriteArchive/BinaryReadArchive on
// top of the archive serde surface via save/load — this
// keeps the existing test.cc round-trip sites compiling unchanged.
class ReplicatedDBCommand
    : public rrr::Serializable<
          rrr::PayloadMember<MakoCommands, ReplicatedDBCommand>::KIND> {
public:
    ReplicatedDBOp op_ = ReplicatedDBOp::PUT;
    std::string key_;
    std::string value_;
    std::vector<KVOperation> batch_ops_;  // Only used when op_ == BATCH

    ReplicatedDBCommand() = default;

    // @unsafe - Factory: creates shared_ptr (non-borrow-checked ownership)
    static rusty::Arc<ReplicatedDBCommand> CreatePut(const std::string& key, const std::string& value);
    // @unsafe - Factory: creates shared_ptr (non-borrow-checked ownership)
    static rusty::Arc<ReplicatedDBCommand> CreateDelete(const std::string& key);
    // @unsafe - Factory: creates shared_ptr (non-borrow-checked ownership)
    static rusty::Arc<ReplicatedDBCommand> CreateBatch(const std::vector<KVOperation>& ops);

    // Serializable interface.
    void save(BinaryWriteArchive& ar) const;
    void load(BinaryReadArchive& ar);

};

/**
 * ReplicatedDB - A replicated key-value store built on Raft + RocksDB.
 *
 * Write path (Put/Delete): serializes as ReplicatedDBCommand, submits through
 * Raft via Start(), blocks until commit callback fires.
 *
 * Read path (Get): reads directly from local RocksDB (stale reads).
 *
 * Apply callback (ApplyEntry): registered via RegLearnerAction, applies
 * committed entries to local RocksDB with idempotency tracking.
 */
// @unsafe - Wraps RocksDB C API pointers and Raft server interaction
class ReplicatedDB {
public:
    // @unsafe - Opens RocksDB, stores raw pointers
    ReplicatedDB(RaftServer* raft, const std::string& db_path);

    // @unsafe - Closes RocksDB
    ~ReplicatedDB();

    // Non-copyable, non-movable
    ReplicatedDB(const ReplicatedDB&) = delete;
    ReplicatedDB& operator=(const ReplicatedDB&) = delete;

    // Write operations (go through Raft)
    // @unsafe - Submits command via Raft, blocks until committed
    bool Put(const std::string& key, const std::string& value);

    // @unsafe - Submits delete command via Raft, blocks until committed
    bool Delete(const std::string& key);

    // @unsafe - Submits batch command via Raft, blocks until committed
    bool Batch(const std::vector<KVOperation>& ops);

    // Read operation (local RocksDB, stale reads)
    // @unsafe - Direct RocksDB read
    bool Get(const std::string& key, std::string* value);

    // Linearizable read via ReadIndex protocol.
    // Confirms this server is still leader and all committed entries are applied,
    // then reads from local RocksDB. Avoids writing a log entry for reads.
    // @unsafe - Calls RaftServer::ReadIndex and RocksDB read
    bool LinearizableGet(const std::string& key, std::string* value);

    // Apply callback - registered as app_next_ on RaftServer's scheduler
    // @unsafe - Applies commands to RocksDB
    // take const janus::Command&;
    // shared_ptr<Marshallable> callers auto-convert via Command's
    // implicit ctor.
    void ApplyEntry(int slot, const janus::Command& cmd);

    // @safe - Returns whether RocksDB is open
    bool IsOpen() const { return db_ != nullptr; }

    // @safe - Returns last applied Raft index
    uint64_t GetLastAppliedIndex() const { return last_applied_index_; }

    // @safe - Returns whether snapshot compression is enabled
    bool IsCompressionEnabled() const { return compression_enabled_; }

    // Snapshot support: create and load RocksDB checkpoints for Raft snapshots
    // @unsafe - Creates RocksDB checkpoint, serializes files into binary blob
    std::string CreateStateMachineSnapshot();

    // @unsafe - Deserializes binary blob, replaces current RocksDB with checkpoint
    void LoadStateMachineSnapshot(const std::string& data);

private:
    // @unsafe - Helper to free RocksDB error strings
    static std::string take_rocksdb_error(char** errptr);

    // @unsafe - Applies a single PUT to RocksDB
    void ApplyPut(const std::string& key, const std::string& value);

    // @unsafe - Applies a single DELETE to RocksDB
    void ApplyDelete(const std::string& key);

    // @unsafe - Persists last_applied_index_ to RocksDB metadata
    void PersistLastAppliedIndex();

    // @unsafe - Loads last_applied_index_ from RocksDB metadata
    void LoadLastAppliedIndex();

    // @unsafe - Closes the current RocksDB instance (for snapshot loading)
    void CloseDB();

    // @unsafe - Opens (or reopens) RocksDB at db_path_
    bool OpenDB();

    RaftServer* raft_;  // Non-owning pointer, lifetime managed externally
    rocksdb_t* db_ = nullptr;
    rocksdb_options_t* options_ = nullptr;
    rocksdb_writeoptions_t* write_options_ = nullptr;
    rocksdb_readoptions_t* read_options_ = nullptr;
    std::string db_path_;
    uint64_t last_applied_index_ = 0;
    bool compression_enabled_ = true;  // LZ4 snapshot compression (env: MAKO_SNAPSHOT_COMPRESSION)

    static constexpr const char* META_LAST_APPLIED = "__raft_last_applied__";

    // Snapshot blob header byte values
    static constexpr uint8_t SNAPSHOT_UNCOMPRESSED = 0;
    static constexpr uint8_t SNAPSHOT_LZ4 = 1;
};

} // namespace janus

// (ReplicatedDBCommand is now a Serializable — no
// TypedMarshallableAdapterTraits specialization. See replicated_db.cc
// for the `reg_serializable_in_deputy` registration. Construction
// sites use `wrap_typed_marshallable(cmd)`; cast sites use
// `marshallable_cast<ReplicatedDBCommand>(value)` — both routed
// through the Phase 4d-prep bridge overloads — the non-Marshallable
// path that wraps as `SerializableMarshallableAdapter`.)
