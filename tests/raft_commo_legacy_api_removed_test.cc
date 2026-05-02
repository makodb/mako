#include <gtest/gtest.h>

#include <type_traits>
#include <utility>

#include "deptran/raft/commo.h"

namespace {

template <class T, class = void>
struct has_send_append_entries2 : std::false_type {};

template <class T>
struct has_send_append_entries2<T, std::void_t<decltype(&T::SendAppendEntries2)>>
    : std::true_type {};

template <class T, class = void>
struct has_send_append_entries : std::false_type {};

template <class T>
struct has_send_append_entries<T, std::void_t<decltype(&T::SendAppendEntries)>>
    : std::true_type {};

}  // namespace

TEST(RaftCommoLegacyApiRemovedTest, LegacyAppendEntriesApiIsNotExposed) {
  static_assert(!has_send_append_entries2<janus::RaftCommo>::value,
                "RaftCommo::SendAppendEntries2 must stay removed");
  static_assert(!has_send_append_entries<janus::RaftCommo>::value,
                "RaftCommo::SendAppendEntries must stay removed");
  SUCCEED();
}
