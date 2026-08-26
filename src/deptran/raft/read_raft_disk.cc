#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <type_traits>

#include <rocksdb/c.h>

import std;

using namespace std;

namespace {

string take_rocksdb_error(char** errptr) {
    if (errptr == nullptr || *errptr == nullptr) {
        return "";
    }
    string err(*errptr);
    rocksdb_free(*errptr);
    *errptr = nullptr;
    return err;
}

string copy_db_value(const char* data, size_t len) {
    if (data == nullptr || len == 0) {
        return "";
    }
    return string(data, len);
}

}  // namespace

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
#if RUSTYCPP_RUST
#[allow(non_snake_case)]
#[cfg_attr(any(), cpp_no_auto_traits)]
#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Default, Eq, PartialEq))]
#[repr(C)]
pub struct RaftData {
    pub max_ballot_seen_: u64,
    pub max_ballot_accepted_: u64,
    pub term: u64,
    pub prevTerm: u64,
    pub slot_id: u64,
    pub ballot: u64,
}
#endif
/*RUSTYCPP:GEN-BEGIN id=raft_disk.data_record version=1 rust_sha256=30b59f182e9a0c2897442cc0084000f4feafb143f81389e78e33ce7eb430d26b*/
struct RaftData;

struct RaftData {
    uint64_t max_ballot_seen_;
    uint64_t max_ballot_accepted_;
    uint64_t term;
    uint64_t prevTerm;
    uint64_t slot_id;
    uint64_t ballot;
};
/*RUSTYCPP:GEN-END id=raft_disk.data_record*/

static_assert(std::is_standard_layout_v<RaftData>);
static_assert(std::is_trivial_v<RaftData>);
static_assert(std::is_trivially_copyable_v<RaftData>);
static_assert(std::is_aggregate_v<RaftData>);
static_assert(sizeof(RaftData) == 6 * sizeof(uint64_t));
static_assert(alignof(RaftData) == alignof(uint64_t));
static_assert(offsetof(RaftData, max_ballot_seen_) == 0);
static_assert(offsetof(RaftData, max_ballot_accepted_) == sizeof(uint64_t));
static_assert(offsetof(RaftData, term) == 2 * sizeof(uint64_t));
static_assert(offsetof(RaftData, prevTerm) == 3 * sizeof(uint64_t));
static_assert(offsetof(RaftData, slot_id) == 4 * sizeof(uint64_t));
static_assert(offsetof(RaftData, ballot) == 5 * sizeof(uint64_t));
static_assert(RaftData{}.max_ballot_seen_ == 0);
static_assert(RaftData{}.ballot == 0);

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

    rocksdb_options_t* options = rocksdb_options_create();
    if (options == nullptr) {
        cerr << "Failed to create RocksDB options" << endl;
        return 1;
    }
    rocksdb_options_set_create_if_missing(options, 0);

    // List existing column families
    size_t cf_count = 0;
    char* err = nullptr;
    char** cf_names = rocksdb_list_column_families(options, db_path.c_str(), &cf_count, &err);

    if (err != nullptr || cf_names == nullptr) {
        string err_str = take_rocksdb_error(&err);
        cerr << "Failed to list column families: "
             << (err_str.empty() ? "unknown error" : err_str) << endl;
        cerr << "Database may not exist at: " << db_path << endl;
        rocksdb_options_destroy(options);
        return 1;
    }

    cout << "\nColumn families found: ";
    for (size_t i = 0; i < cf_count; ++i) {
        cout << cf_names[i] << " ";
    }
    cout << endl;

    vector<const char*> cf_name_ptrs(cf_count);
    vector<const rocksdb_options_t*> cf_options(cf_count, options);
    vector<rocksdb_column_family_handle_t*> handles(cf_count, nullptr);

    for (size_t i = 0; i < cf_count; ++i) {
        cf_name_ptrs[i] = cf_names[i];
    }

    rocksdb_t* db = rocksdb_open_for_read_only_column_families(
        options,
        db_path.c_str(),
        static_cast<int>(cf_count),
        cf_name_ptrs.data(),
        cf_options.data(),
        handles.data(),
        0,
        &err);

    if (err != nullptr || db == nullptr) {
        string err_str = take_rocksdb_error(&err);
        cerr << "Failed to open database: "
             << (err_str.empty() ? "null handle" : err_str) << endl;
        rocksdb_list_column_families_destroy(cf_names, cf_count);
        rocksdb_options_destroy(options);
        return 1;
    }

    rocksdb_column_family_handle_t* state_cf = nullptr;
    rocksdb_column_family_handle_t* logs_cf = nullptr;
    rocksdb_column_family_handle_t* meta_cf = nullptr;

    for (size_t i = 0; i < cf_count; ++i) {
        string cf_name = cf_names[i];
        if (cf_name == "state") {
            state_cf = handles[i];
        } else if (cf_name == "logs") {
            logs_cf = handles[i];
        } else if (cf_name == "meta") {
            meta_cf = handles[i];
        }
    }

    rocksdb_readoptions_t* read_options = rocksdb_readoptions_create();
    if (read_options == nullptr) {
        cerr << "Failed to create read options" << endl;
        for (auto* handle : handles) {
            if (handle != nullptr) {
                rocksdb_column_family_handle_destroy(handle);
            }
        }
        rocksdb_close(db);
        rocksdb_list_column_families_destroy(cf_names, cf_count);
        rocksdb_options_destroy(options);
        return 1;
    }

    // ========== Read State Column Family ==========
    cout << "\n========== STATE ==========" << endl;

    if (state_cf) {
        // Read term
        size_t value_len = 0;
        char* value_ptr = rocksdb_get_cf(db, read_options, state_cf,
                                         "term", std::strlen("term"),
                                         &value_len, &err);
        if (err != nullptr) {
            cerr << "Failed to read term: " << take_rocksdb_error(&err) << endl;
        } else if (value_ptr != nullptr) {
            string value = copy_db_value(value_ptr, value_len);
            rocksdb_free(value_ptr);
            uint64_t term;
            if (ReadUInt64(value, term)) {
                cout << "Term: " << term << endl;
            }
        } else {
            cout << "Term: <not set>" << endl;
        }

        // Read votedFor
        value_len = 0;
        value_ptr = rocksdb_get_cf(db, read_options, state_cf,
                                   "voted_for", std::strlen("voted_for"),
                                   &value_len, &err);
        if (err != nullptr) {
            cerr << "Failed to read voted_for: " << take_rocksdb_error(&err) << endl;
        } else if (value_ptr != nullptr) {
            string value = copy_db_value(value_ptr, value_len);
            rocksdb_free(value_ptr);
            uint32_t voted_for;
            if (ReadUInt32(value, voted_for)) {
                if (voted_for == static_cast<uint32_t>(-1)) {
                    cout << "VotedFor: INVALID_SITEID" << endl;
                } else {
                    cout << "VotedFor: " << voted_for << endl;
                }
            }
        } else {
            cout << "VotedFor: <not set>" << endl;
        }
    } else {
        cout << "State column family not found" << endl;
    }

    // ========== Read Metadata Column Family ==========
    cout << "\n========== METADATA ==========" << endl;

    if (meta_cf) {
        size_t value_len = 0;
        char* value_ptr = rocksdb_get_cf(db, read_options, meta_cf,
                                         "commit_index", std::strlen("commit_index"),
                                         &value_len, &err);
        if (err != nullptr) {
            cerr << "Failed to read commit_index: " << take_rocksdb_error(&err) << endl;
        } else if (value_ptr != nullptr) {
            string value = copy_db_value(value_ptr, value_len);
            rocksdb_free(value_ptr);
            uint64_t commit_index;
            if (ReadUInt64(value, commit_index)) {
                cout << "CommitIndex: " << commit_index << endl;
            }
        } else {
            cout << "CommitIndex: <not set>" << endl;
        }
    } else {
        cout << "Metadata column family not found" << endl;
    }

    // ========== Read Logs Column Family ==========
    cout << "\n========== LOGS ==========" << endl;

    if (logs_cf) {
        rocksdb_iterator_t* it = rocksdb_create_iterator_cf(db, read_options, logs_cf);
        if (it == nullptr) {
            cerr << "Failed to create logs iterator" << endl;
        } else {
            rocksdb_iter_seek_to_first(it);

            int count = 0;
            for (; rocksdb_iter_valid(it); rocksdb_iter_next(it)) {
                size_t key_len = 0;
                size_t value_len = 0;
                const char* key_ptr = rocksdb_iter_key(it, &key_len);
                const char* value_ptr = rocksdb_iter_value(it, &value_len);

                string key = copy_db_value(key_ptr, key_len);
                string value = copy_db_value(value_ptr, value_len);

                RaftData entry;
                if (DecodeRaftData(value, entry)) {
                    cout << "\nLog Entry [" << count++ << "]:" << endl;
                    cout << "  Key: " << key << endl;
                    cout << "  SlotID: " << entry.slot_id << endl;
                    cout << "  Term: " << entry.term << endl;
                    cout << "  PrevTerm: " << entry.prevTerm << endl;
                    cout << "  Ballot: " << entry.ballot << endl;
                    cout << "  MaxBallotSeen: " << entry.max_ballot_seen_ << endl;
                    cout << "  MaxBallotAccepted: " << entry.max_ballot_accepted_ << endl;
                } else {
                    cerr << "Failed to decode log entry: " << key << endl;
                }
            }

            char* iter_err = nullptr;
            rocksdb_iter_get_error(it, &iter_err);
            if (iter_err != nullptr) {
                cerr << "Iterator error: " << take_rocksdb_error(&iter_err) << endl;
            }

            if (count == 0) {
                cout << "No log entries found" << endl;
            } else {
                cout << "\nTotal log entries: " << count << endl;
            }

            rocksdb_iter_destroy(it);
        }
    } else {
        cout << "Logs column family not found" << endl;
    }

    // Cleanup
    rocksdb_readoptions_destroy(read_options);
    for (auto* handle : handles) {
        if (handle != nullptr) {
            rocksdb_column_family_handle_destroy(handle);
        }
    }
    rocksdb_close(db);
    rocksdb_list_column_families_destroy(cf_names, cf_count);
    rocksdb_options_destroy(options);

    cout << "\n======================================" << endl;
    return 0;
}
