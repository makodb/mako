#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <iostream>
#include <memory>
#include <string>
#include <cstdint>
#include <iomanip>

using namespace std;

// Read a uint64_t from RocksDB value
bool ReadUInt64(const string& value, uint64_t& result) {
    if (value.size() != sizeof(uint64_t)) {
        return false;
    }
    memcpy(&result, value.data(), sizeof(uint64_t));
    return true;
}

// Read a uint32_t from RocksDB value
bool ReadUInt32(const string& value, uint32_t& result) {
    if (value.size() != sizeof(uint32_t)) {
        return false;
    }
    memcpy(&result, value.data(), sizeof(uint32_t));
    return true;
}

// Decode RaftData from binary value
struct RaftData {
    uint64_t max_ballot_seen_;
    uint64_t max_ballot_accepted_;
    uint64_t term;
    uint64_t prevTerm;
    uint64_t slot_id;
    uint64_t ballot;
};

bool DecodeRaftData(const string& value, RaftData& entry) {
    size_t expected_size = sizeof(entry.max_ballot_seen_) +
                           sizeof(entry.max_ballot_accepted_) +
                           sizeof(entry.term) +
                           sizeof(entry.prevTerm) +
                           sizeof(entry.slot_id) +
                           sizeof(entry.ballot);

    if (value.size() < expected_size) {
        cerr << "Invalid RaftData size: " << value.size()
             << " (expected at least " << expected_size << ")" << endl;
        return false;
    }

    const char* ptr = value.data();

    memcpy(&entry.max_ballot_seen_, ptr, sizeof(entry.max_ballot_seen_));
    ptr += sizeof(entry.max_ballot_seen_);

    memcpy(&entry.max_ballot_accepted_, ptr, sizeof(entry.max_ballot_accepted_));
    ptr += sizeof(entry.max_ballot_accepted_);

    memcpy(&entry.term, ptr, sizeof(entry.term));
    ptr += sizeof(entry.term);

    memcpy(&entry.prevTerm, ptr, sizeof(entry.prevTerm));
    ptr += sizeof(entry.prevTerm);

    memcpy(&entry.slot_id, ptr, sizeof(entry.slot_id));
    ptr += sizeof(entry.slot_id);

    memcpy(&entry.ballot, ptr, sizeof(entry.ballot));

    return true;
}

void PrintUsage(const char* prog_name) {
    cout << "Usage: " << prog_name << " <db_path>" << endl;
    cout << "Example: " << prog_name << " /tmp/raft_0_partition_0" << endl;
    cout << "\nOr auto-discover databases:" << endl;
    cout << "  ls -d /tmp/raft_*_partition_* | xargs -I {} " << prog_name << " {}" << endl;
}

int main(int argc, char* argv[]) {
    if (argc != 2) {
        PrintUsage(argv[0]);
        return 1;
    }

    string db_path = argv[1];

    cout << "======================================" << endl;
    cout << "Reading Raft persistence: " << db_path << endl;
    cout << "======================================" << endl;

    // Open database with all column families
    rocksdb::Options options;
    options.create_if_missing = false;

    // List existing column families
    vector<string> column_families;
    rocksdb::Status status = rocksdb::DB::ListColumnFamilies(options, db_path, &column_families);

    if (!status.ok()) {
        cerr << "Failed to list column families: " << status.ToString() << endl;
        cerr << "Database may not exist at: " << db_path << endl;
        return 1;
    }

    cout << "\nColumn families found: ";
    for (const auto& cf : column_families) {
        cout << cf << " ";
    }
    cout << endl;

    // Open database with all column families
    vector<rocksdb::ColumnFamilyDescriptor> cf_descriptors;
    for (const auto& cf_name : column_families) {
        cf_descriptors.push_back(
            rocksdb::ColumnFamilyDescriptor(cf_name, rocksdb::ColumnFamilyOptions())
        );
    }

    rocksdb::DB* db_raw;
    vector<rocksdb::ColumnFamilyHandle*> handles;
    status = rocksdb::DB::OpenForReadOnly(options, db_path, cf_descriptors, &handles, &db_raw);

    if (!status.ok()) {
        cerr << "Failed to open database: " << status.ToString() << endl;
        return 1;
    }

    unique_ptr<rocksdb::DB> db(db_raw);

    // Find our column family handles
    rocksdb::ColumnFamilyHandle* state_cf = nullptr;
    rocksdb::ColumnFamilyHandle* logs_cf = nullptr;
    rocksdb::ColumnFamilyHandle* meta_cf = nullptr;

    for (auto handle : handles) {
        string cf_name = handle->GetName();
        if (cf_name == "state") state_cf = handle;
        else if (cf_name == "logs") logs_cf = handle;
        else if (cf_name == "meta") meta_cf = handle;
    }

    rocksdb::ReadOptions read_options;

    // ========== Read State Column Family ==========
    cout << "\n========== STATE ==========" << endl;

    if (state_cf) {
        string value;

        // Read term
        status = db->Get(read_options, state_cf, "term", &value);
        if (status.ok()) {
            uint64_t term;
            if (ReadUInt64(value, term)) {
                cout << "Term: " << term << endl;
            }
        } else if (status.IsNotFound()) {
            cout << "Term: <not set>" << endl;
        }

        // Read votedFor
        status = db->Get(read_options, state_cf, "voted_for", &value);
        if (status.ok()) {
            uint32_t voted_for;
            if (ReadUInt32(value, voted_for)) {
                if (voted_for == (uint32_t)-1) {
                    cout << "VotedFor: INVALID_SITEID" << endl;
                } else {
                    cout << "VotedFor: " << voted_for << endl;
                }
            }
        } else if (status.IsNotFound()) {
            cout << "VotedFor: <not set>" << endl;
        }
    } else {
        cout << "State column family not found" << endl;
    }

    // ========== Read Metadata Column Family ==========
    cout << "\n========== METADATA ==========" << endl;

    if (meta_cf) {
        string value;
        status = db->Get(read_options, meta_cf, "commit_index", &value);
        if (status.ok()) {
            uint64_t commit_index;
            if (ReadUInt64(value, commit_index)) {
                cout << "CommitIndex: " << commit_index << endl;
            }
        } else if (status.IsNotFound()) {
            cout << "CommitIndex: <not set>" << endl;
        }
    } else {
        cout << "Metadata column family not found" << endl;
    }

    // ========== Read Logs Column Family ==========
    cout << "\n========== LOGS ==========" << endl;

    if (logs_cf) {
        rocksdb::Iterator* it = db->NewIterator(read_options, logs_cf);

        int count = 0;
        for (it->SeekToFirst(); it->Valid(); it->Next()) {
            RaftData entry;
            if (DecodeRaftData(it->value().ToString(), entry)) {
                cout << "\nLog Entry [" << count++ << "]:" << endl;
                cout << "  Key: " << it->key().ToString() << endl;
                cout << "  SlotID: " << entry.slot_id << endl;
                cout << "  Term: " << entry.term << endl;
                cout << "  PrevTerm: " << entry.prevTerm << endl;
                cout << "  Ballot: " << entry.ballot << endl;
                cout << "  MaxBallotSeen: " << entry.max_ballot_seen_ << endl;
                cout << "  MaxBallotAccepted: " << entry.max_ballot_accepted_ << endl;
            } else {
                cerr << "Failed to decode log entry: " << it->key().ToString() << endl;
            }
        }

        if (count == 0) {
            cout << "No log entries found" << endl;
        } else {
            cout << "\nTotal log entries: " << count << endl;
        }

        delete it;
    } else {
        cout << "Logs column family not found" << endl;
    }

    // Cleanup column family handles
    for (auto handle : handles) {
        db->DestroyColumnFamilyHandle(handle);
    }

    cout << "\n======================================" << endl;
    return 0;
}
