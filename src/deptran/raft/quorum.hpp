#pragma once

/**
 * @file quorum.hpp
 * @brief Raft's quorum arithmetic, as inline Rust (the DSL block) and the C++
 *        generated from it, which server.cc's kernels call.
 */

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include <rusty/arc.hpp>
#include <rusty/mutex.hpp>
#include <rusty/slice.hpp>
#include <rusty/sync/atomic.hpp>

#include "srpc/srpc.hpp"

#include "../constants.h"  // siteid_t

namespace janus {
namespace raft {

// Pure quorum arithmetic stays separate from reply storage, atomics, and the
// srpc event boundary. `const fn` makes the generated C++ constexpr (and thus
// implicitly inline) without a textual post-processing pass.
#if RUSTYCPP_RUST
pub const fn raft_quorum_reached(received: i32, needed: i32) -> bool {
    received >= needed
}

pub const fn raft_quorum_majority_count(total: usize) -> usize {
    (total / 2) + 1
}

pub const fn raft_quorum_count_reached(count: usize, quorum: usize) -> bool {
    count >= quorum
}

pub const fn raft_quorum_count_below(count: usize, quorum: usize) -> bool {
    count < quorum
}
#endif
/*RUSTYCPP:GEN-BEGIN id=raft_quorum.scalar_decisions version=1 rust_sha256=d8f291093eb4b637d9ab413d3e9a9f74f5fd9b341e15364fab329c4d9f50ef14*/
constexpr bool raft_quorum_reached(int32_t received, int32_t needed);
constexpr size_t raft_quorum_majority_count(size_t total);
constexpr bool raft_quorum_count_reached(size_t count, size_t quorum);
constexpr bool raft_quorum_count_below(size_t count, size_t quorum);
constexpr bool raft_quorum_reached(int32_t received, int32_t needed) {
    return rusty::detail::deref_if_pointer_like(received) >= rusty::detail::deref_if_pointer_like(needed);
}
constexpr size_t raft_quorum_majority_count(size_t total) {
    return ((rusty::detail::deref_if_pointer_like(total) / static_cast<size_t>(2))) + static_cast<size_t>(1);
}
constexpr bool raft_quorum_count_reached(size_t count, size_t quorum) {
    return rusty::detail::deref_if_pointer_like(count) >= rusty::detail::deref_if_pointer_like(quorum);
}
constexpr bool raft_quorum_count_below(size_t count, size_t quorum) {
    return rusty::detail::deref_if_pointer_like(count) < rusty::detail::deref_if_pointer_like(quorum);
}
/*RUSTYCPP:GEN-END id=raft_quorum.scalar_decisions*/

static_assert(raft_quorum_reached(3, 3));
static_assert(!raft_quorum_reached(2, 3));
static_assert(raft_quorum_majority_count(1) == 1);
static_assert(raft_quorum_majority_count(2) == 2);
static_assert(raft_quorum_majority_count(5) == 3);
static_assert(raft_quorum_count_reached(4, 4));
static_assert(raft_quorum_count_below(3, 4));

}  // namespace raft
}  // namespace janus
