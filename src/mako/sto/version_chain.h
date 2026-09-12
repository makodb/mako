#pragma once

#include "lib/common.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace mako {

struct value_chain_prune_result {
    bool pruned;
    size_t cloned_values;
};

// Prune an unpublished head without changing any buffer reachable from the
// previously published head. Values at or above watermark are copied into a
// private prefix and linked from head. The first older value and everything
// below it are omitted. The caller may publish head once this returns, then
// retire the old published chain after an RCU grace period.
inline value_chain_prune_result cow_prune_value_chain(
        char* head, size_t head_size, uint32_t watermark) {
    if (head == nullptr || head_size < static_cast<size_t>(EXTRA_BITS_FOR_VALUE)) {
        Panic("invalid multi-version value-chain head");
    }

    size_t retained_count = 0;
    char* node = value_node_address(head, head_size);
    bool found_cutoff = false;
    while (true) {
        const int16_t next_size = load_node_data_size(node);
        if (next_size == 0) {
            break;
        }
        char* const next = load_node_data(node);
        if (next_size < EXTRA_BITS_FOR_VALUE || next == nullptr) {
            Panic("corrupt multi-version value chain");
        }
        const size_t size = static_cast<size_t>(next_size);
        const uint32_t time_term = load_value_time_term(next, size);
        if (time_term / 10 < watermark) {
            found_cutoff = true;
            break;
        }
        ++retained_count;
        node = value_node_address(next, size);
    }

    if (!found_cutoff) {
        return {false, 0};
    }

    char* first_clone = nullptr;
    char* previous_clone = nullptr;
    size_t first_size = 0;
    size_t previous_size = 0;
    char* source_node = value_node_address(head, head_size);
    for (size_t index = 0; index != retained_count; ++index) {
        const int16_t source_size = load_node_data_size(source_node);
        char* const source = load_node_data(source_node);
        const size_t size = static_cast<size_t>(source_size);
        char* const clone = static_cast<char*>(std::malloc(size));
        if (clone == nullptr) {
            char* allocated = first_clone;
            size_t allocated_size = first_size;
            while (allocated != nullptr) {
                char* const next = load_value_node_data(
                    allocated, allocated_size);
                const int16_t next_size = load_value_node_data_size(
                    allocated, allocated_size);
                std::free(allocated);
                allocated = next;
                allocated_size = static_cast<size_t>(next_size);
            }
            Panic("failed to clone a multi-version value chain");
        }
        std::memcpy(clone, source, size);
        store_value_node_data_size(clone, size, 0);
        store_value_node_data(clone, size, nullptr);
        if (previous_clone == nullptr) {
            first_clone = clone;
            first_size = size;
        } else {
            store_value_node_data_size(
                previous_clone, previous_size,
                static_cast<int16_t>(size));
            store_value_node_data(previous_clone, previous_size, clone);
        }
        previous_clone = clone;
        previous_size = size;
        source_node = value_node_address(source, size);
    }

    if (first_clone == nullptr) {
        store_value_node_data_size(head, head_size, 0);
        store_value_node_data(head, head_size, nullptr);
    } else {
        store_value_node_data_size(
            head, head_size, static_cast<int16_t>(first_size));
        store_value_node_data(head, head_size, first_clone);
    }
    return {true, retained_count};
}

// Free every heap buffer in an unreachable chain. The callback that invokes
// this must run after an RCU grace period. embedded_data names storage owned
// by stuffed_str and is never freed here.
inline size_t free_retired_value_chain(char* head, size_t head_size,
                                       uintptr_t embedded_address) {
    size_t freed = 0;
    char* current = head;
    size_t current_size = head_size;
    while (current != nullptr &&
           reinterpret_cast<uintptr_t>(current) != embedded_address) {
        if (current_size < static_cast<size_t>(EXTRA_BITS_FOR_VALUE)) {
            Panic("corrupt retired multi-version value chain");
        }
        const int16_t next_size =
            load_value_node_data_size(current, current_size);
        char* const next = load_value_node_data(current, current_size);
        if (next_size < 0 || (next_size > 0 && next == nullptr)) {
            Panic("corrupt retired multi-version value chain");
        }
        std::free(current);
        ++freed;
        if (next_size == 0) {
            break;
        }
        current = next;
        current_size = static_cast<size_t>(next_size);
    }
    return freed;
}

// Detach and free the heap-backed value buffers reachable through root's
// metadata node. embedded_data belongs to the versioned_str owner and must
// remain allocated. Pointers are collected before the first free because each
// following node is embedded in the allocation owned by its predecessor.
// Ownership is explicit: a heap node truncated by an earlier cycle also has a
// zero data_size and must not be mistaken for the embedded terminal.
inline size_t reclaim_value_chain_after(char* root, char* embedded_data) {
    if (root == nullptr || embedded_data == nullptr) {
        return 0;
    }
    std::vector<char*> buffers;
    char* current = root;
    while (current != nullptr && load_node_data_size(current) > 0) {
        const int16_t data_size = load_node_data_size(current);
        char* const data = load_node_data(current);
        if (data == nullptr || data_size < EXTRA_BITS_FOR_VALUE) {
            Panic("corrupt multi-version value chain");
        }
        if (data == embedded_data) {
            break;
        }
        buffers.push_back(data);
        char* const next =
            value_node_address(data, static_cast<size_t>(data_size));
        current = next;
    }

    if (buffers.empty()) {
        return 0;
    }
    store_node_data_size(root, 0);
    store_node_data(root, nullptr);
    for (char* buffer : buffers) {
        std::free(buffer);
    }
    return buffers.size();
}

}  // namespace mako
