#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "replicated_db.h"
#include "server.h"
#include "rrr/misc/serializable.hpp"
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

import std;

using namespace janus;

// ReplicatedDBCommand migrated from
// TypedMarshallableAdapter to Serializable. Registration switched
// to `rrr::reg_serializable_in_deputy<T>` (replaces
// `MarshallDeputy::reg_initializer<T>`). Wire format byte-for-byte
// identical: the Marshal-based to_marshal/from_marshal wrappers below
// delegate to save/load via a MarshalSink/MarshalSource bridge, and
// bridge-dispatched `wrap_typed_marshallable` / `marshallable_cast<T>`
// keep the legacy call sites working unchanged.
// registration switched to no-arg form — kind
// auto-derived from `Serializable<T, MakoCommands>` CRTP base.
static int volatile x_replicated_db =
    rrr::SerializableRegistry::reg<ReplicatedDBCommand>();

namespace janus {

#if RUSTYCPP_RUST
pub struct ReplicatedDBFileEntry {
    name: std::string,
    contents: std::string,
}
#endif
/*RUSTYCPP:GEN-BEGIN id=replicated_db.1 version=1 rust_sha256=42b62c5a2a282f767c764d1425abc18fbe0678b2643b518e63f1f45c8552278f*/
struct ReplicatedDBFileEntry;

struct ReplicatedDBFileEntry {
    std::string name;
    std::string contents;
};
/*RUSTYCPP:GEN-END id=replicated_db.1*/

}  // namespace janus

// ===========================================================================
// ReplicatedDBCommand factory methods and serialization
// ===========================================================================

ReplicatedDBCommand janus::replicated_db_command_defaults() {
  ReplicatedDBCommand cmd;
  cmd.op_ = ReplicatedDBOp::PUT;
  cmd.key_ = "";
  cmd.value_ = "";
  cmd.batch_ops_.clear();
  return cmd;
}

ReplicatedDBCommand janus::replicated_db_command_put(const std::string& key,
                                                     const std::string& value) {
  ReplicatedDBCommand cmd = replicated_db_command_defaults();
  cmd.op_ = ReplicatedDBOp::PUT;
  cmd.key_ = key;
  cmd.value_ = value;
  cmd.batch_ops_.clear();
  return cmd;
}

ReplicatedDBCommand janus::replicated_db_command_delete(const std::string& key) {
  ReplicatedDBCommand cmd = replicated_db_command_defaults();
  cmd.op_ = ReplicatedDBOp::DELETE;
  cmd.key_ = key;
  cmd.value_ = "";
  cmd.batch_ops_.clear();
  return cmd;
}

ReplicatedDBCommand janus::replicated_db_command_batch(const std::vector<KVOperation>& ops) {
  ReplicatedDBCommand cmd = replicated_db_command_defaults();
  cmd.op_ = ReplicatedDBOp::BATCH;
  cmd.key_ = "";
  cmd.value_ = "";
  cmd.batch_ops_ = ops;
  return cmd;
}

// @unsafe - Creates shared_ptr (non-borrow-checked ownership)
shared_ptr<ReplicatedDBCommand> ReplicatedDBCommand::CreatePut(
    const std::string& key, const std::string& value) {
  return std::make_shared<ReplicatedDBCommand>(
      replicated_db_command_put(key, value));
}

// @unsafe - Creates shared_ptr (non-borrow-checked ownership)
shared_ptr<ReplicatedDBCommand> ReplicatedDBCommand::CreateDelete(
    const std::string& key) {
  return std::make_shared<ReplicatedDBCommand>(
      replicated_db_command_delete(key));
}

// @unsafe - Creates shared_ptr (non-borrow-checked ownership)
shared_ptr<ReplicatedDBCommand> ReplicatedDBCommand::CreateBatch(
    const std::vector<KVOperation>& ops) {
  return std::make_shared<ReplicatedDBCommand>(
      replicated_db_command_batch(ops));
}

// Serializable save/load — moved here from
// to_marshal/from_marshal. Wire format is byte-for-byte identical;
// the BinaryWriteArchive/BinaryReadArchive `<<`/`>>` overloads for
// uint8_t / uint32_t / std::string match the legacy Marshal encoding.
void ReplicatedDBCommand::save(BinaryWriteArchive& ar) const {
  ar << static_cast<uint8_t>(op_);
  ar << key_;
  ar << value_;
  if (op_ == ReplicatedDBOp::BATCH) {
    uint32_t count = static_cast<uint32_t>(batch_ops_.size());
    ar << count;
    for (const auto& op : batch_ops_) {
      ar << static_cast<uint8_t>(op.op);
      ar << op.key;
      ar << op.value;
    }
  }
}

void ReplicatedDBCommand::load(BinaryReadArchive& ar) {
  uint8_t op_val;
  ar >> op_val;
  op_ = static_cast<ReplicatedDBOp>(op_val);
  ar >> key_;
  ar >> value_;
  if (op_ == ReplicatedDBOp::BATCH) {
    uint32_t count;
    ar >> count;
    batch_ops_.resize(count);
    for (uint32_t i = 0; i < count; i++) {
      uint8_t sub_op_val;
      ar >> sub_op_val;
      batch_ops_[i].op = static_cast<ReplicatedDBOp>(sub_op_val);
      ar >> batch_ops_[i].key;
      ar >> batch_ops_[i].value;
    }
  }
}

// 1 legacy wrappers: keep the existing test.cc call sites
// (cmd->to_marshal(m) / cmd2->from_marshal(m)) compiling. Each wrapper
// builds a single-use BinaryWriteArchive/BinaryReadArchive on top of a
// MarshalSink/MarshalSource adapter and delegates to save/load.
// @unsafe - Marshal I/O is not borrow-checked
Marshal& ReplicatedDBCommand::to_marshal(Marshal& m) const {
  rrr::MarshalSink sink(&m);
  rrr::BinaryWriteArchive ar(rrr::make_sink_proxy(&sink));
  save(ar);
  return m;
}

// @unsafe - Marshal I/O is not borrow-checked
Marshal& ReplicatedDBCommand::from_marshal(Marshal& m) {
  rrr::MarshalSource source(&m);
  rrr::BinaryReadArchive ar(rrr::make_source_proxy(&source));
  load(ar);
  return m;
}

// ===========================================================================
// ReplicatedDB implementation
// ===========================================================================

// @unsafe - Allocates RocksDB option handles and opens db_path_.
ReplicatedDB::ReplicatedDB(RaftServer* raft, const std::string& db_path)
    : raft_(raft), db_path_(db_path), state_core_(ReplicatedDBStateCore::new_()) {
  // @unsafe { std::getenv is not borrow-checked }
  const char* comp_env = std::getenv("MAKO_SNAPSHOT_COMPRESSION");
  if (comp_env && std::strcmp(comp_env, "0") == 0) {
    state_core_.set_compression_enabled(false);
  }

  // @unsafe - Owns these C handles until the destructor.
  options_ = rocksdb_options_create();
  write_options_ = rocksdb_writeoptions_create();
  read_options_ = rocksdb_readoptions_create();

  if (options_ == nullptr || write_options_ == nullptr || read_options_ == nullptr) {
    Log_error("[ReplicatedDB] Failed to allocate RocksDB C API options");
    return;
  }

  // Configure RocksDB options
  rocksdb_options_set_create_if_missing(options_, 1);
  rocksdb_options_set_max_open_files(options_, 256);
  rocksdb_options_set_write_buffer_size(options_, 64 * 1024 * 1024);  // 64MB

  // Write options - no sync for performance (Raft provides durability)
  rocksdb_writeoptions_set_sync(write_options_, 0);

  // Read options
  rocksdb_readoptions_set_verify_checksums(read_options_, 1);

  // Open the database handle. Snapshot loading can later close/reopen only
  // db_, while keeping the option handles above alive.
  char* err = nullptr;
  db_ = rocksdb_open(options_, db_path_.c_str(), &err);
  if (err != nullptr || db_ == nullptr) {
    std::string err_str = take_rocksdb_error(&err);
    Log_error("[ReplicatedDB] Failed to open %s: %s",
              db_path_.c_str(), err_str.empty() ? "null handle" : err_str.c_str());
    db_ = nullptr;
    return;
  }

  // Load last applied index from metadata
  LoadLastAppliedIndex();

  // Register snapshot callbacks on the RaftServer
  // @unsafe { captures 'this' in lambdas for callback }
  if (raft_) {
    raft_->SetStateMachineSnapshotCallbacks(
        [this]() { return CreateStateMachineSnapshot(); },
        [this](const std::string& snap_data) { LoadStateMachineSnapshot(snap_data); });
  }

  Log_info("[ReplicatedDB] Opened database at %s, last_applied_index=%lu",
           db_path_.c_str(), state_core_.last_applied_index());
}

// @unsafe - Closes db_ if open and destroys constructor-owned option handles.
ReplicatedDB::~ReplicatedDB() {
  if (db_ != nullptr) {
    rocksdb_close(db_);
    db_ = nullptr;
  }
  if (read_options_ != nullptr) {
    rocksdb_readoptions_destroy(read_options_);
    read_options_ = nullptr;
  }
  if (write_options_ != nullptr) {
    rocksdb_writeoptions_destroy(write_options_);
    write_options_ = nullptr;
  }
  if (options_ != nullptr) {
    rocksdb_options_destroy(options_);
    options_ = nullptr;
  }
}

// @unsafe - Submits PUT command through Raft, blocks until committed
bool ReplicatedDB::Put(const std::string& key, const std::string& value) {
  if (!replicated_db_can_submit(db_ != nullptr, raft_ != nullptr, true)) return false;

  auto cmd = ReplicatedDBCommand::CreatePut(key, value);

  uint64_t index = 0, term = 0;
  bool is_leader = raft_->Start(cmd, &index, &term);
  if (!is_leader) {
    Log_debug("[ReplicatedDB] Put failed: not leader");
    return false;
  }

  // Block until committed using atomic flag
  // @unsafe - atomic operations and callback registration
  std::atomic<int> committed{0};  // 0=pending, 1=success, -1=rolled back
  raft_->RegisterCommitCallback(index, [&committed](CommitStatus status) {
    committed.store(status == CommitStatus::ROLLEDBACK ? -1 : 1);
  });

  // Poll until callback fires
  while (replicated_db_commit_pending(committed.load())) {
    usleep(100);  // 100us poll interval
  }

  return replicated_db_commit_succeeded(committed.load());
}

// @unsafe - Submits DELETE command through Raft, blocks until committed
bool ReplicatedDB::Delete(const std::string& key) {
  if (!replicated_db_can_submit(db_ != nullptr, raft_ != nullptr, true)) return false;

  auto cmd = ReplicatedDBCommand::CreateDelete(key);

  uint64_t index = 0, term = 0;
  bool is_leader = raft_->Start(cmd, &index, &term);
  if (!is_leader) {
    Log_debug("[ReplicatedDB] Delete failed: not leader");
    return false;
  }

  // Block until committed using atomic flag
  // @unsafe - atomic operations and callback registration
  std::atomic<int> committed{0};
  raft_->RegisterCommitCallback(index, [&committed](CommitStatus status) {
    committed.store(status == CommitStatus::ROLLEDBACK ? -1 : 1);
  });

  while (replicated_db_commit_pending(committed.load())) {
    usleep(100);
  }

  return replicated_db_commit_succeeded(committed.load());
}

// @unsafe - Submits BATCH command through Raft, blocks until committed
bool ReplicatedDB::Batch(const std::vector<KVOperation>& ops) {
  if (!replicated_db_can_submit(db_ != nullptr, raft_ != nullptr, !ops.empty())) return false;

  auto cmd = ReplicatedDBCommand::CreateBatch(ops);

  uint64_t index = 0, term = 0;
  bool is_leader = raft_->Start(cmd, &index, &term);
  if (!is_leader) {
    Log_debug("[ReplicatedDB] Batch failed: not leader");
    return false;
  }

  // Block until committed using atomic flag
  // @unsafe - atomic operations and callback registration
  std::atomic<int> committed{0};
  raft_->RegisterCommitCallback(index, [&committed](CommitStatus status) {
    committed.store(status == CommitStatus::ROLLEDBACK ? -1 : 1);
  });

  while (replicated_db_commit_pending(committed.load())) {
    usleep(100);
  }

  return replicated_db_commit_succeeded(committed.load());
}

// @unsafe - Direct RocksDB read (stale read, no Raft involvement)
bool ReplicatedDB::Get(const std::string& key, std::string* value) {
  if (!replicated_db_can_get(db_ != nullptr, value != nullptr)) return false;

  size_t value_len = 0;
  char* err = nullptr;
  char* value_ptr = rocksdb_get(db_, read_options_,
                                key.data(), key.size(),
                                &value_len, &err);
  if (err != nullptr) {
    std::string err_str = take_rocksdb_error(&err);
    Log_error("[ReplicatedDB] Get error for key '%s': %s",
              key.c_str(), err_str.c_str());
    return false;
  }
  if (!replicated_db_read_found(value_ptr != nullptr)) {
    return false;  // Key not found
  }

  *value = std::string(value_ptr, value_len);
  rocksdb_free(value_ptr);
  return true;
}

// @unsafe - Linearizable read via ReadIndex protocol
bool ReplicatedDB::LinearizableGet(const std::string& key, std::string* value) {
  if (!replicated_db_can_linearizable_read(raft_ != nullptr,
                                           raft_ != nullptr && raft_->IsLeader())) {
    Log_warn("[REPLICATED-DB] LinearizableGet: not leader");
    return false;
  }

  if (!raft_->ReadIndex(5000000)) {  // 5 second timeout
    Log_warn("[REPLICATED-DB] LinearizableGet: ReadIndex failed");
    return false;
  }

  // Safe to read from local RocksDB - we confirmed leadership and
  // all committed entries are applied to the state machine
  return Get(key, value);
}

// @unsafe - Applies committed Raft entries to local RocksDB
void ReplicatedDB::ApplyEntry(int slot, const janus::Command& cmd) {
  if (!db_ || !replicated_db_has_command_payload(cmd.has_value())) return;

  uint64_t index = static_cast<uint64_t>(slot);

  // Idempotency: skip already-applied entries
  if (replicated_db_should_skip_applied(index, state_core_.last_applied_index())) {
    return;
  }

  // Only process ReplicatedDBCommand entries
  if (!replicated_db_command_kind_matches(cmd.kind_,
                                          ReplicatedDBCommand::static_kind())) {
    // Not our command type; still advance the index to avoid re-processing
    state_core_.set_last_applied_index(index);
    PersistLastAppliedIndex();
    return;
  }

  auto db_cmd = marshallable_cast<ReplicatedDBCommand>(cmd);
  if (!db_cmd) {
    Log_error("[ReplicatedDB] Failed to cast payload to ReplicatedDBCommand at index %lu", index);
    state_core_.set_last_applied_index(index);
    PersistLastAppliedIndex();
    return;
  }

  // Apply the operation
  if (replicated_db_command_is_put(db_cmd->op_)) {
    ApplyPut(db_cmd->key_, db_cmd->value_);
  } else if (replicated_db_command_is_delete(db_cmd->op_)) {
    ApplyDelete(db_cmd->key_);
  } else if (replicated_db_command_is_batch(db_cmd->op_)) {
    for (const auto& op : db_cmd->batch_ops_) {
      if (replicated_db_command_is_put(op.op)) {
        ApplyPut(op.key, op.value);
      } else if (replicated_db_command_is_delete(op.op)) {
        ApplyDelete(op.key);
      } else {
        Log_error("[ReplicatedDB] Unknown batch sub-operation %d",
                  static_cast<int>(op.op));
      }
    }
  } else {
    Log_error("[ReplicatedDB] Unknown operation %d at index %lu",
              static_cast<int>(db_cmd->op_), index);
  }

  // Update and persist last applied index
  state_core_.set_last_applied_index(index);
  PersistLastAppliedIndex();
}

// @unsafe - RocksDB C API
void ReplicatedDB::ApplyPut(const std::string& key, const std::string& value) {
  char* err = nullptr;
  rocksdb_put(db_, write_options_,
              key.data(), key.size(),
              value.data(), value.size(), &err);
  if (err != nullptr) {
    std::string err_str = take_rocksdb_error(&err);
    Log_error("[ReplicatedDB] ApplyPut error for key '%s': %s",
              key.c_str(), err_str.c_str());
  }
}

// @unsafe - RocksDB C API
void ReplicatedDB::ApplyDelete(const std::string& key) {
  char* err = nullptr;
  rocksdb_delete(db_, write_options_,
                 key.data(), key.size(), &err);
  if (err != nullptr) {
    std::string err_str = take_rocksdb_error(&err);
    Log_error("[ReplicatedDB] ApplyDelete error for key '%s': %s",
              key.c_str(), err_str.c_str());
  }
}

// @unsafe - RocksDB C API
void ReplicatedDB::PersistLastAppliedIndex() {
  std::string idx_str = std::to_string(state_core_.last_applied_index());
  char* err = nullptr;
  rocksdb_put(db_, write_options_,
              META_LAST_APPLIED, strlen(META_LAST_APPLIED),
              idx_str.data(), idx_str.size(), &err);
  if (err != nullptr) {
    std::string err_str = take_rocksdb_error(&err);
    Log_error("[ReplicatedDB] Failed to persist last_applied_index: %s", err_str.c_str());
  }
}

// @unsafe - RocksDB C API
void ReplicatedDB::LoadLastAppliedIndex() {
  size_t value_len = 0;
  char* err = nullptr;
  char* value_ptr = rocksdb_get(db_, read_options_,
                                META_LAST_APPLIED, strlen(META_LAST_APPLIED),
                                &value_len, &err);
  if (err != nullptr) {
    take_rocksdb_error(&err);
    return;
  }
  if (value_ptr == nullptr) {
    state_core_.set_last_applied_index(0);
    return;
  }

  std::string value(value_ptr, value_len);
  rocksdb_free(value_ptr);
  try {
    state_core_.set_last_applied_index(std::stoull(value));
  } catch (...) {
    Log_error("[ReplicatedDB] Failed to parse last_applied_index from '%s'", value.c_str());
    state_core_.set_last_applied_index(0);
  }
}

// @unsafe - RocksDB C API; frees error strings returned by RocksDB.
std::string ReplicatedDB::take_rocksdb_error(char** errptr) {
  if (errptr == nullptr || *errptr == nullptr) {
    return "";
  }
  std::string err(*errptr);
  rocksdb_free(*errptr);
  *errptr = nullptr;
  return err;
}

// ===========================================================================
// CloseDB / OpenDB helpers (for snapshot loading)
// ===========================================================================

// @unsafe - Closes db_ and nulls the handle. Option handles stay alive so
// OpenDB can reuse the existing RocksDB configuration after snapshot loading.
void ReplicatedDB::CloseDB() {
  if (db_ != nullptr) {
    rocksdb_close(db_);
    db_ = nullptr;
  }
}

// @unsafe - Opens db_path_ using the constructor-owned option handles.
bool ReplicatedDB::OpenDB() {
  if (db_ != nullptr) {
    Log_warn("[ReplicatedDB] OpenDB called but db_ is already open");
    return true;
  }
  char* err = nullptr;
  db_ = rocksdb_open(options_, db_path_.c_str(), &err);
  if (err != nullptr || db_ == nullptr) {
    std::string err_str = take_rocksdb_error(&err);
    Log_error("[ReplicatedDB] Failed to reopen %s: %s",
              db_path_.c_str(), err_str.empty() ? "null handle" : err_str.c_str());
    db_ = nullptr;
    return false;
  }
  return true;
}

// ===========================================================================
// State Machine Snapshot: Create
// ===========================================================================
// Binary format:
//   num_files (uint32_t, 4 bytes)
//   For each file:
//     name_len (uint32_t, 4 bytes)
//     name     (name_len bytes, filename only, no directory prefix)
//     file_size (uint64_t, 8 bytes)
//     file_data (file_size bytes)
// ===========================================================================

// @unsafe - RocksDB checkpoint C API, filesystem I/O
std::string ReplicatedDB::CreateStateMachineSnapshot() {
  if (!db_) {
    Log_error("[ReplicatedDB] CreateStateMachineSnapshot: db_ is null");
    return "";
  }

  // 1. Create a RocksDB checkpoint
  char* err = nullptr;
  rocksdb_checkpoint_t* cp = rocksdb_checkpoint_object_create(db_, &err);
  if (err != nullptr || cp == nullptr) {
    std::string err_str = take_rocksdb_error(&err);
    Log_error("[ReplicatedDB] Failed to create checkpoint object: %s", err_str.c_str());
    return "";
  }

  std::string cp_dir = db_path_ + "_ckpt_" + std::to_string(state_core_.last_applied_index());

  // @unsafe { RocksDB checkpoint handle and filesystem operations }
  rocksdb_checkpoint_create(cp, cp_dir.c_str(), 0, &err);
  rocksdb_checkpoint_object_destroy(cp);

  if (err != nullptr) {
    std::string err_str = take_rocksdb_error(&err);
    Log_error("[ReplicatedDB] Failed to create checkpoint at %s: %s",
              cp_dir.c_str(), err_str.c_str());
    return "";
  }

  // 2. Read all files in the checkpoint directory and serialize
  // @unsafe { directory and file I/O }
  DIR* dir = opendir(cp_dir.c_str());
  if (!dir) {
    Log_error("[ReplicatedDB] Failed to open checkpoint dir %s", cp_dir.c_str());
    return "";
  }

  // Collect filenames (skip . and ..)
  std::vector<std::string> filenames;
  struct dirent* entry;
  while ((entry = readdir(dir)) != nullptr) {
    if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0) {
      continue;
    }
    filenames.push_back(entry->d_name);
  }
  closedir(dir);

  // Serialize into blob
  std::string blob;
  uint32_t num_files = static_cast<uint32_t>(filenames.size());
  blob.append(reinterpret_cast<const char*>(&num_files), sizeof(num_files));

  for (const auto& fname : filenames) {
    std::string filepath = cp_dir + "/" + fname;

    // Get file size
    struct stat st;
    if (stat(filepath.c_str(), &st) != 0) {
      Log_error("[ReplicatedDB] Failed to stat %s", filepath.c_str());
      // Clean up and return empty
      for (const auto& f : filenames) {
        std::string p = cp_dir + "/" + f;
        unlink(p.c_str());
      }
      rmdir(cp_dir.c_str());
      return "";
    }
    uint64_t file_size = static_cast<uint64_t>(st.st_size);

    // Append filename
    uint32_t name_len = static_cast<uint32_t>(fname.size());
    blob.append(reinterpret_cast<const char*>(&name_len), sizeof(name_len));
    blob.append(fname);

    // Append file data
    blob.append(reinterpret_cast<const char*>(&file_size), sizeof(file_size));

    // Read file contents
    std::ifstream ifs(filepath, std::ios::binary);
    if (!ifs) {
      Log_error("[ReplicatedDB] Failed to read checkpoint file %s", filepath.c_str());
      for (const auto& f : filenames) {
        std::string p = cp_dir + "/" + f;
        unlink(p.c_str());
      }
      rmdir(cp_dir.c_str());
      return "";
    }
    std::string file_data(file_size, '\0');
    ifs.read(file_data.data(), file_size);
    blob.append(file_data);
  }

  // 3. Clean up the checkpoint directory
  for (const auto& fname : filenames) {
    std::string filepath = cp_dir + "/" + fname;
    unlink(filepath.c_str());
  }
  rmdir(cp_dir.c_str());

  Log_info("[ReplicatedDB] Created snapshot: %u files, %zu bytes raw",
           num_files, blob.size());

  // 4. Compress the blob with LZ4 if enabled
  // @unsafe { LZ4 C API, memcpy }
  std::string result;
  size_t raw_size = blob.size();

  if (state_core_.compression_enabled()) {
    int max_dst = LZ4_compressBound(static_cast<int>(blob.size()));
    std::string compressed(max_dst, '\0');
    int compressed_size = LZ4_compress(
        blob.data(), compressed.data(), static_cast<int>(blob.size()));

    if (compressed_size > 0) {
      // Header: 1 byte (LZ4) + 4 bytes (original size) + compressed data
      result.resize(1 + sizeof(uint32_t) + compressed_size);
      result[0] = static_cast<char>(SNAPSHOT_LZ4);
      uint32_t orig_size = static_cast<uint32_t>(blob.size());
      std::memcpy(result.data() + 1, &orig_size, sizeof(orig_size));
      std::memcpy(result.data() + 1 + sizeof(uint32_t),
                  compressed.data(), compressed_size);
      Log_info("[ReplicatedDB] Snapshot compressed: %zu -> %zu bytes (%.1f%%)",
               raw_size, result.size(),
               100.0 * static_cast<double>(result.size()) / static_cast<double>(raw_size));
    } else {
      // Compression failed, store uncompressed with header
      Log_warn("[ReplicatedDB] LZ4 compression failed, storing uncompressed");
      result.resize(1 + blob.size());
      result[0] = static_cast<char>(SNAPSHOT_UNCOMPRESSED);
      std::memcpy(result.data() + 1, blob.data(), blob.size());
    }
  } else {
    // Compression disabled, store uncompressed with header
    result.resize(1 + blob.size());
    result[0] = static_cast<char>(SNAPSHOT_UNCOMPRESSED);
    std::memcpy(result.data() + 1, blob.data(), blob.size());
    Log_info("[ReplicatedDB] Snapshot stored uncompressed: %zu bytes", result.size());
  }

  return result;
}

// ===========================================================================
// State Machine Snapshot: Load
// ===========================================================================

// @unsafe - Filesystem I/O, RocksDB close/reopen, LZ4 decompression
void ReplicatedDB::LoadStateMachineSnapshot(const std::string& data) {
  if (data.empty()) {
    Log_error("[ReplicatedDB] LoadStateMachineSnapshot: empty data");
    return;
  }

  // 0. Check compression header and decompress if needed
  // @unsafe { LZ4 C API, memcpy }
  if (!replicated_db_snapshot_has_header(data.size())) {
    Log_error("[ReplicatedDB] LoadStateMachineSnapshot: data too small for header");
    return;
  }

  uint8_t compression = static_cast<uint8_t>(data[0]);
  std::string blob;

  if (replicated_db_snapshot_is_lz4(compression, SNAPSHOT_LZ4)) {
    // LZ4 compressed: header(1) + orig_size(4) + compressed_data
    if (!replicated_db_snapshot_has_bytes(1, sizeof(uint32_t), data.size())) {
      Log_error("[ReplicatedDB] LoadStateMachineSnapshot: truncated LZ4 header");
      return;
    }
    uint32_t orig_size = 0;
    std::memcpy(&orig_size, data.data() + 1, sizeof(orig_size));
    blob.resize(orig_size);
    int decompressed = LZ4_decompress_safe(
        data.data() + 1 + sizeof(uint32_t),
        blob.data(),
        static_cast<int>(data.size() - 1 - sizeof(uint32_t)),
        static_cast<int>(orig_size));
    if (decompressed < 0) {
      Log_error("[ReplicatedDB] LZ4 decompression failed (code %d)", decompressed);
      return;
    }
    Log_info("[ReplicatedDB] Snapshot decompressed: %zu -> %u bytes",
             data.size(), orig_size);
  } else if (replicated_db_snapshot_is_uncompressed(compression, SNAPSHOT_UNCOMPRESSED)) {
    // Uncompressed: header(1) + raw blob
    blob = data.substr(1);
  } else {
    Log_error("[ReplicatedDB] LoadStateMachineSnapshot: unknown compression byte 0x%02x",
              compression);
    return;
  }

  // 1. Parse the blob header
  size_t offset = 0;
  if (!replicated_db_snapshot_has_bytes(offset, sizeof(uint32_t), blob.size())) {
    Log_error("[ReplicatedDB] LoadStateMachineSnapshot: blob too small for header");
    return;
  }
  uint32_t num_files = 0;
  std::memcpy(&num_files, blob.data() + offset, sizeof(num_files));
  offset += sizeof(num_files);

  // Parse all file entries before modifying anything
  std::vector<ReplicatedDBFileEntry> files;
  files.reserve(num_files);

  for (uint32_t i = 0; i < num_files; i++) {
    if (!replicated_db_snapshot_has_bytes(offset, sizeof(uint32_t), blob.size())) {
      Log_error("[ReplicatedDB] LoadStateMachineSnapshot: truncated at file %u name_len", i);
      return;
    }
    uint32_t name_len = 0;
    std::memcpy(&name_len, blob.data() + offset, sizeof(name_len));
    offset += sizeof(name_len);

    if (!replicated_db_snapshot_has_bytes(offset, name_len, blob.size())) {
      Log_error("[ReplicatedDB] LoadStateMachineSnapshot: truncated at file %u name", i);
      return;
    }
    std::string name(blob.data() + offset, name_len);
    offset += name_len;

    if (!replicated_db_snapshot_has_bytes(offset, sizeof(uint64_t), blob.size())) {
      Log_error("[ReplicatedDB] LoadStateMachineSnapshot: truncated at file %u size", i);
      return;
    }
    uint64_t file_size = 0;
    std::memcpy(&file_size, blob.data() + offset, sizeof(file_size));
    offset += sizeof(file_size);

    if (!replicated_db_snapshot_has_bytes(offset, file_size, blob.size())) {
      Log_error("[ReplicatedDB] LoadStateMachineSnapshot: truncated at file %u data", i);
      return;
    }
    std::string contents(blob.data() + offset, file_size);
    offset += file_size;

    files.push_back({std::move(name), std::move(contents)});
  }

  // 2. Close the current RocksDB instance
  CloseDB();

  // 3. Delete the old database directory
  // @unsafe { RocksDB C API - destroy_db removes all DB files }
  {
    // Temporary options handle used only for destroy_db; keep this separate
    // from options_, which remains owned by this ReplicatedDB for reopen.
    rocksdb_options_t* destroy_opts = rocksdb_options_create();
    char* err = nullptr;
    rocksdb_destroy_db(destroy_opts, db_path_.c_str(), &err);
    if (err) {
      Log_warn("[ReplicatedDB] destroy_db warning: %s", err);
      rocksdb_free(err);
    }
    rocksdb_options_destroy(destroy_opts);
  }

  // Ensure the directory exists
  // @unsafe { mkdir }
  mkdir(db_path_.c_str(), 0755);

  // 4. Write the checkpoint files to the database directory
  // @unsafe { filesystem I/O }
  for (const auto& fe : files) {
    std::string filepath = db_path_ + "/" + fe.name;
    std::ofstream ofs(filepath, std::ios::binary);
    if (!ofs) {
      Log_error("[ReplicatedDB] LoadStateMachineSnapshot: failed to write %s", filepath.c_str());
      return;
    }
    ofs.write(fe.contents.data(), fe.contents.size());
  }

  // 5. Reopen RocksDB at the same path
  if (!OpenDB()) {
    Log_error("[ReplicatedDB] LoadStateMachineSnapshot: failed to reopen RocksDB");
    return;
  }

  // 6. Reload last_applied_index_ from the snapshot's RocksDB metadata
  LoadLastAppliedIndex();

  Log_info("[ReplicatedDB] Loaded snapshot: %u files, last_applied_index=%lu",
           num_files, state_core_.last_applied_index());
}
