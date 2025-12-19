#ifndef MAKO_EXAMPLES_TEST_VERIFICATION_H
#define MAKO_EXAMPLES_TEST_VERIFICATION_H

#include <iostream>
#include <string>
#include <vector>
#include <map>
#include "common.h"

using namespace std;

/**
 * Simple Database Integrity Verifier
 * Verifies expected key-value pairs exist in the database
 */
class DatabaseIntegrityVerifier {
public:
    DatabaseIntegrityVerifier(const std::string& test_name = "Database")
        : test_name_(test_name), failed_(false) {}

    // Add an expected key-value pair
    void expect_key_value(const std::string& key, const std::string& value) {
        expected_kvs_[key] = value;
    }

    // Add an expected key (value can be anything)
    void expect_key_exists(const std::string& key) {
        expected_keys_.insert(key);
    }

    // Set the exact expected total count
    void expect_total_count(size_t count) {
        expected_total_count_ = count;
        check_total_count_ = true;
    }

    // Main verification
    bool verify(const std::vector<kv_pair>& records) {
        printf("\n");
        printf("========================================\n");
        printf("=== %s INTEGRITY VERIFICATION ===\n", test_name_.c_str());
        printf("========================================\n");
        printf("Total records scanned: %zu\n", records.size());
        printf("\n");

        // Build lookup map
        std::map<std::string, std::string> db_map;
        for (const auto& record : records) {
            db_map[record.first] = record.second;
        }

        // Verify exact key-value pairs
        if (!expected_kvs_.empty()) {
            verify_exact_kvs(db_map);
        }

        // Verify key existence
        if (!expected_keys_.empty()) {
            verify_keys_exist(db_map);
        }

        // Verify total count
        if (check_total_count_) {
            verify_count(records.size());
        }

        // Print summary
        print_summary();

        return !failed_;
    }

private:
    std::string test_name_;
    std::map<std::string, std::string> expected_kvs_;
    std::set<std::string> expected_keys_;
    bool check_total_count_ = false;
    size_t expected_total_count_ = 0;
    bool failed_;

    void verify_exact_kvs(const std::map<std::string, std::string>& db_map) {
        printf("\n--- Exact Key-Value Verification ---\n");
        printf("Expected: %zu exact key-value pairs\n", expected_kvs_.size());

        int passed = 0;
        for (const auto& expected_kv : expected_kvs_) {
            auto it = db_map.find(expected_kv.first);
            if (it == db_map.end()) {
                printf(RED "✗ FAIL: Key '%s' not found\n" RESET, expected_kv.first.c_str());
                failed_ = true;
            } else if (it->second != expected_kv.second) {
                printf(RED "✗ FAIL: Key '%s' value mismatch\n" RESET, expected_kv.first.c_str());
                printf("  Expected: %s\n", expected_kv.second.substr(0, 50).c_str());
                printf("  Got:      %s\n", it->second.substr(0, 50).c_str());
                failed_ = true;
            } else {
                passed++;
            }
        }

        if (passed == (int)expected_kvs_.size()) {
            printf(GREEN "✓ PASS: All %d key-value pairs match\n" RESET, passed);
        } else {
            printf(RED "✗ FAIL: Only %d/%zu key-value pairs match\n" RESET,
                   passed, expected_kvs_.size());
        }
    }

    void verify_keys_exist(const std::map<std::string, std::string>& db_map) {
        printf("\n--- Key Existence Verification ---\n");
        printf("Expected: %zu keys to exist\n", expected_keys_.size());

        int found = 0;
        for (const auto& key : expected_keys_) {
            if (db_map.find(key) == db_map.end()) {
                printf(RED "✗ FAIL: Key '%s' not found\n" RESET, key.c_str());
                failed_ = true;
            } else {
                found++;
            }
        }

        if (found == (int)expected_keys_.size()) {
            printf(GREEN "✓ PASS: All %d keys exist\n" RESET, found);
        } else {
            printf(RED "✗ FAIL: Only %d/%zu keys exist\n" RESET, found, expected_keys_.size());
        }
    }

    void verify_count(size_t actual_count) {
        printf("\n--- Total Count Verification ---\n");
        printf("Found: %zu keys\n", actual_count);
        printf("Expected: %zu keys\n", expected_total_count_);

        if (actual_count != expected_total_count_) {
            printf(RED "✗ FAIL: Count mismatch\n" RESET);
            failed_ = true;
        } else {
            printf(GREEN "✓ PASS: Count matches\n" RESET);
        }
    }

    void print_summary() {
        printf("\n========================================\n");
        if (failed_) {
            printf(RED "=== VERIFICATION FAILED ===" RESET "\n");
        } else {
            printf(GREEN "=== ALL VERIFICATIONS PASSED ===" RESET "\n");
        }
        printf("========================================\n");
    }
};

#endif // MAKO_EXAMPLES_TEST_VERIFICATION_H
