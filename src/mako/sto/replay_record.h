#pragma once

#include "lib/common.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace mako {

struct ReplayRecordView {
    std::string_view key;
    std::string_view value;
    uint16_t table_id;
    bool is_delete;
};

struct ReplayTransactionView {
    uint32_t time_term;
    uint16_t record_count;
    std::string_view records;
    size_t first_record_index;
};

struct ReplayLogView {
    std::vector<ReplayTransactionView> transactions;
    std::vector<ReplayRecordView> records;
    uint32_t latest_time_term = 0;
    uint32_t latency_tracker = 0;
};

// Decode one record from a replay transaction without forming typed pointers
// into the byte stream. The caller owns buffer for the lifetime of the views.
// On failure, offset and record are unchanged.
inline bool parse_replay_record(const char* buffer, size_t buffer_size,
                                size_t& offset,
                                ReplayRecordView& record) noexcept {
    size_t cursor = offset;
    if (cursor > buffer_size || buffer == nullptr) {
        return false;
    }

    const auto consume_u16 = [&](uint16_t& value) {
        if (buffer_size - cursor < sizeof(value)) {
            return false;
        }
        value = load_unaligned<uint16_t>(buffer + cursor);
        cursor += sizeof(value);
        return true;
    };

    const auto consume_bytes = [&](uint16_t length, std::string_view& value) {
        if (buffer_size - cursor < length) {
            return false;
        }
        value = std::string_view(buffer + cursor, length);
        cursor += length;
        return true;
    };

    uint16_t key_length = 0;
    uint16_t value_length = 0;
    uint16_t encoded_table_id = 0;
    std::string_view key;
    std::string_view value;
    if (!consume_u16(key_length) || !consume_bytes(key_length, key) ||
        !consume_u16(value_length) || !consume_bytes(value_length, value) ||
        !consume_u16(encoded_table_id)) {
        return false;
    }

    constexpr uint16_t delete_bit = uint16_t{1} << 15;
    record = ReplayRecordView{
        key,
        value,
        static_cast<uint16_t>(encoded_table_id & ~delete_bit),
        (encoded_table_id & delete_bit) != 0,
    };
    offset = cursor;
    return true;
}

inline bool valid_replay_table_id(uint16_t table_id) noexcept {
    return table_id != 0 && table_id <= 10000;
}

inline bool append_replay_record_batch(
    const char* buffer, size_t buffer_size, uint16_t record_count,
    std::vector<ReplayRecordView>& records) {
    const size_t original_size = records.size();
    size_t offset = 0;
    for (uint16_t index = 0; index < record_count; ++index) {
        ReplayRecordView record;
        if (!parse_replay_record(buffer, buffer_size, offset, record) ||
            !valid_replay_table_id(record.table_id)) {
            records.resize(original_size);
            return false;
        }
        records.push_back(record);
    }
    if (offset != buffer_size) {
        records.resize(original_size);
        return false;
    }
    return true;
}

inline bool parse_replay_record_batch(
    const char* buffer, size_t buffer_size, uint16_t record_count,
    std::vector<ReplayRecordView>& records) {
    records.clear();
    records.reserve(record_count);
    return append_replay_record_batch(
        buffer, buffer_size, record_count, records);
}

inline bool parse_replay_transaction(const char* buffer, size_t buffer_size,
                                     size_t& offset,
                                     ReplayTransactionView& transaction) noexcept {
    size_t cursor = offset;
    constexpr size_t header_size =
        sizeof(uint32_t) + sizeof(uint16_t) + sizeof(uint32_t);
    if (buffer == nullptr || cursor > buffer_size ||
        buffer_size - cursor < header_size) {
        return false;
    }

    const uint32_t time_term = load_unaligned<uint32_t>(buffer + cursor);
    cursor += sizeof(time_term);
    const uint16_t record_count = load_unaligned<uint16_t>(buffer + cursor);
    cursor += sizeof(record_count);
    const uint32_t records_size = load_unaligned<uint32_t>(buffer + cursor);
    cursor += sizeof(records_size);
    if (buffer_size - cursor < records_size) {
        return false;
    }

    const std::string_view records(buffer + cursor, records_size);
    cursor += records_size;
    transaction = ReplayTransactionView{
        time_term, record_count, records, 0};
    offset = cursor;
    return true;
}

// Parse and validate the complete persisted replay unit before a caller
// publishes its commit timestamp or applies any contained transaction.
inline bool parse_replay_log(const char* buffer, size_t buffer_size,
                             ReplayLogView& log) {
    constexpr size_t footer_size = 2 * sizeof(uint32_t);
    log.transactions.clear();
    log.records.clear();
    log.latest_time_term = 0;
    log.latency_tracker = 0;
    if (buffer == nullptr || buffer_size < footer_size) {
        return false;
    }

    const size_t transaction_bytes = buffer_size - footer_size;
    size_t offset = 0;
    while (offset < transaction_bytes) {
        ReplayTransactionView transaction;
        if (!parse_replay_transaction(
                buffer, transaction_bytes, offset, transaction)) {
            log.transactions.clear();
            log.records.clear();
            return false;
        }
        transaction.first_record_index = log.records.size();
        if (!append_replay_record_batch(
                transaction.records.data(), transaction.records.size(),
                transaction.record_count, log.records)) {
            log.transactions.clear();
            log.records.clear();
            return false;
        }
        log.transactions.push_back(transaction);
    }
    if (offset != transaction_bytes) {
        log.transactions.clear();
        log.records.clear();
        return false;
    }

    log.latest_time_term =
        load_unaligned<uint32_t>(buffer + transaction_bytes);
    log.latency_tracker = load_unaligned<uint32_t>(
        buffer + transaction_bytes + sizeof(log.latest_time_term));
    return true;
}

// Materialize the value consumed by the replay table. This helper deliberately
// initializes every metadata field after resize: output is thread-local in the
// replay worker, so a shorter value must not inherit bytes from its predecessor.
inline void materialize_replay_value(std::string& output,
                                     std::string_view value,
                                     bool is_delete,
                                     uint32_t time_term) {
    const size_t payload_size = is_delete ? size_t{1} : value.size();
    output.resize(payload_size + EXTRA_BITS_FOR_VALUE);
    if (is_delete) {
        output[0] = 'B';
    } else if (!value.empty()) {
        std::memcpy(output.data(), value.data(), value.size());
    }
    initialize_value_metadata(output.data(), output.size());
    store_value_time_term(output.data(), output.size(), time_term);
}

}  // namespace mako
