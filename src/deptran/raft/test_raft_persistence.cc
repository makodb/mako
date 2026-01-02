#include "raft_persistence.h"
#include "server.h"
#include "../__dep__.h"
#include "../classic/tpc_command.h"
#include <iostream>
#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>

using namespace janus;

// Helper function to remove directory recursively
void remove_directory(const std::string& path) {
    std::string cmd = "rm -rf " + path;
    system(cmd.c_str());
}

// Test 1: Basic initialization
bool TestInit() {
    std::cout << "Test 1: Basic Initialization... ";

    std::string test_path = "/tmp/test_raft_persistence_1";
    remove_directory(test_path);

    RaftPersistence persistence;
    bool success = persistence.Init(0, 0, test_path);

    if (!success) {
        std::cout << "FAILED (Init returned false)" << std::endl;
        return false;
    }

    // Check that directory was created
    struct stat info;
    std::string db_path = test_path + "/raft_0_partition_0";
    if (stat(db_path.c_str(), &info) != 0) {
        std::cout << "FAILED (database directory not created)" << std::endl;
        return false;
    }

    std::cout << "PASSED" << std::endl;
    remove_directory(test_path);
    return true;
}

// Test 2: Persist and load term
bool TestPersistLoadTerm() {
    std::cout << "Test 2: Persist and Load Term... ";

    std::string test_path = "/tmp/test_raft_persistence_2";
    remove_directory(test_path);

    {
        RaftPersistence persistence;
        persistence.Init(0, 0, test_path);

        // Persist term
        uint64_t term_write = 42;
        bool success = persistence.PersistTerm(term_write);
        if (!success) {
            std::cout << "FAILED (PersistTerm returned false)" << std::endl;
            return false;
        }
    }

    // Reopen and load
    {
        RaftPersistence persistence;
        persistence.Init(0, 0, test_path);

        uint64_t term_read = 0;
        bool success = persistence.LoadTerm(term_read);
        if (!success) {
            std::cout << "FAILED (LoadTerm returned false)" << std::endl;
            return false;
        }

        if (term_read != 42) {
            std::cout << "FAILED (term mismatch: expected 42, got " << term_read << ")" << std::endl;
            return false;
        }
    }

    std::cout << "PASSED" << std::endl;
    remove_directory(test_path);
    return true;
}

// Test 3: Persist and load votedFor
bool TestPersistLoadVotedFor() {
    std::cout << "Test 3: Persist and Load VotedFor... ";

    std::string test_path = "/tmp/test_raft_persistence_3";
    remove_directory(test_path);

    {
        RaftPersistence persistence;
        persistence.Init(0, 0, test_path);

        // Persist votedFor
        uint32_t voted_for_write = 5;
        bool success = persistence.PersistVotedFor(voted_for_write);
        if (!success) {
            std::cout << "FAILED (PersistVotedFor returned false)" << std::endl;
            return false;
        }
    }

    // Reopen and load
    {
        RaftPersistence persistence;
        persistence.Init(0, 0, test_path);

        uint32_t voted_for_read = 0;
        bool success = persistence.LoadVotedFor(voted_for_read);
        if (!success) {
            std::cout << "FAILED (LoadVotedFor returned false)" << std::endl;
            return false;
        }

        if (voted_for_read != 5) {
            std::cout << "FAILED (votedFor mismatch: expected 5, got " << voted_for_read << ")" << std::endl;
            return false;
        }
    }

    std::cout << "PASSED" << std::endl;
    remove_directory(test_path);
    return true;
}

// Test 4: Load defaults when nothing persisted
bool TestLoadDefaults() {
    std::cout << "Test 4: Load Defaults (Empty Database)... ";

    std::string test_path = "/tmp/test_raft_persistence_4";
    remove_directory(test_path);

    RaftPersistence persistence;
    persistence.Init(0, 0, test_path);

    uint64_t term = 999;
    uint32_t voted_for = 999;

    persistence.LoadTerm(term);
    persistence.LoadVotedFor(voted_for);

    if (term != 0) {
        std::cout << "FAILED (default term should be 0, got " << term << ")" << std::endl;
        return false;
    }

    if (voted_for != (uint32_t)-1) {
        std::cout << "FAILED (default votedFor should be INVALID_SITEID, got " << voted_for << ")" << std::endl;
        return false;
    }

    std::cout << "PASSED" << std::endl;
    remove_directory(test_path);
    return true;
}

// Test 5: Persist and load single log entry
bool TestPersistLoadSingleLog() {
    std::cout << "Test 5: Persist and Load Single Log Entry... ";

    std::string test_path = "/tmp/test_raft_persistence_5";
    remove_directory(test_path);

    {
        RaftPersistence persistence;
        persistence.Init(0, 0, test_path);

        // Create a simple log entry
        RaftData entry;
        entry.term = 10;
        entry.slot_id = 1;
        entry.log_ = std::make_shared<TpcCommitCommand>();

        bool success = persistence.PersistLogEntry(1, entry);
        if (!success) {
            std::cout << "FAILED (PersistLogEntry returned false)" << std::endl;
            return false;
        }
    }

    // Reopen and load
    {
        RaftPersistence persistence;
        persistence.Init(0, 0, test_path);

        std::map<slotid_t, shared_ptr<RaftData>> logs;
        bool success = persistence.LoadAllLogs(logs);
        if (!success) {
            std::cout << "FAILED (LoadAllLogs returned false)" << std::endl;
            return false;
        }

        if (logs.size() != 1) {
            std::cout << "FAILED (expected 1 log entry, got " << logs.size() << ")" << std::endl;
            return false;
        }

        if (logs[1]->term != 10 || logs[1]->slot_id != 1) {
            std::cout << "FAILED (log entry mismatch)" << std::endl;
            return false;
        }
    }

    std::cout << "PASSED" << std::endl;
    remove_directory(test_path);
    return true;
}

// Test 6: Persist and load multiple log entries
bool TestPersistLoadMultipleLogs() {
    std::cout << "Test 6: Persist and Load Multiple Log Entries... ";

    std::string test_path = "/tmp/test_raft_persistence_6";
    remove_directory(test_path);

    const int NUM_ENTRIES = 10;

    {
        RaftPersistence persistence;
        persistence.Init(0, 0, test_path);

        // Create multiple log entries
        for (int i = 1; i <= NUM_ENTRIES; i++) {
            RaftData entry;
            entry.term = i * 10;
            entry.slot_id = i;
            entry.log_ = std::make_shared<TpcCommitCommand>();

            bool success = persistence.PersistLogEntry(i, entry);
            if (!success) {
                std::cout << "FAILED (PersistLogEntry " << i << " returned false)" << std::endl;
                return false;
            }
        }
    }

    // Reopen and load
    {
        RaftPersistence persistence;
        persistence.Init(0, 0, test_path);

        std::map<slotid_t, shared_ptr<RaftData>> logs;
        bool success = persistence.LoadAllLogs(logs);
        if (!success) {
            std::cout << "FAILED (LoadAllLogs returned false)" << std::endl;
            return false;
        }

        if (logs.size() != NUM_ENTRIES) {
            std::cout << "FAILED (expected " << NUM_ENTRIES << " log entries, got " << logs.size() << ")" << std::endl;
            return false;
        }

        // Verify each entry
        for (int i = 1; i <= NUM_ENTRIES; i++) {
            if (logs.find(i) == logs.end()) {
                std::cout << "FAILED (log entry " << i << " not found)" << std::endl;
                return false;
            }

            uint64_t expected_term = i * 10;
            uint64_t expected_slot_id = i;

            if (logs[i]->term != expected_term || logs[i]->slot_id != expected_slot_id) {
                std::cout << "FAILED (log entry " << i << " mismatch)" << std::endl;
                return false;
            }
        }
    }

    std::cout << "PASSED" << std::endl;
    remove_directory(test_path);
    return true;
}

// Test 7: Load log range
bool TestLoadLogRange() {
    std::cout << "Test 7: Load Log Range... ";

    std::string test_path = "/tmp/test_raft_persistence_7";
    remove_directory(test_path);

    {
        RaftPersistence persistence;
        persistence.Init(0, 0, test_path);

        // Create 10 log entries (slot_id 1-10)
        for (int i = 1; i <= 10; i++) {
            RaftData entry;
            entry.term = i;
            entry.slot_id = i;
            entry.log_ = std::make_shared<TpcCommitCommand>();
            persistence.PersistLogEntry(i, entry);
        }
    }

    // Load range [3, 7]
    {
        RaftPersistence persistence;
        persistence.Init(0, 0, test_path);

        std::map<slotid_t, shared_ptr<RaftData>> logs;
        bool success = persistence.LoadLogRange(3, 7, logs);
        if (!success) {
            std::cout << "FAILED (LoadLogRange returned false)" << std::endl;
            return false;
        }

        if (logs.size() != 5) {  // Slot IDs 3,4,5,6,7
            std::cout << "FAILED (expected 5 log entries, got " << logs.size() << ")" << std::endl;
            return false;
        }

        // Verify slot IDs
        for (int i = 3; i <= 7; i++) {
            if (logs.find(i) == logs.end()) {
                std::cout << "FAILED (slot " << i << " not found)" << std::endl;
                return false;
            }
            if (logs[i]->slot_id != i) {
                std::cout << "FAILED (slot_id mismatch for slot " << i << ")" << std::endl;
                return false;
            }
        }
    }

    std::cout << "PASSED" << std::endl;
    remove_directory(test_path);
    return true;
}

// Test 8: Update term multiple times
bool TestUpdateTerm() {
    std::cout << "Test 8: Update Term Multiple Times... ";

    std::string test_path = "/tmp/test_raft_persistence_8";
    remove_directory(test_path);

    RaftPersistence persistence;
    persistence.Init(0, 0, test_path);

    // Update term several times
    for (uint64_t t = 1; t <= 5; t++) {
        persistence.PersistTerm(t);

        uint64_t loaded_term;
        persistence.LoadTerm(loaded_term);

        if (loaded_term != t) {
            std::cout << "FAILED (term mismatch after update " << t << ")" << std::endl;
            return false;
        }
    }

    std::cout << "PASSED" << std::endl;
    remove_directory(test_path);
    return true;
}

// Test 9: Persist commit index
bool TestPersistCommitIndex() {
    std::cout << "Test 9: Persist Commit Index... ";

    std::string test_path = "/tmp/test_raft_persistence_9";
    remove_directory(test_path);

    RaftPersistence persistence;
    persistence.Init(0, 0, test_path);

    uint64_t commit_index = 42;
    bool success = persistence.PersistCommitIndex(commit_index);

    if (!success) {
        std::cout << "FAILED (PersistCommitIndex returned false)" << std::endl;
        return false;
    }

    std::cout << "PASSED" << std::endl;
    remove_directory(test_path);
    return true;
}

// Test 10: Multiple site IDs (different databases)
bool TestMultipleSites() {
    std::cout << "Test 10: Multiple Site IDs (Isolation)... ";

    std::string test_path = "/tmp/test_raft_persistence_10";
    remove_directory(test_path);

    // Site 0
    {
        RaftPersistence persistence;
        persistence.Init(0, 0, test_path);
        persistence.PersistTerm(100);
    }

    // Site 1
    {
        RaftPersistence persistence;
        persistence.Init(1, 0, test_path);
        persistence.PersistTerm(200);
    }

    // Verify site 0 term is still 100
    {
        RaftPersistence persistence;
        persistence.Init(0, 0, test_path);
        uint64_t term;
        persistence.LoadTerm(term);
        if (term != 100) {
            std::cout << "FAILED (site 0 term corrupted)" << std::endl;
            return false;
        }
    }

    // Verify site 1 term is 200
    {
        RaftPersistence persistence;
        persistence.Init(1, 0, test_path);
        uint64_t term;
        persistence.LoadTerm(term);
        if (term != 200) {
            std::cout << "FAILED (site 1 term incorrect)" << std::endl;
            return false;
        }
    }

    std::cout << "PASSED" << std::endl;
    remove_directory(test_path);
    return true;
}

int main() {
    std::cout << "================================" << std::endl;
    std::cout << "Raft Persistence Unit Tests" << std::endl;
    std::cout << "================================" << std::endl;

    int passed = 0;
    int failed = 0;

    if (TestInit()) passed++; else failed++;
    if (TestPersistLoadTerm()) passed++; else failed++;
    if (TestPersistLoadVotedFor()) passed++; else failed++;
    if (TestLoadDefaults()) passed++; else failed++;
    if (TestPersistLoadSingleLog()) passed++; else failed++;
    if (TestPersistLoadMultipleLogs()) passed++; else failed++;
    if (TestLoadLogRange()) passed++; else failed++;
    if (TestUpdateTerm()) passed++; else failed++;
    if (TestPersistCommitIndex()) passed++; else failed++;
    if (TestMultipleSites()) passed++; else failed++;

    std::cout << "================================" << std::endl;
    std::cout << "Results: " << passed << " passed, " << failed << " failed" << std::endl;
    std::cout << "================================" << std::endl;

    return (failed == 0) ? 0 : 1;
}
