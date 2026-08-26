#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "replicated_db.h"
#include "server.h"
#include "rrr/misc/serializable.hpp"
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

import std;

using namespace janus;

namespace {

constexpr uint64_t kReplicatedDBWaitTimeoutUs = 10'000'000;
constexpr uint64_t kReplicatedDBPollIntervalUs = 100;
constexpr size_t kSnapshotEntryFixedBytes =
    sizeof(uint32_t) + sizeof(uint64_t);
constexpr size_t kSnapshotLz4HeaderBytes = 1 + sizeof(uint32_t);
// The vendored legacy LZ4 header documents an approximately 1.9 GiB signed-int
// input ceiling but does not expose modern LZ4_MAX_INPUT_SIZE. Pin the modern
// compatible value explicitly and check it before every narrowing conversion.
constexpr size_t kMaxSnapshotRawBytes = 0x7E000000ULL;
constexpr size_t kMaxSnapshotWireBytes =
    kMaxSnapshotRawBytes + (kMaxSnapshotRawBytes / 255) + 16 +
    kSnapshotLz4HeaderBytes;
constexpr uint64_t kMaxSnapshotFiles = 1U << 20;
constexpr const char* kApplyFormatKey = "__raft_apply_format__";
constexpr const char* kApplyFormatValue = "atomic-v1";
constexpr unsigned int kLinuxRenameExchange = 1U << 1;
static_assert(kMaxSnapshotRawBytes <=
              static_cast<size_t>(std::numeric_limits<int>::max()));
static_assert(kMaxSnapshotWireBytes <=
              static_cast<size_t>(std::numeric_limits<int>::max()));

// @safe - Parsed snapshot entry owns its basename and file contents.
struct ParsedSnapshotFile {
  std::string name;
  std::string contents;
};

// @safe - Accepts only a single non-special path component.
bool IsSafeSnapshotFileName(const std::string& name) {
  if (name.empty() || name == "." || name == "..") {
    return false;
  }
  for (const char byte : name) {
    if (byte == '\0' || byte == '/' || byte == '\\') {
      return false;
    }
  }
  return true;
}

// @safe - Strict, allocation-free decimal parser with checked overflow.
bool ParseSnapshotAppliedIndex(
    const std::string_view value, uint64_t* parsed) {
  if (parsed == nullptr || value.empty()) {
    return false;
  }

  uint64_t result = 0;
  for (const char byte : value) {
    if (byte < '0' || byte > '9') {
      return false;
    }
    const uint64_t digit = static_cast<uint64_t>(byte - '0');
    if (result > (UINT64_MAX - digit) / 10) {
      return false;
    }
    result = result * 10 + digit;
  }

  *parsed = result;
  return true;
}

// @unsafe - Creates a private sibling directory through the POSIX mkdtemp API.
bool CreateUniqueSnapshotDirectory(
    const std::string& prefix, std::string* path) {
  if (path == nullptr) {
    return false;
  }
  *path = prefix + "-XXXXXX";
  return ::mkdtemp(path->data()) != nullptr;
}

// @unsafe - Writes and fsyncs one file in a newly-created private directory.
bool WriteSnapshotFile(
    const std::string& path, const std::string& contents) {
  const int fd = ::open(
      path.c_str(),
      O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd < 0) {
    return false;
  }

  bool succeeded = true;
  size_t written = 0;
  while (written < contents.size()) {
    const size_t remaining = contents.size() - written;
    const size_t chunk = std::min(
        remaining,
        static_cast<size_t>(std::numeric_limits<ssize_t>::max()));
    const ssize_t result =
        ::write(fd, contents.data() + written, chunk);
    if (result < 0 && errno == EINTR) {
      continue;
    }
    if (result <= 0) {
      succeeded = false;
      break;
    }
    written += static_cast<size_t>(result);
  }

  if (succeeded && ::fsync(fd) != 0) {
    succeeded = false;
  }
  if (::close(fd) != 0) {
    succeeded = false;
  }
  return succeeded;
}

// @unsafe - Fsyncs directory entries through a POSIX directory descriptor.
bool SyncSnapshotDirectory(const std::string& path) {
  const int fd = ::open(
      path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) {
    return false;
  }
  const bool synced = ::fsync(fd) == 0;
  const bool closed = ::close(fd) == 0;
  return synced && closed;
}

// @unsafe - Calls Linux renameat2 through syscall so both existing directory
// names change in one atomic filesystem transaction. The caller fsyncs their
// common parent before treating the exchange as durable.
bool ExchangeSnapshotDirectories(
    const std::string& first, const std::string& second, int* error_out) {
  if (error_out != nullptr) {
    *error_out = 0;
  }
#if defined(SYS_renameat2)
  long result = -1;
  do {
    result = ::syscall(
        SYS_renameat2, AT_FDCWD, first.c_str(), AT_FDCWD, second.c_str(),
        kLinuxRenameExchange);
  } while (result != 0 && errno == EINTR);
  if (result == 0) {
    return true;
  }
  if (error_out != nullptr) {
    *error_out = errno;
  }
  return false;
#else
  if (error_out != nullptr) {
    *error_out = ENOSYS;
  }
  return false;
#endif
}

// @safe - Computes the directory containing a configured database path.
std::string SnapshotParentDirectory(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  if (slash == std::string::npos) {
    return ".";
  }
  if (slash == 0) {
    return "/";
  }
  return path.substr(0, slash);
}

// @unsafe - Removes only validated basenames from a private staging directory.
bool RemoveFlatSnapshotDirectory(
    const std::string& path,
    const std::vector<ParsedSnapshotFile>& files) {
  bool succeeded = true;
  for (const auto& file : files) {
    const std::string file_path = path + "/" + file.name;
    if (::unlink(file_path.c_str()) != 0 && errno != ENOENT) {
      succeeded = false;
    }
  }
  if (::rmdir(path.c_str()) != 0 && errno != ENOENT) {
    succeeded = false;
  }
  return succeeded;
}

// @unsafe - Removes a closed RocksDB directory through the RocksDB C API.
bool DestroySnapshotDatabaseDirectory(const std::string& path) {
  rocksdb_options_t* destroy_options = rocksdb_options_create();
  if (destroy_options == nullptr) {
    return false;
  }

  char* err = nullptr;
  rocksdb_destroy_db(destroy_options, path.c_str(), &err);
  rocksdb_options_destroy(destroy_options);
  if (err == nullptr) {
    return true;
  }

  Log_error("[ReplicatedDB] Failed to destroy snapshot database {}: {}",
            path.c_str(), err);
  rocksdb_free(err);
  return false;
}

// @unsafe - Opens a staged checkpoint without create-if-missing and validates
// its required applied-index metadata through the RocksDB C API.
bool ValidateSnapshotDatabase(
    const std::string& path,
    rocksdb_readoptions_t* read_options,
    const char* metadata_key,
    uint64_t* applied_index) {
  if (read_options == nullptr || metadata_key == nullptr ||
      applied_index == nullptr) {
    return false;
  }

  rocksdb_options_t* validate_options = rocksdb_options_create();
  if (validate_options == nullptr) {
    return false;
  }
  rocksdb_options_set_create_if_missing(validate_options, 0);

  char* err = nullptr;
  rocksdb_t* staged_db =
      rocksdb_open(validate_options, path.c_str(), &err);
  if (err != nullptr || staged_db == nullptr) {
    Log_error("[ReplicatedDB] Staged snapshot is not a valid RocksDB at {}: {}",
              path.c_str(), err != nullptr ? err : "null handle");
    if (err != nullptr) {
      rocksdb_free(err);
    }
    if (staged_db != nullptr) {
      rocksdb_close(staged_db);
    }
    rocksdb_options_destroy(validate_options);
    return false;
  }

  size_t value_len = 0;
  char* value = rocksdb_get(
      staged_db, read_options,
      metadata_key, std::strlen(metadata_key), &value_len, &err);
  bool valid = err == nullptr && value != nullptr &&
      ParseSnapshotAppliedIndex(
          std::string_view(value, value_len), applied_index);
  if (valid) {
    valid = std::to_string(*applied_index) ==
            std::string(value, value_len);
  }

  size_t format_len = 0;
  char* format_err = nullptr;
  char* format_value = rocksdb_get(
      staged_db, read_options, kApplyFormatKey, std::strlen(kApplyFormatKey),
      &format_len, &format_err);
  valid = valid && format_err == nullptr && format_value != nullptr &&
          std::string_view(format_value, format_len) == kApplyFormatValue;
  if (!valid) {
    Log_error("[ReplicatedDB] Staged snapshot has invalid atomic apply "
              "metadata {}: {}",
              metadata_key,
              err != nullptr ? err :
              (format_err != nullptr ? format_err : "missing or malformed"));
  }

  if (value != nullptr) {
    rocksdb_free(value);
  }
  if (err != nullptr) {
    rocksdb_free(err);
  }
  if (format_value != nullptr) {
    rocksdb_free(format_value);
  }
  if (format_err != nullptr) {
    rocksdb_free(format_err);
  }
  rocksdb_close(staged_db);
  rocksdb_options_destroy(validate_options);
  return valid;
}

}  // namespace

// A prepared install owns one fully-validated sibling RocksDB directory. Until
// Commit(), destruction can only remove that private staging directory and
// therefore cannot change the live state machine. Once Commit() starts, the
// owner performs all cleanup/rollback because staging_path may atomically become
// the old live database.
class ReplicatedDB::PreparedSnapshotInstall final
    : public PreparedStateMachineSnapshotInstall {
 public:
  PreparedSnapshotInstall(
      ReplicatedDB* owner,
      std::string staging_path,
      uint64_t snapshot_applied_index,
      uint32_t num_files)
      : owner_(owner),
        staging_path_(std::move(staging_path)),
        snapshot_applied_index_(snapshot_applied_index),
        num_files_(num_files) {}

  PreparedSnapshotInstall(const PreparedSnapshotInstall&) = delete;
  PreparedSnapshotInstall& operator=(const PreparedSnapshotInstall&) = delete;

  // @unsafe - Removes only the private, validated staging database when Raft
  // never reached the durable-snapshot publication point.
  ~PreparedSnapshotInstall() override {
    if (commit_attempted_ || staging_path_.empty()) {
      return;
    }
    const std::string parent_directory =
        SnapshotParentDirectory(staging_path_);
    if (!DestroySnapshotDatabaseDirectory(staging_path_)) {
      Log_warn("[ReplicatedDB] Aborted prepared snapshot retained staging "
               "database at {}",
               staging_path_.c_str());
    } else if (!SyncSnapshotDirectory(parent_directory)) {
      Log_warn("[ReplicatedDB] Aborted prepared snapshot could not sync "
               "staging removal under {}",
               parent_directory.c_str());
    }
  }

  // @unsafe - Transfers cleanup/rollback responsibility to the owning
  // ReplicatedDB before the atomic directory exchange can begin.
  bool Commit() override {
    if (commit_attempted_ || owner_ == nullptr || staging_path_.empty()) {
      return false;
    }
    commit_attempted_ = true;
    return owner_->CommitPreparedStateMachineSnapshot(
        staging_path_, snapshot_applied_index_, num_files_);
  }

 private:
  ReplicatedDB* owner_ = nullptr;
  std::string staging_path_;
  uint64_t snapshot_applied_index_ = 0;
  uint32_t num_files_ = 0;
  bool commit_attempted_ = false;
};

// ReplicatedDBCommand's registry key comes from its explicit MakoCommands
// membership. Wire format remains byte-for-byte identical.
static int volatile x_replicated_db =
    rrr::SerializableRegistry::reg<ReplicatedDBCommand>(ReplicatedDBCommand::static_kind());

// ===========================================================================
// ReplicatedDBCommand factory methods and serialization
// ===========================================================================

// @unsafe - unique-owner mutation window on a factory-fresh Arc
rusty::Arc<ReplicatedDBCommand> ReplicatedDBCommand::CreatePut(
    const std::string& key, const std::string& value) {
  auto cmd = rusty::Arc<ReplicatedDBCommand>::make();
  {
    auto& mut_cmd = cmd.get_mut().unwrap();
    mut_cmd.op_ = ReplicatedDBOp::PUT;
    mut_cmd.key_ = key;
    mut_cmd.value_ = value;
  }
  return cmd;
}

// @unsafe - unique-owner mutation window on a factory-fresh Arc
rusty::Arc<ReplicatedDBCommand> ReplicatedDBCommand::CreateDelete(
    const std::string& key) {
  auto cmd = rusty::Arc<ReplicatedDBCommand>::make();
  {
    auto& mut_cmd = cmd.get_mut().unwrap();
    mut_cmd.op_ = ReplicatedDBOp::DELETE;
    mut_cmd.key_ = key;
    mut_cmd.value_ = "";
  }
  return cmd;
}

// @unsafe - unique-owner mutation window on a factory-fresh Arc
rusty::Arc<ReplicatedDBCommand> ReplicatedDBCommand::CreateBatch(
    const std::vector<KVOperation>& ops) {
  auto cmd = rusty::Arc<ReplicatedDBCommand>::make();
  {
    auto& mut_cmd = cmd.get_mut().unwrap();
    mut_cmd.op_ = ReplicatedDBOp::BATCH;
    mut_cmd.batch_ops_ = ops;
  }
  return cmd;
}

// Serializable save/load — moved here from
// the legacy Marshal wire. Wire format is byte-for-byte identical;
// the BinaryWriteArchive/BinaryReadArchive `<<`/`>>` overloads for
// uint8_t / uint32_t / std::string match the legacy Marshal encoding.
void ReplicatedDBCommand::save(BinaryWriteArchive& ar) const {
  rrr::Serialize_::serialize(static_cast<uint8_t>(op_), ar);
  rrr::Serialize_::serialize(key_, ar);
  rrr::Serialize_::serialize(value_, ar);
  if (replicated_db_op_is_batch(static_cast<uint8_t>(op_))) {
    uint32_t count = static_cast<uint32_t>(batch_ops_.size());
    rrr::Serialize_::serialize(count, ar);
    for (const auto& op : batch_ops_) {
      rrr::Serialize_::serialize(static_cast<uint8_t>(op.op), ar);
      rrr::Serialize_::serialize(op.key, ar);
      rrr::Serialize_::serialize(op.value, ar);
    }
  }
}

void ReplicatedDBCommand::load(BinaryReadArchive& ar) {
  uint8_t op_val;
  rrr::Deserialize_::deserialize(op_val, ar);
  op_ = static_cast<ReplicatedDBOp>(op_val);
  rrr::Deserialize_::deserialize(key_, ar);
  rrr::Deserialize_::deserialize(value_, ar);
  if (replicated_db_op_is_batch(static_cast<uint8_t>(op_))) {
    uint32_t count;
    rrr::Deserialize_::deserialize(count, ar);
    batch_ops_.resize(count);
    for (uint32_t i = 0; i < count; i++) {
      uint8_t sub_op_val;
      rrr::Deserialize_::deserialize(sub_op_val, ar);
      batch_ops_[i].op = static_cast<ReplicatedDBOp>(sub_op_val);
      rrr::Deserialize_::deserialize(batch_ops_[i].key, ar);
      rrr::Deserialize_::deserialize(batch_ops_[i].value, ar);
    }
  }
}

// Marshal-deprecation slice C2: the legacy to_marshal/from_marshal
// bridge wrappers are deleted; save/load over the archives is the only
// serialization surface (test.cc call sites flipped to save/load).

// ===========================================================================
// ReplicatedDB implementation
// ===========================================================================

// @unsafe - Opens RocksDB, stores raw pointers
ReplicatedDB::ReplicatedDB(RaftServer* raft, const std::string& db_path,
                           bool register_snapshot_callbacks)
    : raft_(raft), db_path_(db_path) {
  // @unsafe { std::getenv is not borrow-checked }
  const char* comp_env = std::getenv("MAKO_SNAPSHOT_COMPRESSION");
  if (comp_env && std::strcmp(comp_env, "0") == 0) {
    compression_enabled_ = false;
  }

  // @unsafe - RocksDB C API calls
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

  // Open the database
  char* err = nullptr;
  rocksdb_t* opened_db = rocksdb_open(options_, db_path_.c_str(), &err);
  if (err != nullptr || opened_db == nullptr) {
    std::string err_str = take_rocksdb_error(&err);
    Log_error("[ReplicatedDB] Failed to open {}: {}",
              db_path_.c_str(), err_str.empty() ? "null handle" : err_str.c_str());
    if (opened_db != nullptr) {
      rocksdb_close(opened_db);
    }
    db_ = nullptr;
    return;
  }
  db_ = opened_db;
  // Only initial construction may create an empty database. Every reopen in a
  // snapshot transaction must fail closed if its expected directory vanished.
  rocksdb_options_set_create_if_missing(options_, 0);

  // Load last applied index from metadata
  if (!LoadLastAppliedIndex()) {
    Log_error("[ReplicatedDB] Refusing unsafe application database at {}",
              db_path_.c_str());
    rocksdb_close(db_);
    db_ = nullptr;
    return;
  }

  // Register snapshot callbacks on the RaftServer
  // @unsafe { captures 'this' in lambdas for callback }
  if (raft_ && register_snapshot_callbacks) {
    snapshot_callback_owner_token_ =
        raft_->SetStateMachineSnapshotCallbacks(
            [this](uint64_t expected_applied_index) {
              return CreateStateMachineSnapshot(expected_applied_index);
            },
            [this](const std::string& snap_data,
                   uint64_t expected_last_included_index) {
              return PrepareStateMachineSnapshot(
                  snap_data, expected_last_included_index);
            });
  }

  Log_info("[ReplicatedDB] Opened database at {}, last_applied_index={}",
           db_path_.c_str(), GetLastAppliedIndex());
}

// @unsafe - Closes RocksDB, destroys options
ReplicatedDB::~ReplicatedDB() {
  // Unregister callbacks before closing RocksDB. Clear waits on the server
  // mutex, which also serializes snapshot callback invocation.
  if (raft_ != nullptr && snapshot_callback_owner_token_ != 0) {
    raft_->ClearStateMachineSnapshotCallbacks(
        snapshot_callback_owner_token_);
    snapshot_callback_owner_token_ = 0;
  }

  auto operation_guard = db_operation_gate_.write();
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

// @unsafe - Synchronizes raw RocksDB handle lifetime with snapshot exchange.
bool ReplicatedDB::IsOpen() const {
  auto operation_guard = db_operation_gate_.read();
  return db_ != nullptr;
}

// @safe - Atomic applied-index observation.
uint64_t ReplicatedDB::GetLastAppliedIndex() const {
  return last_applied_index_.load(
      rusty::sync::atomic::Ordering::Acquire);
}

// @unsafe - RocksDB iteration/write are protected by the exclusive database
// operation gate.  The caller owns the Raft state-machine application gate,
// which keeps the checked Raft boundary stable until it installs the learner.
bool ReplicatedDB::BootstrapEmptyStateMachine(
    uint64_t raft_applied_index) {
  auto operation_guard = db_operation_gate_.write();
  if (db_ == nullptr || raft_ == nullptr ||
      raft_applied_index == UINT64_MAX) {
    return false;
  }

  const uint64_t observed_raft_applied = raft_->GetAppliedIndex();
  const uint64_t current = last_applied_index_.load(
      rusty::sync::atomic::Ordering::Acquire);
  if (observed_raft_applied != raft_applied_index) {
    Log_error("[ReplicatedDB] Refusing empty-state bootstrap: "
              "database marker={} requested={} raft_applied={}",
              current, raft_applied_index, observed_raft_applied);
    return false;
  }

  // LoadLastAppliedIndex initializes a new RocksDB with exactly these two
  // metadata records.  Any additional key is an application effect whose
  // history cannot safely be skipped.
  rocksdb_iterator_t* iterator =
      rocksdb_create_iterator(db_, read_options_);
  if (iterator == nullptr) {
    return false;
  }
  bool saw_index = false;
  bool saw_format = false;
  bool metadata_only = true;
  const std::string current_index = std::to_string(current);
  rocksdb_iter_seek_to_first(iterator);
  for (; rocksdb_iter_valid(iterator); rocksdb_iter_next(iterator)) {
    size_t key_len = 0;
    size_t value_len = 0;
    const char* key_ptr = rocksdb_iter_key(iterator, &key_len);
    const char* value_ptr = rocksdb_iter_value(iterator, &value_len);
    const std::string key(key_ptr, key_len);
    const std::string value(value_ptr, value_len);
    if (key == META_LAST_APPLIED && value == current_index && !saw_index) {
      saw_index = true;
    } else if (key == kApplyFormatKey && value == kApplyFormatValue &&
               !saw_format) {
      saw_format = true;
    } else {
      metadata_only = false;
      break;
    }
  }
  char* iterator_err = nullptr;
  rocksdb_iter_get_error(iterator, &iterator_err);
  rocksdb_iter_destroy(iterator);
  if (iterator_err != nullptr) {
    const std::string error = take_rocksdb_error(&iterator_err);
    Log_error("[ReplicatedDB] Empty-state bootstrap scan failed: {}",
              error.c_str());
    return false;
  }
  if (!metadata_only || !saw_index || !saw_format) {
    Log_error("[ReplicatedDB] Refusing to bootstrap a non-empty or "
              "incompletely marked database");
    return false;
  }
  if (current == raft_applied_index) {
    return true;
  }
  if (current != 0) {
    Log_error("[ReplicatedDB] Refusing empty-state bootstrap from non-zero "
              "database marker {} to {}",
              current, raft_applied_index);
    return false;
  }

  rocksdb_writebatch_t* batch = rocksdb_writebatch_create();
  if (batch == nullptr) {
    return false;
  }
  const std::string index = std::to_string(raft_applied_index);
  rocksdb_writebatch_put(batch, META_LAST_APPLIED,
                         strlen(META_LAST_APPLIED),
                         index.data(), index.size());
  rocksdb_writebatch_put(batch, kApplyFormatKey, strlen(kApplyFormatKey),
                         kApplyFormatValue, strlen(kApplyFormatValue));
  rocksdb_writeoptions_t* bootstrap_write_options =
      rocksdb_writeoptions_create();
  if (bootstrap_write_options == nullptr) {
    rocksdb_writebatch_destroy(batch);
    return false;
  }
  // Unlike an ordinary applied entry, this adopted origin cannot be rebuilt
  // by replaying the intentionally skipped prefix.  Make its WAL record
  // durable before publishing the in-memory marker or learner callback.
  rocksdb_writeoptions_set_sync(bootstrap_write_options, 1);
  char* err = nullptr;
  rocksdb_write(db_, bootstrap_write_options, batch, &err);
  rocksdb_writeoptions_destroy(bootstrap_write_options);
  rocksdb_writebatch_destroy(batch);
  if (err != nullptr) {
    const std::string error = take_rocksdb_error(&err);
    Log_error("[ReplicatedDB] Empty-state bootstrap write failed: {}",
              error.c_str());
    return false;
  }

  last_applied_index_.store(
      raft_applied_index, rusty::sync::atomic::Ordering::Release);
  Log_info("[ReplicatedDB] Adopted live Raft boundary {} for empty state",
           raft_applied_index);
  return true;
}

// @unsafe - Submits PUT command through Raft, blocks until committed
bool ReplicatedDB::Put(const std::string& key, const std::string& value) {
  if (replicated_db_required_value_missing(IsOpen()) ||
      replicated_db_required_value_missing(raft_ != nullptr)) return false;

  janus::Command command(ReplicatedDBCommand::CreatePut(key, value));
  return SubmitAndWait(command, "Put");
}

// @unsafe - Submits DELETE command through Raft, blocks until committed
bool ReplicatedDB::Delete(const std::string& key) {
  if (replicated_db_required_value_missing(IsOpen()) ||
      replicated_db_required_value_missing(raft_ != nullptr)) return false;

  janus::Command command(ReplicatedDBCommand::CreateDelete(key));
  return SubmitAndWait(command, "Delete");
}

// @unsafe - Submits BATCH command through Raft, blocks until committed
bool ReplicatedDB::Batch(const std::vector<KVOperation>& ops) {
  if (replicated_db_required_value_missing(IsOpen()) ||
      replicated_db_required_value_missing(raft_ != nullptr) ||
      replicated_db_required_value_missing(!ops.empty())) return false;

  janus::Command command(ReplicatedDBCommand::CreateBatch(ops));
  return SubmitAndWait(command, "Batch");
}

// @unsafe - Bridges a Raft callback into an owned cross-thread wait state.
bool ReplicatedDB::SubmitAndWait(
    const janus::Command& command, const char* operation) {
  using rusty::sync::atomic::Ordering;

  auto commit_state =
      rusty::Arc<rusty::sync::atomic::AtomicI32>::make(0);
  auto callback_state = commit_state.clone();
  uint64_t index = 0;
  uint64_t term = 0;
  uint64_t callback_token = 0;

  const RaftStartResult start_result = raft_->StartWithCallback(
      command, &index, &term,
      [callback_state](CommitStatus status) {
        callback_state->store(
            replicated_db_commit_callback_state(
                status == CommitStatus::ROLLEDBACK),
            Ordering::Release);
      },
      &callback_token);
  if (raft_server_start_was_rejected(start_result)) {
    Log_debug("[ReplicatedDB] {} failed: not leader", operation);
    return false;
  }
  if (raft_server_start_is_indeterminate(start_result)) {
    // SubmitAndWait's bool result cannot distinguish a safe rejection from a
    // command that may already be durable.  Fail-stop instead of inviting an
    // automatic retry through the legacy bool API.
    Log_fatal("[ReplicatedDB] {} local append outcome is indeterminate for "
              "slot {} term {}; refusing to report retryable failure",
              operation, index, term);
  }

  const auto started_at = std::chrono::steady_clock::now();
  const bool running_in_fiber = Fiber::current_fiber().is_some();
  bool timed_out = false;

  for (;;) {
    const int32_t state = commit_state->load(Ordering::Acquire);
    const uint64_t applied_index = raft_->GetAppliedIndex();
    if (replicated_db_commit_failed(state) ||
        (replicated_db_commit_succeeded(state) &&
         replicated_db_apply_reached(applied_index, index))) {
      break;
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - started_at).count();
    timed_out = replicated_db_wait_timed_out(
        static_cast<uint64_t>(elapsed), kReplicatedDBWaitTimeoutUs);
    if (timed_out) {
      break;
    }

    if (running_in_fiber) {
      // Yield the owning reactor so Raft can replicate and deliver callbacks.
      Fiber::sleep(static_cast<int>(kReplicatedDBPollIntervalUs));
    } else {
      std::this_thread::sleep_for(
          std::chrono::microseconds(kReplicatedDBPollIntervalUs));
    }
  }

  // Cancellation synchronizes with callback delivery under the server lock.
  // A token mismatch cannot erase a later registration for the same index.
  raft_->UnregisterCommitCallback(index, callback_token);

  const int32_t final_state = commit_state->load(Ordering::Acquire);
  const uint64_t final_applied_index = raft_->GetAppliedIndex();
  const bool succeeded =
      replicated_db_commit_succeeded(final_state) &&
      replicated_db_apply_reached(final_applied_index, index);
  if (!succeeded && timed_out) {
    Log_warn("[ReplicatedDB] {} timed out waiting for index {} "
             "(commit_state={}, applied_index={})",
             operation, index, final_state, final_applied_index);
  }
  return succeeded;
}

// @unsafe - Direct RocksDB read (stale read, no Raft involvement)
bool ReplicatedDB::Get(const std::string& key, std::string* value) {
  auto operation_guard = db_operation_gate_.read();
  if (replicated_db_required_value_missing(db_ != nullptr) ||
      replicated_db_required_value_missing(value != nullptr)) return false;

  size_t value_len = 0;
  char* err = nullptr;
  char* value_ptr = rocksdb_get(db_, read_options_,
                                key.data(), key.size(),
                                &value_len, &err);
  if (err != nullptr) {
    std::string err_str = take_rocksdb_error(&err);
    Log_error("[ReplicatedDB] Get error for key '{}': {}",
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
  if (replicated_db_required_value_missing(raft_ != nullptr) ||
      !replicated_db_is_leader(raft_->IsLeader())) {
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
bool ReplicatedDB::ApplyEntry(slotid_t slot, const janus::Command& cmd) {
  auto operation_guard = db_operation_gate_.read();
  if (!db_ || !replicated_db_has_command_payload(cmd.has_value())) return false;

  const uint64_t index = slot;
  const uint64_t last_applied_index = last_applied_index_.load(
      rusty::sync::atomic::Ordering::Acquire);

  // Idempotency: skip already-applied entries
  if (replicated_db_should_skip_applied(index, last_applied_index)) {
    return true;
  }
  if (last_applied_index == UINT64_MAX || index != last_applied_index + 1) {
    Log_error("[ReplicatedDB] Refusing out-of-order apply: index={} last={}",
              index, last_applied_index);
    return false;
  }

  rocksdb_writebatch_t* batch = rocksdb_writebatch_create();
  if (batch == nullptr) {
    return false;
  }
  bool valid_command = true;

  // Non-ReplicatedDB Raft entries still advance the state-machine boundary,
  // but every ReplicatedDB operation and that boundary marker share this one
  // atomic RocksDB batch.
  if (!replicated_db_command_kind_matches(
          cmd.kind_, ReplicatedDBCommand::static_kind())) {
    // No data mutation.
  } else {
    const auto db_cmd = marshallable_cast<ReplicatedDBCommand>(cmd);
    if (db_cmd.is_none()) {
      Log_error("[ReplicatedDB] Failed to cast payload at index {}", index);
      valid_command = false;
    } else {
      switch (db_cmd.unwrap()->op_) {
        case ReplicatedDBOp::PUT:
          rocksdb_writebatch_put(
              batch, db_cmd.unwrap()->key_.data(), db_cmd.unwrap()->key_.size(),
              db_cmd.unwrap()->value_.data(), db_cmd.unwrap()->value_.size());
          break;
        case ReplicatedDBOp::DELETE:
          rocksdb_writebatch_delete(
              batch, db_cmd.unwrap()->key_.data(), db_cmd.unwrap()->key_.size());
          break;
        case ReplicatedDBOp::BATCH:
          for (const auto& op : db_cmd.unwrap()->batch_ops_) {
            if (op.op == ReplicatedDBOp::PUT) {
              rocksdb_writebatch_put(batch, op.key.data(), op.key.size(),
                                     op.value.data(), op.value.size());
            } else if (op.op == ReplicatedDBOp::DELETE) {
              rocksdb_writebatch_delete(
                  batch, op.key.data(), op.key.size());
            } else {
              Log_error("[ReplicatedDB] Unknown batch sub-operation {}",
                        static_cast<int>(op.op));
              valid_command = false;
              break;
            }
          }
          break;
        default:
          Log_error("[ReplicatedDB] Unknown operation {} at index {}",
                    static_cast<int>(db_cmd.unwrap()->op_), index);
          valid_command = false;
          break;
        }
    }
  }

  if (!valid_command) {
    rocksdb_writebatch_destroy(batch);
    return false;
  }

  const std::string idx_str = std::to_string(index);
  rocksdb_writebatch_put(batch, META_LAST_APPLIED, strlen(META_LAST_APPLIED),
                         idx_str.data(), idx_str.size());
  rocksdb_writebatch_put(batch, kApplyFormatKey, strlen(kApplyFormatKey),
                         kApplyFormatValue, strlen(kApplyFormatValue));
  char* err = nullptr;
  rocksdb_write(db_, write_options_, batch, &err);
  rocksdb_writebatch_destroy(batch);
  if (err != nullptr) {
    const std::string err_str = take_rocksdb_error(&err);
    Log_error("[ReplicatedDB] Atomic apply failed at index {}: {}",
              index, err_str.c_str());
    return false;
  }

  last_applied_index_.store(
      index, rusty::sync::atomic::Ordering::Release);
  return true;
}

// @unsafe - RocksDB C API
void ReplicatedDB::ApplyPut(const std::string& key, const std::string& value) {
  char* err = nullptr;
  rocksdb_put(db_, write_options_,
              key.data(), key.size(),
              value.data(), value.size(), &err);
  if (err != nullptr) {
    std::string err_str = take_rocksdb_error(&err);
    Log_error("[ReplicatedDB] ApplyPut error for key '{}': {}",
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
    Log_error("[ReplicatedDB] ApplyDelete error for key '{}': {}",
              key.c_str(), err_str.c_str());
  }
}

// @unsafe - RocksDB C API
void ReplicatedDB::PersistLastAppliedIndex() {
  const uint64_t last_applied_index = last_applied_index_.load(
      rusty::sync::atomic::Ordering::Acquire);
  std::string idx_str = std::to_string(last_applied_index);
  char* err = nullptr;
  rocksdb_put(db_, write_options_,
              META_LAST_APPLIED, strlen(META_LAST_APPLIED),
              idx_str.data(), idx_str.size(), &err);
  if (err != nullptr) {
    std::string err_str = take_rocksdb_error(&err);
    Log_error("[ReplicatedDB] Failed to persist last_applied_index: {}", err_str.c_str());
  }
}

// @unsafe - RocksDB C API
bool ReplicatedDB::LoadLastAppliedIndex() {
  size_t format_len = 0;
  char* format_err = nullptr;
  char* format_ptr = rocksdb_get(
      db_, read_options_, kApplyFormatKey, strlen(kApplyFormatKey),
      &format_len, &format_err);
  if (format_err != nullptr) {
    const std::string error = take_rocksdb_error(&format_err);
    Log_error("[ReplicatedDB] Failed to read apply format: {}", error.c_str());
    return false;
  }

  size_t value_len = 0;
  char* err = nullptr;
  char* value_ptr = rocksdb_get(db_, read_options_,
                                META_LAST_APPLIED, strlen(META_LAST_APPLIED),
                                &value_len, &err);
  if (err != nullptr) {
    const std::string error = take_rocksdb_error(&err);
    if (format_ptr != nullptr) rocksdb_free(format_ptr);
    Log_error("[ReplicatedDB] Failed to read applied index: {}", error.c_str());
    return false;
  }

  const bool has_format = format_ptr != nullptr;
  const bool has_index = value_ptr != nullptr;
  if (!has_format && !has_index) {
    // Only a genuinely empty RocksDB may be initialized into the atomic apply
    // format. An old DB with user effects but no trustworthy marker is
    // ambiguous and must not be replayed in place.
    rocksdb_iterator_t* iterator = rocksdb_create_iterator(db_, read_options_);
    if (iterator == nullptr) {
      return false;
    }
    rocksdb_iter_seek_to_first(iterator);
    const bool empty_database = !rocksdb_iter_valid(iterator);
    char* iterator_err = nullptr;
    rocksdb_iter_get_error(iterator, &iterator_err);
    rocksdb_iter_destroy(iterator);
    if (iterator_err != nullptr) {
      const std::string error = take_rocksdb_error(&iterator_err);
      Log_error("[ReplicatedDB] Failed to inspect empty database: {}",
                error.c_str());
      return false;
    }
    if (!empty_database) {
      Log_error("[ReplicatedDB] Existing data has no atomic apply marker");
      return false;
    }

    rocksdb_writebatch_t* initialize = rocksdb_writebatch_create();
    if (initialize == nullptr) {
      return false;
    }
    constexpr const char* zero = "0";
    rocksdb_writebatch_put(initialize, META_LAST_APPLIED,
                           strlen(META_LAST_APPLIED), zero, 1);
    rocksdb_writebatch_put(initialize, kApplyFormatKey,
                           strlen(kApplyFormatKey), kApplyFormatValue,
                           strlen(kApplyFormatValue));
    char* initialize_err = nullptr;
    rocksdb_write(db_, write_options_, initialize, &initialize_err);
    rocksdb_writebatch_destroy(initialize);
    if (initialize_err != nullptr) {
      const std::string error = take_rocksdb_error(&initialize_err);
      Log_error("[ReplicatedDB] Failed to initialize apply metadata: {}",
                error.c_str());
      return false;
    }
    last_applied_index_.store(
        0, rusty::sync::atomic::Ordering::Release);
    return true;
  }
  if (!has_format || !has_index) {
    if (format_ptr != nullptr) rocksdb_free(format_ptr);
    if (value_ptr != nullptr) rocksdb_free(value_ptr);
    Log_error("[ReplicatedDB] Incomplete atomic apply metadata");
    return false;
  }

  const std::string format(format_ptr, format_len);
  rocksdb_free(format_ptr);
  std::string value(value_ptr, value_len);
  rocksdb_free(value_ptr);
  uint64_t parsed_index = 0;
  if (format != kApplyFormatValue ||
      !ParseSnapshotAppliedIndex(value, &parsed_index) ||
      std::to_string(parsed_index) != value) {
    Log_error("[ReplicatedDB] Invalid apply metadata format='{}' index='{}'",
              format.c_str(), value.c_str());
    return false;
  }
  last_applied_index_.store(
      parsed_index, rusty::sync::atomic::Ordering::Release);
  return true;
}

// @unsafe - RocksDB C API
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

// @unsafe - Caller holds db_operation_gate_ exclusively. Closes RocksDB and
// nulls db_ while keeping the configured options alive.
void ReplicatedDB::CloseDB() {
  if (db_ != nullptr) {
    rocksdb_close(db_);
    db_ = nullptr;
  }
}

// @unsafe - Caller holds db_operation_gate_ exclusively. Opens only an existing
// RocksDB at db_path_; options_ has create_if_missing disabled after startup.
bool ReplicatedDB::OpenDB() {
  if (db_ != nullptr) {
    Log_warn("[ReplicatedDB] OpenDB called but db_ is already open");
    return true;
  }
  char* err = nullptr;
  rocksdb_t* opened_db = rocksdb_open(options_, db_path_.c_str(), &err);
  if (err != nullptr || opened_db == nullptr) {
    std::string err_str = take_rocksdb_error(&err);
    Log_error("[ReplicatedDB] Failed to reopen {}: {}",
              db_path_.c_str(), err_str.empty() ? "null handle" : err_str.c_str());
    if (opened_db != nullptr) {
      rocksdb_close(opened_db);
    }
    db_ = nullptr;
    return false;
  }
  db_ = opened_db;
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
  return CreateStateMachineSnapshot(GetLastAppliedIndex());
}

// @unsafe - RocksDB checkpoint C API, filesystem I/O
std::string ReplicatedDB::CreateStateMachineSnapshot(
    uint64_t expected_applied_index) {
  auto operation_guard = db_operation_gate_.read();
  if (!db_) {
    Log_error("[ReplicatedDB] CreateStateMachineSnapshot: db_ is null");
    return "";
  }
  const uint64_t applied_index = GetLastAppliedIndex();
  if (!replicated_db_snapshot_index_matches(
          applied_index, expected_applied_index)) {
    Log_error("[ReplicatedDB] Refusing snapshot at Raft boundary {}: "
              "state machine is applied through {}",
              expected_applied_index, applied_index);
    return "";
  }

  std::string checkpoint_path;
  auto cleanup_checkpoint = [&checkpoint_path]() {
    if (!checkpoint_path.empty() &&
        !DestroySnapshotDatabaseDirectory(checkpoint_path)) {
      Log_warn("[ReplicatedDB] Retained checkpoint directory {} after cleanup failure",
               checkpoint_path.c_str());
    }
  };

  try {
    // Reserve a unique same-parent checkpoint name, then remove the empty
    // mkdtemp directory because RocksDB requires the destination not to exist.
    if (!CreateUniqueSnapshotDirectory(
            db_path_ + ".snapshot-checkpoint", &checkpoint_path) ||
        ::rmdir(checkpoint_path.c_str()) != 0) {
      Log_error("[ReplicatedDB] Failed to reserve a checkpoint directory");
      return "";
    }

    char* err = nullptr;
    using CheckpointOwner = std::unique_ptr<
        rocksdb_checkpoint_t, decltype(&rocksdb_checkpoint_object_destroy)>;
    CheckpointOwner checkpoint(
        rocksdb_checkpoint_object_create(db_, &err),
        &rocksdb_checkpoint_object_destroy);
    if (err != nullptr || checkpoint == nullptr) {
      std::string err_str = take_rocksdb_error(&err);
      Log_error("[ReplicatedDB] Failed to create checkpoint object: {}",
                err_str.empty() ? "null handle" : err_str.c_str());
      return "";
    }

    rocksdb_checkpoint_create(
        checkpoint.get(), checkpoint_path.c_str(), 0, &err);
    checkpoint.reset();
    if (err != nullptr) {
      std::string err_str = take_rocksdb_error(&err);
      Log_error("[ReplicatedDB] Failed to create checkpoint at {}: {}",
                checkpoint_path.c_str(), err_str.c_str());
      cleanup_checkpoint();
      return "";
    }

    // Validate the checkpoint itself, not only the live atomic observed before
    // creation.  This proves that the bytes handed to Raft carry the exact
    // applied boundary whose prefix will be compacted.
    uint64_t checkpoint_applied_index = 0;
    if (!ValidateSnapshotDatabase(
            checkpoint_path, read_options_, META_LAST_APPLIED,
            &checkpoint_applied_index) ||
        !replicated_db_snapshot_index_matches(
            checkpoint_applied_index, expected_applied_index)) {
      Log_error("[ReplicatedDB] Checkpoint applied index {} does not match "
                "Raft snapshot boundary {}",
                checkpoint_applied_index, expected_applied_index);
      cleanup_checkpoint();
      return "";
    }

    DIR* raw_dir = ::opendir(checkpoint_path.c_str());
    if (raw_dir == nullptr) {
      Log_error("[ReplicatedDB] Failed to open checkpoint dir {}",
                checkpoint_path.c_str());
      cleanup_checkpoint();
      return "";
    }
    std::unique_ptr<DIR, decltype(&::closedir)> dir(raw_dir, &::closedir);

    std::vector<std::string> filenames;
    errno = 0;
    while (struct dirent* entry = ::readdir(dir.get())) {
      if (std::strcmp(entry->d_name, ".") == 0 ||
          std::strcmp(entry->d_name, "..") == 0) {
        continue;
      }
      if (!IsSafeSnapshotFileName(entry->d_name)) {
        Log_error("[ReplicatedDB] Checkpoint contained unsafe filename");
        cleanup_checkpoint();
        return "";
      }
      filenames.emplace_back(entry->d_name);
      if (!replicated_db_snapshot_file_count_is_valid(
              filenames.size(), kMaxSnapshotFiles)) {
        Log_error("[ReplicatedDB] Checkpoint file count exceeds limit {}",
                  kMaxSnapshotFiles);
        cleanup_checkpoint();
        return "";
      }
    }
    if (errno != 0) {
      Log_error("[ReplicatedDB] Failed while enumerating checkpoint {}: {}",
                checkpoint_path.c_str(), std::strerror(errno));
      cleanup_checkpoint();
      return "";
    }
    dir.reset();
    if (!replicated_db_snapshot_file_count_is_valid(
            filenames.size(), kMaxSnapshotFiles)) {
      Log_error("[ReplicatedDB] Checkpoint contains no files");
      cleanup_checkpoint();
      return "";
    }
    std::sort(filenames.begin(), filenames.end());

    std::string blob;
    const uint32_t num_files = static_cast<uint32_t>(filenames.size());
    blob.append(reinterpret_cast<const char*>(&num_files), sizeof(num_files));

    for (const auto& filename : filenames) {
      const std::string file_path = checkpoint_path + "/" + filename;
      struct stat file_stat {};
      if (::lstat(file_path.c_str(), &file_stat) != 0 ||
          !S_ISREG(file_stat.st_mode) || file_stat.st_size < 0) {
        Log_error("[ReplicatedDB] Checkpoint entry is not a regular file: {}",
                  file_path.c_str());
        cleanup_checkpoint();
        return "";
      }

      const uint64_t file_size = static_cast<uint64_t>(file_stat.st_size);
      if (file_size > static_cast<uint64_t>(kMaxSnapshotRawBytes)) {
        Log_error("[ReplicatedDB] Checkpoint file {} exceeds snapshot budget",
                  file_path.c_str());
        cleanup_checkpoint();
        return "";
      }
      const size_t file_size_native = static_cast<size_t>(file_size);
      const size_t name_size = filename.size();
      if (name_size > static_cast<size_t>(UINT32_MAX) ||
          !replicated_db_snapshot_has_bytes(
              blob.size(), sizeof(uint32_t), kMaxSnapshotRawBytes) ||
          !replicated_db_snapshot_has_bytes(
              blob.size() + sizeof(uint32_t), name_size,
              kMaxSnapshotRawBytes) ||
          !replicated_db_snapshot_has_bytes(
              blob.size() + sizeof(uint32_t) + name_size,
              sizeof(uint64_t), kMaxSnapshotRawBytes) ||
          !replicated_db_snapshot_has_bytes(
              blob.size() + sizeof(uint32_t) + name_size + sizeof(uint64_t),
              file_size_native, kMaxSnapshotRawBytes)) {
        Log_error("[ReplicatedDB] Checkpoint archive exceeds {} byte budget",
                  kMaxSnapshotRawBytes);
        cleanup_checkpoint();
        return "";
      }

      const uint32_t name_len = static_cast<uint32_t>(name_size);
      blob.append(reinterpret_cast<const char*>(&name_len), sizeof(name_len));
      blob.append(filename);
      blob.append(reinterpret_cast<const char*>(&file_size), sizeof(file_size));

      std::ifstream input(file_path, std::ios::binary);
      if (!input) {
        Log_error("[ReplicatedDB] Failed to read checkpoint file {}",
                  file_path.c_str());
        cleanup_checkpoint();
        return "";
      }
      std::string file_data(file_size_native, '\0');
      if (file_size_native != 0) {
        input.read(
            file_data.data(), static_cast<std::streamsize>(file_size_native));
        if (input.gcount() != static_cast<std::streamsize>(file_size_native) ||
            input.fail()) {
          Log_error("[ReplicatedDB] Short read from checkpoint file {} "
                    "(expected {}, got {})",
                    file_path.c_str(), file_size_native, input.gcount());
          cleanup_checkpoint();
          return "";
        }
      }
      blob.append(file_data);
    }

    cleanup_checkpoint();
    checkpoint_path.clear();
    if (blob.empty() || !replicated_db_snapshot_size_within_limit(
                            blob.size(), kMaxSnapshotRawBytes)) {
      Log_error("[ReplicatedDB] Refusing empty or oversized checkpoint archive");
      return "";
    }

    Log_info("[ReplicatedDB] Created snapshot: {} files, {} bytes raw",
             num_files, blob.size());

    std::string result;
    const size_t raw_size = blob.size();
    if (compression_enabled_) {
      const int raw_size_lz4 = static_cast<int>(raw_size);
      const int max_destination = LZ4_compressBound(raw_size_lz4);
      if (max_destination <= 0) {
        Log_error("[ReplicatedDB] LZ4 rejected {} byte snapshot input",
                  raw_size);
        return "";
      }
      std::string compressed(static_cast<size_t>(max_destination), '\0');
      const int compressed_size = LZ4_compress(
          blob.data(), compressed.data(), raw_size_lz4);
      if (compressed_size > 0) {
        result.resize(kSnapshotLz4HeaderBytes +
                      static_cast<size_t>(compressed_size));
        result[0] = static_cast<char>(SNAPSHOT_LZ4);
        const uint32_t original_size = static_cast<uint32_t>(raw_size);
        std::memcpy(result.data() + 1, &original_size, sizeof(original_size));
        std::memcpy(result.data() + kSnapshotLz4HeaderBytes,
                    compressed.data(), static_cast<size_t>(compressed_size));
        Log_info("[ReplicatedDB] Snapshot compressed: {} -> {} bytes ({:.1f}%)",
                 raw_size, result.size(),
                 100.0 * static_cast<double>(result.size()) /
                     static_cast<double>(raw_size));
      } else {
        Log_warn("[ReplicatedDB] LZ4 compression failed, storing uncompressed");
      }
    }

    if (result.empty()) {
      result.resize(1 + raw_size);
      result[0] = static_cast<char>(SNAPSHOT_UNCOMPRESSED);
      std::memcpy(result.data() + 1, blob.data(), raw_size);
      Log_info("[ReplicatedDB] Snapshot stored uncompressed: {} bytes",
               result.size());
    }
    return result;
  } catch (const std::exception& error) {
    cleanup_checkpoint();
    Log_error("[ReplicatedDB] Snapshot creation failed: {}", error.what());
  } catch (...) {
    cleanup_checkpoint();
    Log_error("[ReplicatedDB] Snapshot creation failed with unknown exception");
  }
  return "";
}

// ===========================================================================
// State Machine Snapshot: Prepare / Commit / Load
// ===========================================================================

// @unsafe - Validates an untrusted archive and durably stages its filesystem
// contents. The live RocksDB handle and canonical directory remain untouched.
std::unique_ptr<PreparedStateMachineSnapshotInstall>
ReplicatedDB::PrepareStateMachineSnapshot(
    const std::string& data, uint64_t expected_last_included_index) {
  auto operation_guard = db_operation_gate_.read();
  if (db_ == nullptr || options_ == nullptr || read_options_ == nullptr) {
    Log_error("[ReplicatedDB] PrepareStateMachineSnapshot: database is not open");
    return nullptr;
  }
  if (!replicated_db_snapshot_has_header(data.size()) ||
      !replicated_db_snapshot_size_within_limit(
          data.size(), kMaxSnapshotWireBytes)) {
    Log_error("[ReplicatedDB] LoadStateMachineSnapshot: empty or oversized "
              "payload ({} bytes, limit {})",
              data.size(), kMaxSnapshotWireBytes);
    return nullptr;
  }

  std::string blob;
  std::vector<ParsedSnapshotFile> files;
  uint32_t num_files = 0;

  // Parse and allocate before touching the live database. Any allocation
  // failure is a rejected snapshot, not a partially-installed checkpoint.
  try {
    const uint8_t compression = static_cast<uint8_t>(data[0]);
    if (replicated_db_snapshot_is_lz4(compression, SNAPSHOT_LZ4)) {
      if (!replicated_db_snapshot_has_bytes(
              0, kSnapshotLz4HeaderBytes, data.size()) ||
          data.size() == kSnapshotLz4HeaderBytes) {
        Log_error("[ReplicatedDB] LoadStateMachineSnapshot: truncated LZ4 payload");
        return nullptr;
      }

      uint32_t original_size = 0;
      std::memcpy(&original_size, data.data() + 1, sizeof(original_size));
      const size_t compressed_size = data.size() - kSnapshotLz4HeaderBytes;
      if (!replicated_db_snapshot_size_within_limit(
              original_size, kMaxSnapshotRawBytes) ||
          compressed_size >
              static_cast<size_t>(std::numeric_limits<int>::max())) {
        Log_error("[ReplicatedDB] LoadStateMachineSnapshot: LZ4 sizes exceed "
                  "snapshot/API limits");
        return nullptr;
      }

      blob.resize(original_size);
      const int decompressed = LZ4_decompress_safe(
          data.data() + kSnapshotLz4HeaderBytes,
          blob.data(),
          static_cast<int>(compressed_size),
          static_cast<int>(original_size));
      if (decompressed != static_cast<int>(original_size)) {
        Log_error("[ReplicatedDB] LZ4 decompression length mismatch "
                  "(expected {}, got {})",
                  original_size, decompressed);
        return nullptr;
      }
      Log_info("[ReplicatedDB] Snapshot decompressed: {} -> {} bytes",
               data.size(), original_size);
    } else if (replicated_db_snapshot_is_uncompressed(
                   compression, SNAPSHOT_UNCOMPRESSED)) {
      if (!replicated_db_snapshot_size_within_limit(
              data.size() - 1, kMaxSnapshotRawBytes)) {
        Log_error("[ReplicatedDB] LoadStateMachineSnapshot: uncompressed "
                  "snapshot exceeds {} byte limit",
                  kMaxSnapshotRawBytes);
        return nullptr;
      }
      blob.assign(data.data() + 1, data.size() - 1);
    } else {
      Log_error("[ReplicatedDB] LoadStateMachineSnapshot: unknown compression byte 0x{:02x}",
                compression);
      return nullptr;
    }

    size_t offset = 0;
    if (!replicated_db_snapshot_has_bytes(
            offset, sizeof(uint32_t), blob.size())) {
      Log_error("[ReplicatedDB] LoadStateMachineSnapshot: blob too small for header");
      return nullptr;
    }
    std::memcpy(&num_files, blob.data() + offset, sizeof(num_files));
    offset += sizeof(num_files);

    const size_t remaining = blob.size() - offset;
    if (!replicated_db_snapshot_file_count_is_valid(
            num_files, kMaxSnapshotFiles) ||
        static_cast<uint64_t>(num_files) >
            static_cast<uint64_t>(remaining / kSnapshotEntryFixedBytes)) {
      Log_error("[ReplicatedDB] LoadStateMachineSnapshot: implausible file count {}",
                num_files);
      return nullptr;
    }
    files.reserve(num_files);
    std::unordered_set<std::string> file_names;
    file_names.reserve(num_files);

    for (uint32_t i = 0; i < num_files; ++i) {
      if (!replicated_db_snapshot_has_bytes(
              offset, sizeof(uint32_t), blob.size())) {
        Log_error("[ReplicatedDB] LoadStateMachineSnapshot: truncated at file {} name_len", i);
        return nullptr;
      }
      uint32_t name_len = 0;
      std::memcpy(&name_len, blob.data() + offset, sizeof(name_len));
      offset += sizeof(name_len);

      if (name_len == 0 ||
          !replicated_db_snapshot_has_bytes(offset, name_len, blob.size())) {
        Log_error("[ReplicatedDB] LoadStateMachineSnapshot: invalid file {} name length", i);
        return nullptr;
      }
      std::string name(blob.data() + offset, name_len);
      offset += name_len;
      if (!IsSafeSnapshotFileName(name)) {
        Log_error("[ReplicatedDB] LoadStateMachineSnapshot: unsafe file name at entry {}", i);
        return nullptr;
      }
      if (!file_names.insert(name).second) {
        Log_error("[ReplicatedDB] LoadStateMachineSnapshot: duplicate file name '{}'",
                  name.c_str());
        return nullptr;
      }

      if (!replicated_db_snapshot_has_bytes(
              offset, sizeof(uint64_t), blob.size())) {
        Log_error("[ReplicatedDB] LoadStateMachineSnapshot: truncated at file {} size", i);
        return nullptr;
      }
      uint64_t file_size = 0;
      std::memcpy(&file_size, blob.data() + offset, sizeof(file_size));
      offset += sizeof(file_size);

      if (file_size > static_cast<uint64_t>(kMaxSnapshotRawBytes) ||
          !replicated_db_snapshot_has_u64_bytes(
              static_cast<uint64_t>(offset), file_size,
              static_cast<uint64_t>(blob.size()))) {
        Log_error("[ReplicatedDB] LoadStateMachineSnapshot: truncated at file {} data", i);
        return nullptr;
      }
      const size_t file_size_native = static_cast<size_t>(file_size);
      std::string contents(blob.data() + offset, file_size_native);
      offset += file_size_native;
      files.push_back({std::move(name), std::move(contents)});
    }

    if (offset != blob.size()) {
      Log_error("[ReplicatedDB] LoadStateMachineSnapshot: {} trailing bytes",
                blob.size() - offset);
      return nullptr;
    }
  } catch (const std::exception& error) {
    Log_error("[ReplicatedDB] LoadStateMachineSnapshot: parse/allocation failure: {}",
              error.what());
    return nullptr;
  } catch (...) {
    Log_error("[ReplicatedDB] LoadStateMachineSnapshot: unknown parse/allocation failure");
    return nullptr;
  }

  std::string staging_path;
  if (!CreateUniqueSnapshotDirectory(
          db_path_ + ".snapshot-incoming", &staging_path)) {
    Log_error("[ReplicatedDB] LoadStateMachineSnapshot: failed to create staging directory");
    return nullptr;
  }

  // @unsafe { writes attacker-controlled bytes only under validated basenames
  // in the private staging directory }
  for (const auto& file : files) {
    const std::string file_path = staging_path + "/" + file.name;
    if (!WriteSnapshotFile(file_path, file.contents)) {
      Log_error("[ReplicatedDB] LoadStateMachineSnapshot: failed to write {}",
                file_path.c_str());
      RemoveFlatSnapshotDirectory(staging_path, files);
      return nullptr;
    }
  }
  if (!SyncSnapshotDirectory(staging_path)) {
    Log_error("[ReplicatedDB] LoadStateMachineSnapshot: failed to sync staging directory");
    RemoveFlatSnapshotDirectory(staging_path, files);
    return nullptr;
  }

  uint64_t snapshot_applied_index = 0;
  if (!ValidateSnapshotDatabase(
          staging_path, read_options_, META_LAST_APPLIED,
          &snapshot_applied_index)) {
    if (!DestroySnapshotDatabaseDirectory(staging_path)) {
      RemoveFlatSnapshotDirectory(staging_path, files);
    }
    return nullptr;
  }
  if (!replicated_db_snapshot_index_matches(
          snapshot_applied_index, expected_last_included_index)) {
    Log_error("[ReplicatedDB] LoadStateMachineSnapshot: staged applied index {} "
              "does not match Raft snapshot boundary {}",
              snapshot_applied_index, expected_last_included_index);
    if (!DestroySnapshotDatabaseDirectory(staging_path)) {
      RemoveFlatSnapshotDirectory(staging_path, files);
    }
    return nullptr;
  }
  if (!SyncSnapshotDirectory(staging_path)) {
    Log_error("[ReplicatedDB] LoadStateMachineSnapshot: failed to sync validated database");
    DestroySnapshotDatabaseDirectory(staging_path);
    return nullptr;
  }

  struct stat live_path_stat {};
  struct stat staging_path_stat {};
  if (::lstat(db_path_.c_str(), &live_path_stat) != 0 ||
      !S_ISDIR(live_path_stat.st_mode) ||
      ::lstat(staging_path.c_str(), &staging_path_stat) != 0 ||
      !S_ISDIR(staging_path_stat.st_mode)) {
    Log_error("[ReplicatedDB] LoadStateMachineSnapshot: live/staged paths "
              "must be real directories, not symlinks");
    DestroySnapshotDatabaseDirectory(staging_path);
    return nullptr;
  }

  Log_info("[ReplicatedDB] Prepared snapshot: {} files, "
           "last_applied_index={}, staging={}",
           num_files, snapshot_applied_index, staging_path.c_str());
  return std::make_unique<PreparedSnapshotInstall>(
      this, std::move(staging_path), snapshot_applied_index, num_files);
}

// @unsafe - Atomically exchanges a previously validated sibling directory into
// db_path_. After exchange begins this method owns every cleanup/rollback path.
bool ReplicatedDB::CommitPreparedStateMachineSnapshot(
    const std::string& staging_path,
    uint64_t snapshot_applied_index,
    uint32_t num_files) {
  auto operation_guard = db_operation_gate_.write();
  const std::string parent_directory = SnapshotParentDirectory(db_path_);
  if (db_ == nullptr || options_ == nullptr || read_options_ == nullptr) {
    Log_error("[ReplicatedDB] CommitPreparedStateMachineSnapshot: database is "
              "not open");
    if (!DestroySnapshotDatabaseDirectory(staging_path)) {
      Log_warn("[ReplicatedDB] Preserving prepared snapshot at {}",
               staging_path.c_str());
    }
    return false;
  }

  // Revalidate both names at the commit boundary. The staged database was
  // already opened and checked during Prepare; this closes the filesystem
  // substitution window before renameat2(RENAME_EXCHANGE).
  struct stat live_path_stat {};
  struct stat staging_path_stat {};
  if (::lstat(db_path_.c_str(), &live_path_stat) != 0 ||
      !S_ISDIR(live_path_stat.st_mode) ||
      ::lstat(staging_path.c_str(), &staging_path_stat) != 0 ||
      !S_ISDIR(staging_path_stat.st_mode)) {
    Log_error("[ReplicatedDB] CommitPreparedStateMachineSnapshot: live/staged "
              "paths must be real directories, not symlinks");
    DestroySnapshotDatabaseDirectory(staging_path);
    return false;
  }

  // Both directory names exist on the same filesystem. Linux RENAME_EXCHANGE
  // changes them atomically, so a process or power failure can observe the old
  // or new database at db_path_, but never a missing canonical database.
  CloseDB();
  int exchange_error = 0;
  if (!ExchangeSnapshotDirectories(
          db_path_, staging_path, &exchange_error)) {
    Log_error("[ReplicatedDB] LoadStateMachineSnapshot: atomic directory "
              "exchange unavailable/failed: {}",
              std::strerror(exchange_error));
    const bool reopened = OpenDB();
    if (!reopened) {
      Log_error("[ReplicatedDB] LoadStateMachineSnapshot: failed to reopen "
                "unchanged live database after exchange failure");
      Log_warn("[ReplicatedDB] Preserving validated snapshot at {} for restart recovery",
               staging_path.c_str());
    } else {
      DestroySnapshotDatabaseDirectory(staging_path);
    }
    return false;
  }

  // After the exchange, staging_path holds the old database. A rollback is the
  // same atomic exchange in reverse; it never deletes either recovery point.
  auto rollback_exchange = [&]() {
    int rollback_error = 0;
    if (!ExchangeSnapshotDirectories(
            db_path_, staging_path, &rollback_error)) {
      Log_error("[ReplicatedDB] LoadStateMachineSnapshot: atomic rollback "
                "exchange failed: {}",
                std::strerror(rollback_error));
      return false;
    }
    const bool directory_synced = SyncSnapshotDirectory(parent_directory);
    const bool reopened = OpenDB();
    if (!directory_synced || !reopened) {
      // The reverse exchange is not yet known durable, or the restored live
      // database could not be opened. Keep both valid directory images so a
      // fail-stop restart can observe either rename outcome and recover.
      Log_warn("[ReplicatedDB] Snapshot rollback retained alternate database at {} "
               "(parent_synced={}, reopened={})",
               staging_path.c_str(), directory_synced, reopened);
      return false;
    }
    if (!DestroySnapshotDatabaseDirectory(staging_path)) {
      Log_warn("[ReplicatedDB] Snapshot rollback retained rejected database at {}",
               staging_path.c_str());
    } else if (!SyncSnapshotDirectory(parent_directory)) {
      Log_warn("[ReplicatedDB] Snapshot rollback could not sync rejected "
               "database removal");
    }
    return true;
  };

  if (!SyncSnapshotDirectory(parent_directory)) {
    Log_error("[ReplicatedDB] LoadStateMachineSnapshot: failed to sync installed directory entry");
    if (!rollback_exchange()) {
      Log_error("[ReplicatedDB] LoadStateMachineSnapshot: rollback after sync failure was incomplete");
    }
    return false;
  }
  if (!OpenDB()) {
    Log_error("[ReplicatedDB] LoadStateMachineSnapshot: failed to reopen installed RocksDB");
    if (!rollback_exchange()) {
      Log_error("[ReplicatedDB] LoadStateMachineSnapshot: rollback after open failure was incomplete");
    }
    return false;
  }

  last_applied_index_.store(
      snapshot_applied_index, rusty::sync::atomic::Ordering::Release);

  // staging_path now holds the old database. Its removal is garbage collection
  // after the new database is validated, durably exchanged, and open.
  if (!DestroySnapshotDatabaseDirectory(staging_path)) {
    Log_warn("[ReplicatedDB] Loaded snapshot but retained old database at {}",
             staging_path.c_str());
  } else if (!SyncSnapshotDirectory(parent_directory)) {
    Log_warn("[ReplicatedDB] Loaded snapshot but failed to sync old database removal");
  }

  Log_info("[ReplicatedDB] Loaded snapshot: {} files, last_applied_index={}",
           num_files, snapshot_applied_index);
  return true;
}

// @unsafe - Used only when the matching Raft snapshot is already durable (for
// startup recovery and direct compatibility tests).
bool ReplicatedDB::LoadStateMachineSnapshot(
    const std::string& data, uint64_t expected_last_included_index) {
  auto prepared = PrepareStateMachineSnapshot(
      data, expected_last_included_index);
  return prepared != nullptr && prepared->Commit();
}
