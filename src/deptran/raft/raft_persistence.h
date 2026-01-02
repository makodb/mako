#pragma once

#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <memory>
#include <string>
#include <vector>
#include <map>
#include "../constants.h"

namespace janus {

struct RaftData;

class RaftPersistence {
public:
    RaftPersistence();
    ~RaftPersistence();

    // Initialize database
    bool Init(uint32_t site_id, uint32_t partition_id, const std::string& base_path = "/tmp");

    // State persistence (synchronous)
    bool PersistTerm(uint64_t term);
    bool PersistVotedFor(uint32_t voted_for);
    bool PersistState(uint64_t term, uint32_t voted_for);

    // Log persistence (synchronous)
    bool PersistLogEntry(slotid_t slot_id, const RaftData& entry);
    bool PersistLogEntries(const std::map<slotid_t, std::shared_ptr<RaftData>>& entries);

    // Metadata persistence (can be async)
    bool PersistCommitIndex(uint64_t commit_index);

    // Recovery
    bool LoadTerm(uint64_t& term);
    bool LoadVotedFor(uint32_t& voted_for);
    bool LoadAllLogs(std::map<slotid_t, std::shared_ptr<RaftData>>& logs);
    bool LoadLogRange(slotid_t start_slot, slotid_t end_slot, std::map<slotid_t, std::shared_ptr<RaftData>>& logs);

private:
    std::unique_ptr<rocksdb::DB> db_;
    rocksdb::ColumnFamilyHandle* state_cf_;
    rocksdb::ColumnFamilyHandle* logs_cf_;
    rocksdb::ColumnFamilyHandle* meta_cf_;

    rocksdb::Options options_;
    rocksdb::WriteOptions sync_write_options_;
    rocksdb::WriteOptions async_write_options_;
    rocksdb::ReadOptions read_options_;

    std::string db_path_;

    // Helper methods
    std::string GenerateLogKey(slotid_t slot_id);
    bool SerializeRaftData(const RaftData& entry, std::string& value);
    bool DeserializeRaftData(const std::string& value, RaftData& entry);
};

} // namespace janus
