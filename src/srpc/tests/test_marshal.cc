#include <stdint.h>
#include <stddef.h>

#include <rusty/arc.hpp>
#include <rusty/option.hpp>
#include <rusty/box.hpp>
#include <gtest/gtest.h>
#include "../srpc.hpp"

import std;
import rusty;

import std;

using namespace srpc;

class MarshalTest : public ::testing::Test {
public:
    // The SinkProxy Box member makes the implicit dtor noexcept(false);
    // gtest's virtual ~Test() is noexcept, so pin it back explicitly.
    ~MarshalTest() noexcept override {}
protected:
    srpc::BufferSink sink;
    srpc::BinaryWriteArchive war{srpc::make_sink_proxy_buffer(&sink)};
};

TEST_F(MarshalTest, BasicIntegerTypes) {
    i8 i8_val = -128;
    i16 i16_val = -32768;
    i32 i32_val = -2147483648;
    i64 i64_val = -9223372036854775807LL;
    
    srpc::Serialize_::serialize(i8_val, war);
    srpc::Serialize_::serialize(i16_val, war);
    srpc::Serialize_::serialize(i32_val, war);
    srpc::Serialize_::serialize(i64_val, war);
    
    i8 i8_out;
    i16 i16_out;
    i32 i32_out;
    i64 i64_out;
    
    srpc::BufferSource src(sink.bytes.data(), sink.bytes.len());
    srpc::BinaryReadArchive rar(srpc::make_source_proxy_buffer(&src));
    srpc::Deserialize_::deserialize(i8_out, rar);
    srpc::Deserialize_::deserialize(i16_out, rar);
    srpc::Deserialize_::deserialize(i32_out, rar);
    srpc::Deserialize_::deserialize(i64_out, rar);
    
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
    
    srpc::Serialize_::serialize(u8_val, war);
    srpc::Serialize_::serialize(u16_val, war);
    srpc::Serialize_::serialize(u32_val, war);
    srpc::Serialize_::serialize(u64_val, war);
    
    uint8_t u8_out;
    uint16_t u16_out;
    uint32_t u32_out;
    uint64_t u64_out;
    
    srpc::BufferSource src(sink.bytes.data(), sink.bytes.len());
    srpc::BinaryReadArchive rar(srpc::make_source_proxy_buffer(&src));
    srpc::Deserialize_::deserialize(u8_out, rar);
    srpc::Deserialize_::deserialize(u16_out, rar);
    srpc::Deserialize_::deserialize(u32_out, rar);
    srpc::Deserialize_::deserialize(u64_out, rar);
    
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
    
    srpc::Serialize_::serialize(v32_small, war);
    srpc::Serialize_::serialize(v32_large, war);
    srpc::Serialize_::serialize(v64_small, war);
    srpc::Serialize_::serialize(v64_large, war);
    
    v32 v32_small_out, v32_large_out;
    v64 v64_small_out, v64_large_out;
    
    srpc::BufferSource src(sink.bytes.data(), sink.bytes.len());
    srpc::BinaryReadArchive rar(srpc::make_source_proxy_buffer(&src));
    srpc::Deserialize_::deserialize(v32_small_out, rar);
    srpc::Deserialize_::deserialize(v32_large_out, rar);
    srpc::Deserialize_::deserialize(v64_small_out, rar);
    srpc::Deserialize_::deserialize(v64_large_out, rar);
    
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
    
    srpc::Serialize_::serialize(d1, war);
    srpc::Serialize_::serialize(d2, war);
    srpc::Serialize_::serialize(d3, war);
    srpc::Serialize_::serialize(d4, war);
    srpc::Serialize_::serialize(d5, war);
    
    double d1_out, d2_out, d3_out, d4_out, d5_out;
    srpc::BufferSource src(sink.bytes.data(), sink.bytes.len());
    srpc::BinaryReadArchive rar(srpc::make_source_proxy_buffer(&src));
    srpc::Deserialize_::deserialize(d1_out, rar);
    srpc::Deserialize_::deserialize(d2_out, rar);
    srpc::Deserialize_::deserialize(d3_out, rar);
    srpc::Deserialize_::deserialize(d4_out, rar);
    srpc::Deserialize_::deserialize(d5_out, rar);
    
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
    
    srpc::Serialize_::serialize(empty_str, war);
    srpc::Serialize_::serialize(short_str, war);
    srpc::Serialize_::serialize(long_str, war);
    srpc::Serialize_::serialize(unicode_str, war);
    srpc::Serialize_::serialize(special_chars, war);
    
    std::string empty_out, short_out, long_out, unicode_out, special_out;
    srpc::BufferSource src(sink.bytes.data(), sink.bytes.len());
    srpc::BinaryReadArchive rar(srpc::make_source_proxy_buffer(&src));
    srpc::Deserialize_::deserialize(empty_out, rar);
    srpc::Deserialize_::deserialize(short_out, rar);
    srpc::Deserialize_::deserialize(long_out, rar);
    srpc::Deserialize_::deserialize(unicode_out, rar);
    srpc::Deserialize_::deserialize(special_out, rar);
    
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
    
    srpc::Serialize_::serialize(p1, war);
    srpc::Serialize_::serialize(p2, war);
    srpc::Serialize_::serialize(p3, war);
    
    std::pair<int, std::string> p1_out;
    std::pair<double, double> p2_out;
    std::pair<std::string, std::vector<int>> p3_out;
    
    srpc::BufferSource src(sink.bytes.data(), sink.bytes.len());
    srpc::BinaryReadArchive rar(srpc::make_source_proxy_buffer(&src));
    srpc::Deserialize_::deserialize(p1_out, rar);
    srpc::Deserialize_::deserialize(p2_out, rar);
    srpc::Deserialize_::deserialize(p3_out, rar);
    
    EXPECT_EQ(p1, p1_out);
    EXPECT_EQ(p2, p2_out);
    EXPECT_EQ(p3, p3_out);
}

TEST_F(MarshalTest, VectorValues) {
    std::vector<int> empty_vec;
    std::vector<int> int_vec = {1, 2, 3, 4, 5};
    std::vector<std::string> str_vec = {"one", "two", "three"};
    std::vector<std::vector<int>> nested_vec = {{1, 2}, {3, 4, 5}, {6}};
    
    srpc::Serialize_::serialize(empty_vec, war);
    srpc::Serialize_::serialize(int_vec, war);
    srpc::Serialize_::serialize(str_vec, war);
    srpc::Serialize_::serialize(nested_vec, war);
    
    std::vector<int> empty_vec_out, int_vec_out;
    std::vector<std::string> str_vec_out;
    std::vector<std::vector<int>> nested_vec_out;
    
    srpc::BufferSource src(sink.bytes.data(), sink.bytes.len());
    srpc::BinaryReadArchive rar(srpc::make_source_proxy_buffer(&src));
    srpc::Deserialize_::deserialize(empty_vec_out, rar);
    srpc::Deserialize_::deserialize(int_vec_out, rar);
    srpc::Deserialize_::deserialize(str_vec_out, rar);
    srpc::Deserialize_::deserialize(nested_vec_out, rar);
    
    EXPECT_EQ(empty_vec, empty_vec_out);
    EXPECT_EQ(int_vec, int_vec_out);
    EXPECT_EQ(str_vec, str_vec_out);
    EXPECT_EQ(nested_vec, nested_vec_out);
}

TEST_F(MarshalTest, ListValues) {
    std::list<int> empty_list;
    std::list<double> double_list = {1.1, 2.2, 3.3, 4.4};
    std::list<std::string> str_list = {"alpha", "beta", "gamma"};
    
    srpc::Serialize_::serialize(empty_list, war);
    srpc::Serialize_::serialize(double_list, war);
    srpc::Serialize_::serialize(str_list, war);
    
    std::list<int> empty_list_out;
    std::list<double> double_list_out;
    std::list<std::string> str_list_out;
    
    srpc::BufferSource src(sink.bytes.data(), sink.bytes.len());
    srpc::BinaryReadArchive rar(srpc::make_source_proxy_buffer(&src));
    srpc::Deserialize_::deserialize(empty_list_out, rar);
    srpc::Deserialize_::deserialize(double_list_out, rar);
    srpc::Deserialize_::deserialize(str_list_out, rar);
    
    EXPECT_EQ(empty_list, empty_list_out);
    EXPECT_EQ(double_list, double_list_out);
    EXPECT_EQ(str_list, str_list_out);
}

TEST_F(MarshalTest, SetValues) {
    std::set<int> empty_set;
    std::set<int> int_set = {5, 3, 1, 4, 2};
    std::set<std::string> str_set = {"zebra", "apple", "monkey"};
    
    srpc::Serialize_::serialize(empty_set, war);
    srpc::Serialize_::serialize(int_set, war);
    srpc::Serialize_::serialize(str_set, war);
    
    std::set<int> empty_set_out, int_set_out;
    std::set<std::string> str_set_out;
    
    srpc::BufferSource src(sink.bytes.data(), sink.bytes.len());
    srpc::BinaryReadArchive rar(srpc::make_source_proxy_buffer(&src));
    srpc::Deserialize_::deserialize(empty_set_out, rar);
    srpc::Deserialize_::deserialize(int_set_out, rar);
    srpc::Deserialize_::deserialize(str_set_out, rar);
    
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
    
    srpc::Serialize_::serialize(empty_map, war);
    srpc::Serialize_::serialize(int_str_map, war);
    srpc::Serialize_::serialize(str_vec_map, war);
    
    std::map<int, std::string> empty_map_out, int_str_map_out;
    std::map<std::string, std::vector<int>> str_vec_map_out;
    
    srpc::BufferSource src(sink.bytes.data(), sink.bytes.len());
    srpc::BinaryReadArchive rar(srpc::make_source_proxy_buffer(&src));
    srpc::Deserialize_::deserialize(empty_map_out, rar);
    srpc::Deserialize_::deserialize(int_str_map_out, rar);
    srpc::Deserialize_::deserialize(str_vec_map_out, rar);
    
    EXPECT_EQ(empty_map, empty_map_out);
    EXPECT_EQ(int_str_map, int_str_map_out);
    EXPECT_EQ(str_vec_map, str_vec_map_out);
}

TEST_F(MarshalTest, UnorderedSetValues) {
    std::unordered_set<int> empty_uset;
    std::unordered_set<int> int_uset = {10, 20, 30, 40, 50};
    std::unordered_set<std::string> str_uset = {"hash", "table", "set"};
    
    srpc::Serialize_::serialize(empty_uset, war);
    srpc::Serialize_::serialize(int_uset, war);
    srpc::Serialize_::serialize(str_uset, war);
    
    std::unordered_set<int> empty_uset_out, int_uset_out;
    std::unordered_set<std::string> str_uset_out;
    
    srpc::BufferSource src(sink.bytes.data(), sink.bytes.len());
    srpc::BinaryReadArchive rar(srpc::make_source_proxy_buffer(&src));
    srpc::Deserialize_::deserialize(empty_uset_out, rar);
    srpc::Deserialize_::deserialize(int_uset_out, rar);
    srpc::Deserialize_::deserialize(str_uset_out, rar);
    
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
    
    srpc::Serialize_::serialize(empty_umap, war);
    srpc::Serialize_::serialize(int_double_umap, war);
    srpc::Serialize_::serialize(str_int_umap, war);
    
    std::unordered_map<int, double> empty_umap_out, int_double_umap_out;
    std::unordered_map<std::string, int> str_int_umap_out;
    
    srpc::BufferSource src(sink.bytes.data(), sink.bytes.len());
    srpc::BinaryReadArchive rar(srpc::make_source_proxy_buffer(&src));
    srpc::Deserialize_::deserialize(empty_umap_out, rar);
    srpc::Deserialize_::deserialize(int_double_umap_out, rar);
    srpc::Deserialize_::deserialize(str_int_umap_out, rar);
    
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
    
    srpc::Serialize_::serialize(complex_data, war);
    
    ComplexType complex_data_out;
    srpc::BufferSource src(sink.bytes.data(), sink.bytes.len());
    srpc::BinaryReadArchive rar(srpc::make_source_proxy_buffer(&src));
    srpc::Deserialize_::deserialize(complex_data_out, rar);
    
    EXPECT_EQ(complex_data, complex_data_out);
}

TEST_F(MarshalTest, LargeDataSets) {
    const size_t large_size = 100000;
    std::vector<int> large_vec;
    for (size_t i = 0; i < large_size; ++i) {
        large_vec.push_back(i);
    }
    
    srpc::Serialize_::serialize(large_vec, war);
    
    std::vector<int> large_vec_out;
    srpc::BufferSource src(sink.bytes.data(), sink.bytes.len());
    srpc::BinaryReadArchive rar(srpc::make_source_proxy_buffer(&src));
    srpc::Deserialize_::deserialize(large_vec_out, rar);
    
    EXPECT_EQ(large_vec, large_vec_out);
}

TEST_F(MarshalTest, MixedDataTypes) {
    i32 int_val = 42;
    std::string str_val = "test string";
    std::vector<double> vec_val = {1.1, 2.2, 3.3};
    std::map<int, std::string> map_val = {{1, "one"}, {2, "two"}};
    
    srpc::Serialize_::serialize(int_val, war);
    srpc::Serialize_::serialize(str_val, war);
    srpc::Serialize_::serialize(vec_val, war);
    srpc::Serialize_::serialize(map_val, war);
    
    i32 int_out;
    std::string str_out;
    std::vector<double> vec_out;
    std::map<int, std::string> map_out;

    srpc::BufferSource src(sink.bytes.data(), sink.bytes.len());
    srpc::BinaryReadArchive rar(srpc::make_source_proxy_buffer(&src));
    srpc::Deserialize_::deserialize(int_out, rar);
    srpc::Deserialize_::deserialize(str_out, rar);
    srpc::Deserialize_::deserialize(vec_out, rar);
    srpc::Deserialize_::deserialize(map_out, rar);
    
    EXPECT_EQ(int_val, int_out);
    EXPECT_EQ(str_val, str_out);
    EXPECT_EQ(vec_val, vec_out);
    EXPECT_EQ(map_val, map_out);
}

TEST_F(MarshalTest, ContentSizeTracking) {
    EXPECT_TRUE(sink.bytes.len() == 0);
    EXPECT_EQ(sink.bytes.len(), 0u);
    
    i32 val = 42;
    srpc::Serialize_::serialize(val, war);
    
    EXPECT_FALSE(sink.bytes.len() == 0);
    EXPECT_EQ(sink.bytes.len(), sizeof(i32));
    
    std::string str = "Hello, World!";
    size_t prev_size = sink.bytes.len();
    srpc::Serialize_::serialize(str, war);
    
    EXPECT_GT(sink.bytes.len(), prev_size);
}

TEST_F(MarshalTest, PartialReadWrite) {
    const size_t data_size = 1024;
    std::vector<char> write_data(data_size);
    for (size_t i = 0; i < data_size; ++i) {
        write_data[i] = static_cast<char>(i % 256);
    }
    
    sink.write_bytes(reinterpret_cast<const std::uint8_t*>(write_data.data()), data_size);
    EXPECT_EQ(sink.bytes.len(), data_size);

    srpc::BufferSource src(sink.bytes.data(), sink.bytes.len());
    std::vector<char> read_data(data_size);
    size_t read = src.read_bytes(reinterpret_cast<std::uint8_t*>(read_data.data()), data_size);
    EXPECT_EQ(read, data_size);
    
    EXPECT_EQ(write_data, read_data);
}

TEST_F(MarshalTest, PeekOperation) {
    i32 val1 = 100;
    i32 val2 = 200;

    srpc::Serialize_::serialize(val1, war);
    srpc::Serialize_::serialize(val2, war);

    i32 peeked_val;
    size_t peeked;
    {
        srpc::BufferSource peek_src(sink.bytes.data(), sink.bytes.len());
        srpc::BinaryReadArchive peek_rar(srpc::make_source_proxy_buffer(&peek_src));
        srpc::Deserialize_::deserialize(peeked_val, peek_rar);
        peeked = sink.bytes.len() - peek_src.remaining();
    }
    EXPECT_EQ(peeked, sizeof(i32));
    EXPECT_EQ(peeked_val, val1);

    i32 read_val1, read_val2;
    srpc::BufferSource src(sink.bytes.data(), sink.bytes.len());
    srpc::BinaryReadArchive rar(srpc::make_source_proxy_buffer(&src));
    srpc::Deserialize_::deserialize(read_val1, rar);
    srpc::Deserialize_::deserialize(read_val2, rar);

    EXPECT_EQ(read_val1, val1);
    EXPECT_EQ(read_val2, val2);
}

TEST_F(MarshalTest, MultipleChunks) {
    const size_t num_items = 10000;
    std::vector<i64> values;
    
    for (size_t i = 0; i < num_items; ++i) {
        i64 val = static_cast<i64>(i * 1000000);
        values.push_back(val);
        srpc::Serialize_::serialize(val, war);
    }
    
    srpc::BufferSource src(sink.bytes.data(), sink.bytes.len());
    srpc::BinaryReadArchive rar(srpc::make_source_proxy_buffer(&src));
    for (size_t i = 0; i < num_items; ++i) {
        i64 val;
        srpc::Deserialize_::deserialize(val, rar);
        EXPECT_EQ(val, values[i]);
    }

    EXPECT_TRUE(src.remaining() == 0);
}

TEST_F(MarshalTest, EdgeCasesEmptyCollections) {
    std::vector<int> empty_vec;
    std::list<double> empty_list;
    std::set<std::string> empty_set;
    std::map<int, int> empty_map;
    
    srpc::Serialize_::serialize(empty_vec, war);
    srpc::Serialize_::serialize(empty_list, war);
    srpc::Serialize_::serialize(empty_set, war);
    srpc::Serialize_::serialize(empty_map, war);
    
    std::vector<int> vec_out;
    std::list<double> list_out;
    std::set<std::string> set_out;
    std::map<int, int> map_out;

    srpc::BufferSource src(sink.bytes.data(), sink.bytes.len());
    srpc::BinaryReadArchive rar(srpc::make_source_proxy_buffer(&src));
    srpc::Deserialize_::deserialize(vec_out, rar);
    srpc::Deserialize_::deserialize(list_out, rar);
    srpc::Deserialize_::deserialize(set_out, rar);
    srpc::Deserialize_::deserialize(map_out, rar);
    
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
    
    srpc::Serialize_::serialize(inf_pos, war);
    srpc::Serialize_::serialize(inf_neg, war);
    srpc::Serialize_::serialize(nan_val, war);
    srpc::Serialize_::serialize(denorm, war);
    
    double inf_pos_out, inf_neg_out, nan_val_out, denorm_out;
    srpc::BufferSource src(sink.bytes.data(), sink.bytes.len());
    srpc::BinaryReadArchive rar(srpc::make_source_proxy_buffer(&src));
    srpc::Deserialize_::deserialize(inf_pos_out, rar);
    srpc::Deserialize_::deserialize(inf_neg_out, rar);
    srpc::Deserialize_::deserialize(nan_val_out, rar);
    srpc::Deserialize_::deserialize(denorm_out, rar);
    
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
        
        srpc::Serialize_::serialize(int_val, war);
        srpc::Serialize_::serialize(double_val, war);
        srpc::Serialize_::serialize(str_val, war);
    }
    
    srpc::BufferSource src(sink.bytes.data(), sink.bytes.len());
    srpc::BinaryReadArchive rar(srpc::make_source_proxy_buffer(&src));
    for (int i = 0; i < num_items; ++i) {
        i32 int_out;
        double double_out;
        std::string str_out;
        
        srpc::Deserialize_::deserialize(int_out, rar);
        srpc::Deserialize_::deserialize(double_out, rar);
        srpc::Deserialize_::deserialize(str_out, rar);
        
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

// Roundtrip + golden-bytes regression for the DSL SparseInt varint
// codec (the legacy reinterpret_cast kernels are deleted; a
// differential test against them passed at conversion time across all
// of these boundaries, including i64 min/max and the legacy case-8
// 9-bytes-written/8-reported quirk).
TEST(SparseIntCodec, RoundTripAtAllBoundariesWithGoldens) {
    const int64_t vals64[] = {
        0, 1, -1, 63, -64, 64, -65,
        8191, -8192, 8192, -8193,
        1048575, -1048576, 1048576, -1048577,
        134217727, -134217728, 134217728LL, -134217729LL,
        17179869183LL, -17179869184LL, 17179869184LL, -17179869185LL,
        2199023255551LL, -2199023255552LL, 2199023255552LL, -2199023255553LL,
        281474976710655LL, -281474976710656LL, 281474976710656LL, -281474976710657LL,
        36028797018963967LL, -36028797018963968LL,
        std::numeric_limits<int64_t>::max(), std::numeric_limits<int64_t>::min(),
    };
    for (int64_t v : vals64) {
        uint8_t buf[16];
        memset(buf, 0, sizeof(buf));
        size_t n = srpc::SparseInt::dump64(v, buf);
        EXPECT_EQ(n, srpc::SparseInt::val_size(v)) << "size for " << v;
        EXPECT_EQ(srpc::SparseInt::load64(buf), v) << "roundtrip for " << v;
    }
    const int32_t vals32[] = {
        0, 1, -1, 63, -64, 64, -65, 8191, -8192, 8192, -8193,
        1048575, -1048576, 1048576, -1048577, 134217727, -134217728,
        std::numeric_limits<int32_t>::max(), std::numeric_limits<int32_t>::min(),
    };
    for (int32_t v : vals32) {
        uint8_t buf[16];
        memset(buf, 0, sizeof(buf));
        size_t n = srpc::SparseInt::dump32(v, buf);
        (void)n;
        EXPECT_EQ(srpc::SparseInt::load32(buf), v) << "roundtrip for " << v;
    }
    // Golden wire bytes (hand-derived; guard against silent format drift).
    struct Golden { int64_t v; size_t n; uint8_t bytes[3]; };
    const Golden goldens[] = {
        {0,    1, {0x00}}, {1, 1, {0x01}}, {-1, 1, {0x7F}},
        {63,   1, {0x3F}}, {-64, 1, {0x40}},
        {64,   2, {0x80, 0x40}}, {-65, 2, {0xBF, 0xBF}},
    };
    for (const auto& g : goldens) {
        uint8_t buf[16];
        memset(buf, 0, sizeof(buf));
        size_t n = srpc::SparseInt::dump64(g.v, buf);
        EXPECT_EQ(n, g.n) << "golden size for " << g.v;
        EXPECT_EQ(0, memcmp(buf, g.bytes, g.n)) << "golden bytes for " << g.v;
    }
}
