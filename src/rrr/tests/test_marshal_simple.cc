#include <stddef.h>
#include <stdint.h>

#include "simple_test_runner.h"
#include "../rrr.hpp"

import std;
import rusty;

using namespace rrr;

void test_basic_integers() {
    rrr::BufferSink sink;
    rrr::BinaryWriteArchive war(rrr::make_sink_proxy(&sink));

    i32 val_in = 42;
    rrr::Serialize_::serialize(val_in, war);

    rrr::BufferSource src(sink.bytes.data(), sink.bytes.len());
    rrr::BinaryReadArchive rar(rrr::make_source_proxy(&src));
    i32 val_out;
    rrr::Deserialize_::deserialize(val_out, rar);

    TEST_ASSERT_EQ(val_in, val_out);
}

void test_multiple_integers() {
    rrr::BufferSink sink;
    rrr::BinaryWriteArchive war(rrr::make_sink_proxy(&sink));

    i8 i8_val = -128;
    i16 i16_val = -32768;
    i32 i32_val = -2147483648;
    i64 i64_val = -9223372036854775807LL;

    rrr::Serialize_::serialize(i8_val, war);
    rrr::Serialize_::serialize(i16_val, war);
    rrr::Serialize_::serialize(i32_val, war);
    rrr::Serialize_::serialize(i64_val, war);

    i8 i8_out;
    i16 i16_out;
    i32 i32_out;
    i64 i64_out;

    rrr::BufferSource src(sink.bytes.data(), sink.bytes.len());
    rrr::BinaryReadArchive rar(rrr::make_source_proxy(&src));
    rrr::Deserialize_::deserialize(i8_out, rar);
    rrr::Deserialize_::deserialize(i16_out, rar);
    rrr::Deserialize_::deserialize(i32_out, rar);
    rrr::Deserialize_::deserialize(i64_out, rar);
    
    TEST_ASSERT_EQ(i8_val, i8_out);
    TEST_ASSERT_EQ(i16_val, i16_out);
    TEST_ASSERT_EQ(i32_val, i32_out);
    TEST_ASSERT_EQ(i64_val, i64_out);
}

void test_strings() {
    rrr::BufferSink sink;
    rrr::BinaryWriteArchive war(rrr::make_sink_proxy(&sink));

    std::string str_in = "Hello, Marshal!";
    rrr::Serialize_::serialize(str_in, war);

    rrr::BufferSource src(sink.bytes.data(), sink.bytes.len());
    rrr::BinaryReadArchive rar(rrr::make_source_proxy(&src));
    std::string str_out;
    rrr::Deserialize_::deserialize(str_out, rar);
    
    TEST_ASSERT_EQ(str_in, str_out);
}

void test_vectors() {
    rrr::BufferSink sink;
    rrr::BinaryWriteArchive war(rrr::make_sink_proxy(&sink));

    std::vector<int> vec_in = {1, 2, 3, 4, 5};
    rrr::Serialize_::serialize(vec_in, war);

    rrr::BufferSource src(sink.bytes.data(), sink.bytes.len());
    rrr::BinaryReadArchive rar(rrr::make_source_proxy(&src));
    std::vector<int> vec_out;
    rrr::Deserialize_::deserialize(vec_out, rar);
    
    TEST_ASSERT_EQ(vec_in.size(), vec_out.size());
    for (size_t i = 0; i < vec_in.size(); ++i) {
        TEST_ASSERT_EQ(vec_in[i], vec_out[i]);
    }
}

void test_maps() {
    rrr::BufferSink sink;
    rrr::BinaryWriteArchive war(rrr::make_sink_proxy(&sink));

    std::map<int, std::string> map_in = {{1, "one"}, {2, "two"}, {3, "three"}};
    rrr::Serialize_::serialize(map_in, war);

    rrr::BufferSource src(sink.bytes.data(), sink.bytes.len());
    rrr::BinaryReadArchive rar(rrr::make_source_proxy(&src));
    std::map<int, std::string> map_out;
    rrr::Deserialize_::deserialize(map_out, rar);
    
    TEST_ASSERT_EQ(map_in.size(), map_out.size());
    for (const auto& pair : map_in) {
        TEST_ASSERT_EQ(map_out.count(pair.first), 1u);
        TEST_ASSERT_EQ(map_out[pair.first], pair.second);
    }
}

void test_content_size() {
    rrr::BufferSink sink;
    rrr::BinaryWriteArchive war(rrr::make_sink_proxy(&sink));

    TEST_ASSERT_TRUE(sink.bytes.len() == 0);
    TEST_ASSERT_EQ(sink.bytes.len(), 0u);

    i32 val = 42;
    rrr::Serialize_::serialize(val, war);

    TEST_ASSERT_FALSE(sink.bytes.len() == 0);
    TEST_ASSERT_EQ(sink.bytes.len(), sizeof(i32));
}

void test_peek() {
    rrr::BufferSink sink;
    rrr::BinaryWriteArchive war(rrr::make_sink_proxy(&sink));

    i32 val1 = 100;
    i32 val2 = 200;

    rrr::Serialize_::serialize(val1, war);
    rrr::Serialize_::serialize(val2, war);

    // Non-consuming peek: read through a fresh BufferSource over the
    // same bytes; the main read cursor below is unaffected.
    i32 peeked_val;
    rrr::BufferSource peek_src(sink.bytes.data(), sink.bytes.len());
    size_t peeked = peek_src.read_bytes(reinterpret_cast<uint8_t*>(&peeked_val), sizeof(i32));
    TEST_ASSERT_EQ(peeked, sizeof(i32));
    TEST_ASSERT_EQ(peeked_val, val1);

    rrr::BufferSource src(sink.bytes.data(), sink.bytes.len());
    rrr::BinaryReadArchive rar(rrr::make_source_proxy(&src));
    i32 read_val1, read_val2;
    rrr::Deserialize_::deserialize(read_val1, rar);
    rrr::Deserialize_::deserialize(read_val2, rar);
    
    TEST_ASSERT_EQ(read_val1, val1);
    TEST_ASSERT_EQ(read_val2, val2);
}

void test_large_data() {
    rrr::BufferSink sink;
    rrr::BinaryWriteArchive war(rrr::make_sink_proxy(&sink));

    const size_t large_size = 10000;
    std::vector<i32> large_vec;
    for (size_t i = 0; i < large_size; ++i) {
        large_vec.push_back(i);
    }

    rrr::Serialize_::serialize(large_vec, war);

    rrr::BufferSource src(sink.bytes.data(), sink.bytes.len());
    rrr::BinaryReadArchive rar(rrr::make_source_proxy(&src));
    std::vector<i32> large_vec_out;
    rrr::Deserialize_::deserialize(large_vec_out, rar);
    
    TEST_ASSERT_EQ(large_vec.size(), large_vec_out.size());
    for (size_t i = 0; i < large_size; ++i) {
        TEST_ASSERT_EQ(large_vec[i], large_vec_out[i]);
    }
}

int main(int argc, char** argv) {
    SimpleTestRunner runner;
    
    runner.AddTest("test_basic_integers", test_basic_integers);
    runner.AddTest("test_multiple_integers", test_multiple_integers);
    runner.AddTest("test_strings", test_strings);
    runner.AddTest("test_vectors", test_vectors);
    runner.AddTest("test_maps", test_maps);
    runner.AddTest("test_content_size", test_content_size);
    runner.AddTest("test_peek", test_peek);
    runner.AddTest("test_large_data", test_large_data);
    
    runner.RunAllTests();
    
    return runner.GetExitCode();
}