#include "raft_persistence.h"
#include "server.h"
#include "../__dep__.h"
#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <rrr/misc/marshal.hpp>

namespace janus {

RaftPersistence::RaftPersistence()
    : db_(nullptr),
      state_cf_(nullptr),
      logs_cf_(nullptr),
      meta_cf_(nullptr) {
}

RaftPersistence::~RaftPersistence() {
    if (db_) {
        // Close column family handles
        if (state_cf_) {
            db_->DestroyColumnFamilyHandle(state_cf_);
        }
        if (logs_cf_) {
            db_->DestroyColumnFamilyHandle(logs_cf_);
        }
        if (meta_cf_) {
            db_->DestroyColumnFamilyHandle(meta_cf_);
        }
    }
}

bool RaftPersistence::Init(uint32_t site_id, uint32_t partition_id, const std::string& base_path) {
    // Setup database path
    db_path_ = base_path + "/raft_" + std::to_string(site_id) +
               "_partition_" + std::to_string(partition_id);

    // Create parent directory if it doesn't exist
    std::string cmd = "mkdir -p " + db_path_;
    system(cmd.c_str());

    // Configure RocksDB options
    options_.create_if_missing = true;
    options_.create_missing_column_families = true;

    // Setup write options
    sync_write_options_.sync = true;   // Durability for safety
    async_write_options_.sync = false; // Performance for non-critical

    // Try to open existing database with column families first
    std::vector<std::string> existing_column_families;
    rocksdb::Status status = rocksdb::DB::ListColumnFamilies(options_, db_path_, &existing_column_families);

    rocksdb::DB* db_raw = nullptr;
    std::vector<rocksdb::ColumnFamilyHandle*> handles;

    if (status.ok() && !existing_column_families.empty()) {
        // Database exists, open with existing column families
        std::vector<rocksdb::ColumnFamilyDescriptor> column_families;
        for (const auto& cf_name : existing_column_families) {
            column_families.push_back(
                rocksdb::ColumnFamilyDescriptor(cf_name, rocksdb::ColumnFamilyOptions())
            );
        }

        status = rocksdb::DB::Open(options_, db_path_, column_families, &handles, &db_raw);
        if (!status.ok()) {
            Log_error("Failed to open existing RocksDB at %s: %s",
                     db_path_.c_str(), status.ToString().c_str());
            return false;
        }

        // Find our column families in the handles
        for (size_t i = 0; i < handles.size(); i++) {
            std::string cf_name = handles[i]->GetName();
            if (cf_name == "state") {
                state_cf_ = handles[i];
            } else if (cf_name == "logs") {
                logs_cf_ = handles[i];
            } else if (cf_name == "meta") {
                meta_cf_ = handles[i];
            }
        }

    } else {
        // Database doesn't exist or has no column families, create fresh
        std::vector<rocksdb::ColumnFamilyDescriptor> column_families = {
            {rocksdb::kDefaultColumnFamilyName, rocksdb::ColumnFamilyOptions()},
            {"state", rocksdb::ColumnFamilyOptions()},
            {"logs", rocksdb::ColumnFamilyOptions()},
            {"meta", rocksdb::ColumnFamilyOptions()}
        };

        status = rocksdb::DB::Open(options_, db_path_, column_families, &handles, &db_raw);
        if (!status.ok()) {
            Log_error("Failed to create RocksDB at %s: %s",
                     db_path_.c_str(), status.ToString().c_str());
            return false;
        }

        // Assign column family handles (index 0 is default, 1-3 are our CFs)
        state_cf_ = handles[1];
        logs_cf_ = handles[2];
        meta_cf_ = handles[3];
    }

    db_.reset(db_raw);

    Log_info("RaftPersistence initialized at %s", db_path_.c_str());
    return true;
}

// ============================================================================
// State Persistence
// ============================================================================

bool RaftPersistence::PersistTerm(uint64_t term) {
    std::string value(reinterpret_cast<char*>(&term), sizeof(term));
    rocksdb::Status status = db_->Put(sync_write_options_, state_cf_, "term", value);

    if (!status.ok()) {
        Log_error("Failed to persist term %lu: %s", term, status.ToString().c_str());
        return false;
    }
    return true;
}

bool RaftPersistence::PersistVotedFor(uint32_t voted_for) {
    std::string value(reinterpret_cast<char*>(&voted_for), sizeof(voted_for));
    rocksdb::Status status = db_->Put(sync_write_options_, state_cf_, "voted_for", value);

    if (!status.ok()) {
        Log_error("Failed to persist votedFor %u: %s", voted_for, status.ToString().c_str());
        return false;
    }
    return true;
}

bool RaftPersistence::PersistState(uint64_t term, uint32_t voted_for) {
    bool success = PersistTerm(term);
    success = PersistVotedFor(voted_for) && success;
    return success;
}

// ============================================================================
// Log Persistence
// ============================================================================

std::string RaftPersistence::GenerateLogKey(slotid_t slot_id) {
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "log:%016lu", slot_id);
    return std::string(buffer);
}

bool RaftPersistence::SerializeRaftData(const RaftData& entry, std::string& value) {
    // Use Marshal to serialize everything including the Marshallable log_
    rrr::Marshal m;

    // Serialize primitive fields
    m << entry.max_ballot_seen_;
    m << entry.max_ballot_accepted_;
    m << entry.term;
    m << entry.prevTerm;
    m << entry.slot_id;
    m << entry.ballot;

    // Serialize log_ field (shared_ptr<Marshallable>)
    // First write a flag indicating whether log_ is present
    uint8_t has_log = (entry.log_ != nullptr) ? 1 : 0;
    m << has_log;

    if (has_log) {
        // Use MarshallDeputy to serialize the Marshallable
        rrr::MarshallDeputy md(entry.log_);
        m << md;
    }

    // Extract the serialized data from Marshal into the string
    size_t content_size = m.content_size();
    value.resize(content_size);
    m.read(&value[0], content_size);

    return true;
}

bool RaftPersistence::DeserializeRaftData(const std::string& value, RaftData& entry) {
    // Use Marshal to deserialize everything including the Marshallable log_
    rrr::Marshal m;
    m.write(value.data(), value.size());

    // Deserialize primitive fields
    m >> entry.max_ballot_seen_;
    m >> entry.max_ballot_accepted_;
    m >> entry.term;
    m >> entry.prevTerm;
    m >> entry.slot_id;
    m >> entry.ballot;

    // Deserialize log_ field (shared_ptr<Marshallable>)
    uint8_t has_log;
    m >> has_log;

    if (has_log) {
        // Use MarshallDeputy to deserialize the Marshallable
        rrr::MarshallDeputy md;
        m >> md;
        entry.log_ = md.sp_data_;
        Log_info("[PERSIST-LOAD] slot=%ld loaded log_ kind=%d", entry.slot_id, md.kind_);
    } else {
        entry.log_ = nullptr;
    }

    return true;
}

bool RaftPersistence::PersistLogEntry(slotid_t slot_id, const RaftData& entry) {
    std::string key = GenerateLogKey(slot_id);
    std::string value;

    if (!SerializeRaftData(entry, value)) {
        Log_error("Failed to serialize log entry at slot %lu", slot_id);
        return false;
    }

    rocksdb::Status status = db_->Put(sync_write_options_, logs_cf_, key, value);

    if (!status.ok()) {
        Log_error("Failed to persist log entry %lu: %s", slot_id, status.ToString().c_str());
        return false;
    }
    return true;
}

bool RaftPersistence::PersistLogEntries(const std::map<slotid_t, std::shared_ptr<RaftData>>& entries) {
    for (const auto& pair : entries) {
        slotid_t slot_id = pair.first;
        const RaftData& entry = *pair.second;
        if (!PersistLogEntry(slot_id, entry)) {
            return false;
        }
    }
    return true;
}

// ============================================================================
// Metadata Persistence
// ============================================================================

bool RaftPersistence::PersistCommitIndex(uint64_t commit_index) {
    std::string value(reinterpret_cast<char*>(&commit_index), sizeof(commit_index));
    rocksdb::Status status = db_->Put(async_write_options_, meta_cf_, "commit_index", value);

    if (!status.ok()) {
        Log_error("Failed to persist commit_index %lu: %s", commit_index, status.ToString().c_str());
        return false;
    }
    return true;
}

// ============================================================================
// Recovery
// ============================================================================

bool RaftPersistence::LoadTerm(uint64_t& term) {
    std::string value;
    rocksdb::Status status = db_->Get(read_options_, state_cf_, "term", &value);

    if (status.IsNotFound()) {
        term = 0;  // Default initial term
        return true;
    }

    if (!status.ok()) {
        Log_error("Failed to load term: %s", status.ToString().c_str());
        return false;
    }

    if (value.size() != sizeof(term)) {
        Log_error("Invalid term value size: %zu (expected %zu)", value.size(), sizeof(term));
        return false;
    }

    memcpy(&term, value.data(), sizeof(term));
    return true;
}

bool RaftPersistence::LoadVotedFor(uint32_t& voted_for) {
    std::string value;
    rocksdb::Status status = db_->Get(read_options_, state_cf_, "voted_for", &value);

    if (status.IsNotFound()) {
        voted_for = (uint32_t)-1;  // INVALID_SITEID
        return true;
    }

    if (!status.ok()) {
        Log_error("Failed to load votedFor: %s", status.ToString().c_str());
        return false;
    }

    if (value.size() != sizeof(voted_for)) {
        Log_error("Invalid votedFor value size: %zu (expected %zu)", value.size(), sizeof(voted_for));
        return false;
    }

    memcpy(&voted_for, value.data(), sizeof(voted_for));
    return true;
}

bool RaftPersistence::LoadAllLogs(std::map<slotid_t, std::shared_ptr<RaftData>>& logs) {
    logs.clear();

    // Iterate over logs column family
    rocksdb::Iterator* it = db_->NewIterator(read_options_, logs_cf_);

    for (it->SeekToFirst(); it->Valid(); it->Next()) {
        auto entry = std::make_shared<RaftData>();
        if (DeserializeRaftData(it->value().ToString(), *entry)) {
            logs[entry->slot_id] = entry;
        } else {
            Log_error("Failed to deserialize log entry with key %s", it->key().ToString().c_str());
        }
    }

    delete it;

    Log_info("Loaded %zu log entries from persistence", logs.size());
    return true;
}

bool RaftPersistence::LoadLogRange(slotid_t start_slot, slotid_t end_slot, std::map<slotid_t, std::shared_ptr<RaftData>>& logs) {
    logs.clear();

    std::string start_key = GenerateLogKey(start_slot);
    std::string end_key = GenerateLogKey(end_slot + 1);  // Exclusive end

    rocksdb::Iterator* it = db_->NewIterator(read_options_, logs_cf_);

    for (it->Seek(start_key); it->Valid() && it->key().ToString() < end_key; it->Next()) {
        auto entry = std::make_shared<RaftData>();
        if (DeserializeRaftData(it->value().ToString(), *entry)) {
            logs[entry->slot_id] = entry;
        } else {
            Log_error("Failed to deserialize log entry with key %s", it->key().ToString().c_str());
        }
    }

    delete it;
    return true;
}

} // namespace janus
