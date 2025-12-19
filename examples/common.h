#ifndef MAKO_EXAMPLES_COMMON_H
#define MAKO_EXAMPLES_COMMON_H

#include <iostream>
#include <vector>
#include <string>
#include <unistd.h>
#include <limits.h>
#include <stdlib.h>
#include <mako.hh>
#include "../src/mako/benchmarks/mbta_sharded_ordered_index.hh"

using namespace std;

typedef std::pair<std::string, std::string> kv_pair;

// Color codes for terminal output
#define GREEN "\033[32m"
#define RED "\033[31m"
#define RESET "\033[0m"

// Verification macros with colored output
#define VERIFY_PASS(test_name) \
    printf("[" GREEN "PASS" RESET "] %s\n", test_name)

#define VERIFY_FAIL(test_name) \
    printf("[" RED "FAIL" RESET "] %s\n", test_name)

#define VERIFY(condition, test_name) \
    do { \
        if (condition) { \
            VERIFY_PASS(test_name); \
        } else { \
            VERIFY_FAIL(test_name); \
            abort(); \
        } \
    } while(0)

#define VERIFY_EQ(actual, expected, test_name) \
    do { \
        if ((actual) == (expected)) { \
            VERIFY_PASS(test_name); \
        } else { \
            VERIFY_FAIL(test_name); \
            cerr << "  Expected: " << (expected) << ", Got: " << (actual) << endl; \
            abort(); \
        } \
    } while(0)

// Keep existing ASSERT macros for compatibility
#ifndef ASSERT
#define ASSERT(cond) \
    if (!(cond)) { \
        cerr << "Assertion failed: " << #cond << " at " << __FILE__ << ":" << __LINE__ << endl; \
        abort(); \
    }
#endif

#ifndef ASSERT_EQ
#define ASSERT_EQ(a, b) \
    if ((a) != (b)) { \
        cerr << "Assertion failed: " << #a << " != " << #b << " at " << __FILE__ << ":" << __LINE__ << endl; \
        abort(); \
    }
#endif

// Get the directory path of this header file
std::string get_current_absolute_path() {
    char resolved_path[PATH_MAX];
    realpath(__FILE__, resolved_path);
    
    std::string full_path(resolved_path);
    size_t last_slash = full_path.find_last_of('/');
    if (last_slash != std::string::npos) {
        return full_path.substr(0, last_slash + 1);
    }
    return "./";
}

// Scan callback for collecting results
class new_scan_callback_bench : public abstract_ordered_index::scan_callback {
public:
    new_scan_callback_bench() {}
    
    virtual bool invoke(const char *keyp, size_t keylen, const string &value) {
        values.emplace_back(std::string(keyp, keylen), value);
        return true;
    }
    
    std::vector<kv_pair> values;
};

// Scan a table and return all key-value pairs
std::vector<kv_pair> scan_tables(abstract_db *db, abstract_ordered_index* table) {
    str_arena arena;
    void *buf = NULL;
    char WS = static_cast<char>(0);
    std::string startKey(1, WS);
    char WE = static_cast<char>(255);
    std::string endKey(1, WE);
    new_scan_callback_bench calloc;
    void *txn0 = db->new_txn(0, arena, buf, abstract_db::HINT_DEFAULT);
    table->scan(txn0, startKey, &endKey, calloc);
    db->commit_txn(txn0);
    return calloc.values;
}

inline std::vector<kv_pair> scan_tables(abstract_db *db, mbta_sharded_ordered_index* table) {
    str_arena arena;
    void *buf = NULL;
    char WS = static_cast<char>(0);
    std::string startKey(1, WS);
    char WE = static_cast<char>(255);
    std::string endKey(1, WE);
    new_scan_callback_bench calloc;
    void *txn0 = db->new_txn(0, arena, buf, abstract_db::HINT_DEFAULT);
    table->scan(txn0, startKey, &endKey, calloc);
    db->commit_txn(txn0);
    return calloc.values;
}

#endif // MAKO_EXAMPLES_COMMON_H
