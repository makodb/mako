#pragma once
#include "__dep__.h"
#include "rrr/rrr.hpp"
#include "../mako_commands.h"
#include "rusty/cell.hpp"
#include "rusty/rusty.hpp"
#include "rusty/slice.hpp"
#include <string>
#include <vector>
#include <atomic>
#include <functional>
#include <rocksdb/c.h>
#include "lz4.h"

namespace janus {

class RaftServer;  // Forward declaration

// @safe - Trivially copyable enum for operation type
#if RUSTYCPP_RUST
#[repr(u8)]
pub enum ReplicatedDBOp {
    PUT = 1,
    DELETE = 2,
    BATCH = 3,
}

#[repr(u8)]
pub enum ReplicatedDBApplyAction {
    UNKNOWN = 0,
    PUT = 1,
    DELETE = 2,
    BATCH = 3,
}
#endif
/*RUSTYCPP:GEN-BEGIN id=replicated_db.op version=1 rust_sha256=46fc9e0a3dc549b036880df4121cd58e1ab750266c5e667a0c0eee5a67b13286*/
enum class ReplicatedDBOp : uint8_t;
inline constexpr ReplicatedDBOp ReplicatedDBOp_PUT();
inline constexpr ReplicatedDBOp ReplicatedDBOp_DELETE();
inline constexpr ReplicatedDBOp ReplicatedDBOp_BATCH();
enum class ReplicatedDBApplyAction;
inline constexpr ReplicatedDBApplyAction ReplicatedDBApplyAction_UNKNOWN();
inline constexpr ReplicatedDBApplyAction ReplicatedDBApplyAction_PUT();
inline constexpr ReplicatedDBApplyAction ReplicatedDBApplyAction_DELETE();
inline constexpr ReplicatedDBApplyAction ReplicatedDBApplyAction_BATCH();

enum class ReplicatedDBOp : uint8_t {
    PUT = 1,
    DELETE = 2,
    BATCH = 3
};
inline constexpr ReplicatedDBOp ReplicatedDBOp_PUT() { return ReplicatedDBOp::PUT; }
inline constexpr ReplicatedDBOp ReplicatedDBOp_DELETE() { return ReplicatedDBOp::DELETE; }
inline constexpr ReplicatedDBOp ReplicatedDBOp_BATCH() { return ReplicatedDBOp::BATCH; }

enum class ReplicatedDBApplyAction {
    UNKNOWN = 0,
    PUT = 1,
    DELETE = 2,
    BATCH = 3
};
inline constexpr ReplicatedDBApplyAction ReplicatedDBApplyAction_UNKNOWN() { return ReplicatedDBApplyAction::UNKNOWN; }
inline constexpr ReplicatedDBApplyAction ReplicatedDBApplyAction_PUT() { return ReplicatedDBApplyAction::PUT; }
inline constexpr ReplicatedDBApplyAction ReplicatedDBApplyAction_DELETE() { return ReplicatedDBApplyAction::DELETE; }
inline constexpr ReplicatedDBApplyAction ReplicatedDBApplyAction_BATCH() { return ReplicatedDBApplyAction::BATCH; }
/*RUSTYCPP:GEN-END id=replicated_db.op*/

struct KVOperation;

inline KVOperation kv_operation_defaults();
inline KVOperation kv_operation_make(ReplicatedDBOp op,
                                     const std::string& key,
                                     const std::string& value);

#if RUSTYCPP_RUST
pub struct KVOperation {
    op: janus::ReplicatedDBOp,
    key: std::string,
    value: std::string,
}

impl KVOperation {
    fn defaults() -> KVOperation {
        kv_operation_defaults()
    }

    fn make(op: janus::ReplicatedDBOp,
            key: std::string,
            value: std::string) -> KVOperation {
        kv_operation_make(op, key, value)
    }

    fn put(key: std::string, value: std::string) -> KVOperation {
        kv_operation_make(janus::ReplicatedDBOp::PUT, key, value)
    }

    fn delete_key(key: std::string) -> KVOperation {
        kv_operation_make(janus::ReplicatedDBOp::DELETE, key, "")
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=replicated_db.1 version=1 rust_sha256=5536196124b0b9fe676d3704d9f3fcec6fa296040c8bc0835f6bb27355a23e2a*/
struct KVOperation;

struct KVOperation {
    janus::ReplicatedDBOp op;
    std::string key;
    std::string value;

    static KVOperation defaults();
    static KVOperation make(janus::ReplicatedDBOp op, std::string key, std::string value);
    static KVOperation put(std::string key, std::string value);
    static KVOperation delete_key(std::string key);
};


inline KVOperation KVOperation::defaults() {
    return kv_operation_defaults();
}

inline KVOperation KVOperation::make(janus::ReplicatedDBOp op, std::string key, std::string value) {
    return kv_operation_make(std::move(op), std::move(key), std::move(value));
}

inline KVOperation KVOperation::put(std::string key, std::string value) {
    return kv_operation_make(rusty::clone(rusty::clone(janus::ReplicatedDBOp::PUT)), std::move(key), std::move(value));
}

inline KVOperation KVOperation::delete_key(std::string key) {
    return kv_operation_make(rusty::clone(rusty::clone(janus::ReplicatedDBOp::DELETE)), std::move(key), "");
}
/*RUSTYCPP:GEN-END id=replicated_db.1*/

inline KVOperation kv_operation_defaults() {
    KVOperation kv_op{};
    kv_op.op = ReplicatedDBOp::PUT;
    kv_op.key = "";
    kv_op.value = "";
    return kv_op;
}

inline KVOperation kv_operation_make(ReplicatedDBOp op,
                                     const std::string& key,
                                     const std::string& value) {
    KVOperation kv_op = kv_operation_defaults();
    kv_op.op = op;
    kv_op.key = key;
    kv_op.value = value;
    return kv_op;
}

// TypeList-derived kind. Wire payload preserved
// byte-for-byte:
//   uint8_t op | std::string key | std::string value
//   [if op == BATCH] uint32_t count
//   for each batch op: uint8_t op | std::string key | std::string value
// Construction sites use `wrap_typed_marshallable` (still works through
// the Phase 4d-prep bridge dispatch) and `marshallable_cast<T>` (also
// dispatched through the bridge for any non-Marshallable T). The
// legacy `to_marshal` / `from_marshal` member functions are kept as
// thin wrappers that build a BinaryWriteArchive/BinaryReadArchive on
// top of a MarshalSink/MarshalSource and delegate to save/load — this
// keeps the existing test.cc round-trip sites compiling unchanged.

class ReplicatedDBCommand : public rrr::Serializable<ReplicatedDBCommand,
                                                     MakoCommands> {
public:
    ReplicatedDBOp op_ = ReplicatedDBOp::PUT;
    std::string key_;
    std::string value_;
    std::vector<KVOperation> batch_ops_;  // Only used when op_ == BATCH

    ReplicatedDBCommand() = default;

    // @unsafe - Factory: creates shared_ptr (non-borrow-checked ownership)
    static shared_ptr<ReplicatedDBCommand> CreatePut(const std::string& key, const std::string& value);
    // @unsafe - Factory: creates shared_ptr (non-borrow-checked ownership)
    static shared_ptr<ReplicatedDBCommand> CreateDelete(const std::string& key);
    // @unsafe - Factory: creates shared_ptr (non-borrow-checked ownership)
    static shared_ptr<ReplicatedDBCommand> CreateBatch(const std::vector<KVOperation>& ops);

    // Serializable interface.
    void save(BinaryWriteArchive& ar) const;
    void load(BinaryReadArchive& ar);

    // Legacy Marshal-based round-trip wrappers — kept for
    // test sites that exercise the on-wire encoding directly. They
    // delegate to save/load via MarshalSink/MarshalSource so the bytes
    // are byte-for-byte identical to the pre-migration encoding.
    Marshal& to_marshal(Marshal& m) const;
    Marshal& from_marshal(Marshal& m);
};

ReplicatedDBCommand replicated_db_command_defaults();
ReplicatedDBCommand replicated_db_command_put(const std::string& key,
                                              const std::string& value);
ReplicatedDBCommand replicated_db_command_delete(const std::string& key);
ReplicatedDBCommand replicated_db_command_batch(const std::vector<KVOperation>& ops);

#if RUSTYCPP_RUST
pub fn replicated_db_command_defaults_dsl() -> janus::ReplicatedDBCommand {
    replicated_db_command_defaults()
}

pub fn replicated_db_command_put_dsl(key: std::string,
                                     value: std::string) -> janus::ReplicatedDBCommand {
    replicated_db_command_put(key, value)
}

pub fn replicated_db_command_delete_dsl(key: std::string) -> janus::ReplicatedDBCommand {
    replicated_db_command_delete(key)
}

pub fn replicated_db_command_batch_dsl(
    ops: std::vector<janus::KVOperation>
) -> janus::ReplicatedDBCommand {
    replicated_db_command_batch(ops)
}
#endif
/*RUSTYCPP:GEN-BEGIN id=replicated_db.2 version=1 rust_sha256=a5e09d09cab6ae0917950fca55c2e545f2edc220fd93a5910a733bd2799dae53*/
inline janus::ReplicatedDBCommand replicated_db_command_defaults_dsl() {
    return replicated_db_command_defaults();
}

inline janus::ReplicatedDBCommand replicated_db_command_put_dsl(std::string key, std::string value) {
    return replicated_db_command_put(std::move(key), std::move(value));
}

inline janus::ReplicatedDBCommand replicated_db_command_delete_dsl(std::string key) {
    return replicated_db_command_delete(std::move(key));
}

inline janus::ReplicatedDBCommand replicated_db_command_batch_dsl(std::vector<janus::KVOperation> ops) {
    return replicated_db_command_batch(std::move(ops));
}
/*RUSTYCPP:GEN-END id=replicated_db.2*/

#if RUSTYCPP_RUST
pub fn replicated_db_has_command_payload(has_value: bool) -> bool {
    has_value
}

pub fn replicated_db_should_skip_applied(index: u64,
                                         last_applied_index: u64) -> bool {
    index <= last_applied_index
}

pub fn replicated_db_command_kind_matches(kind: i32, expected_kind: i32) -> bool {
    kind == expected_kind
}

pub fn replicated_db_command_is_put(op: janus::ReplicatedDBOp) -> bool {
    op == janus::ReplicatedDBOp::PUT
}

pub fn replicated_db_command_is_delete(op: janus::ReplicatedDBOp) -> bool {
    op == janus::ReplicatedDBOp::DELETE
}

pub fn replicated_db_command_is_batch(op: janus::ReplicatedDBOp) -> bool {
    op == janus::ReplicatedDBOp::BATCH
}

pub fn replicated_db_command_should_encode_batch(op: janus::ReplicatedDBOp,
                                                 batch_count: u64) -> bool {
    op == janus::ReplicatedDBOp::BATCH && batch_count > 0
}

pub fn replicated_db_command_kind_is_known(op: janus::ReplicatedDBOp) -> bool {
    op == janus::ReplicatedDBOp::PUT ||
        op == janus::ReplicatedDBOp::DELETE ||
        op == janus::ReplicatedDBOp::BATCH
}

pub fn replicated_db_command_apply_action(
    op: janus::ReplicatedDBOp
) -> janus::ReplicatedDBApplyAction {
    if op == janus::ReplicatedDBOp::PUT {
        janus::ReplicatedDBApplyAction::PUT
    } else if op == janus::ReplicatedDBOp::DELETE {
        janus::ReplicatedDBApplyAction::DELETE
    } else if op == janus::ReplicatedDBOp::BATCH {
        janus::ReplicatedDBApplyAction::BATCH
    } else {
        janus::ReplicatedDBApplyAction::UNKNOWN
    }
}

pub fn replicated_db_can_submit(has_db: bool,
                                has_raft: bool,
                                has_ops: bool) -> bool {
    has_db && has_raft && has_ops
}

pub fn replicated_db_commit_succeeded(commit_state: i32) -> bool {
    commit_state > 0
}

pub fn replicated_db_commit_pending(commit_state: i32) -> bool {
    commit_state == 0
}

pub fn replicated_db_commit_callback_state(rolled_back: bool) -> i32 {
    if rolled_back {
        -1
    } else {
        1
    }
}

pub fn replicated_db_can_get(has_db: bool, has_value_out: bool) -> bool {
    has_db && has_value_out
}

pub fn replicated_db_read_found(has_value_ptr: bool) -> bool {
    has_value_ptr
}

pub fn replicated_db_can_linearizable_read(has_raft: bool,
                                           is_leader: bool) -> bool {
    has_raft && is_leader
}

pub fn replicated_db_snapshot_has_header(size: usize) -> bool {
    size >= 1
}

pub fn replicated_db_snapshot_is_lz4(compression: u8,
                                     lz4_tag: u8) -> bool {
    compression == lz4_tag
}

pub fn replicated_db_snapshot_is_uncompressed(compression: u8,
                                              uncompressed_tag: u8) -> bool {
    compression == uncompressed_tag
}

pub fn replicated_db_snapshot_has_bytes(offset: usize,
                                        needed: usize,
                                        total: usize) -> bool {
    offset <= total && needed <= total - offset
}
#endif
/*RUSTYCPP:GEN-BEGIN id=replicated_db.command_helpers version=1 rust_sha256=f9bb817d26f5837b19180aaa0770c50a7d4d2de755610f15e43f9669b23e15cf*/
inline bool replicated_db_has_command_payload(bool has_value);
inline bool replicated_db_should_skip_applied(uint64_t index, uint64_t last_applied_index);
inline bool replicated_db_command_kind_matches(int32_t kind, int32_t expected_kind);
inline bool replicated_db_can_submit(bool has_db, bool has_raft, bool has_ops);
inline bool replicated_db_commit_succeeded(int32_t commit_state);
inline bool replicated_db_commit_pending(int32_t commit_state);
inline int32_t replicated_db_commit_callback_state(bool rolled_back);
inline bool replicated_db_can_get(bool has_db, bool has_value_out);
inline bool replicated_db_read_found(bool has_value_ptr);
inline bool replicated_db_can_linearizable_read(bool has_raft, bool is_leader);
inline bool replicated_db_snapshot_has_header(size_t size);
inline bool replicated_db_snapshot_is_lz4(uint8_t compression, uint8_t lz4_tag);
inline bool replicated_db_snapshot_is_uncompressed(uint8_t compression, uint8_t uncompressed_tag);
inline bool replicated_db_snapshot_has_bytes(size_t offset, size_t needed, size_t total);

inline bool replicated_db_has_command_payload(bool has_value) {
    return has_value;
}

inline bool replicated_db_should_skip_applied(uint64_t index, uint64_t last_applied_index) {
    return index <= last_applied_index;
}

inline bool replicated_db_command_kind_matches(int32_t kind, int32_t expected_kind) {
    return kind == expected_kind;
}

inline bool replicated_db_command_is_put(janus::ReplicatedDBOp op) {
    return op == janus::ReplicatedDBOp::PUT;
}

inline bool replicated_db_command_is_delete(janus::ReplicatedDBOp op) {
    return op == janus::ReplicatedDBOp::DELETE;
}

inline bool replicated_db_command_is_batch(janus::ReplicatedDBOp op) {
    return op == janus::ReplicatedDBOp::BATCH;
}

inline bool replicated_db_command_should_encode_batch(janus::ReplicatedDBOp op, uint64_t batch_count) {
    return (rusty::detail::deref_if_pointer_like(op) == rusty::clone(janus::ReplicatedDBOp::BATCH)) && (rusty::detail::deref_if_pointer_like(batch_count) > 0);
}

inline bool replicated_db_command_kind_is_known(janus::ReplicatedDBOp op) {
    return ((rusty::detail::deref_if_pointer_like(op) == rusty::clone(janus::ReplicatedDBOp::PUT)) || (rusty::detail::deref_if_pointer_like(op) == rusty::clone(janus::ReplicatedDBOp::DELETE))) || (rusty::detail::deref_if_pointer_like(op) == rusty::clone(janus::ReplicatedDBOp::BATCH));
}

inline janus::ReplicatedDBApplyAction replicated_db_command_apply_action(janus::ReplicatedDBOp op) {
    if (rusty::detail::deref_if_pointer_like(op) == rusty::clone(janus::ReplicatedDBOp::PUT)) {
        return rusty::clone(rusty::clone(janus::ReplicatedDBApplyAction::PUT));
    } else if (rusty::detail::deref_if_pointer_like(op) == rusty::clone(janus::ReplicatedDBOp::DELETE)) {
        return rusty::clone(rusty::clone(janus::ReplicatedDBApplyAction::DELETE));
    } else if (rusty::detail::deref_if_pointer_like(op) == rusty::clone(janus::ReplicatedDBOp::BATCH)) {
        return rusty::clone(rusty::clone(janus::ReplicatedDBApplyAction::BATCH));
    } else {
        return rusty::clone(rusty::clone(janus::ReplicatedDBApplyAction::UNKNOWN));
    }
}

inline bool replicated_db_can_submit(bool has_db, bool has_raft, bool has_ops) {
    return (rusty::detail::deref_if_pointer_like(has_db) && rusty::detail::deref_if_pointer_like(has_raft)) && rusty::detail::deref_if_pointer_like(has_ops);
}

inline bool replicated_db_commit_succeeded(int32_t commit_state) {
    return rusty::detail::deref_if_pointer_like(commit_state) > 0;
}

inline bool replicated_db_commit_pending(int32_t commit_state) {
    return rusty::detail::deref_if_pointer_like(commit_state) == static_cast<int32_t>(0);
}

inline int32_t replicated_db_commit_callback_state(bool rolled_back) {
    if (rolled_back) {
        return -1;
    } else {
        return static_cast<int32_t>(1);
    }
}

inline bool replicated_db_can_get(bool has_db, bool has_value_out) {
    return rusty::detail::deref_if_pointer_like(has_db) && rusty::detail::deref_if_pointer_like(has_value_out);
}

inline bool replicated_db_read_found(bool has_value_ptr) {
    return std::move(has_value_ptr);
}

inline bool replicated_db_can_linearizable_read(bool has_raft, bool is_leader) {
    return rusty::detail::deref_if_pointer_like(has_raft) && rusty::detail::deref_if_pointer_like(is_leader);
}

inline bool replicated_db_snapshot_has_header(size_t size) {
    return rusty::detail::deref_if_pointer_like(size) >= 1;
}

inline bool replicated_db_snapshot_is_lz4(uint8_t compression, uint8_t lz4_tag) {
    return rusty::detail::deref_if_pointer_like(compression) == rusty::detail::deref_if_pointer_like(lz4_tag);
}

inline bool replicated_db_snapshot_is_uncompressed(uint8_t compression, uint8_t uncompressed_tag) {
    return rusty::detail::deref_if_pointer_like(compression) == rusty::detail::deref_if_pointer_like(uncompressed_tag);
}

inline bool replicated_db_snapshot_has_bytes(size_t offset, size_t needed, size_t total) {
    return (rusty::detail::deref_if_pointer_like(offset) <= rusty::detail::deref_if_pointer_like(total)) && (rusty::detail::deref_if_pointer_like(needed) <= (rusty::detail::deref_if_pointer_like(total) - rusty::detail::deref_if_pointer_like(offset)));
}
/*RUSTYCPP:GEN-END id=replicated_db.command_helpers*/

#if RUSTYCPP_RUST
pub struct ReplicatedDBStateCore {
    last_applied_index_: rusty::Cell<u64>,
    compression_enabled_: rusty::Cell<bool>,
}

impl ReplicatedDBStateCore {
    // @safe
    fn new() -> ReplicatedDBStateCore {
        ReplicatedDBStateCore {
            last_applied_index_: rusty::Cell::<u64>::new_(0),
            compression_enabled_: rusty::Cell::<bool>::new_(true),
        }
    }

    // @safe
    fn last_applied_index(&self) -> u64 {
        self.last_applied_index_.get()
    }

    // @safe
    fn set_last_applied_index(&mut self, value: u64) {
        self.last_applied_index_.set(value)
    }

    // @safe
    fn compression_enabled(&self) -> bool {
        self.compression_enabled_.get()
    }

    // @safe
    fn set_compression_enabled(&mut self, value: bool) {
        self.compression_enabled_.set(value)
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=replicated_db.state_core version=1 rust_sha256=a6209d25e2a81b13f7ea677e1a28ba0e38a62bdea2c81b0e47336226bbb55996*/
struct ReplicatedDBStateCore;

struct ReplicatedDBStateCore {
    rusty::Cell<uint64_t> last_applied_index_;
    rusty::Cell<bool> compression_enabled_;

    static ReplicatedDBStateCore new_();
    uint64_t last_applied_index() const;
    void set_last_applied_index(uint64_t value);
    bool compression_enabled() const;
    void set_compression_enabled(bool value);
};


inline ReplicatedDBStateCore ReplicatedDBStateCore::new_() {
    return ReplicatedDBStateCore{.last_applied_index_ = rusty::Cell<uint64_t>::new_(static_cast<uint64_t>(0)), .compression_enabled_ = rusty::Cell<bool>::new_(true)};
}

inline uint64_t ReplicatedDBStateCore::last_applied_index() const {
    return this->last_applied_index_.get();
}

inline void ReplicatedDBStateCore::set_last_applied_index(uint64_t value) {
    this->last_applied_index_.set(std::move(value));
}

inline bool ReplicatedDBStateCore::compression_enabled() const {
    return this->compression_enabled_.get();
}

inline void ReplicatedDBStateCore::set_compression_enabled(bool value) {
    this->compression_enabled_.set(std::move(value));
}
/*RUSTYCPP:GEN-END id=replicated_db.state_core*/

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
// @unsafe - wraps RocksDB C API pointers and a borrowed RaftServer. This is a
// storage boundary, not an early DSL candidate.
class ReplicatedDB {
public:
    // @unsafe - Allocates RocksDB option handles and opens db_path_.
    ReplicatedDB(RaftServer* raft, const std::string& db_path);

    // @unsafe - Closes db_ if open and destroys RocksDB option handles.
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
    uint64_t GetLastAppliedIndex() const { return state_core_.last_applied_index(); }

    // @safe - Returns whether snapshot compression is enabled
    bool IsCompressionEnabled() const { return state_core_.compression_enabled(); }

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

    // @unsafe - Dispatches a DSL-classified command into RocksDB apply helpers.
    void ApplyCommand(const ReplicatedDBCommand& db_cmd, uint64_t index);

    // @unsafe - Dispatches one batch operation into RocksDB apply helpers.
    void ApplyBatchOperation(const KVOperation& op);

    // @unsafe - Persists last_applied_index_ to RocksDB metadata
    void PersistLastAppliedIndex();

    // @unsafe - Loads last_applied_index_ from RocksDB metadata
    void LoadLastAppliedIndex();

    // @unsafe - Closes db_ for snapshot loading; option handles stay alive.
    void CloseDB();

    // @unsafe - Reopens db_path_ with the constructor-owned option handles.
    bool OpenDB();

    // @unsafe - borrowed RaftServer; ReplicatedDB registers callbacks but does
    // not own or delete the server.
    RaftServer* raft_;
    // @unsafe - RocksDB database handle. The constructor/OpenDB open it;
    // CloseDB/destructor close it.
    rocksdb_t* db_ = nullptr;
    // @unsafe - RocksDB option handles. The constructor creates these and the
    // destructor destroys them. CloseDB intentionally leaves them alive so
    // LoadStateMachineSnapshot can reopen db_ with the same configuration.
    rocksdb_options_t* options_ = nullptr;
    rocksdb_writeoptions_t* write_options_ = nullptr;
    rocksdb_readoptions_t* read_options_ = nullptr;
    std::string db_path_;
    ReplicatedDBStateCore state_core_;

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
