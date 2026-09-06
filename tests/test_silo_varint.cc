#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include <gtest/gtest.h>
#include "mako/record/inline_str.h"
#include "mako/record/serializer.h"
#include "mako/varint.h"

import std;

// Test fixture for varint operations
class VarintTest : public ::testing::Test {
protected:
    uint8_t buffer[10];
    
    void SetUp() override {
        memset(buffer, 0, sizeof(buffer));
    }
};

template <typename T>
void ExpectUnalignedFixedWidthRoundTrip(T original) {
    alignas(T) uint8_t storage[sizeof(T) + alignof(T)] = {};
    uint8_t expected[sizeof(T)] = {};
    NDB_MEMCPY(expected, &original, sizeof(T));

    for (size_t offset = 1; offset < alignof(T); ++offset) {
        uint8_t* const begin = storage + offset;
        EXPECT_EQ((serializer<T, false>::write(begin, original)),
                  begin + sizeof(T));
        EXPECT_EQ(memcmp(begin, expected, sizeof(T)), 0);

        T decoded{};
        EXPECT_EQ((serializer<T, false>::read(begin, &decoded)),
                  begin + sizeof(T));
        EXPECT_EQ(decoded, original);
    }
}

TEST(SerializerTest, FixedWidthRoundTripsAtUnalignedAddress) {
    ExpectUnalignedFixedWidthRoundTrip<int16_t>(-12345);
    ExpectUnalignedFixedWidthRoundTrip<int32_t>(-123456789);
    ExpectUnalignedFixedWidthRoundTrip<uint32_t>(0xfedcba98U);
    ExpectUnalignedFixedWidthRoundTrip<uint64_t>(0xfedcba9876543210ULL);
    ExpectUnalignedFixedWidthRoundTrip<float>(123.25F);
}

TEST(SerializerTest, FixedWidthWritePreservesDeletedCopySupport) {
    struct DeletedCopy {
        uint32_t value;
        DeletedCopy() = default;
        DeletedCopy(const DeletedCopy&) = delete;
    };
    static_assert(std::is_trivially_copyable_v<DeletedCopy>);
    static_assert(!std::is_copy_constructible_v<DeletedCopy>);

    DeletedCopy original;
    original.value = 0x12345678U;
    uint8_t encoded[sizeof(original)] = {};
    EXPECT_EQ((serializer<DeletedCopy, false>::write(encoded, original)),
              encoded + sizeof(original));
    EXPECT_EQ(memcmp(encoded, &original, sizeof(original)), 0);

    using Adapter = generic_serializer<serializer<DeletedCopy, false>>;
    alignas(DeletedCopy) uint8_t source[sizeof(DeletedCopy) + 1] = {};
    alignas(DeletedCopy) uint8_t destination[sizeof(DeletedCopy) + 1] = {};
    NDB_MEMCPY(source + 1, &original, sizeof(original));
    EXPECT_EQ(Adapter::write(encoded, source + 1), encoded + sizeof(original));
    EXPECT_EQ(Adapter::read(encoded, destination + 1),
              encoded + sizeof(original));
    EXPECT_EQ(memcmp(destination + 1, &original, sizeof(original)), 0);
}

TEST(SerializerTest, GenericAdapterAcceptsUnalignedObjectFields) {
    using Adapter = generic_serializer<serializer<int32_t, true>>;
    alignas(int32_t) uint8_t source[sizeof(int32_t) + 1] = {};
    alignas(int32_t) uint8_t destination[sizeof(int32_t) + 1] = {};
    uint8_t encoded[serializer<int32_t, true>::max_nbytes()] = {};
    const int32_t original = -123456789;
    NDB_MEMCPY(source + 1, &original, sizeof(original));

    EXPECT_EQ(Adapter::nbytes(source + 1),
              (serializer<int32_t, true>::nbytes(&original)));
    EXPECT_EQ(Adapter::max_nbytes(),
              (serializer<int32_t, true>::max_nbytes()));
    uint8_t * const encoded_end = Adapter::write(encoded, source + 1);
    const uint8_t * const decoded_end =
      Adapter::failsafe_read(encoded, encoded_end - encoded, destination + 1);

    ASSERT_EQ(decoded_end, encoded_end);
    int32_t decoded = 0;
    NDB_MEMCPY(&decoded, destination + 1, sizeof(decoded));
    EXPECT_EQ(decoded, original);
}

TEST(SerializerTest, NontrivialInlineStringRetainsClampingAssignment) {
    using Text = inline_str_8<8>;
    static_assert(!std::is_trivially_copyable_v<Text>);
    static_assert(alignof(Text) == 1);

    Text original("abc");
    uint8_t encoded[sizeof(Text)];
    memset(encoded, 0xa5, sizeof(encoded));
    EXPECT_EQ((serializer<Text, false>::write(encoded, original)),
              encoded + sizeof(Text));
    EXPECT_EQ(encoded[0], 3U);
    EXPECT_EQ(memcmp(encoded + 1, "abc", 3), 0);
    for (size_t i = 4; i < sizeof(encoded); ++i)
        EXPECT_EQ(encoded[i], 0xa5U);

    Text decoded;
    EXPECT_EQ((serializer<Text, false>::read(encoded, &decoded)),
              encoded + sizeof(Text));
    EXPECT_EQ(decoded.str(), "abc");

    Text unchanged("safe");
    EXPECT_EQ((serializer<Text, false>::failsafe_read(
                  encoded, sizeof(encoded) - 1, &unchanged)),
              nullptr);
    EXPECT_EQ(unchanged.str(), "safe");

    encoded[0] = 0xff;
    serializer<Text, false>::read(encoded, &decoded);
    EXPECT_EQ(decoded.size(), decoded.max_size());

    EXPECT_EQ((serializer<Text, false>::failsafe_read(
                  encoded, sizeof(encoded), &unchanged)),
              nullptr);
    EXPECT_EQ(unchanged.str(), "safe");

    using WideText = inline_str_16<300>;
    WideText wide("wide");
    uint8_t wide_encoded[sizeof(WideText)] = {};
    EXPECT_EQ((serializer<WideText, true>::write(wide_encoded, wide)),
              wide_encoded + sizeof(WideText));
    const uint16_t wide_size = 4;
    EXPECT_EQ(memcmp(wide_encoded, &wide_size, sizeof(wide_size)), 0);
    EXPECT_EQ(memcmp(wide_encoded + sizeof(wide_size), "wide", 4), 0);
    WideText wide_decoded;
    EXPECT_EQ((serializer<WideText, true>::read(wide_encoded, &wide_decoded)),
              wide_encoded + sizeof(WideText));
    EXPECT_EQ(wide_decoded.str(), "wide");

    using FixedText = inline_str_fixed<5>;
    FixedText fixed("xy");
    uint8_t fixed_encoded[sizeof(FixedText)] = {};
    EXPECT_EQ((serializer<FixedText, true>::write(fixed_encoded, fixed)),
              fixed_encoded + sizeof(FixedText));
    EXPECT_EQ(memcmp(fixed_encoded, "xy   ", sizeof(FixedText)), 0);
    FixedText fixed_decoded;
    EXPECT_EQ((serializer<FixedText, true>::read(
                  fixed_encoded, &fixed_decoded)),
              fixed_encoded + sizeof(FixedText));
    EXPECT_EQ(fixed_decoded.str(), fixed.str());

    using BaseText = inline_str_base<uint8_t, 8>;
    BaseText base("abc");
    uint8_t base_encoded[serializer<BaseText, true>::max_nbytes()] = {};
    uint8_t * const base_end =
        serializer<BaseText, true>::write(base_encoded, base);
    BaseText base_decoded;
    EXPECT_EQ((serializer<BaseText, true>::failsafe_read(
                  base_encoded, base_end - base_encoded, &base_decoded)),
              base_end);
    EXPECT_EQ(base_decoded.str(), "abc");

    uint8_t malformed[256] = {};
    malformed[0] = 0xff;
    EXPECT_EQ((serializer<BaseText, true>::failsafe_read(
                  malformed, sizeof(malformed), &base_decoded)),
              nullptr);
    EXPECT_EQ((serializer<BaseText, true>::failsafe_skip(
                  malformed, sizeof(malformed), nullptr)),
              0U);
}

TEST(SerializerTest, ZigZagBoundariesRoundTripWithCanonicalBytes) {
    struct Case {
        int32_t value;
        uint8_t bytes[5];
        size_t size;
    };
    const Case cases[] = {
        {INT32_MIN, {0xff, 0xff, 0xff, 0xff, 0x0f}, 5},
        {-1, {0x01, 0, 0, 0, 0}, 1},
        {0, {0x00, 0, 0, 0, 0}, 1},
        {1, {0x02, 0, 0, 0, 0}, 1},
        {INT32_MAX, {0xfe, 0xff, 0xff, 0xff, 0x0f}, 5},
    };

    for (const Case &test_case : cases) {
        SCOPED_TRACE(test_case.value);
        uint8_t encoded[serializer<int32_t, true>::max_nbytes()] = {};
        uint8_t * const end =
            serializer<int32_t, true>::write(encoded, test_case.value);
        ASSERT_EQ(static_cast<size_t>(end - encoded), test_case.size);
        EXPECT_EQ(memcmp(encoded, test_case.bytes, test_case.size), 0);

        int32_t decoded = 123;
        EXPECT_EQ((serializer<int32_t, true>::failsafe_read(
                      encoded, test_case.size, &decoded)),
                  end);
        EXPECT_EQ(decoded, test_case.value);

        decoded = 123;
        EXPECT_EQ((serializer<int32_t, true>::failsafe_read(
                      encoded, test_case.size - 1, &decoded)),
                  nullptr);
        EXPECT_EQ(decoded, 123);
    }
}

// Unit Tests for write_uvint32
TEST_F(VarintTest, WriteUvint32_SingleByte) {
    // Test values that fit in a single byte (0-127)
    uint32_t value = 42;
    uint8_t* end = write_uvint32(buffer, value);
    
    EXPECT_EQ(end - buffer, 1);  // Should write 1 byte
    EXPECT_EQ(buffer[0], 42);     // Value should be stored directly
}

TEST_F(VarintTest, WriteUvint32_MinValue) {
    uint32_t value = 0;
    uint8_t* end = write_uvint32(buffer, value);
    
    EXPECT_EQ(end - buffer, 1);
    EXPECT_EQ(buffer[0], 0);
}

TEST_F(VarintTest, WriteUvint32_MaxSingleByte) {
    uint32_t value = 0x7F;  // 127
    uint8_t* end = write_uvint32(buffer, value);
    
    EXPECT_EQ(end - buffer, 1);
    EXPECT_EQ(buffer[0], 0x7F);
}

TEST_F(VarintTest, WriteUvint32_TwoBytes) {
    uint32_t value = 128;  // Requires 2 bytes
    uint8_t* end = write_uvint32(buffer, value);
    
    EXPECT_EQ(end - buffer, 2);
    EXPECT_EQ(buffer[0] & 0x80, 0x80);  // High bit set on first byte
}

TEST_F(VarintTest, WriteUvint32_MaxValue) {
    uint32_t value = 0xFFFFFFFF;
    uint8_t* end = write_uvint32(buffer, value);
    
    EXPECT_EQ(end - buffer, 5);  // Max value requires 5 bytes
}

TEST_F(VarintTest, WriteUvint32_PowersOfTwo) {
    std::vector<uint32_t> powers = {1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024};
    
    for (uint32_t value : powers) {
        memset(buffer, 0, sizeof(buffer));
        uint8_t* end = write_uvint32(buffer, value);
        EXPECT_GT(end - buffer, 0) << "Failed for value: " << value;
    }
}

// Unit Tests for read_uvint32
TEST_F(VarintTest, ReadUvint32_SingleByte) {
    buffer[0] = 42;
    uint32_t value = 0;
    
    const uint8_t* end = read_uvint32(buffer, &value);
    
    EXPECT_EQ(end - buffer, 1);
    EXPECT_EQ(value, 42);
}

TEST_F(VarintTest, ReadUvint32_MinValue) {
    buffer[0] = 0;
    uint32_t value = 99;  // Non-zero to ensure it gets overwritten
    
    const uint8_t* end = read_uvint32(buffer, &value);
    
    EXPECT_EQ(value, 0);
}

TEST_F(VarintTest, ReadUvint32_TwoBytes) {
    // Write 128 first
    write_uvint32(buffer, 128);
    uint32_t value = 0;
    
    const uint8_t* end = read_uvint32(buffer, &value);
    
    EXPECT_EQ(value, 128);
}

// Round-trip tests
TEST_F(VarintTest, RoundTrip_RandomValues) {
    std::vector<uint32_t> test_values = {
        0, 1, 127, 128, 255, 256, 
        1000, 10000, 100000,
        0x7FFFFFFF,  // Max signed int
        0xFFFFFFFF   // Max unsigned int
    };
    
    for (uint32_t original : test_values) {
        memset(buffer, 0, sizeof(buffer));
        
        // Write
        uint8_t* write_end = write_uvint32(buffer, original);
        size_t bytes_written = write_end - buffer;
        
        // Read
        uint32_t decoded = 0;
        const uint8_t* read_end = read_uvint32(buffer, &decoded);
        size_t bytes_read = read_end - buffer;
        
        EXPECT_EQ(bytes_written, bytes_read) << "Mismatch for value: " << original;
        EXPECT_EQ(original, decoded) << "Round-trip failed for value: " << original;
    }
}

// Unit Tests for size_uvint32
TEST_F(VarintTest, SizeUvint32_Boundaries) {
    EXPECT_EQ(size_uvint32(0), 1);
    EXPECT_EQ(size_uvint32(0x7F), 1);
    EXPECT_EQ(size_uvint32(0x80), 2);
    EXPECT_EQ(size_uvint32(0x3FFF), 2);
    EXPECT_EQ(size_uvint32(0x4000), 3);
    EXPECT_EQ(size_uvint32(0x1FFFFF), 3);
    EXPECT_EQ(size_uvint32(0x200000), 4);
    EXPECT_EQ(size_uvint32(0xFFFFFFF), 4);
    EXPECT_EQ(size_uvint32(0x10000000), 5);
    EXPECT_EQ(size_uvint32(0xFFFFFFFF), 5);
}

TEST_F(VarintTest, SizeUvint32_MatchesActual) {
    std::vector<uint32_t> test_values = {
        0, 1, 100, 127, 128, 255, 256, 
        10000, 100000, 1000000, 
        0x7FFFFFFF, 0xFFFFFFFF
    };
    
    for (uint32_t value : test_values) {
        size_t predicted_size = size_uvint32(value);
        
        memset(buffer, 0, sizeof(buffer));
        uint8_t* end = write_uvint32(buffer, value);
        size_t actual_size = end - buffer;
        
        EXPECT_EQ(predicted_size, actual_size) 
            << "Size mismatch for value: " << value;
    }
}

// Unit Tests for skip_uvint32
TEST_F(VarintTest, SkipUvint32_NoRaw) {
    write_uvint32(buffer, 128);
    
    size_t skipped = skip_uvint32(buffer, nullptr);
    EXPECT_EQ(skipped, 2);
}

TEST_F(VarintTest, SkipUvint32_WithRaw) {
    uint8_t raw[5] = {0};
    write_uvint32(buffer, 128);
    
    size_t skipped = skip_uvint32(buffer, raw);
    
    EXPECT_EQ(skipped, 2);
    EXPECT_EQ(raw[0], buffer[0]);
    EXPECT_EQ(raw[1], buffer[1]);
}

// Failsafe tests
TEST_F(VarintTest, FailsafeRead_Success) {
    write_uvint32(buffer, 42);
    uint32_t value = 0;
    
    const uint8_t* result = failsafe_read_uvint32(buffer, 10, &value);
    
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(value, 42);
}

TEST_F(VarintTest, FailsafeRead_InsufficientBytes) {
    write_uvint32(buffer, 128);  // Needs 2 bytes
    uint32_t value = 0;
    
    const uint8_t* result = failsafe_read_uvint32(buffer, 1, &value);
    
    EXPECT_EQ(result, nullptr);  // Should fail with insufficient bytes
}

TEST_F(VarintTest, FailsafeRead_ZeroBytes) {
    uint32_t value = 0;
    
    const uint8_t* result = failsafe_read_uvint32(buffer, 0, &value);
    
    EXPECT_EQ(result, nullptr);
}

TEST_F(VarintTest, FailsafeSkip_Success) {
    write_uvint32(buffer, 1000);
    
    size_t skipped = failsafe_skip_uvint32(buffer, 10, nullptr);
    
    EXPECT_GT(skipped, 0);
}

TEST_F(VarintTest, FailsafeSkip_InsufficientBytes) {
    write_uvint32(buffer, 128);
    
    size_t skipped = failsafe_skip_uvint32(buffer, 1, nullptr);
    
    EXPECT_EQ(skipped, 0);
}

// Edge case tests
TEST_F(VarintTest, EdgeCase_ConsecutiveWrites) {
    uint8_t big_buffer[50];
    uint8_t* ptr = big_buffer;
    
    std::vector<uint32_t> values = {1, 127, 128, 1000, 0xFFFFFF};
    
    for (uint32_t value : values) {
        ptr = write_uvint32(ptr, value);
    }
    
    // Read them back
    const uint8_t* read_ptr = big_buffer;
    for (uint32_t expected : values) {
        uint32_t decoded = 0;
        read_ptr = read_uvint32(read_ptr, &decoded);
        EXPECT_EQ(decoded, expected);
    }
}

TEST_F(VarintTest, EdgeCase_BufferBoundaries) {
    // Test writing at the very end of buffer capacity
    uint8_t small_buffer[5];
    
    // Max value needs exactly 5 bytes
    uint8_t* end = write_uvint32(small_buffer, 0xFFFFFFFF);
    
    EXPECT_EQ(end - small_buffer, 5);
}

// Performance indicator test (not a benchmark, just sanity check)
TEST_F(VarintTest, Sanity_EncodingEfficiency) {
    // Smaller values should use fewer bytes
    EXPECT_LT(size_uvint32(100), size_uvint32(10000));
    EXPECT_LT(size_uvint32(10000), size_uvint32(1000000));
    EXPECT_LT(size_uvint32(1000000), size_uvint32(0xFFFFFFFF));
}
