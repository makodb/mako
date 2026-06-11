module;

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <inttypes.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#include <rusty/arc.hpp>
#include <rusty/box.hpp>
#include <rusty/fn.hpp>
#include <rusty/rusty.hpp>

export module rrr.marshal;

import std;
import rusty;
import rrr.basetypes;
import rrr.debugging;
import rrr.misc;
import rrr.serializable;
import rrr.threading;

// @safe - Marshal: append-only byte buffer with separate write/read
// cursors, backed by a single rusty::Vec<uint8_t>. Replaces the prior
// chunk-linked-list implementation; see docs/dev/marshal_perf_baseline.md
// for the perf comparison that motivated the swap (V2 wins 16-81%
// across every benchmark scenario).
//
// Public API is unchanged: write(p,n) / read(p,n) / peek<T>(out,n) /
// content_size / set_bookmark / write_bookmark / read_from_marshal /
// reset / MarshalSink + MarshalSource adapters. The chunk-specific
// helpers `read_chnk` and `read_reuse_chnk` are removed (no external
// callers) and `init_block_read` becomes a buffer pre-reserve.
//
// Unsafety footprint: every public method is `// @safe` with at most
// one inline `// @unsafe { ... }` block around the libc `memcpy`
// call. No raw `char*` arithmetic, no `chunk*` linked-list walks, no
// `char**` bookmark pointers.
export namespace rrr {


// @safe - Wrapper for std::min (pure function, no side effects)
template<typename T>
inline T safe_min(const T& a, const T& b) {
  // @unsafe
  { return std::min(a, b); }
}

// removed the entire `RPC_STATISTICS` block
// and `stat_marshal_in` declaration. After Phase 5b-7/5b-8 deleted
// the marshal-out side, the marshal-in side became dead too once
// `Marshal::read_from_fd` / `Marshal::chnk_read_from_fd` / `chunk::read_from_fd`
// had no production callers anywhere in the codebase. The receive path
// uses `FdSource` (`serializable.hpp`) instead.

// not thread safe, for better performance
class Marshal;


// @safe - Vec<uint8_t>-backed byte queue with separate write/read
// cursors. Append-only writes go to buf_; reads memcpy from buf_.data
// + read_pos_ and advance read_pos_. When read_pos_ catches up to
// buf_.size() (fully drained), both reset to zero so steady-state
// write/read loops don't grow buf_ unboundedly.
// Pre-reserved capacity on first construction so small payloads don't
// pay a realloc-on-first-write. 4 KB matches the legacy chunk-list's
// default chunk size, keeping per-Marshal memory footprint comparable
// for the bench comparison.
//
// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is
// the source of truth; the transpiler regenerates the matching
// `RUSTYCPP:GEN-BEGIN ... END` block with the C++ constexpr. Lifted
// from `Marshal` class scope (private static constexpr) to namespace
// scope because DSL constants live at namespace level; the one
// existing use site references it unqualified, so namespace lookup
// still resolves to the new constant.
#if RUSTYCPP_RUST
const kInitialCapacity: usize = 4096;
#endif
/*RUSTYCPP:GEN-BEGIN id=marshal.initial_capacity version=1 rust_sha256=72b7f808695d048223203b2f3e49c2f8b175eace523aaf80b1b560ffaccb10a6*/
constexpr size_t kInitialCapacity = static_cast<size_t>(4096);
/*RUSTYCPP:GEN-END id=marshal.initial_capacity*/

class Marshal: public NoCopy {
private:
  rusty::Vec<std::uint8_t> buf_{};
  std::size_t read_pos_{0};
  rrr::i32 write_cnt_{0};

public:

  bool found_dep{false};
  bool valid_id{false};

  // @safe - Bookmark over the Vec<uint8_t>: stores an absolute offset
  // into buf_ and the size of the reserved slot. set_bookmark grows
  // buf_ by `size` zero bytes and records the offset; write_bookmark
  // memcpy's the patch in. The legacy chunk-list version held an
  // array of `char**` pointing into chunk-local storage — now
  // unnecessary because buf_ is contiguous.
  struct bookmark {
    std::size_t offset = 0;
    std::size_t size = 0;

    // @safe - Default ctor.
    bookmark() = default;

    bookmark(const bookmark&) = delete;
    bookmark& operator=(const bookmark&) = delete;

    // @safe - Move ctor (POD, just copies fields and zeros the source).
    bookmark(bookmark&& other) noexcept : offset(other.offset), size(other.size) {
      other.offset = 0;
      other.size = 0;
    }

    // @safe - Move assignment.
    bookmark& operator=(bookmark&& other) noexcept {
      if (this != &other) {
        offset = other.offset;
        size = other.size;
        other.offset = 0;
        other.size = 0;
      }
      return *this;
    }

    // @safe - Trivial dtor (no heap state).
    ~bookmark() = default;
  };

  // @safe - Default ctor: reserve starter capacity so small writes
  // don't pay the first-grow cost.
  Marshal() {
    buf_.reserve(kInitialCapacity);
  }

  // @safe - Trivial dtor — Vec releases the heap on drop. noexcept to
  // match NoCopy::~NoCopy()'s exception spec.
  ~Marshal() noexcept = default;

  // @safe - Explicit move declarations restore implicit-move
  // suppression caused by the user-declared destructor above. With
  // these, `Marshal` becomes move-constructible / move-assignable
  // (delegated through NoCopy's defaulted move members + rusty::Vec's
  // move). Copy stays deleted via NoCopy.
  Marshal(Marshal&&) noexcept = default;
  Marshal& operator=(Marshal&&) noexcept = default;

  // @safe - Rust-style factory matching `fn new() -> Self`. Equivalent
  // to default construction; provided for symmetry with the rest of
  // the rrr `new_()` rollout.
  static Marshal new_() {
    return Marshal{};
  }

  // @safe - Pre-reserve `block_size` bytes of capacity. The chunk-list
  // version allocated a single chunk of this size up front; here we
  // just hint the Vec to reserve. Legal to call when buf_ is empty.
  void init_block_read(std::size_t block_size) {
    buf_.reserve(block_size);
  }

  // @safe - Empty when fully drained.
  bool empty() const { return read_pos_ >= buf_.size(); }

  // @safe - Bytes between read cursor and write tail.
  std::size_t content_size() const { return buf_.size() - read_pos_; }

  // @safe - Same as content_size in the contiguous-buf representation;
  // kept for API compatibility with the chunk-list assertion calls
  // that compared cached size against a chunk-walk.
  std::size_t content_size_slow() const { return content_size(); }

  // @safe - Append n bytes from caller-owned p to buf_. Memcpy is
  // quarantined in Vec::extend_from_slice's internal @unsafe block
  // (rusty-cpp's Vec<uint8_t> fast path).
  std::size_t write(const void* p, std::size_t n) {
    // @unsafe { caller-provided `const void*` cast to a byte span;
    //           Vec::extend_from_slice memcpy. }
    {
      const auto* bytes = static_cast<const std::uint8_t*>(p);
      buf_.extend_from_slice(std::span<const std::uint8_t>(bytes, n));
    }
    write_cnt_ += static_cast<rrr::i32>(n);
    return n;
  }

  // @safe - Bounded memcpy out of buf_, advance read_pos_, reset on
  // full drain.
  std::size_t read(void* p, std::size_t n) {
    const std::size_t avail = buf_.size() - read_pos_;
    const std::size_t copy = std::min(n, avail);
    if (copy == 0) return 0;
    // @unsafe { libc memcpy from buf_.data()+read_pos_ to caller p. }
    {
      std::memcpy(p, buf_.data() + read_pos_, copy);
    }
    read_pos_ += copy;
    if (read_pos_ == buf_.size()) {
      // Fully drained — recycle storage so steady-state write/read
      // loops don't grow buf_ unboundedly. Vec::clear keeps the
      // capacity, only sets len back to 0.
      buf_.clear();
      read_pos_ = 0;
    }
    return copy;
  }

  // @safe - Type-safe overload of `read` for trivially-copyable T.
  template<typename T>
  std::size_t read(T& out, std::size_t n = sizeof(T)) {
    static_assert(std::is_trivially_copyable_v<T>, "read requires trivially copyable type");
    // @unsafe { reinterpret_cast for type-safe wrapper }
    {
      return read(reinterpret_cast<void*>(&out), n);
    }
  }

  // @safe - Like read() but doesn't advance the cursor; for trivially-
  // copyable T.
  template<typename T>
  std::size_t peek(T& out, std::size_t n = sizeof(T)) const {
    static_assert(std::is_trivially_copyable_v<T>, "peek requires trivially copyable type");
    const std::size_t avail = buf_.size() - read_pos_;
    const std::size_t copy = std::min(n, avail);
    if (copy == 0) return 0;
    // @unsafe { libc memcpy from buf_.data()+read_pos_; T* address-of. }
    {
      std::memcpy(reinterpret_cast<void*>(&out), buf_.data() + read_pos_, copy);
    }
    return copy;
  }

  // @safe - Splice n bytes from another Marshal into this one. Both
  // sides advance their cursors; source resets on full drain.
  std::size_t read_from_marshal(Marshal& src, std::size_t n) {
    verify(src.content_size() >= n);
    if (n == 0) return 0;
    // @unsafe { span over src.buf_'s unread range handed to
    //           Vec::extend_from_slice memcpy. }
    {
      auto* bytes = src.buf_.data() + src.read_pos_;
      buf_.extend_from_slice(std::span<const std::uint8_t>(bytes, n));
    }
    write_cnt_ += static_cast<rrr::i32>(n);
    src.read_pos_ += n;
    if (src.read_pos_ == src.buf_.size()) {
      src.buf_.clear();
      src.read_pos_ = 0;
    }
    return n;
  }

  // @safe - Empty buf_, reset read cursor and write count.
  void reset() {
    buf_.clear();
    read_pos_ = 0;
    write_cnt_ = 0;
  }

  // @safe - Reserve n zero-bytes at the current write tail; returns a
  // (offset, n) bookmark the caller patches with write_bookmark.
  bookmark set_bookmark(std::size_t n) {
    bookmark bm;
    bm.offset = buf_.size();
    bm.size = n;
    // Append n zero bytes — rusty::Vec::push is @safe. Could be replaced
    // with a resize_with primitive when added to Vec.
    for (std::size_t i = 0; i < n; ++i) {
      buf_.push(std::uint8_t{0});
    }
    write_cnt_ += static_cast<rrr::i32>(n);
    return bm;
  }

  // @safe - Patch the reserved bookmark slot with `value`. T must fit
  // exactly into bm.size bytes.
  template<typename T>
  void write_bookmark(bookmark& bm, const T& value) {
    static_assert(std::is_trivially_copyable_v<T>,
                  "write_bookmark requires trivially copyable T");
    verify(sizeof(T) <= bm.size);
    verify(bm.offset + bm.size <= buf_.size());
    // @unsafe { libc memcpy at buf_.data()+bm.offset. }
    {
      std::memcpy(buf_.data() + bm.offset, &value, bm.size);
    }
  }

  // @safe - Returns and resets the write counter.
  rrr::i32 get_and_reset_write_cnt() {
    rrr::i32 cnt = write_cnt_;
    write_cnt_ = 0;
    return cnt;
  }
};

// ---------------------------------------------------------------------------
// Marshal ↔ Archive bridges (relocated from serializable.hpp).
//
// MarshalSink wraps an `rrr::Marshal*` and forwards `write(p, n)` to
// `Marshal::write(p, n)`, so new `BinaryWriteArchive`-based code can
// emit bytes directly into an existing `Marshal` buffer without the
// caller having to allocate a separate `BufferSink` and copy.
//
// MarshalSource is the dual: wraps a `Marshal*` and forwards
// `read(p, n)` to `Marshal::read(p, n)`.
//
// Lifetime: non-owning. Caller owns the underlying `Marshal` and must
// keep it alive for the lifetime of the Sink/Source.
//
// Defined inline here (with Marshal's full class def in scope) to
// avoid the impl-side cycle that originally forced these into
// serializable.cpp.
// ---------------------------------------------------------------------------

// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is
// the source of truth; the transpiler regenerates the matching
// `RUSTYCPP:GEN-BEGIN ... END` block.
//
// `write(const void*, size_t)` and `read(void*, size_t)` live OUTSIDE
// the DSL block as free functions (`marshal_sink_write` /
// `marshal_source_read`) — the bodies' `void*` parameter isn't
// expressible in inline-Rust today. The `fn marshal(&self)`
// accessor returns the raw Marshal pointer for downstream code that
// drives Marshal's own API directly. The DSL `fn new` factory keeps
// the existing 1-arg paren-init form working via C++20 aggregate
// paren-init rules.
#if RUSTYCPP_RUST
struct MarshalSink {
    m_: *mut Marshal,
}

impl MarshalSink {
    fn new(m: *mut Marshal) -> MarshalSink {
        MarshalSink { m_: m as *mut Marshal }
    }

    fn marshal(&self) -> *mut Marshal {
        self.m_
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=marshal.sink version=1 rust_sha256=fdb6a6556d0ae770840f46cf0e26f7e29b41668e39a2d3b94a9acd83c168c38a*/
struct MarshalSink;

struct MarshalSink {
    Marshal* m_;

    static MarshalSink new_(Marshal* m);
    Marshal* marshal() const;
};


MarshalSink MarshalSink::new_(Marshal* m) {
    return MarshalSink{.m_ = const_cast<Marshal*>(reinterpret_cast<const Marshal*>(m))};
}

Marshal* MarshalSink::marshal() const {
    return this->m_;
}
/*RUSTYCPP:GEN-END id=marshal.sink*/

// @unsafe { Marshal::write through raw pointer + verify }
inline void marshal_sink_write(MarshalSink& self, const void* p, size_t n) {
    size_t actual = self.m_->write(p, n);
    verify(actual == n);
}

#if RUSTYCPP_RUST
struct MarshalSource {
    m_: *mut Marshal,
}

impl MarshalSource {
    fn new(m: *mut Marshal) -> MarshalSource {
        MarshalSource { m_: m as *mut Marshal }
    }

    fn marshal(&self) -> *mut Marshal {
        self.m_
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=marshal.source version=1 rust_sha256=5af7a973d3b8d88d285fca4d0459cd8a5a3cd0bbef06401dd5f3854497e46442*/
struct MarshalSource;

struct MarshalSource {
    Marshal* m_;

    static MarshalSource new_(Marshal* m);
    Marshal* marshal() const;
};


MarshalSource MarshalSource::new_(Marshal* m) {
    return MarshalSource{.m_ = const_cast<Marshal*>(reinterpret_cast<const Marshal*>(m))};
}

Marshal* MarshalSource::marshal() const {
    return this->m_;
}
/*RUSTYCPP:GEN-END id=marshal.source*/

// @unsafe { Marshal::read through raw pointer }
inline size_t marshal_source_read(MarshalSource& self, void* p, size_t n) {
    return self.m_->read(p, n);
}

class MarshalSinkAdapter : public SinkBase {
  MarshalSink* sink_;
 public:
  explicit MarshalSinkAdapter(MarshalSink* s) noexcept : sink_(s) {}
  void write_bytes(const uint8_t* p, size_t n) override { marshal_sink_write(*sink_, p, n); }
  MarshalSink* sink() const noexcept { return sink_; }
};

class MarshalSourceAdapter : public SourceBase {
  MarshalSource* source_;
 public:
  explicit MarshalSourceAdapter(MarshalSource* s) noexcept : source_(s) {}
  size_t read_bytes(uint8_t* p, size_t n) override { return marshal_source_read(*source_, p, n); }
  MarshalSource* source() const noexcept { return source_; }
};

inline SinkProxy make_sink_proxy(MarshalSink* sink) {
  return rusty::make_box<MarshalSinkAdapter>(sink);
}
inline SourceProxy make_source_proxy(MarshalSource* source) {
  return rusty::make_box<MarshalSourceAdapter>(source);
}

// @safe
// @lifetime: (&'a, const i8&) -> &'a
inline rrr::Marshal &operator<<(rrr::Marshal &m, const rrr::i8 &v) {
  verify(m.write(&v, sizeof(v)) == sizeof(v));
  return m;
}

// @safe
// @lifetime: (&'a, const i16&) -> &'a
inline rrr::Marshal &operator<<(rrr::Marshal &m, const rrr::i16 &v) {
  verify(m.write(&v, sizeof(v)) == sizeof(v));
  return m;
}

// @safe
// @lifetime: (&'a, const i32&) -> &'a
inline rrr::Marshal &operator<<(rrr::Marshal &m, const rrr::i32 &v) {
  verify(m.write(&v, sizeof(v)) == sizeof(v));
  return m;
}

// @safe
// @lifetime: (&'a, const i64&) -> &'a
inline rrr::Marshal &operator<<(rrr::Marshal &m, const rrr::i64 &v) {
  verify(m.write(&v, sizeof(v)) == sizeof(v));

	if (m.found_dep) {
		if (v != -1) {
			m.valid_id = true;
		} else {
		}
		m.found_dep = false;
	}

  return m;
}

// @safe - Writes v32 to marshal
// @lifetime: (&'a, const v32&) -> &'a
inline rrr::Marshal &operator<<(rrr::Marshal &m, const rrr::v32 &v) {
  // @unsafe
  {
    char buf[5];
    size_t bsize = rrr::SparseInt::dump(v.get(), buf);
    verify(m.write(buf, bsize) == bsize);
    return m;
  }
}

// @safe - Writes v64 to marshal
// @lifetime: (&'a, const v64&) -> &'a
inline rrr::Marshal &operator<<(rrr::Marshal &m, const rrr::v64 &v) {
  // @unsafe
  {
    char buf[9];
    size_t bsize = rrr::SparseInt::dump(v.get(), buf);
    verify(m.write(buf, bsize) == bsize);
    return m;
  }
}

// @safe
// @lifetime: (&'a, const uint8_t&) -> &'a
inline rrr::Marshal &operator<<(rrr::Marshal &m, const uint8_t &u) {
  verify(m.write(&u, sizeof(u)) == sizeof(u));
  return m;
}

// @safe
// @lifetime: (&'a, const uint16_t&) -> &'a
inline rrr::Marshal &operator<<(rrr::Marshal &m, const uint16_t &u) {
  verify(m.write(&u, sizeof(u)) == sizeof(u));
  return m;
}

// @safe
// @lifetime: (&'a, const uint32_t&) -> &'a
inline rrr::Marshal &operator<<(rrr::Marshal &m, const uint32_t &u) {
  verify(m.write(&u, sizeof(u)) == sizeof(u));
  return m;
}

// @safe
// @lifetime: (&'a, const uint64_t&) -> &'a
inline rrr::Marshal &operator<<(rrr::Marshal &m, const uint64_t &u) {
  verify(m.write(&u, sizeof(u)) == sizeof(u));
  return m;
}

// @safe
// @lifetime: (&'a, const double&) -> &'a
inline rrr::Marshal &operator<<(rrr::Marshal &m, const double &v) {
  verify(m.write(&v, sizeof(v)) == sizeof(v));
  return m;
}

// @safe
// @lifetime: (&'a, const std::string&) -> &'a
inline rrr::Marshal &operator<<(rrr::Marshal &m, const std::string &v) {
  v64 v_len{static_cast<rrr::i64>(v.length())};
  m << v_len;
  if (v_len.get() > 0) {
    verify(m.write(v.c_str(), v_len.get()) == (size_t) v_len.get());
  }

	if (v == "dep") {
		m.found_dep = true;
	} else if (v == "hb") {
		m.valid_id = true;
	} else {
    m.valid_id = true;
	}

  return m;
}

// @safe
// @lifetime: (&'a, const T1&, const T2&) -> &'a
template<class T1, class T2>
inline rrr::Marshal &operator<<(rrr::Marshal &m, const std::pair<T1, T2> &v) {
    m << v.first;
    m << v.second;
    return m;
}

// @safe
// @lifetime: (&'a, const rusty::Vec<T>&) -> &'a
template<class T>
inline rrr::Marshal &operator<<(rrr::Marshal &m, const rusty::Vec<T> &v) {
    v64 v_len{static_cast<rrr::i64>(v.size())};
    m << v_len;
    for (typename rusty::Vec<T>::const_iterator it = v.begin(); it != v.end();
         ++it) {
      m << *it;
    }
    return m;
}

// @safe
// @lifetime: (&'a, const std::vector<T>&) -> &'a
template<class T>
inline rrr::Marshal &operator<<(rrr::Marshal &m, const std::vector<T> &v) {
  // Keep std::vector support for non-rrr call sites while rrr internals move to rusty containers.
  v64 v_len{static_cast<rrr::i64>(v.size())};
  m << v_len;
  for (typename std::vector<T>::const_iterator it = v.begin(); it != v.end();
       ++it) {
    m << *it;
  }
  return m;
}

// @safe
// @lifetime: (&'a, const std::list<T>&) -> &'a
template<class T>
inline rrr::Marshal &operator<<(rrr::Marshal &m, const std::list<T> &v) {
    v64 v_len{static_cast<rrr::i64>(v.size())};
    m << v_len;
    for (typename std::list<T>::const_iterator it = v.begin(); it != v.end();
         ++it) {
      m << *it;
    }
    return m;
}

// @safe
// @lifetime: (&'a, const rusty::BTreeSet<T>&) -> &'a
template<class T>
inline rrr::Marshal &operator<<(rrr::Marshal &m, const rusty::BTreeSet<T> &v) {
    v64 v_len{static_cast<rrr::i64>(v.size())};
    m << v_len;
    for (typename rusty::BTreeSet<T>::const_iterator it = v.begin(); it != v.end();
         ++it) {
      m << *it;
    }
    return m;
}

// @safe
// @lifetime: (&'a, const std::set<T>&) -> &'a
template<class T>
inline rrr::Marshal &operator<<(rrr::Marshal &m, const std::set<T> &v) {
  v64 v_len{static_cast<rrr::i64>(v.size())};
  m << v_len;
  for (typename std::set<T>::const_iterator it = v.begin(); it != v.end();
       ++it) {
    m << *it;
  }
  return m;
}

// @safe
// @lifetime: (&'a, const rusty::BTreeMap<K,V>&) -> &'a
template<class K, class V>
inline rrr::Marshal &operator<<(rrr::Marshal &m, const rusty::BTreeMap<K, V> &v) {
    v64 v_len{static_cast<rrr::i64>(v.size())};
    m << v_len;
    // rusty::BTreeMap iter `operator*()` returns
    // `std::tuple<const K&, const V&>` (post-2026-04 API).
    for (typename rusty::BTreeMap<K, V>::const_iterator it = v.begin(); it != v.end();
         ++it) {
      auto kv = *it;
      m << std::get<0>(kv) << std::get<1>(kv);
    }
    return m;
}

// @safe
// @lifetime: (&'a, const std::map<K,V>&) -> &'a
template<class K, class V>
inline rrr::Marshal &operator<<(rrr::Marshal &m, const std::map<K, V> &v) {
  v64 v_len{static_cast<rrr::i64>(v.size())};
  m << v_len;
  for (typename std::map<K, V>::const_iterator it = v.begin(); it != v.end();
       ++it) {
    m << it->first << it->second;
  }
  return m;
}

// @safe
// @lifetime: (&'a, const rusty::HashSet<T>&) -> &'a
template<class T>
inline rrr::Marshal &operator<<(rrr::Marshal &m,
                                const rusty::HashSet<T> &v) {
    v64 v_len{static_cast<rrr::i64>(v.size())};
    m << v_len;
    for (typename rusty::HashSet<T>::const_iterator it = v.begin();
         it != v.end(); ++it) {
      m << *it;
    }
    return m;
}

// @safe
// @lifetime: (&'a, const std::unordered_set<T>&) -> &'a
template<class T>
inline rrr::Marshal &operator<<(rrr::Marshal &m,
                                const std::unordered_set<T> &v) {
  v64 v_len{static_cast<rrr::i64>(v.size())};
  m << v_len;
  for (typename std::unordered_set<T>::const_iterator it = v.begin();
       it != v.end(); ++it) {
    m << *it;
  }
  return m;
}

// @safe
// @lifetime: (&'a, const rusty::HashMap<K,V>&) -> &'a
template<class K, class V>
inline rrr::Marshal &operator<<(rrr::Marshal &m,
                                const rusty::HashMap<K, V> &v) {
    v64 v_len{static_cast<rrr::i64>(v.size())};
    m << v_len;
    // rusty::HashMap iter `operator*()` returns
    // `std::tuple<const K&, const V&>` (post-2026-04 API).
    for (typename rusty::HashMap<K, V>::const_iterator it = v.begin();
         it != v.end(); ++it) {
      auto kv = *it;
      m << std::get<0>(kv) << std::get<1>(kv);
    }
    return m;
}

// @safe
// @lifetime: (&'a, const std::unordered_map<K,V>&) -> &'a
template<class K, class V>
inline rrr::Marshal &operator<<(rrr::Marshal &m,
                                const std::unordered_map<K, V> &v) {
  v64 v_len{static_cast<rrr::i64>(v.size())};
  m << v_len;
  for (typename std::unordered_map<K, V>::const_iterator it = v.begin();
       it != v.end(); ++it) {
    m << it->first << it->second;
  }
  return m;
}

// @safe
// @lifetime: (&'a, i8&) -> &'a
inline rrr::Marshal &operator>>(rrr::Marshal &m, rrr::i8 &v) {
  verify(m.read(&v, sizeof(v)) == sizeof(v));
  return m;
}

// @safe
// @lifetime: (&'a, i16&) -> &'a
inline rrr::Marshal &operator>>(rrr::Marshal &m, rrr::i16 &v) {
  verify(m.read(&v, sizeof(v)) == sizeof(v));
  return m;
}

// @safe
// @lifetime: (&'a, i32&) -> &'a
inline rrr::Marshal &operator>>(rrr::Marshal &m, rrr::i32 &v) {
  verify(m.read(&v, sizeof(v)) == sizeof(v));
  return m;
}

// @safe
// @lifetime: (&'a, i64&) -> &'a
inline rrr::Marshal &operator>>(rrr::Marshal &m, rrr::i64 &v) {
  verify(m.read(&v, sizeof(v)) == sizeof(v));
  return m;
}

// @safe
// @lifetime: (&'a, v32&) -> &'a
inline rrr::Marshal &operator>>(rrr::Marshal &m, rrr::v32 &v) {
  char byte0;
  verify(m.peek(byte0, 1) == 1);
  size_t bsize = rrr::SparseInt::buf_size(byte0);
  char buf[5];
  verify(m.read(buf, bsize) == bsize);
  i32 val = rrr::SparseInt::load_i32(buf);
  v.set(val);
  return m;
}

// @safe
// @lifetime: (&'a, v64&) -> &'a
inline rrr::Marshal &operator>>(rrr::Marshal &m, rrr::v64 &v) {
  char byte0;
  verify(m.peek(byte0, 1) == 1);
  size_t bsize = rrr::SparseInt::buf_size(byte0);
  char buf[9];
  verify(m.read(buf, bsize) == bsize);
  i64 val = rrr::SparseInt::load_i64(buf);
  v.set(val);
  return m;
}

// @safe
// @lifetime: (&'a, uint8_t&) -> &'a
inline rrr::Marshal &operator>>(rrr::Marshal &m, uint8_t &u) {
  verify(m.read(&u, sizeof(u)) == sizeof(u));
  return m;
}

// @safe
// @lifetime: (&'a, uint16_t&) -> &'a
inline rrr::Marshal &operator>>(rrr::Marshal &m, uint16_t &u) {
  verify(m.read(&u, sizeof(u)) == sizeof(u));
  return m;
}

// @safe
// @lifetime: (&'a, uint32_t&) -> &'a
inline rrr::Marshal &operator>>(rrr::Marshal &m, uint32_t &u) {
  verify(m.read(&u, sizeof(u)) == sizeof(u));
  return m;
}

// @safe
// @lifetime: (&'a, uint64_t&) -> &'a
inline rrr::Marshal &operator>>(rrr::Marshal &m, uint64_t &u) {
  verify(m.read(&u, sizeof(u)) == sizeof(u));
  return m;
}

// @safe
// @lifetime: (&'a, double&) -> &'a
inline rrr::Marshal &operator>>(rrr::Marshal &m, double &v) {
  verify(m.read(&v, sizeof(v)) == sizeof(v));
  return m;
}

// @safe
// @lifetime: (&'a, std::string&) -> &'a
inline rrr::Marshal &operator>>(rrr::Marshal &m, std::string &v) {
  v64 v_len;
  m >> v_len;
  v.resize(v_len.get());
  if (v_len.get() > 0) {
    verify(m.read(&v[0], v_len.get()) == (size_t) v_len.get());
  }
  return m;
}

// @safe
// @lifetime: (&'a, std::pair<T1,T2>&) -> &'a
template<class T1, class T2>
inline rrr::Marshal &operator>>(rrr::Marshal &m, std::pair<T1, T2> &v) {
  m >> v.first;
  m >> v.second;
  return m;
}

// @safe
// @lifetime: (&'a, rusty::Vec<T>&) -> &'a
template<class T>
inline rrr::Marshal &operator>>(rrr::Marshal &m, rusty::Vec<T> &v) {
  v64 v_len;
  m >> v_len;
  v.clear();
  v.reserve(v_len.get());
  for (int i = 0; i < v_len.get(); i++) {
    T elem;
    m >> elem;
    v.push_back(elem);
  }
  return m;
}

// @safe
// @lifetime: (&'a, std::vector<T>&) -> &'a
template<class T>
inline rrr::Marshal &operator>>(rrr::Marshal &m, std::vector<T> &v) {
  v64 v_len;
  m >> v_len;
  v.clear();
  v.reserve(v_len.get());
  for (int i = 0; i < v_len.get(); i++) {
    T elem;
    m >> elem;
    v.push_back(elem);
  }
  return m;
}

// @safe
// @lifetime: (&'a, std::list<T>&) -> &'a
template<class T>
inline rrr::Marshal &operator>>(rrr::Marshal &m, std::list<T> &v) {
  v64 v_len;
  m >> v_len;
  v.clear();
  for (int i = 0; i < v_len.get(); i++) {
    T elem;
    m >> elem;
    v.push_back(elem);
  }
  return m;
}

// @safe
// @lifetime: (&'a, rusty::BTreeSet<T>&) -> &'a
template<class T>
inline rrr::Marshal &operator>>(rrr::Marshal &m, rusty::BTreeSet<T> &v) {
  v64 v_len;
  m >> v_len;
  v.clear();
  for (int i = 0; i < v_len.get(); i++) {
    T elem;
    m >> elem;
    v.insert(elem);
  }
  return m;
}

// @safe
// @lifetime: (&'a, std::set<T>&) -> &'a
template<class T>
inline rrr::Marshal &operator>>(rrr::Marshal &m, std::set<T> &v) {
  v64 v_len;
  m >> v_len;
  v.clear();
  for (int i = 0; i < v_len.get(); i++) {
    T elem;
    m >> elem;
    v.insert(elem);
  }
  return m;
}

// @safe
// @lifetime: (&'a, rusty::BTreeMap<K,V>&) -> &'a
template<class K, class V>
inline rrr::Marshal &operator>>(rrr::Marshal &m, rusty::BTreeMap<K, V> &v) {
  v64 v_len;
  m >> v_len;
  v.clear();
  for (int i = 0; i < v_len.get(); i++) {
    K key;
    V value;
    m >> key >> value;
    insert_into_map(v, key, value);
  }
  return m;
}

// @safe
// @lifetime: (&'a, std::map<K,V>&) -> &'a
template<class K, class V>
inline rrr::Marshal &operator>>(rrr::Marshal &m, std::map<K, V> &v) {
  v64 v_len;
  m >> v_len;
  v.clear();
  for (int i = 0; i < v_len.get(); i++) {
    K key;
    V value;
    m >> key >> value;
    insert_into_map(v, key, value);
  }
  return m;
}

// @safe
// @lifetime: (&'a, rusty::HashSet<T>&) -> &'a
template<class T>
inline rrr::Marshal &operator>>(rrr::Marshal &m, rusty::HashSet<T> &v) {
  v64 v_len;
  m >> v_len;
  v.clear();
  for (int i = 0; i < v_len.get(); i++) {
    T elem;
    m >> elem;
    v.insert(elem);
  }
  return m;
}

// @safe
// @lifetime: (&'a, std::unordered_set<T>&) -> &'a
template<class T>
inline rrr::Marshal &operator>>(rrr::Marshal &m, std::unordered_set<T> &v) {
  v64 v_len;
  m >> v_len;
  v.clear();
  for (int i = 0; i < v_len.get(); i++) {
    T elem;
    m >> elem;
    v.insert(elem);
  }
  return m;
}

// @safe
// @lifetime: (&'a, rusty::HashMap<K,V>&) -> &'a
template<class K, class V>
inline rrr::Marshal &operator>>(rrr::Marshal &m, rusty::HashMap<K, V> &v) {
  v64 v_len;
  m >> v_len;
  v.clear();
  for (int i = 0; i < v_len.get(); i++) {
    K key;
    V value;
    m >> key >> value;
    insert_into_map(v, key, value);
  }
  return m;
}

// @safe
// @lifetime: (&'a, std::unordered_map<K,V>&) -> &'a
template<class K, class V>
inline rrr::Marshal &operator>>(rrr::Marshal &m, std::unordered_map<K, V> &v) {
  v64 v_len;
  m >> v_len;
  v.clear();
  for (int i = 0; i < v_len.get(); i++) {
    K key;
    V value;
    m >> key >> value;
    insert_into_map(v, key, value);
  }
  return m;
}

// 2 step 5 (2026-05-05): Marshal& operators for MarshallDeputy
// retired with the class.  janus::Command (SerializableEnvelope<
// MakoCommands>) has its own Marshal& archive operators in
// serializable_envelope.hpp.


}  // export namespace rrr

// ============================================================================
// Implementation
// ============================================================================
// Marshal is fully header-emitted now — all methods are inline in the class
// definition above. The chunk-list out-of-class definitions (~Marshal,
// content_size_slow, write, read, read_chnk, read_reuse_chnk,
// read_from_marshal, set_bookmark) are gone; their Vec<uint8_t>-backed
// replacements are inline above. No translation-unit-local state remains.
//
// @safe - impl namespace placeholder. Retained as a no-op so module
// consumers' expectations about `namespace rrr` being closed in this
// TU are preserved.
namespace rrr {
} // namespace rrr
