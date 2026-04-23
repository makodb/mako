#include <gtest/gtest.h>
#include "mako/varint.h"
#include <vector>
#include <limits>

// Test fixture for varint operations
class VarintTest : public ::testing::Test {
protected:
    uint8_t buffer[10];
    
    void SetUp() override {
        memset(buffer, 0, sizeof(buffer));
    }
};

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
