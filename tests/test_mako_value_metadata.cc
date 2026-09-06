#include "lib/common.h"
#include "sto/replay_record.h"
#include "sto/version_chain.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace {

TEST(MakoValueMetadata, EncodeInitializesCanonicalSuffixAtEveryAlignment) {
    std::array<bool, alignof(mako::Node)> observed_node_alignments{};

    for (size_t payload_size = 0;
         payload_size < 2 * alignof(mako::Node);
         ++payload_size) {
        SCOPED_TRACE(payload_size);
        const std::string payload(payload_size, 'x');
        const std::string encoded = mako::Encode(payload);

        ASSERT_EQ(encoded.size(),
                  payload.size() + mako::EXTRA_BITS_FOR_VALUE);
        EXPECT_EQ(std::string_view(encoded.data(), payload.size()), payload);

        const std::string_view metadata(
            encoded.data() + payload.size(), mako::EXTRA_BITS_FOR_VALUE);
        EXPECT_TRUE(std::all_of(metadata.begin(), metadata.end(),
                                [](char byte) { return byte == '\0'; }));
        EXPECT_EQ(mako::load_value_time_term(encoded.data(), encoded.size()), 0U);
        EXPECT_EQ(
            mako::load_value_node_timestamp(encoded.data(), encoded.size()),
            0U);
        EXPECT_EQ(
            mako::load_value_node_data_size(encoded.data(), encoded.size()),
            0);
        EXPECT_EQ(mako::load_value_node_data(encoded.data(), encoded.size()),
                  nullptr);

        const uintptr_t node_address = reinterpret_cast<uintptr_t>(
            mako::value_node_address(encoded.data(), encoded.size()));
        observed_node_alignments[node_address % alignof(mako::Node)] = true;
    }

    EXPECT_TRUE(std::all_of(observed_node_alignments.begin(),
                            observed_node_alignments.end(),
                            [](bool observed) { return observed; }));
}

TEST(MakoValueMetadata, MemberStoresPreservePayloadAndWireOffsets) {
    constexpr uint32_t time_term = 0xfedcba98U;
    constexpr uint32_t timestamp = 0x76543210U;
    constexpr int16_t data_size = 0x1234;
    char next_version = 'v';

    for (size_t payload_size = 0;
         payload_size < 2 * alignof(mako::Node);
         ++payload_size) {
        SCOPED_TRACE(payload_size);
        const std::string payload(payload_size, 'p');
        std::string encoded = mako::Encode(payload);

        mako::store_value_time_term(encoded.data(), encoded.size(), time_term);
        mako::store_value_node_timestamp(
            encoded.data(), encoded.size(), timestamp);
        mako::store_value_node_data_size(
            encoded.data(), encoded.size(), data_size);
        mako::store_value_node_data(
            encoded.data(), encoded.size(), &next_version);

        EXPECT_EQ(std::string_view(encoded.data(), payload.size()), payload);
        EXPECT_EQ(mako::load_value_time_term(encoded.data(), encoded.size()),
                  time_term);
        EXPECT_EQ(
            mako::load_value_node_timestamp(encoded.data(), encoded.size()),
            timestamp);
        EXPECT_EQ(
            mako::load_value_node_data_size(encoded.data(), encoded.size()),
            data_size);
        EXPECT_EQ(mako::load_value_node_data(encoded.data(), encoded.size()),
                  &next_version);

        std::string expected(mako::EXTRA_BITS_FOR_VALUE, '\0');
        std::memcpy(expected.data(), &time_term, sizeof(time_term));
        char *expected_node =
            expected.data() + mako::EXTRA_BITS_FOR_VALUE - mako::BITS_OF_NODE;
        std::memcpy(expected_node + offsetof(mako::Node, timestamp),
                    &timestamp, sizeof(timestamp));
        std::memcpy(expected_node + offsetof(mako::Node, data_size),
                    &data_size, sizeof(data_size));
        char *next_version_pointer = &next_version;
        std::memcpy(expected_node + offsetof(mako::Node, data),
                    &next_version_pointer, sizeof(next_version_pointer));
        EXPECT_EQ(std::string_view(
                      encoded.data() + payload.size(),
                      mako::EXTRA_BITS_FOR_VALUE),
                  expected);
    }
}

void append_u16(std::vector<char>& bytes, uint16_t value) {
    const size_t offset = bytes.size();
    bytes.resize(offset + sizeof(value));
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

void append_u32(std::vector<char>& bytes, uint32_t value) {
    const size_t offset = bytes.size();
    bytes.resize(offset + sizeof(value));
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

void append_replay_record(std::vector<char>& bytes, std::string_view key,
                          std::string_view value, uint16_t table_id,
                          bool is_delete = false) {
    append_u16(bytes, static_cast<uint16_t>(key.size()));
    bytes.insert(bytes.end(), key.begin(), key.end());
    append_u16(bytes, static_cast<uint16_t>(value.size()));
    bytes.insert(bytes.end(), value.begin(), value.end());
    if (is_delete) {
        table_id |= uint16_t{1} << 15;
    }
    append_u16(bytes, table_id);
}

TEST(MakoReplayFields, ProductionParserHandlesOddOffsetsAndTruncation) {
    constexpr std::string_view key = "key";
    constexpr std::string_view value = "data";

    std::vector<char> bytes;
    bytes.reserve(32);
    append_replay_record(bytes, key, value, 37, true);

    // The odd key length puts the following scalar on an odd address.
    constexpr size_t value_length_offset = sizeof(uint16_t) + key.size();
    ASSERT_EQ(value_length_offset % alignof(uint16_t), 1U);

    std::vector<mako::ReplayRecordView> records;
    ASSERT_TRUE(mako::parse_replay_record_batch(
        bytes.data(), bytes.size(), 1, records));
    ASSERT_EQ(records.size(), 1U);
    const auto& record = records.front();
    EXPECT_EQ(record.key, key);
    EXPECT_EQ(record.value, value);
    EXPECT_TRUE(record.is_delete);
    EXPECT_EQ(record.table_id, 37);

    for (size_t truncated_size = 0; truncated_size < bytes.size();
         ++truncated_size) {
        SCOPED_TRACE(truncated_size);
        std::vector<mako::ReplayRecordView> unchanged{
            mako::ReplayRecordView{"old-key", "old-value", 91, false}};
        EXPECT_FALSE(mako::parse_replay_record_batch(
            bytes.data(), truncated_size, 1, unchanged));
        EXPECT_TRUE(unchanged.empty());
    }

    bytes.push_back('x');
    EXPECT_FALSE(mako::parse_replay_record_batch(
        bytes.data(), bytes.size(), 1, records));

    std::vector<char> invalid_table;
    append_replay_record(invalid_table, key, value, 0);
    EXPECT_FALSE(mako::parse_replay_record_batch(
        invalid_table.data(), invalid_table.size(), 1, records));
    invalid_table.clear();
    append_replay_record(invalid_table, key, value, 10001);
    EXPECT_FALSE(mako::parse_replay_record_batch(
        invalid_table.data(), invalid_table.size(), 1, records));
}

TEST(MakoReplayFields, ReusedValueGetsCanonicalMetadataForShortAndDelete) {
    constexpr uint32_t first_time_term = 0x10203040U;
    constexpr uint32_t second_time_term = 0x50607080U;
    char next_version = 'v';
    std::string output;

    mako::materialize_replay_value(
        output, "a payload that is deliberately long", false,
        first_time_term);
    ASSERT_GT(output.size(), size_t{1} + mako::EXTRA_BITS_FOR_VALUE);
    mako::store_value_node_timestamp(output.data(), output.size(), 0xaabbccddU);
    mako::store_value_node_data_size(output.data(), output.size(), 123);
    mako::store_value_node_data(
        output.data(), output.size(), &next_version);

    mako::materialize_replay_value(
        output, "x", false, second_time_term);
    ASSERT_EQ(output.size(), size_t{1} + mako::EXTRA_BITS_FOR_VALUE);
    EXPECT_EQ(output[0], 'x');
    EXPECT_EQ(mako::load_value_time_term(output.data(), output.size()),
              second_time_term);
    EXPECT_EQ(mako::load_value_node_timestamp(output.data(), output.size()),
              0U);
    EXPECT_EQ(mako::load_value_node_data_size(output.data(), output.size()), 0);
    EXPECT_EQ(mako::load_value_node_data(output.data(), output.size()), nullptr);
    EXPECT_TRUE(std::all_of(
        output.begin() + 1 + sizeof(second_time_term), output.end(),
        [](char byte) { return byte == '\0'; }));

    mako::store_value_node_timestamp(output.data(), output.size(), 42U);
    mako::store_value_node_data_size(output.data(), output.size(), 7);
    mako::store_value_node_data(
        output.data(), output.size(), &next_version);
    mako::materialize_replay_value(output, "ignored", true, first_time_term);
    ASSERT_EQ(output.size(), size_t{1} + mako::EXTRA_BITS_FOR_VALUE);
    EXPECT_EQ(output[0], 'B');
    EXPECT_EQ(mako::load_value_time_term(output.data(), output.size()),
              first_time_term);
    EXPECT_EQ(mako::load_value_node_timestamp(output.data(), output.size()),
              0U);
    EXPECT_EQ(mako::load_value_node_data_size(output.data(), output.size()), 0);
    EXPECT_EQ(mako::load_value_node_data(output.data(), output.size()), nullptr);
    EXPECT_TRUE(std::all_of(
        output.begin() + 1 + sizeof(first_time_term), output.end(),
        [](char byte) { return byte == '\0'; }));
}

void append_replay_transaction(std::vector<char>& log, uint32_t time_term,
                               const std::vector<char>& records,
                               uint16_t record_count) {
    append_u32(log, time_term);
    append_u16(log, record_count);
    append_u32(log, static_cast<uint32_t>(records.size()));
    log.insert(log.end(), records.begin(), records.end());
}

TEST(MakoReplayLog, ProductionParserValidatesCompleteMultiTransactionFrame) {
    std::vector<char> first_records;
    append_replay_record(first_records, "key", "value", 7);
    std::vector<char> second_records;
    append_replay_record(second_records, "odd", "x", 9, true);

    std::vector<char> log;
    append_replay_transaction(log, 101, first_records, 1);
    append_replay_transaction(log, 202, second_records, 1);
    append_u32(log, 303);
    append_u32(log, 404);

    mako::ReplayLogView parsed;
    ASSERT_TRUE(mako::parse_replay_log(log.data(), log.size(), parsed));
    ASSERT_EQ(parsed.transactions.size(), 2U);
    ASSERT_EQ(parsed.records.size(), 2U);
    EXPECT_EQ(parsed.transactions[0].time_term, 101U);
    EXPECT_EQ(parsed.transactions[0].record_count, 1U);
    EXPECT_EQ(parsed.transactions[0].first_record_index, 0U);
    EXPECT_EQ(parsed.transactions[0].records,
              std::string_view(first_records.data(), first_records.size()));
    EXPECT_EQ(parsed.transactions[1].time_term, 202U);
    EXPECT_EQ(parsed.transactions[1].record_count, 1U);
    EXPECT_EQ(parsed.transactions[1].first_record_index, 1U);
    EXPECT_EQ(parsed.transactions[1].records,
              std::string_view(second_records.data(), second_records.size()));
    EXPECT_EQ(parsed.latest_time_term, 303U);
    EXPECT_EQ(parsed.latency_tracker, 404U);
    EXPECT_EQ(parsed.records[0].key, "key");
    EXPECT_EQ(parsed.records[1].key, "odd");

}

TEST(MakoReplayLog, RejectsShortFooterPartialHeaderAndInvalidRecordBlock) {
    mako::ReplayLogView unchanged;
    std::array<char, 7> short_log{};
    for (size_t size = 0; size <= short_log.size(); ++size) {
        SCOPED_TRACE(size);
        EXPECT_FALSE(mako::parse_replay_log(short_log.data(), size, unchanged));
        EXPECT_TRUE(unchanged.transactions.empty());
        EXPECT_TRUE(unchanged.records.empty());
        EXPECT_EQ(unchanged.latest_time_term, 0U);
        EXPECT_EQ(unchanged.latency_tracker, 0U);
    }

    for (size_t header_bytes = 1; header_bytes < 10; ++header_bytes) {
        SCOPED_TRACE(header_bytes);
        std::vector<char> partial(header_bytes, '\0');
        append_u32(partial, 11);
        append_u32(partial, 12);
        EXPECT_FALSE(
            mako::parse_replay_log(partial.data(), partial.size(), unchanged));
    }

    std::vector<char> records;
    append_replay_record(records, "key", "value", 7);
    std::vector<char> truncated;
    append_u32(truncated, 101);
    append_u16(truncated, 1);
    append_u32(truncated, static_cast<uint32_t>(records.size() + 1));
    truncated.insert(truncated.end(), records.begin(), records.end());
    append_u32(truncated, 303);
    append_u32(truncated, 404);
    EXPECT_FALSE(mako::parse_replay_log(
        truncated.data(), truncated.size(), unchanged));

    std::vector<char> trailing;
    append_replay_transaction(trailing, 101, records, 1);
    trailing.push_back('x');
    append_u32(trailing, 303);
    append_u32(trailing, 404);
    EXPECT_FALSE(
        mako::parse_replay_log(trailing.data(), trailing.size(), unchanged));
}

char* allocate_version(std::string_view payload) {
    const size_t size = payload.size() + mako::EXTRA_BITS_FOR_VALUE;
    char* const value = static_cast<char*>(std::malloc(size));
    if (value == nullptr) {
        return nullptr;
    }
    std::memcpy(value, payload.data(), payload.size());
    mako::initialize_value_metadata(value, size);
    return value;
}

TEST(MakoValueVersions, ReclamationUsesExplicitEmbeddedOwnershipBoundary) {
    constexpr std::string_view first_payload = "first";
    constexpr std::string_view second_payload = "second";
    constexpr std::string_view terminal_payload = "terminal";
    char* const first = allocate_version(first_payload);
    char* const second = allocate_version(second_payload);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    std::string terminal = mako::Encode(std::string(terminal_payload));

    const size_t first_size =
        first_payload.size() + mako::EXTRA_BITS_FOR_VALUE;
    const size_t second_size =
        second_payload.size() + mako::EXTRA_BITS_FOR_VALUE;
    const size_t terminal_size =
        terminal_payload.size() + mako::EXTRA_BITS_FOR_VALUE;
    mako::store_value_node_data_size(
        first, first_size, static_cast<int16_t>(second_size));
    mako::store_value_node_data(first, first_size, second);
    mako::store_value_node_data_size(
        second, second_size, static_cast<int16_t>(terminal_size));
    mako::store_value_node_data(second, second_size, terminal.data());

    std::string root = mako::Encode("root");
    mako::store_value_node_data_size(
        root.data(), root.size(), static_cast<int16_t>(first_size));
    mako::store_value_node_data(root.data(), root.size(), first);
    char* const root_node =
        mako::value_node_address(root.data(), root.size());

    EXPECT_EQ(mako::reclaim_value_chain_after(root_node, terminal.data()), 2U);
    EXPECT_EQ(mako::load_node_data_size(root_node), 0);
    EXPECT_EQ(mako::load_node_data(root_node), nullptr);
    EXPECT_EQ(std::string_view(terminal.data(), terminal_payload.size()),
              terminal_payload);
    EXPECT_EQ(mako::load_value_node_data_size(
                  terminal.data(), terminal.size()),
              0);

    std::string direct_root = mako::Encode("direct-root");
    mako::store_value_node_data_size(
        direct_root.data(), direct_root.size(),
        static_cast<int16_t>(terminal.size()));
    mako::store_value_node_data(
        direct_root.data(), direct_root.size(), terminal.data());
    char* const direct_root_node =
        mako::value_node_address(direct_root.data(), direct_root.size());
    EXPECT_EQ(mako::reclaim_value_chain_after(
                  direct_root_node, terminal.data()),
              0U);
    EXPECT_EQ(mako::load_node_data_size(direct_root_node),
              static_cast<int16_t>(terminal.size()));
    EXPECT_EQ(mako::load_node_data(direct_root_node), terminal.data());
}

TEST(MakoValueVersions, RepeatedReclamationFreesTruncatedHeapAnchor) {
    std::string terminal = mako::Encode("embedded");
    const size_t terminal_size = terminal.size();

    char* const older = allocate_version("older");
    char* const anchor = allocate_version("anchor");
    ASSERT_NE(older, nullptr);
    ASSERT_NE(anchor, nullptr);
    const size_t older_size = std::string_view("older").size() +
                              mako::EXTRA_BITS_FOR_VALUE;
    const size_t anchor_size = std::string_view("anchor").size() +
                               mako::EXTRA_BITS_FOR_VALUE;
    mako::store_value_node_data_size(
        older, older_size, static_cast<int16_t>(terminal_size));
    mako::store_value_node_data(older, older_size, terminal.data());
    mako::store_value_node_data_size(
        anchor, anchor_size, static_cast<int16_t>(older_size));
    mako::store_value_node_data(anchor, anchor_size, older);

    char* const anchor_node =
        mako::value_node_address(anchor, anchor_size);
    EXPECT_EQ(mako::reclaim_value_chain_after(
                  anchor_node, terminal.data()),
              1U);
    EXPECT_EQ(mako::load_node_data_size(anchor_node), 0);

    char* const newer = allocate_version("newer");
    ASSERT_NE(newer, nullptr);
    const size_t newer_size = std::string_view("newer").size() +
                              mako::EXTRA_BITS_FOR_VALUE;
    mako::store_value_node_data_size(
        newer, newer_size, static_cast<int16_t>(anchor_size));
    mako::store_value_node_data(newer, newer_size, anchor);

    std::string root = mako::Encode("root");
    mako::store_value_node_data_size(
        root.data(), root.size(), static_cast<int16_t>(newer_size));
    mako::store_value_node_data(root.data(), root.size(), newer);
    char* const root_node =
        mako::value_node_address(root.data(), root.size());

    EXPECT_EQ(mako::reclaim_value_chain_after(
                  root_node, terminal.data()),
              2U);
    EXPECT_EQ(mako::load_node_data_size(root_node), 0);
    EXPECT_EQ(mako::load_node_data(root_node), nullptr);
    EXPECT_EQ(std::string_view(
                  terminal.data(), std::string_view("embedded").size()),
              "embedded");
}

}  // namespace
