#include "replicated_db.h"
#include "server.h"

using namespace janus;

// @unsafe - Static registration with Marshallable factory
static int volatile x_replicated_db =
    MarshallDeputy::reg_initializer(MarshallDeputy::CMD_REPLICATED_DB,
                                     []() -> Marshallable* {
                                       return new ReplicatedDBCommand();
                                     });

// ===========================================================================
// ReplicatedDBCommand factory methods and serialization
// ===========================================================================

// @unsafe - Creates shared_ptr (non-borrow-checked ownership)
shared_ptr<ReplicatedDBCommand> ReplicatedDBCommand::CreatePut(
    const std::string& key, const std::string& value) {
  auto cmd = std::make_shared<ReplicatedDBCommand>();
  cmd->op_ = ReplicatedDBOp::PUT;
  cmd->key_ = key;
  cmd->value_ = value;
  return cmd;
}

// @unsafe - Creates shared_ptr (non-borrow-checked ownership)
shared_ptr<ReplicatedDBCommand> ReplicatedDBCommand::CreateDelete(
    const std::string& key) {
  auto cmd = std::make_shared<ReplicatedDBCommand>();
  cmd->op_ = ReplicatedDBOp::DELETE;
  cmd->key_ = key;
  cmd->value_ = "";
  return cmd;
}

// @unsafe - Creates shared_ptr (non-borrow-checked ownership)
shared_ptr<ReplicatedDBCommand> ReplicatedDBCommand::CreateBatch(
    const std::vector<KVOperation>& ops) {
  auto cmd = std::make_shared<ReplicatedDBCommand>();
  cmd->op_ = ReplicatedDBOp::BATCH;
  cmd->batch_ops_ = ops;
  return cmd;
}

// @unsafe - Marshal I/O is not borrow-checked
Marshal& ReplicatedDBCommand::to_marshal(Marshal& m) const {
  m << static_cast<uint8_t>(op_);
  m << key_;
  m << value_;
  if (op_ == ReplicatedDBOp::BATCH) {
    uint32_t count = static_cast<uint32_t>(batch_ops_.size());
    m << count;
    for (const auto& op : batch_ops_) {
      m << static_cast<uint8_t>(op.op);
      m << op.key;
      m << op.value;
    }
  }
  return m;
}

// @unsafe - Marshal I/O is not borrow-checked
Marshal& ReplicatedDBCommand::from_marshal(Marshal& m) {
  uint8_t op_val;
  m >> op_val;
  op_ = static_cast<ReplicatedDBOp>(op_val);
  m >> key_;
  m >> value_;
  if (op_ == ReplicatedDBOp::BATCH) {
    uint32_t count;
    m >> count;
    batch_ops_.resize(count);
    for (uint32_t i = 0; i < count; i++) {
      uint8_t sub_op_val;
      m >> sub_op_val;
      batch_ops_[i].op = static_cast<ReplicatedDBOp>(sub_op_val);
      m >> batch_ops_[i].key;
      m >> batch_ops_[i].value;
    }
  }
  return m;
}

// ===========================================================================
// ReplicatedDB implementation
// ===========================================================================

// @unsafe - Opens RocksDB, stores raw pointers
ReplicatedDB::ReplicatedDB(RaftServer* raft, const std::string& db_path)
    : raft_(raft), db_path_(db_path) {
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

  Log_info("[ReplicatedDB] Opened database at %s, last_applied_index=%lu",
           db_path_.c_str(), last_applied_index_);
}

// @unsafe - Closes RocksDB, destroys options
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
  if (!db_ || !raft_) return false;

  auto cmd = ReplicatedDBCommand::CreatePut(key, value);
  auto cmd_base = std::dynamic_pointer_cast<Marshallable>(cmd);

  uint64_t index = 0, term = 0;
  bool is_leader = raft_->Start(cmd_base, &index, &term);
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
  while (committed.load() == 0) {
    usleep(100);  // 100us poll interval
  }

  return committed.load() > 0;
}

// @unsafe - Submits DELETE command through Raft, blocks until committed
bool ReplicatedDB::Delete(const std::string& key) {
  if (!db_ || !raft_) return false;

  auto cmd = ReplicatedDBCommand::CreateDelete(key);
  auto cmd_base = std::dynamic_pointer_cast<Marshallable>(cmd);

  uint64_t index = 0, term = 0;
  bool is_leader = raft_->Start(cmd_base, &index, &term);
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

  while (committed.load() == 0) {
    usleep(100);
  }

  return committed.load() > 0;
}

// @unsafe - Direct RocksDB read (stale read, no Raft involvement)
bool ReplicatedDB::Get(const std::string& key, std::string* value) {
  if (!db_ || !value) return false;

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
  if (value_ptr == nullptr) {
    return false;  // Key not found
  }

  *value = std::string(value_ptr, value_len);
  rocksdb_free(value_ptr);
  return true;
}

// @unsafe - Applies committed Raft entries to local RocksDB
void ReplicatedDB::ApplyEntry(int slot, shared_ptr<Marshallable> cmd) {
  if (!db_ || !cmd) return;

  uint64_t index = static_cast<uint64_t>(slot);

  // Idempotency: skip already-applied entries
  if (index <= last_applied_index_) {
    return;
  }

  // Only process ReplicatedDBCommand entries
  if (cmd->kind_ != MarshallDeputy::CMD_REPLICATED_DB) {
    // Not our command type; still advance the index to avoid re-processing
    last_applied_index_ = index;
    PersistLastAppliedIndex();
    return;
  }

  auto db_cmd = std::dynamic_pointer_cast<ReplicatedDBCommand>(cmd);
  if (!db_cmd) {
    Log_error("[ReplicatedDB] Failed to cast Marshallable to ReplicatedDBCommand at index %lu", index);
    last_applied_index_ = index;
    PersistLastAppliedIndex();
    return;
  }

  // Apply the operation
  switch (db_cmd->op_) {
    case ReplicatedDBOp::PUT:
      ApplyPut(db_cmd->key_, db_cmd->value_);
      break;
    case ReplicatedDBOp::DELETE:
      ApplyDelete(db_cmd->key_);
      break;
    case ReplicatedDBOp::BATCH:
      for (const auto& op : db_cmd->batch_ops_) {
        switch (op.op) {
          case ReplicatedDBOp::PUT:
            ApplyPut(op.key, op.value);
            break;
          case ReplicatedDBOp::DELETE:
            ApplyDelete(op.key);
            break;
          default:
            Log_error("[ReplicatedDB] Unknown batch sub-operation %d", static_cast<int>(op.op));
            break;
        }
      }
      break;
    default:
      Log_error("[ReplicatedDB] Unknown operation %d at index %lu",
                static_cast<int>(db_cmd->op_), index);
      break;
  }

  // Update and persist last applied index
  last_applied_index_ = index;
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
  std::string idx_str = std::to_string(last_applied_index_);
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
    last_applied_index_ = 0;
    return;
  }

  std::string value(value_ptr, value_len);
  rocksdb_free(value_ptr);
  try {
    last_applied_index_ = std::stoull(value);
  } catch (...) {
    Log_error("[ReplicatedDB] Failed to parse last_applied_index from '%s'", value.c_str());
    last_applied_index_ = 0;
  }
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
