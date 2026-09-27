#pragma once

// The `rusty` facade for the transpiled C++ lane (MAKO_RAFT_LANE=cpp).
//
// The Raft core names a handful of C++ objects it holds but never inspects --
// `rusty::RaftCommand`, `rusty::LearnerAction`, the std::function and
// shared_ptr carriers, the reactor handles -- plus the log shims
// `rusty::raft_log_<level>_<arity>`. Under rustc those come from the facade
// crate src/rusty-rustc/src/lib.rs as opaque, size-pinned byte arrays whose
// Drop and Clone call `raft_destroy_*` / `*_clone_into` kernels. This header
// is the same facade for the crate-mode C++ the transpiler emits, and it makes
// the same choice: the carriers are opaque byte arrays of the pinned size and
// alignment, and every lifecycle operation is the SAME extern "C" kernel the
// rustc lane imports.
//
// Why opaque rather than the real types: the crate-mode modules can see
// foreign types only through their global module fragment, and a header
// there may not `import` anything -- while every real type behind these
// carriers (janus::Command, srpc::IntEvent, ...) lives behind an
// `import srpc.*`. Keeping the carriers opaque also makes the two cores'
// import sets identical, which is what L4's parity check compares.
//
// Semantics, carrier by carrier, mirror the rustc facade:
//  * default construction is all-zero bytes, the empty state of every
//    carrier that has a Default there;
//  * a move is bitwise and leaves the source all-zero (Rust forgets a
//    moved-from value; C++ destroys it, so it must be left empty);
//  * destruction calls the destroy kernel unless the bytes are all zero --
//    the destroy kernels are no-ops on zero, and the Arc carriers, which have
//    no empty state, are only ever all-zero after a move;
//  * copy exists only where the rustc facade implements Clone: through the
//    clone kernel for the two Arc carriers, and as a hard abort for
//    CommoPeerPtr, whose rustc Clone panics for the same reason.
//
// Nothing here may import a module; see the ORDERING note in
// rust_facade_types.h for the rule this header lives under.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <charconv>
#include <concepts>
#include <string>
#include <string_view>
#include <type_traits>

#include <rusty/ffi.hpp>

#include "raft_kernel_pods.h"

namespace rusty {

// `core::ffi::c_char`, which rusty/ffi.hpp does not define.
namespace ffi {
using c_char = char;
}  // namespace ffi

namespace raft_cpp_lane_detail {
inline bool all_zero(const unsigned char* bytes, size_t size) noexcept {
  for (size_t i = 0; i < size; ++i) {
    if (bytes[i] != 0) return false;
  }
  return true;
}
}  // namespace raft_cpp_lane_detail

#define RAFT_CPP_LANE_CARRIER_BODY(Name, Size, Destroy)                        \
  unsigned char bytes_[Size] = {};                                           \
  Name() noexcept = default;                                                   \
  Name(Name&& other) noexcept {                                                \
    std::memcpy(bytes_, other.bytes_, Size);                                   \
    std::memset(other.bytes_, 0, Size);                                        \
  }                                                                            \
  Name& operator=(Name&& other) noexcept {                                     \
    if (this != &other) {                                                      \
      reset_();                                                                \
      std::memcpy(bytes_, other.bytes_, Size);                                 \
      std::memset(other.bytes_, 0, Size);                                      \
    }                                                                          \
    return *this;                                                              \
  }                                                                            \
  ~Name() { reset_(); }                                                        \
  void reset_() noexcept {                                                     \
    if (!raft_cpp_lane_detail::all_zero(bytes_, Size)) {                       \
      Destroy(this);                                                           \
      std::memset(bytes_, 0, Size);                                            \
    }                                                                          \
  }

// A carrier with a destroy kernel and no copy.
#define RAFT_CPP_LANE_CARRIER(Name, Size, Align, Destroy)                      \
  struct Name;                                                                 \
  extern "C" void Destroy(Name*);                                              \
  struct alignas(Align) Name {                                                 \
    RAFT_CPP_LANE_CARRIER_BODY(Name, Size, Destroy)                            \
    Name(const Name&) = delete;                                                \
    Name& operator=(const Name&) = delete;                                     \
  };                                                                           \
  static_assert(sizeof(Name) == Size && alignof(Name) == Align)

// A carrier whose copy is a clone kernel constructing into raw storage.
#define RAFT_CPP_LANE_CLONE_CARRIER(Name, Size, Align, Destroy, CloneInto)     \
  struct Name;                                                                 \
  extern "C" void Destroy(Name*);                                              \
  extern "C" void CloneInto(const Name*, Name*);                               \
  struct alignas(Align) Name {                                                 \
    RAFT_CPP_LANE_CARRIER_BODY(Name, Size, Destroy)                            \
    Name(const Name& other) { CloneInto(&other, this); }                       \
    Name& operator=(const Name& other) {                                       \
      if (this != &other) {                                                    \
        reset_();                                                              \
        CloneInto(&other, this);                                               \
      }                                                                        \
      return *this;                                                            \
    }                                                                          \
    Name clone() const { return Name(*this); }                                 \
  };                                                                           \
  static_assert(sizeof(Name) == Size && alignof(Name) == Align)

// Sizes and alignments: src/rusty-rustc/src/lib.rs, pinned from the C++ side
// by the static_assert block in src/deptran/raft/server.h.
RAFT_CPP_LANE_CARRIER(RaftCommand, 24, 8, raft_destroy_command);
RAFT_CPP_LANE_CARRIER(RaftResponsePtr, 16, 8, raft_destroy_response_ptr);
RAFT_CPP_LANE_CARRIER(RaftCheckedMutex, 48, 8, raft_destroy_checked_mutex);
RAFT_CPP_LANE_CARRIER(RaftAsyncCallbackLifetimePtr, 16, 8,
                      raft_destroy_async_callback_lifetime_ptr);
RAFT_CPP_LANE_CARRIER(RaftSnapshotManagerPtr, 16, 8,
                      raft_destroy_snapshot_manager_ptr);
RAFT_CPP_LANE_CARRIER(RaftCreateSnapshotCb, 48, 16,
                      raft_destroy_create_snapshot_cb);
RAFT_CPP_LANE_CARRIER(RaftPrepareSnapshotCb, 48, 16,
                      raft_destroy_prepare_snapshot_cb);
RAFT_CPP_LANE_CARRIER(RaftStdMutex, 40, 8, raft_destroy_std_mutex);
RAFT_CPP_LANE_CARRIER(RaftLeaderChangeCb, 48, 16, raft_destroy_leader_change_cb);
RAFT_CPP_LANE_CARRIER(RaftStdThread, 8, 8, raft_destroy_std_thread);
RAFT_CPP_LANE_CARRIER(RaftVoteQuorumPtr, 16, 8, raft_destroy_vote_quorum_ptr);
RAFT_CPP_LANE_CARRIER(RaftByteString, 24, 8, raft_destroy_byte_string);
RAFT_CPP_LANE_CARRIER(LearnerAction, 48, 16, raft_destroy_learner_action);
RAFT_CPP_LANE_CARRIER(RaftTpcCommitPtr, 8, 8, raft_destroy_tpc_commit_ptr);
RAFT_CPP_LANE_CLONE_CARRIER(RaftIntEventPtr, 8, 8, raft_destroy_int_event_ptr,
                            raft_int_event_clone_into);
RAFT_CPP_LANE_CLONE_CARRIER(RaftPollThreadPtr, 8, 8,
                            raft_destroy_poll_thread_ptr,
                            raft_poll_thread_clone_into);

#undef RAFT_CPP_LANE_CLONE_CARRIER
#undef RAFT_CPP_LANE_CARRIER
#undef RAFT_CPP_LANE_CARRIER_BODY

// `std::shared_ptr<janus::RpcPeer>`, deptran's PeerRegistry element. The
// registry is a Paxos-shared twin the Raft core carries but never runs; the
// rustc facade's Clone panics, and so does this copy.
struct alignas(8) CommoPeerPtr {
  unsigned char bytes_[16] = {};
  CommoPeerPtr() noexcept = default;
  CommoPeerPtr(CommoPeerPtr&& other) noexcept {
    std::memcpy(bytes_, other.bytes_, sizeof bytes_);
    std::memset(other.bytes_, 0, sizeof bytes_);
  }
  CommoPeerPtr& operator=(CommoPeerPtr&& other) noexcept {
    std::memcpy(bytes_, other.bytes_, sizeof bytes_);
    std::memset(other.bytes_, 0, sizeof bytes_);
    return *this;
  }
  CommoPeerPtr(const CommoPeerPtr&) { std::abort(); }
  CommoPeerPtr& operator=(const CommoPeerPtr&) { std::abort(); }
  CommoPeerPtr clone() const { std::abort(); }
};

// Types the core names only through pointers or through the unrun twin.
struct Communicator;
struct ReactorPollThread {};

extern "C" void raft_create_int_event_into(RaftIntEventPtr* out);
inline RaftIntEventPtr raft_new_int_event() {
  RaftIntEventPtr slot;
  raft_create_int_event_into(&slot);
  return slot;
}

extern "C" void raft_stamped_commit_into(const RaftCommand* cmd, int64_t term,
                                         RaftTpcCommitPtr* out);
inline RaftTpcCommitPtr raft_stamped_commit(const RaftCommand* cmd, int64_t term) {
  RaftTpcCommitPtr slot;
  raft_stamped_commit_into(cmd, term, &slot);
  return slot;
}

// ---- logging --------------------------------------------------------------
// The rustc facade's raft_log_format, line for line: `{}` and `{:x}` only,
// doubled braces are literal, a lone `}` stays visible, anything else is
// written as a visible marker rather than dropped.

inline constexpr int32_t RAFT_LOG_ERROR = 1;
inline constexpr int32_t RAFT_LOG_WARN = 2;
inline constexpr int32_t RAFT_LOG_INFO = 3;
inline constexpr int32_t RAFT_LOG_DEBUG = 4;

extern "C" bool raft_log_enabled(int32_t level);
extern "C" void raft_log_line(int32_t level, const uint8_t* text, size_t len);

namespace raft_cpp_lane_detail {

template <class T>
void append_integer(std::string& out, T value, bool hex) {
  char buf[40];
  using Wide = std::conditional_t<std::is_signed_v<T>, long long, unsigned long long>;
  auto res = std::to_chars(buf, buf + sizeof buf, static_cast<Wide>(value), hex ? 16 : 10);
  out.append(buf, res.ptr);
}

inline void append_utf8(std::string& out, char32_t c) {
  if (c < 0x80) {
    out.push_back(static_cast<char>(c));
  } else if (c < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (c >> 6)));
    out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
  } else if (c < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (c >> 12)));
    out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (c >> 18)));
    out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
  }
}

template <class T>
void append_arg(std::string& out, const T& value, bool hex) {
  using U = std::remove_cvref_t<T>;
  if constexpr (std::is_same_v<U, bool>) {
    out += value ? "true" : "false";
  } else if constexpr (std::is_same_v<U, char32_t>) {
    append_utf8(out, value);
  } else if constexpr (std::is_integral_v<U>) {
    append_integer(out, value, hex);
  } else if constexpr (std::is_floating_point_v<U>) {
    out += std::to_string(value);
  } else if constexpr (std::is_convertible_v<const U&, std::string_view>) {
    out += std::string_view(value);
  } else if constexpr (requires { value.as_str(); }) {
    out += std::string_view(value.as_str());
  } else if constexpr (requires { value.as_bytes(); }) {
    auto bytes = value.as_bytes();
    out.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  } else if constexpr (requires { *value; }) {
    append_arg(out, *value, hex);
  } else {
    static_assert(sizeof(U) == 0, "raft log argument type has no rendering");
  }
}

using AppendFn = void (*)(std::string&, const void*, bool);

template <class T>
void append_erased(std::string& out, const void* value, bool hex) {
  append_arg(out, *static_cast<const T*>(value), hex);
}

struct ErasedArg {
  const void* value;
  AppendFn append;
};

inline std::string format(std::string_view fmt, const ErasedArg* args, size_t nargs) {
  std::string out;
  out.reserve(fmt.size() + 16 * nargs);
  size_t next = 0;
  std::string_view rest = fmt;
  for (;;) {
    size_t pos = rest.find_first_of("{}");
    if (pos == std::string_view::npos) {
      out += rest;
      break;
    }
    out += rest.substr(0, pos);
    std::string_view tail = rest.substr(pos);
    if (tail.starts_with("{{")) {
      out.push_back('{');
      rest = tail.substr(2);
      continue;
    }
    if (tail.starts_with("}}")) {
      out.push_back('}');
      rest = tail.substr(2);
      continue;
    }
    if (tail.starts_with('}')) {
      out.push_back('}');
      rest = tail.substr(1);
      continue;
    }
    size_t close = tail.find('}');
    if (close == std::string_view::npos) {
      out += tail;
      break;
    }
    std::string_view spec = tail.substr(1, close - 1);
    if (next < nargs && spec.empty()) {
      args[next].append(out, args[next].value, false);
    } else if (next < nargs && spec == ":x") {
      args[next].append(out, args[next].value, true);
    } else if (next < nargs) {
      out += "{bad spec}";
    } else {
      out += "{missing arg}";
    }
    ++next;
    rest = tail.substr(close + 1);
  }
  return out;
}

template <class... A>
void emit(int32_t level, std::string_view fmt, const A&... args) {
  if (!raft_log_enabled(level)) return;
  const ErasedArg erased[sizeof...(A) + 1] = {
      {static_cast<const void*>(&args), &append_erased<A>}..., {nullptr, nullptr}};
  std::string line = format(fmt, erased, sizeof...(A));
  raft_log_line(level, reinterpret_cast<const uint8_t*>(line.data()), line.size());
}

}  // namespace raft_cpp_lane_detail

#define RAFT_CPP_LANE_LOG_LEVEL(level, prefix)                                 \
  template <class... A>                                                        \
  void prefix##_0(std::string_view fmt) {                                      \
    raft_cpp_lane_detail::emit(level, fmt);                                    \
  }                                                                            \
  template <class A0>                                                          \
  void prefix##_1(std::string_view fmt, const A0& a0) {                        \
    raft_cpp_lane_detail::emit(level, fmt, a0);                                \
  }                                                                            \
  template <class... A>                                                        \
  requires(sizeof...(A) == 2) void prefix##_2(std::string_view f, const A&... a) { \
    raft_cpp_lane_detail::emit(level, f, a...);                                \
  }                                                                            \
  template <class... A>                                                        \
  requires(sizeof...(A) == 3) void prefix##_3(std::string_view f, const A&... a) { \
    raft_cpp_lane_detail::emit(level, f, a...);                                \
  }                                                                            \
  template <class... A>                                                        \
  requires(sizeof...(A) == 4) void prefix##_4(std::string_view f, const A&... a) { \
    raft_cpp_lane_detail::emit(level, f, a...);                                \
  }                                                                            \
  template <class... A>                                                        \
  requires(sizeof...(A) == 5) void prefix##_5(std::string_view f, const A&... a) { \
    raft_cpp_lane_detail::emit(level, f, a...);                                \
  }                                                                            \
  template <class... A>                                                        \
  requires(sizeof...(A) == 6) void prefix##_6(std::string_view f, const A&... a) { \
    raft_cpp_lane_detail::emit(level, f, a...);                                \
  }                                                                            \
  template <class... A>                                                        \
  requires(sizeof...(A) == 7) void prefix##_7(std::string_view f, const A&... a) { \
    raft_cpp_lane_detail::emit(level, f, a...);                                \
  }                                                                            \
  template <class... A>                                                        \
  requires(sizeof...(A) == 8) void prefix##_8(std::string_view f, const A&... a) { \
    raft_cpp_lane_detail::emit(level, f, a...);                                \
  }                                                                            \
  template <class... A>                                                        \
  requires(sizeof...(A) == 9) void prefix##_9(std::string_view f, const A&... a) { \
    raft_cpp_lane_detail::emit(level, f, a...);                                \
  }                                                                            \
  template <class... A>                                                        \
  requires(sizeof...(A) == 10) void prefix##_10(std::string_view f, const A&... a) { \
    raft_cpp_lane_detail::emit(level, f, a...);                                \
  }

RAFT_CPP_LANE_LOG_LEVEL(RAFT_LOG_DEBUG, raft_log_debug)
RAFT_CPP_LANE_LOG_LEVEL(RAFT_LOG_INFO, raft_log_info)
RAFT_CPP_LANE_LOG_LEVEL(RAFT_LOG_WARN, raft_log_warn)
RAFT_CPP_LANE_LOG_LEVEL(RAFT_LOG_ERROR, raft_log_error)

#undef RAFT_CPP_LANE_LOG_LEVEL

}  // namespace rusty
