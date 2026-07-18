#include <stdint.h>
#include <stddef.h>

#include <rusty/arc.hpp>
#include <rusty/option.hpp>
#include <rusty/box.hpp>
#include <gtest/gtest.h>
#include "../rrr.hpp"

import std;
import rusty;

import std;

using namespace rrr;

class MarshalTest : public ::testing::Test {
protected:
    rusty::Option<rusty::Box<Marshal>> m;

    void SetUp() override {
        m = rusty::make_box<Marshal>();
    }
};

TEST_F(MarshalTest, BasicIntegerTypes) {
    i8 i8_val = -128;
    i16 i16_val = -32768;
    i32 i32_val = -2147483648;
    i64 i64_val = -9223372036854775807LL;
    
    rrr::Serialize_::serialize(i8_val, *m.as_ref().unwrap());
    rrr::Serialize_::serialize(i16_val, *m.as_ref().unwrap());
    rrr::Serialize_::serialize(i32_val, *m.as_ref().unwrap());
    rrr::Serialize_::serialize(i64_val, *m.as_ref().unwrap());
    
    i8 i8_out;
    i16 i16_out;
    i32 i32_out;
    i64 i64_out;
    
    rrr::Deserialize_::deserialize(i8_out, *m.as_ref().unwrap());
    rrr::Deserialize_::deserialize(i16_out, *m.as_ref().unwrap());
    rrr::Deserialize_::deserialize(i32_out, *m.as_ref().unwrap());
    rrr::Deserialize_::deserialize(i64_out, *m.as_ref().unwrap());
    
    EXPECT_EQ(i8_val, i8_out);
    EXPECT_EQ(i16_val, i16_out);
    EXPECT_EQ(i32_val, i32_out);
    EXPECT_EQ(i64_val, i64_out);
}

TEST_F(MarshalTest, UnsignedIntegerTypes) {
    uint8_t u8_val = 255;
    uint16_t u16_val = 65535;
    uint32_t u32_val = 4294967295U;
    uint64_t u64_val = 18446744073709551615ULL;
    
    rrr::Serialize_::serialize(u8_val, *m.as_ref().unwrap());
    rrr::Serialize_::serialize(u16_val, *m.as_ref().unwrap());
    rrr::Serialize_::serialize(u32_val, *m.as_ref().unwrap());
    rrr::Serialize_::serialize(u64_val, *m.as_ref().unwrap());
    
    uint8_t u8_out;
    uint16_t u16_out;
    uint32_t u32_out;
    uint64_t u64_out;
    
    rrr::Deserialize_::deserialize(u8_out, *m.as_ref().unwrap());
    rrr::Deserialize_::deserialize(u16_out, *m.as_ref().unwrap());
    rrr::Deserialize_::deserialize(u32_out, *m.as_ref().unwrap());
    rrr::Deserialize_::deserialize(u64_out, *m.as_ref().unwrap());
    
    EXPECT_EQ(u8_val, u8_out);
    EXPECT_EQ(u16_val, u16_out);
    EXPECT_EQ(u32_val, u32_out);
    EXPECT_EQ(u64_val, u64_out);
}

TEST_F(MarshalTest, VariableLengthIntegers) {
    v32 v32_small(42);
    v32 v32_large(2147483647);
    v64 v64_small(100);
    v64 v64_large(9223372036854775807LL);
    
    rrr::Serialize_::serialize(v32_small, *m.as_ref().unwrap());
    rrr::Serialize_::serialize(v32_large, *m.as_ref().unwrap());
    rrr::Serialize_::serialize(v64_small, *m.as_ref().unwrap());
    rrr::Serialize_::serialize(v64_large, *m.as_ref().unwrap());
    
    v32 v32_small_out, v32_large_out;
    v64 v64_small_out, v64_large_out;
    
    rrr::Deserialize_::deserialize(v32_small_out, *m.as_ref().unwrap());
    rrr::Deserialize_::deserialize(v32_large_out, *m.as_ref().unwrap());
    rrr::Deserialize_::deserialize(v64_small_out, *m.as_ref().unwrap());
    rrr::Deserialize_::deserialize(v64_large_out, *m.as_ref().unwrap());
    
    EXPECT_EQ(v32_small.get(), v32_small_out.get());
    EXPECT_EQ(v32_large.get(), v32_large_out.get());
    EXPECT_EQ(v64_small.get(), v64_small_out.get());
    EXPECT_EQ(v64_large.get(), v64_large_out.get());
}

TEST_F(MarshalTest, DoubleValues) {
    double d1 = 3.14159265359;
    double d2 = -1.23456789e10;
    double d3 = std::numeric_limits<double>::max();
    double d4 = std::numeric_limits<double>::min();
    double d5 = std::numeric_limits<double>::epsilon();
    
    rrr::Serialize_::serialize(d1, *m.as_ref().unwrap());
    rrr::Serialize_::serialize(d2, *m.as_ref().unwrap());
    rrr::Serialize_::serialize(d3, *m.as_ref().unwrap());
    rrr::Serialize_::serialize(d4, *m.as_ref().unwrap());
    rrr::Serialize_::serialize(d5, *m.as_ref().unwrap());
    
    double d1_out, d2_out, d3_out, d4_out, d5_out;
    rrr::Deserialize_::deserialize(d1_out, *m.as_ref().unwrap());
    rrr::Deserialize_::deserialize(d2_out, *m.as_ref().unwrap());
    rrr::Deserialize_::deserialize(d3_out, *m.as_ref().unwrap());
    rrr::Deserialize_::deserialize(d4_out, *m.as_ref().unwrap());
    rrr::Deserialize_::deserialize(d5_out, *m.as_ref().unwrap());
    
    EXPECT_DOUBLE_EQ(d1, d1_out);
    EXPECT_DOUBLE_EQ(d2, d2_out);
    EXPECT_DOUBLE_EQ(d3, d3_out);
    EXPECT_DOUBLE_EQ(d4, d4_out);
    EXPECT_DOUBLE_EQ(d5, d5_out);
}

TEST_F(MarshalTest, StringValues) {
    std::string empty_str = "";
    std::string short_str = "Hello";
    std::string long_str(10000, 'A');
    std::string unicode_str = "Hello, 世界! 🚀";
    std::string special_chars = "Line1\nLine2\tTab\r\nCRLF\0Null";
    
    rrr::Serialize_::serialize(empty_str, *m.as_ref().unwrap());
    rrr::Serialize_::serialize(short_str, *m.as_ref().unwrap());
    rrr::Serialize_::serialize(long_str, *m.as_ref().unwrap());
    rrr::Serialize_::serialize(unicode_str, *m.as_ref().unwrap());
    rrr::Serialize_::serialize(special_chars, *m.as_ref().unwrap());
    
    std::string empty_out, short_out, long_out, unicode_out, special_out;
    rrr::Deserialize_::deserialize(empty_out, *m.as_ref().unwrap());
    rrr::Deserialize_::deserialize(short_out, *m.as_ref().unwrap());
    rrr::Deserialize_::deserialize(long_out, *m.as_ref().unwrap());
    rrr::Deserialize_::deserialize(unicode_out, *m.as_ref().unwrap());
    rrr::Deserialize_::deserialize(special_out, *m.as_ref().unwrap());
    
    EXPECT_EQ(empty_str, empty_out);
    EXPECT_EQ(short_str, short_out);
    EXPECT_EQ(long_str, long_out);
    EXPECT_EQ(unicode_str, unicode_out);
    EXPECT_EQ(special_chars, special_out);
}

TEST_F(MarshalTest, PairValues) {
    std::pair<int, std::string> p1(42, "answer");
    std::pair<double, double> p2(3.14, 2.71);
    std::pair<std::string, std::vector<int>> p3("numbers", {1, 2, 3, 4, 5});
    
    rrr::Serialize_::serialize(p1, *m.as_ref().unwrap());
    rrr::Serialize_::serialize(p2, *m.as_ref().unwrap());
    rrr::Serialize_::serialize(p3, *m.as_ref().unwrap());
    
    std::pair<int, std::string> p1_out;
    std::pair<double, double> p2_out;
    std::pair<std::string, std::vector<int>> p3_out;
    
    rrr::Deserialize_::deserialize(p1_out, *m.as_ref().unwrap());
    rrr::Deserialize_::deserialize(p2_out, *m.as_ref().unwrap());
    rrr::Deserialize_::deserialize(p3_out, *m.as_ref().unwrap());
    
    EXPECT_EQ(p1, p1_out);
    EXPECT_EQ(p2, p2_out);
    EXPECT_EQ(p3, p3_out);
}

TEST_F(MarshalTest, VectorValues) {
    std::vector<int> empty_vec;
    std::vector<int> int_vec = {1, 2, 3, 4, 5};
    std::vector<std::string> str_vec = {"one", "two", "three"};
    std::vector<std::vector<int>> nested_vec = {{1, 2}, {3, 4, 5}, {6}};
    
    rrr::Serialize_::serialize(empty_vec, *m.as_ref().unwrap());
    rrr::Serialize_::serialize(int_vec, *m.as_ref().unwrap());
    rrr::Serialize_::serialize(str_vec, *m.as_ref().unwrap());
    rrr::Serialize_::serialize(nested_vec, *m.as_ref().unwrap());
    
    std::vector<int> empty_vec_out, int_vec_out;
    std::vector<std::string> str_vec_out;
    std::vector<std::vector<int>> nested_vec_out;
    
    rrr::Deserialize_::deserialize(empty_vec_out, *m.as_ref().unwrap());
    rrr::Deserialize_::deserialize(int_vec_out, *m.as_ref().unwrap());
    rrr::Deserialize_::deserialize(str_vec_out, *m.as_ref().unwrap());
    rrr::Deserialize_::deserialize(nested_vec_out, *m.as_ref().unwrap());
    
    EXPECT_EQ(empty_vec, empty_vec_out);
    EXPECT_EQ(int_vec, int_vec_out);
    EXPECT_EQ(str_vec, str_vec_out);
    EXPECT_EQ(nested_vec, nested_vec_out);
}

TEST_F(MarshalTest, ListValues) {
    std::list<int> empty_list;
    std::list<double> double_list = {1.1, 2.2, 3.3, 4.4};
    std::list<std::string> str_list = {"alpha", "beta", "gamma"};
    
    rrr::Serialize_::serialize(empty_list, *m.as_ref().unwrap());
    rrr::Serialize_::serialize(double_list, *m.as_ref().unwrap());
    rrr::Serialize_::serialize(str_list, *m.as_ref().unwrap());
    
    std::list<int> empty_list_out;
    std::list<double> double_list_out;
    std::list<std::string> str_list_out;
    
    rrr::Deserialize_::deserialize(empty_list_out, *m.as_ref().unwrap());
    rrr::Deserialize_::deserialize(double_list_out, *m.as_ref().unwrap());
    rrr::Deserialize_::deserialize(str_list_out, *m.as_ref().unwrap());
    
    EXPECT_EQ(empty_list, empty_list_out);
    EXPECT_EQ(double_list, double_list_out);
    EXPECT_EQ(str_list, str_list_out);
}

TEST_F(MarshalTest, SetValues) {
    std::set<int> empty_set;
    std::set<int> int_set = {5, 3, 1, 4, 2};
    std::set<std::string> str_set = {"zebra", "apple", "monkey"};
    
    rrr::Serialize_::serialize(empty_set, *m.as_ref().unwrap());
    rrr::Serialize_::serialize(int_set, *m.as_ref().unwrap());
    rrr::Serialize_::serialize(str_set, *m.as_ref().unwrap());
    
    std::set<int> empty_set_out, int_set_out;
    std::set<std::string> str_set_out;
    
    rrr::Deserialize_::deserialize(empty_set_out, *m.as_ref().unwrap());
    rrr::Deserialize_::deserialize(int_set_out, *m.as_ref().unwrap());
    rrr::Deserialize_::deserialize(str_set_out, *m.as_ref().unwrap());
    
    EXPECT_EQ(empty_set, empty_set_out);
    EXPECT_EQ(int_set, int_set_out);
    EXPECT_EQ(str_set, str_set_out);
}

TEST_F(MarshalTest, MapValues) {
    std::map<int, std::string> empty_map;
    std::map<int, std::string> int_str_map = {{1, "one"}, {2, "two"}, {3, "three"}};
    std::map<std::string, std::vector<int>> str_vec_map = {
        {"evens", {2, 4, 6}},
        {"odds", {1, 3, 5}}
    };
    
    rrr::Serialize_::serialize(empty_map, *m.as_ref().unwrap());
    rrr::Serialize_::serialize(int_str_map, *m.as_ref().unwrap());
    rrr::Serialize_::serialize(str_vec_map, *m.as_ref().unwrap());
    
    std::map<int, std::string> empty_map_out, int_str_map_out;
    std::map<std::string, std::vector<int>> str_vec_map_out;
    
    rrr::Deserialize_::deserialize(empty_map_out, *m.as_ref().unwrap());
    rrr::Deserialize_::deserialize(int_str_map_out, *m.as_ref().unwrap());
    rrr::Deserialize_::deserialize(str_vec_map_out, *m.as_ref().unwrap());
    
    EXPECT_EQ(empty_map, empty_map_out);
    EXPECT_EQ(int_str_map, int_str_map_out);
    EXPECT_EQ(str_vec_map, str_vec_map_out);
}

TEST_F(MarshalTest, UnorderedSetValues) {
    std::unordered_set<int> empty_uset;
    std::unordered_set<int> int_uset = {10, 20, 30, 40, 50};
    std::unordered_set<std::string> str_uset = {"hash", "table", "set"};
    
    rrr::Serialize_::serialize(empty_uset, *m.as_ref().unwrap());
    rrr::Serialize_::serialize(int_uset, *m.as_ref().unwrap());
    rrr::Serialize_::serialize(str_uset, *m.as_ref().unwrap());
    
    std::unordered_set<int> empty_uset_out, int_uset_out;
    std::unordered_set<std::string> str_uset_out;
    
    rrr::Deserialize_::deserialize(empty_uset_out, *m.as_ref().unwrap());
    rrr::Deserialize_::deserialize(int_uset_out, *m.as_ref().unwrap());
    rrr::Deserialize_::deserialize(str_uset_out, *m.as_ref().unwrap());
    
    EXPECT_EQ(empty_uset, empty_uset_out);
    EXPECT_EQ(int_uset, int_uset_out);
    EXPECT_EQ(str_uset, str_uset_out);
}

TEST_F(MarshalTest, UnorderedMapValues) {
    std::unordered_map<int, double> empty_umap;
    std::unordered_map<int, double> int_double_umap = {{1, 1.1}, {2, 2.2}, {3, 3.3}};
    std::unordered_map<std::string, int> str_int_umap = {
        {"first", 1},
        {"second", 2},
        {"third", 3}
    };
    
    rrr::Serialize_::serialize(empty_umap, *m.as_ref().unwrap());
    rrr::Serialize_::serialize(int_double_umap, *m.as_ref().unwrap());
    rrr::Serialize_::serialize(str_int_umap, *m.as_ref().unwrap());
    
    std::unordered_map<int, double> empty_umap_out, int_double_umap_out;
    std::unordered_map<std::string, int> str_int_umap_out;
    
    rrr::Deserialize_::deserialize(empty_umap_out, *m.as_ref().unwrap());
    rrr::Deserialize_::deserialize(int_double_umap_out, *m.as_ref().unwrap());
    rrr::Deserialize_::deserialize(str_int_umap_out, *m.as_ref().unwrap());
    
    EXPECT_EQ(empty_umap, empty_umap_out);
    EXPECT_EQ(int_double_umap, int_double_umap_out);
    EXPECT_EQ(str_int_umap, str_int_umap_out);
}

TEST_F(MarshalTest, ComplexNestedStructures) {
    typedef std::map<std::string, std::vector<std::pair<int, double>>> ComplexType;
    
    ComplexType complex_data = {
        {"group1", {{1, 1.1}, {2, 2.2}, {3, 3.3}}},
        {"group2", {{10, 10.5}, {20, 20.5}}},
        {"empty_group", {}}
    };
    
    rrr::Serialize_::serialize(complex_data, *m.as_ref().unwrap());
    
    ComplexType complex_data_out;
    rrr::Deserialize_::deserialize(complex_data_out, *m.as_ref().unwrap());
    
    EXPECT_EQ(complex_data, complex_data_out);
}

TEST_F(MarshalTest, LargeDataSets) {
    const size_t large_size = 100000;
    std::vector<int> large_vec;
    for (size_t i = 0; i < large_size; ++i) {
        large_vec.push_back(i);
    }
    
    rrr::Serialize_::serialize(large_vec, *m.as_ref().unwrap());
    
    std::vector<int> large_vec_out;
    rrr::Deserialize_::deserialize(large_vec_out, *m.as_ref().unwrap());
    
    EXPECT_EQ(large_vec, large_vec_out);
}

TEST_F(MarshalTest, MixedDataTypes) {
    i32 int_val = 42;
    std::string str_val = "test string";
    std::vector<double> vec_val = {1.1, 2.2, 3.3};
    std::map<int, std::string> map_val = {{1, "one"}, {2, "two"}};
    
    rrr::Serialize_::serialize(int_val, *m.as_ref().unwrap());
    rrr::Serialize_::serialize(str_val, *m.as_ref().unwrap());
    rrr::Serialize_::serialize(vec_val, *m.as_ref().unwrap());
    rrr::Serialize_::serialize(map_val, *m.as_ref().unwrap());
    
    i32 int_out;
    std::string str_out;
    std::vector<double> vec_out;
    std::map<int, std::string> map_out;
    
    rrr::Deserialize_::deserialize(int_out, *m.as_ref().unwrap());
    rrr::Deserialize_::deserialize(str_out, *m.as_ref().unwrap());
    rrr::Deserialize_::deserialize(vec_out, *m.as_ref().unwrap());
    rrr::Deserialize_::deserialize(map_out, *m.as_ref().unwrap());
    
    EXPECT_EQ(int_val, int_out);
    EXPECT_EQ(str_val, str_out);
    EXPECT_EQ(vec_val, vec_out);
    EXPECT_EQ(map_val, map_out);
}

TEST_F(MarshalTest, ContentSizeTracking) {
    EXPECT_TRUE(m.as_ref().unwrap()->empty());
    EXPECT_EQ(m.as_ref().unwrap()->content_size(), 0u);
    
    i32 val = 42;
    rrr::Serialize_::serialize(val, *m.as_ref().unwrap());
    
    EXPECT_FALSE(m.as_ref().unwrap()->empty());
    EXPECT_EQ(m.as_ref().unwrap()->content_size(), sizeof(i32));
    
    std::string str = "Hello, World!";
    size_t prev_size = m.as_ref().unwrap()->content_size();
    rrr::Serialize_::serialize(str, *m.as_ref().unwrap());
    
    EXPECT_GT(m.as_ref().unwrap()->content_size(), prev_size);
}

TEST_F(MarshalTest, PartialReadWrite) {
    const size_t data_size = 1024;
    std::vector<char> write_data(data_size);
    for (size_t i = 0; i < data_size; ++i) {
        write_data[i] = static_cast<char>(i % 256);
    }
    
    size_t written = m.as_ref().unwrap()->write_bytes(reinterpret_cast<const std::uint8_t*>(write_data.data()), data_size);
    EXPECT_EQ(written, data_size);
    
    std::vector<char> read_data(data_size);
    size_t read = m.as_ref().unwrap()->read(reinterpret_cast<std::uint8_t*>(read_data.data()), data_size);
    EXPECT_EQ(read, data_size);
    
    EXPECT_EQ(write_data, read_data);
}

TEST_F(MarshalTest, PeekOperation) {
    i32 val1 = 100;
    i32 val2 = 200;

    rrr::Serialize_::serialize(val1, *m.as_ref().unwrap());
    rrr::Serialize_::serialize(val2, *m.as_ref().unwrap());

    i32 peeked_val;
    size_t peeked = m.as_ref().unwrap()->peek(peeked_val);
    EXPECT_EQ(peeked, sizeof(i32));
    EXPECT_EQ(peeked_val, val1);

    i32 read_val1, read_val2;
    rrr::Deserialize_::deserialize(read_val1, *m.as_ref().unwrap());
    rrr::Deserialize_::deserialize(read_val2, *m.as_ref().unwrap());

    EXPECT_EQ(read_val1, val1);
    EXPECT_EQ(read_val2, val2);
}

TEST_F(MarshalTest, MultipleChunks) {
    const size_t num_items = 10000;
    std::vector<i64> values;
    
    for (size_t i = 0; i < num_items; ++i) {
        i64 val = static_cast<i64>(i * 1000000);
        values.push_back(val);
        rrr::Serialize_::serialize(val, *m.as_ref().unwrap());
    }
    
    for (size_t i = 0; i < num_items; ++i) {
        i64 val;
        rrr::Deserialize_::deserialize(val, *m.as_ref().unwrap());
        EXPECT_EQ(val, values[i]);
    }
    
    EXPECT_TRUE(m.as_ref().unwrap()->empty());
}

TEST_F(MarshalTest, EdgeCasesEmptyCollections) {
    std::vector<int> empty_vec;
    std::list<double> empty_list;
    std::set<std::string> empty_set;
    std::map<int, int> empty_map;
    
    rrr::Serialize_::serialize(empty_vec, *m.as_ref().unwrap());
    rrr::Serialize_::serialize(empty_list, *m.as_ref().unwrap());
    rrr::Serialize_::serialize(empty_set, *m.as_ref().unwrap());
    rrr::Serialize_::serialize(empty_map, *m.as_ref().unwrap());
    
    std::vector<int> vec_out;
    std::list<double> list_out;
    std::set<std::string> set_out;
    std::map<int, int> map_out;
    
    rrr::Deserialize_::deserialize(vec_out, *m.as_ref().unwrap());
    rrr::Deserialize_::deserialize(list_out, *m.as_ref().unwrap());
    rrr::Deserialize_::deserialize(set_out, *m.as_ref().unwrap());
    rrr::Deserialize_::deserialize(map_out, *m.as_ref().unwrap());
    
    EXPECT_TRUE(vec_out.empty());
    EXPECT_TRUE(list_out.empty());
    EXPECT_TRUE(set_out.empty());
    EXPECT_TRUE(map_out.empty());
}

TEST_F(MarshalTest, SpecialFloatingPointValues) {
    double inf_pos = std::numeric_limits<double>::infinity();
    double inf_neg = -std::numeric_limits<double>::infinity();
    double nan_val = std::numeric_limits<double>::quiet_NaN();
    double denorm = std::numeric_limits<double>::denorm_min();
    
    rrr::Serialize_::serialize(inf_pos, *m.as_ref().unwrap());
    rrr::Serialize_::serialize(inf_neg, *m.as_ref().unwrap());
    rrr::Serialize_::serialize(nan_val, *m.as_ref().unwrap());
    rrr::Serialize_::serialize(denorm, *m.as_ref().unwrap());
    
    double inf_pos_out, inf_neg_out, nan_val_out, denorm_out;
    rrr::Deserialize_::deserialize(inf_pos_out, *m.as_ref().unwrap());
    rrr::Deserialize_::deserialize(inf_neg_out, *m.as_ref().unwrap());
    rrr::Deserialize_::deserialize(nan_val_out, *m.as_ref().unwrap());
    rrr::Deserialize_::deserialize(denorm_out, *m.as_ref().unwrap());
    
    EXPECT_TRUE(std::isinf(inf_pos_out) && inf_pos_out > 0);
    EXPECT_TRUE(std::isinf(inf_neg_out) && inf_neg_out < 0);
    EXPECT_TRUE(std::isnan(nan_val_out));
    EXPECT_EQ(denorm, denorm_out);
}

TEST_F(MarshalTest, RandomizedStressTest) {
    std::mt19937 gen(42);
    std::uniform_int_distribution<> dis(1, 100);
    std::uniform_real_distribution<> real_dis(-1000.0, 1000.0);
    
    std::vector<i32> ints;
    std::vector<double> doubles;
    std::vector<std::string> strings;
    
    const int num_items = 1000;
    
    for (int i = 0; i < num_items; ++i) {
        i32 int_val = dis(gen);
        double double_val = real_dis(gen);
        std::string str_val = "String_" + std::to_string(i);
        
        ints.push_back(int_val);
        doubles.push_back(double_val);
        strings.push_back(str_val);
        
        rrr::Serialize_::serialize(int_val, *m.as_ref().unwrap());
        rrr::Serialize_::serialize(double_val, *m.as_ref().unwrap());
        rrr::Serialize_::serialize(str_val, *m.as_ref().unwrap());
    }
    
    for (int i = 0; i < num_items; ++i) {
        i32 int_out;
        double double_out;
        std::string str_out;
        
        rrr::Deserialize_::deserialize(int_out, *m.as_ref().unwrap());
        rrr::Deserialize_::deserialize(double_out, *m.as_ref().unwrap());
        rrr::Deserialize_::deserialize(str_out, *m.as_ref().unwrap());
        
        EXPECT_EQ(int_out, ints[i]);
        EXPECT_DOUBLE_EQ(double_out, doubles[i]);
        EXPECT_EQ(str_out, strings[i]);
    }
}

// removed `CustomMarshallable` class definition and the long-
// commented-out `MarshallableObjects` test (referenced a
// `deputy.sp_data_` field that was later removed). The class was
// defined but never exercised; its `entity_size` override targeted
// a virtual that's also gone now.

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
