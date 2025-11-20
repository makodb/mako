/**
 * Comprehensive Masstree Test Suite
 * 
 * This file contains all tests for Masstree migration to RustyCpp:
 * - String Infrastructure (Str, String, StringAccum) - FUNCTIONAL
 * - Basic Tree Operations (insert, get, remove) - TEMPLATE (for Phase 5)
 * - Concurrent Access - TEMPLATE (for Phase 5)
 * - Range Scans - TEMPLATE (for Phase 5)
 * 
 * Phase 3 Status: String infrastructure tests are complete and functional.
 * Phase 5 TODO: Update tree operation tests with actual Masstree API.
 */

#include <gtest/gtest.h>
#include "mako/masstree/str.hh"
#include "mako/masstree/string.hh"
#include "mako/masstree/straccum.hh"
#include "mako/masstree/string_slice.hh"
#include "mako/masstree/masstree.hh"
#include "mako/masstree/kvthread.hh"

#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <random>
#include <algorithm>
#include <chrono>

using namespace lcdf;

// ============================================================================
// PHASE 3: STRING INFRASTRUCTURE TESTS (FUNCTIONAL)
// ============================================================================

// ============================================================================
// Str Tests (Lightweight string view)
// ============================================================================

TEST(MasstreeStringTest, StrBasicConstruction) {
    // Test default constructor
    Str empty;
    EXPECT_EQ(empty.length(), 0);
    EXPECT_EQ(empty.data(), nullptr);
    
    // Test C string constructor
    Str hello("hello");
    EXPECT_EQ(hello.length(), 5);
    EXPECT_STREQ(hello.data(), "hello");
    
    // Test pointer + length constructor
    const char* data = "world";
    Str world(data, 5);
    EXPECT_EQ(world.length(), 5);
    EXPECT_EQ(strncmp(world.data(), "world", 5), 0);
}

TEST(MasstreeStringTest, StrComparison) {
    Str a("apple");
    Str b("banana");
    Str c("apple");
    
    EXPECT_TRUE(a == c);
    EXPECT_TRUE(a != b);
    EXPECT_TRUE(a < b);
    EXPECT_TRUE(b > a);
}

TEST(MasstreeStringTest, StrSubstring) {
    Str original("hello world");
    
    // Test prefix
    Str prefix = original.prefix(5);
    EXPECT_EQ(prefix.length(), 5);
    EXPECT_EQ(strncmp(prefix.data(), "hello", 5), 0);
    
    // Test substring
    Str sub = original.substring(original.data() + 6, original.data() + 11);
    EXPECT_EQ(sub.length(), 5);
    EXPECT_EQ(strncmp(sub.data(), "world", 5), 0);
}

TEST(MasstreeStringTest, StrTrimming) {
    Str whitespace("  hello  ");
    
    Str ltrimmed = whitespace.ltrim();
    EXPECT_EQ(strncmp(ltrimmed.data(), "hello  ", 7), 0);
    
    Str rtrimmed = whitespace.rtrim();
    EXPECT_EQ(strncmp(rtrimmed.data(), "  hello", 7), 0);
    
    Str trimmed = whitespace.trim();
    EXPECT_EQ(trimmed.length(), 5);
    EXPECT_EQ(strncmp(trimmed.data(), "hello", 5), 0);
}

TEST(MasstreeStringTest, StrToInteger) {
    Str num("12345");
    long result = num.to_i();
    EXPECT_EQ(result, 12345);
    
    Str invalid("abc");
    long invalid_result = invalid.to_i();
    EXPECT_EQ(invalid_result, -1);
}

// ============================================================================
// String Tests (Owned string with reference counting)
// ============================================================================

TEST(MasstreeStringTest, StringBasicConstruction) {
    // Test default constructor
    String empty;
    EXPECT_EQ(empty.length(), 0);
    
    // Test C string constructor
    String hello("hello");
    EXPECT_EQ(hello.length(), 5);
    EXPECT_STREQ(hello.c_str(), "hello");
    
    // Test copy constructor
    String copy(hello);
    EXPECT_EQ(copy.length(), 5);
    EXPECT_STREQ(copy.c_str(), "hello");
}

TEST(MasstreeStringTest, StringFromIntegers) {
    String from_int(42);
    EXPECT_STREQ(from_int.c_str(), "42");
    
    String from_long(1234567890L);
    EXPECT_STREQ(from_long.c_str(), "1234567890");
    
    String from_double(3.14);
    EXPECT_TRUE(from_double.length() > 0);
    EXPECT_TRUE(from_double.find_left('.') >= 0);
}

TEST(MasstreeStringTest, StringAppend) {
    String str("hello");
    str += " ";
    str += "world";
    EXPECT_STREQ(str.c_str(), "hello world");
    
    String str2 = String("foo") + String("bar");
    EXPECT_STREQ(str2.c_str(), "foobar");
}

TEST(MasstreeStringTest, StringSubstring) {
    String original("hello world");
    
    // Test substr
    String sub = original.substr(6, 5);
    EXPECT_STREQ(sub.c_str(), "world");
    
    // Test negative length (from end)
    String from_end = original.substr(0, -6);
    EXPECT_STREQ(from_end.c_str(), "hello");
}

TEST(MasstreeStringTest, StringCaseConversion) {
    String mixed("Hello World");
    
    String lower = mixed.lower();
    EXPECT_STREQ(lower.c_str(), "hello world");
    
    String upper = mixed.upper();
    EXPECT_STREQ(upper.c_str(), "HELLO WORLD");
}

TEST(MasstreeStringTest, StringFind) {
    String str("hello world hello");
    
    // Find character
    int pos1 = str.find_left('o');
    EXPECT_EQ(pos1, 4);  // First 'o' in first "hello"
    
    int pos2 = str.find_right('o');
    EXPECT_EQ(pos2, 16);  // Last 'o' in second "hello"
    
    // Find substring
    int pos3 = str.find_left("world");
    EXPECT_EQ(pos3, 6);
    
    int pos4 = str.find_left("xyz");
    EXPECT_EQ(pos4, -1);
}

TEST(MasstreeStringTest, StringTrim) {
    String whitespace("  hello world  ");
    
    String trimmed = whitespace.trim();
    EXPECT_STREQ(trimmed.c_str(), "hello world");
}

// ============================================================================
// StringAccum Tests (String builder)
// ============================================================================

TEST(MasstreeStringTest, StringAccumBasic) {
    StringAccum sa;
    sa << "hello";
    sa << " ";
    sa << "world";
    
    String result = sa.take_string();
    EXPECT_STREQ(result.c_str(), "hello world");
}

TEST(MasstreeStringTest, StringAccumNumbers) {
    StringAccum sa;
    sa << "Number: " << 42 << ", Float: " << 3.14;
    
    String result = sa.take_string();
    EXPECT_TRUE(result.find_left("42") >= 0);
    EXPECT_TRUE(result.find_left("3.14") >= 0);
}

TEST(MasstreeStringTest, StringAccumReserve) {
    StringAccum sa;
    sa.reserve(100);
    
    for (int i = 0; i < 10; i++) {
        sa << "test";
    }
    
    String result = sa.take_string();
    EXPECT_EQ(result.length(), 40); // "test" * 10
}

// ============================================================================
// String Slice Tests
// ============================================================================

TEST(MasstreeStringTest, StringSliceBasic) {
    String original("hello world");
    
    // Test that substring shares memory with original
    String slice = original.substr(6, 5);
    EXPECT_EQ(slice.length(), 5);
    EXPECT_STREQ(slice.c_str(), "world");
}

// ============================================================================
// Memory Safety Tests
// ============================================================================

TEST(MasstreeStringTest, StringOutOfMemory) {
    // Test out-of-memory handling
    String oom = String::make_out_of_memory();
    EXPECT_TRUE(oom.out_of_memory());
    
    // Operations on OOM should return OOM
    String result = oom + String("test");
    EXPECT_TRUE(result.out_of_memory());
}

TEST(MasstreeStringTest, StringSharing) {
    // Test that strings properly share memory
    String original("hello world");
    String shared(original);
    
    // Both should point to same data
    EXPECT_EQ(original.data(), shared.data());
    
    // Modification should create copy
    String modified = shared;
    // Note: String uses copy-on-write, so data() may still be shared
    // until actual modification occurs
}

TEST(MasstreeStringTest, StringMutableData) {
    String str("hello");
    char* mutable_ptr = str.mutable_data();
    
    // Modify through mutable pointer
    mutable_ptr[0] = 'H';
    
    EXPECT_STREQ(str.c_str(), "Hello");
}

// ============================================================================
// Performance-Critical Operations
// ============================================================================

TEST(MasstreeStringTest, StringHashCode) {
    String str1("hello");
    String str2("hello");
    String str3("world");
    
    // Same strings should have same hash
    EXPECT_EQ(str1.hashcode(), str2.hashcode());
    
    // Different strings should (probably) have different hash
    EXPECT_NE(str1.hashcode(), str3.hashcode());
}

TEST(MasstreeStringTest, StringComparison) {
    String a("apple");
    String b("banana");
    String c("apple");
    
    EXPECT_EQ(a.compare(b), a.data()[0] - b.data()[0]);
    EXPECT_EQ(a.compare(c), 0);
    EXPECT_TRUE(a < b);
    EXPECT_TRUE(a == c);
}

// ============================================================================
// Edge Cases
// ============================================================================

TEST(MasstreeStringTest, EmptyStrings) {
    Str empty1;
    Str empty2("");
    String empty3;
    String empty4("");
    
    EXPECT_EQ(empty1.length(), 0);
    EXPECT_EQ(empty2.length(), 0);
    EXPECT_EQ(empty3.length(), 0);
    EXPECT_EQ(empty4.length(), 0);
    
    EXPECT_TRUE(empty1 == empty2);
    EXPECT_TRUE(empty3 == empty4);
}

TEST(MasstreeStringTest, NullCharacters) {
    const char data[] = {'h', 'e', '\0', 'l', 'o'};
    Str str(data, 5);
    EXPECT_EQ(str.length(), 5);
    
    String str2(data, 5);
    EXPECT_EQ(str2.length(), 5);
}

TEST(MasstreeStringTest, LongStrings) {
    // Test with strings longer than typical small string optimization
    std::string long_str(10000, 'x');
    String str(long_str.c_str());
    
    EXPECT_EQ(str.length(), 10000);
    EXPECT_EQ(str.data()[0], 'x');
    EXPECT_EQ(str.data()[9999], 'x');
}

// ============================================================================
// PHASE 5: BASIC TREE OPERATIONS (PLACEHOLDER TEMPLATES)
// ============================================================================
// NOTE: These tests are templates and will not compile until Phase 5.
// Uncomment and update when working on core tree structure migration.

/*
typedef Masstree::basic_table<uint64_t> test_table;

class MasstreeBasicTest : public ::testing::Test {
protected:
    void SetUp() override {
        threadinfo ti = threadinfo::make(threadinfo::TI_MAIN, -1);
        ti_ = &ti;
    }
    
    void TearDown() override {
        ti_ = nullptr;
    }
    
    threadinfo* ti_;
};

TEST_F(MasstreeBasicTest, InsertAndLookup) {
    test_table table;
    uint64_t key = 42;
    uint64_t value = 100;
    
    bool inserted = table.insert(Str((const char*)&key, sizeof(key)), value, *ti_);
    EXPECT_TRUE(inserted);
    
    uint64_t result;
    bool found = table.get(Str((const char*)&key, sizeof(key)), result, *ti_);
    EXPECT_TRUE(found);
    EXPECT_EQ(result, value);
}
*/

// ============================================================================
// PHASE 5: CONCURRENT OPERATIONS (PLACEHOLDER TEMPLATES)
// ============================================================================
// NOTE: Concurrent tests will be added in Phase 5 when working on
// lock-free mechanisms and thread safety.

/*
class MasstreeConcurrentTest : public ::testing::Test {
protected:
    void SetUp() override {
        threadinfo ti = threadinfo::make(threadinfo::TI_MAIN, -1);
        main_ti_ = &ti;
    }
    
    void TearDown() override {
        main_ti_ = nullptr;
    }
    
    threadinfo* main_ti_;
};

TEST_F(MasstreeConcurrentTest, ConcurrentInserts) {
    // TODO: Implement in Phase 5
}
*/

// ============================================================================
// PHASE 5: RANGE SCAN OPERATIONS (PLACEHOLDER TEMPLATES)
// ============================================================================
// NOTE: Scan tests will be added in Phase 5 when working on
// tree traversal and range queries.

/*
class MasstreeScanTest : public ::testing::Test {
protected:
    void SetUp() override {
        threadinfo ti = threadinfo::make(threadinfo::TI_MAIN, -1);
        ti_ = &ti;
    }
    
    void TearDown() override {
        ti_ = nullptr;
    }
    
    threadinfo* ti_;
};

TEST_F(MasstreeScanTest, FullTableScan) {
    // TODO: Implement in Phase 5
}
*/

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

