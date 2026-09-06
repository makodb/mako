#pragma once

#include "lib/common.h"

#include <cstddef>
#include <cstdlib>
#include <vector>

namespace mako {

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
