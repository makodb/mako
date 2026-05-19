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
#include <rusty/rc.hpp>
#include <rusty/rusty.hpp>

export module rrr.marshal;

import std;
import rrr.basetypes;
import rrr.debugging;
import rrr.misc;
import rrr.serializable;
import rrr.threading;

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
// 11 confirmed `Marshal::read_from_fd` /
// `Marshal::chnk_read_from_fd` / `chunk::read_from_fd` had no
// production callers anywhere in the codebase. The receive path
// uses `FdSource` (`serializable.hpp`) instead.

// not thread safe, for better performance
class Marshal;


class Marshal: public NoCopy {
private:
  // Migrated from RefCounted to std::shared_ptr for automatic reference counting
  // removed `marshallable_entity`,
  // `shared_data`, `written_to_socket` fields and the
  // `raw_bytes(MarshallDeputy, sz)` ctor — they backed the dead
  // bypass-to-socket fast path.
  struct raw_bytes {
    char *ptr = nullptr;
    size_t size = 0;
    static const size_t min_size;

    raw_bytes(size_t sz = min_size) {
      size = std::max(sz, min_size);
      ptr = new char[size];
    }
    raw_bytes(const void *p, size_t n) {
      size = std::max(n, min_size);
      ptr = new char[size];
      memcpy(ptr, p, n);
    }

    size_t resize_to(size_t new_sz){
      size = safe_min(size, new_sz);
      //char *x = new char[size];
      //memcpy(x, ptr, size);
      //delete[] ptr;
      //ptr = x;
      return size;
    }

    raw_bytes(const raw_bytes &) = delete;
    raw_bytes &operator=(const raw_bytes &) = delete;
    ~raw_bytes() { if(ptr)delete[] ptr; }
  };

  struct chunk: public NoCopy {
   private:

    // Private constructor for shared_copy - takes shared_ptr by value, copies it
    chunk(std::shared_ptr<raw_bytes> dt, size_t rd_idx, size_t wr_idx)
        : data(dt),  // Copy shared_ptr, increments refcount
          read_idx(rd_idx),
          write_idx(wr_idx), next(nullptr) {
      assert(write_idx <= data->size);
      assert(read_idx <= write_idx);
    }

   public:

    std::shared_ptr<raw_bytes> data;  // Migrated from raw_bytes* to shared_ptr
    size_t read_idx;
    size_t write_idx;
    chunk *next;

    // Updated constructors to use std::make_shared instead of new.
    // removed `chunk(MarshallDeputy, sz)`
    // ctor (backed dead bypass-to-socket fast path).
    chunk() : data(std::make_shared<raw_bytes>()),
              read_idx(0), write_idx(0), next(nullptr) { }

    chunk(size_t sz)
        : data(std::make_shared<raw_bytes>(sz)),
          read_idx(0), write_idx(0), next(nullptr) {}

    chunk(const void *p, size_t n)
        : data(std::make_shared<raw_bytes>(p, n)),
          read_idx(0), write_idx(n), next(nullptr) { }
    // Destructor is now default - shared_ptr handles cleanup automatically
    ~chunk() = default;

    // NOTE: This function is only intended for Marshal::read_from_marshal.
    // @unsafe - Creates a new chunk sharing the same data buffer
    chunk *shared_copy() const {
      //if(read_idx != 0 && write_idx != 0) Log_info("read_idx: %d and write_idx: %d", read_idx, write_idx);
      return new chunk(data, read_idx, write_idx);
    }

    size_t resize_to_current() {
      // removed
      // `verify(data->shared_data == false)` — `shared_data` no
      // longer exists on raw_bytes.
      size_t sz = data->resize_to(write_idx);
      verify(data->size == write_idx);
      return sz;
    }

    // @safe - Returns the content size
    size_t content_size() const {
      assert(write_idx <= data->size);
      assert(read_idx <= write_idx);
      return write_idx - read_idx;
    }

    // @unsafe - Returns pointer to heap data, not reference to local
    // SAFETY: Returns pointer into data->ptr array which outlives this function
    char *set_bookmark() {
      assert(write_idx <= data->size);
      assert(read_idx <= write_idx);

      char* result = &data->ptr[write_idx++];

      assert(write_idx <= data->size);
      assert(read_idx <= write_idx);
      return result;
    }

    size_t write(const void *p, size_t n) {
      assert(write_idx <= data->size);
      assert(read_idx <= write_idx);

      size_t n_write = safe_min(n, data->size - write_idx);
      if (n_write > 0) {
        memcpy(data->ptr + write_idx, p, n_write);
      }
      write_idx += n_write;

      assert(write_idx <= data->size);
      assert(read_idx <= write_idx);
      return n_write;
    }

    // @safe - Reads data from chunk buffer
    // SAFETY: Internal @unsafe block handles raw pointer arithmetic and memcpy
    size_t read(void *p, size_t n) {
      assert(write_idx <= data->size);
      assert(read_idx <= write_idx);

      size_t n_read = safe_min(n, write_idx - read_idx);
      // @unsafe - raw pointer arithmetic
      {
        if (n_read > 0) {
          memcpy(p, data->ptr + read_idx, n_read);
        }
      }
      read_idx += n_read;

      assert(write_idx <= data->size);
      assert(read_idx <= write_idx);
      return n_read;
    }

    // removed `is_shared_data_chunk()` —
    // `data->shared_data` no longer exists.

    // @safe - Peeks at data in chunk buffer
    // SAFETY: Internal @unsafe block handles raw pointer arithmetic and memcpy
    size_t peek(void *p, size_t n) const {
      assert(write_idx <= data->size);
      assert(read_idx <= write_idx);
      size_t n_peek = safe_min(n, write_idx - read_idx);
      // @unsafe - raw pointer arithmetic
      {
        if (n_peek > 0) {
          memcpy(p, data->ptr + read_idx, n_peek);
        }
      }

      return n_peek;
    }

    size_t discard(size_t n) {
      assert(write_idx <= data->size);
      assert(read_idx <= write_idx);

      size_t n_discard = safe_min(n, write_idx - read_idx);
      read_idx += n_discard;

      assert(write_idx <= data->size);
      assert(read_idx <= write_idx);
      return n_discard;
    }

    // removed `chunk::write_to_fd(int)` —
    // its only caller was `Marshal::write_to_fd(int)` which went
    // away in the same commit (no production callers).

    // removed `chunk::read_from_fd(int,
    // size_t)`. Its only callers were `Marshal::read_from_fd` and
    // `Marshal::chnk_read_from_fd` — both of which were unreferenced
    // by any production caller and went away in the same commit.
    // The receive path uses `FdSource` (`serializable.hpp`) for
    // direct fd reads.

    // check if it is not possible to write to the chunk anymore.
    bool fully_written() const {
      assert(write_idx <= data->size);
      assert(read_idx <= write_idx);
      return write_idx == data->size;
    }

    // check if it is not possible to read any data even if retry later
    bool fully_read() const {
      assert(write_idx <= data->size);
      assert(read_idx <= write_idx);
      //Log_info("fully read %d %d", read_idx, data->size);
      return read_idx == data->size;
    }

    void reset() {
      read_idx = write_idx = 0;
    }
  };

  chunk *head_;
  chunk *tail_;
  i32 write_cnt_;
  size_t content_size_;

  // for debugging purpose
  size_t content_size_slow() const;

 public:

	bool found_dep;
  bool valid_id;

	// @unsafe - Contains raw pointer for deferred writes
	struct bookmark {
    size_t size = 0;
    char **ptr = nullptr;

    // @safe - Default constructor
    bookmark() = default;

    // Non-copyable
    bookmark(const bookmark&) = delete;
    bookmark& operator=(const bookmark&) = delete;

    // @safe - Move constructor transfers ownership
    bookmark(bookmark&& other) noexcept : size(other.size), ptr(other.ptr) {
      other.size = 0;
      other.ptr = nullptr;
    }

    // @unsafe - Move assignment (uses delete[])
    bookmark& operator=(bookmark&& other) noexcept {
      if (this != &other) {
        delete[] ptr;
        size = other.size;
        ptr = other.ptr;
        other.size = 0;
        other.ptr = nullptr;
      }
      return *this;
    }

    // @unsafe - Destructor (uses delete[])
    ~bookmark() {
      delete[] ptr;
    }
  };

  Marshal()
      : head_(nullptr), tail_(nullptr), write_cnt_(0), content_size_(0) { }
  ~Marshal();

  void init_block_read(size_t block_size){
    head_ = tail_ = new chunk(block_size);
  }

  // @safe - Simple empty check
  bool empty() const {
    assert(content_size_ == content_size_slow());
    return content_size_ == 0;
  }
  // @safe - Returns cached content size
  size_t content_size() const {
    assert(content_size_ == content_size_slow());
    return content_size_;
  }

  // @unsafe - Writes data to marshal buffer (uses raw pointer members)
  size_t write(const void *p, size_t n);
  // @safe - Reads data from marshal buffer (raw pointer version, for internal use)
  // SAFETY: Internal @unsafe block handles raw pointer operations
  size_t read(void *p, size_t n);
  // @safe - Reads data into a reference (type-safe version)
  // SAFETY: Internal @unsafe block handles raw pointer operations
  template<typename T>
  size_t read(T& out, size_t n = sizeof(T)) {
    static_assert(std::is_trivially_copyable_v<T>, "read requires trivially copyable type");
    // @unsafe - reinterpret_cast for type-safe wrapper
    {
      return read(reinterpret_cast<void*>(&out), n);
    }
  }
  // @safe - Peeks at data without consuming
  // SAFETY: Internal @unsafe block handles raw pointer operations
  template<typename T>
  size_t peek(T& out, size_t n = sizeof(T)) const {
    static_assert(std::is_trivially_copyable_v<T>, "peek requires trivially copyable type");
    // @unsafe - raw pointer operations
    {
      assert(tail_ == nullptr || tail_->next == nullptr);
      assert(empty() || (head_ != nullptr && !head_->fully_read()));
      char* pc = reinterpret_cast<char*>(&out);
      size_t n_peek = 0;
      chunk* chnk = head_;
      while (chnk != nullptr && n - n_peek > 0) {
        size_t cnt = chnk->peek(pc + n_peek, n - n_peek);
        if (cnt == 0) {
          break;
        }
        n_peek += cnt;
        chnk = chnk->next;
      }
      assert(n_peek <= n);
      assert(tail_ == nullptr || tail_->next == nullptr);
      assert(empty() || (head_ != nullptr && !head_->fully_read()));
      return n_peek;
    }
  }

  // removed `read_from_fd(int)` and
  // `chnk_read_from_fd(int, size_t)`. Neither had any production
  // callers; the receive path uses `FdSource`
  // (`serializable.hpp`) instead.

  // @unsafe - Reuses chunks from another marshal (uses raw pointer members)
  size_t read_reuse_chnk(Marshal& m, size_t nbytes);

  // @unsafe - Reads data into chunk (uses raw pointer members)
  size_t read_chnk(void* p, size_t n);

  // NOTE: This function is only used *internally* to chop a slice of marshal object.
  // Use case 1: In C++ server io thread, when a compelete packet is received, read it off
  //             into a Marshal object and hand over to worker threads.
  // Use case 2: In Python extension, buffer message in Marshal object, and send to network.
  // @safe - Transfers data between Marshal objects
  // SAFETY: Internal @unsafe block wraps raw pointer operations (head_, tail_, chunk*)
  size_t read_from_marshal(Marshal &m, size_t n);

  // removed `write_to_fd(int)`. It had no
  // callers; new code uses `FdSink` (serializable.hpp) to write
  // archive bytes directly to a file descriptor.

  void reset(){
    head_->reset();
    content_size_ = 0;
    write_cnt_ = 0;
  }

  // @safe - Creates bookmark for deferred writes, returns by move
  // SAFETY: Internal @unsafe block handles raw pointer operations
  bookmark set_bookmark(size_t n);

  // @safe - Writes value to bookmark locations
  // SAFETY: Internal @unsafe block handles pointer operations
  template<typename T>
  void write_bookmark(bookmark& bm, const T& value) {
    // @unsafe
    {
      static_assert(sizeof(T) <= sizeof(size_t) * 8, "bookmark value too large");
      const char *pc = reinterpret_cast<const char*>(&value);
      assert(bm.ptr != nullptr);
      for (size_t i = 0; i < bm.size; i++) {
        *(bm.ptr[i]) = pc[i];
      }
    }
  }

  // @safe - Returns and resets write counter
  i32 get_and_reset_write_cnt() {
    i32 cnt = write_cnt_;
    write_cnt_ = 0;
    return cnt;
  }

  // removed `bypass_copying` — the dead
  // bypass-to-socket fast path that no production type ever
  // enabled (no caller set `bypass_to_socket_=true`).
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

class MarshalSink {
  Marshal* m_;
 public:
  explicit MarshalSink(Marshal* m) noexcept : m_(m) {}

  Marshal* marshal() const noexcept { return m_; }

  void write(const void* p, size_t n) {
    size_t actual = m_->write(p, n);
    verify(actual == n);
  }
};

class MarshalSource {
  Marshal* m_;
 public:
  explicit MarshalSource(Marshal* m) noexcept : m_(m) {}

  Marshal* marshal() const noexcept { return m_; }

  size_t read(void* p, size_t n) {
    return m_->read(p, n);
  }
};

class MarshalSinkAdapter : public SinkBase {
  MarshalSink* sink_;
 public:
  explicit MarshalSinkAdapter(MarshalSink* s) noexcept : sink_(s) {}
  void write(const void* p, size_t n) override { sink_->write(p, n); }
  MarshalSink* sink() const noexcept { return sink_; }
};

class MarshalSourceAdapter : public SourceBase {
  MarshalSource* source_;
 public:
  explicit MarshalSourceAdapter(MarshalSource* s) noexcept : source_(s) {}
  size_t read(void* p, size_t n) override { return source_->read(p, n); }
  MarshalSource* source() const noexcept { return source_; }
};

inline SinkProxy make_sink_proxy(MarshalSink* sink) {
  return rusty::make_box<MarshalSinkAdapter>(sink);
}
inline SourceProxy make_source_proxy(MarshalSource* source) {
  return rusty::make_box<MarshalSourceAdapter>(source);
}

// @unsafe
// @lifetime: (&'a, const i8&) -> &'a
inline rrr::Marshal &operator<<(rrr::Marshal &m, const rrr::i8 &v) {
  verify(m.write(&v, sizeof(v)) == sizeof(v));
  return m;
}

// @unsafe
// @lifetime: (&'a, const i16&) -> &'a
inline rrr::Marshal &operator<<(rrr::Marshal &m, const rrr::i16 &v) {
  verify(m.write(&v, sizeof(v)) == sizeof(v));
  return m;
}

// @unsafe
// @lifetime: (&'a, const i32&) -> &'a
inline rrr::Marshal &operator<<(rrr::Marshal &m, const rrr::i32 &v) {
  verify(m.write(&v, sizeof(v)) == sizeof(v));
  return m;
}

// @unsafe
// @lifetime: (&'a, const i64&) -> &'a
inline rrr::Marshal &operator<<(rrr::Marshal &m, const rrr::i64 &v) {
  //Log_info("The sizeof v is: %d", sizeof(v));
  //auto start = std::chrono::steady_clock::now();
  verify(m.write(&v, sizeof(v)) == sizeof(v));
  //auto end = std::chrono::steady_clock::now();
  //auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end-start).count();
  //Log_info("Time of << for int64 is: %d", duration);
	
	if (m.found_dep) {
		if (v != -1) {
			//Log_info("valid id: %d and %d", m.found_dep, v);
			m.valid_id = true;
		} else {
			//Log_info("invalid id: %d and %d", m.found_dep, v);
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

// @unsafe
// @lifetime: (&'a, const uint8_t&) -> &'a
inline rrr::Marshal &operator<<(rrr::Marshal &m, const uint8_t &u) {
  verify(m.write(&u, sizeof(u)) == sizeof(u));
  return m;
}

// @unsafe
// @lifetime: (&'a, const uint16_t&) -> &'a
inline rrr::Marshal &operator<<(rrr::Marshal &m, const uint16_t &u) {
  verify(m.write(&u, sizeof(u)) == sizeof(u));
  return m;
}

// @unsafe
// @lifetime: (&'a, const uint32_t&) -> &'a
inline rrr::Marshal &operator<<(rrr::Marshal &m, const uint32_t &u) {
  verify(m.write(&u, sizeof(u)) == sizeof(u));
  return m;
}

// @unsafe
// @lifetime: (&'a, const uint64_t&) -> &'a
inline rrr::Marshal &operator<<(rrr::Marshal &m, const uint64_t &u) {
  //Log_info("The sizeof u is: %d", sizeof(u));
  //auto start = std::chrono::steady_clock::now();
  verify(m.write(&u, sizeof(u)) == sizeof(u));
  //auto end = std::chrono::steady_clock::now();
  //auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end-start).count();
  //Log_info("Time of << for uint64 is: %d", duration);
  
  return m;
}

// @unsafe
// @lifetime: (&'a, const double&) -> &'a
inline rrr::Marshal &operator<<(rrr::Marshal &m, const double &v) {
  verify(m.write(&v, sizeof(v)) == sizeof(v));
  return m;
}

// SAFETY: Writes string data safely with bounds checking
// @unsafe
// @lifetime: (&'a, const std::string&) -> &'a
inline rrr::Marshal &operator<<(rrr::Marshal &m, const std::string &v) {
  v64 v_len = v.length();
  m << v_len;
  if (v_len.get() > 0) {
    verify(m.write(v.c_str(), v_len.get()) == (size_t) v_len.get());
  }

	if (v == "dep") {
		// Log_info("dep: %s", v.c_str());
		m.found_dep = true;
	} else if (v == "hb") { 
		m.valid_id = true;
	} else {
    m.valid_id = true;
		// Log_info("not dep: %s", v.c_str());
	}

  return m;
}

// @unsafe
// @lifetime: (&'a, const T1&, const T2&) -> &'a
template<class T1, class T2>
inline rrr::Marshal &operator<<(rrr::Marshal &m, const std::pair<T1, T2> &v) {
  // @unsafe {
    m << v.first;
    m << v.second;
    return m;
  // }
}

// @unsafe
// @lifetime: (&'a, const rusty::Vec<T>&) -> &'a
template<class T>
inline rrr::Marshal &operator<<(rrr::Marshal &m, const rusty::Vec<T> &v) {
  // @unsafe {
    v64 v_len = v.size();
    m << v_len;
    for (typename rusty::Vec<T>::const_iterator it = v.begin(); it != v.end();
         ++it) {
      m << *it;
    }
    return m;
  // }
}

// @unsafe
// @lifetime: (&'a, const std::vector<T>&) -> &'a
template<class T>
inline rrr::Marshal &operator<<(rrr::Marshal &m, const std::vector<T> &v) {
  // Keep std::vector support for non-rrr call sites while rrr internals move to rusty containers.
  v64 v_len = v.size();
  m << v_len;
  for (typename std::vector<T>::const_iterator it = v.begin(); it != v.end();
       ++it) {
    m << *it;
  }
  return m;
}

// @unsafe
// @lifetime: (&'a, const std::list<T>&) -> &'a
template<class T>
inline rrr::Marshal &operator<<(rrr::Marshal &m, const std::list<T> &v) {
  // @unsafe {
    v64 v_len = v.size();
    m << v_len;
    for (typename std::list<T>::const_iterator it = v.begin(); it != v.end();
         ++it) {
      m << *it;
    }
    return m;
  // }
}

// @unsafe
// @lifetime: (&'a, const rusty::BTreeSet<T>&) -> &'a
template<class T>
inline rrr::Marshal &operator<<(rrr::Marshal &m, const rusty::BTreeSet<T> &v) {
  // @unsafe {
    v64 v_len = v.size();
    m << v_len;
    for (typename rusty::BTreeSet<T>::const_iterator it = v.begin(); it != v.end();
         ++it) {
      m << *it;
    }
    return m;
  // }
}

// @unsafe
// @lifetime: (&'a, const std::set<T>&) -> &'a
template<class T>
inline rrr::Marshal &operator<<(rrr::Marshal &m, const std::set<T> &v) {
  v64 v_len = v.size();
  m << v_len;
  for (typename std::set<T>::const_iterator it = v.begin(); it != v.end();
       ++it) {
    m << *it;
  }
  return m;
}

// @unsafe
// @lifetime: (&'a, const rusty::BTreeMap<K,V>&) -> &'a
template<class K, class V>
inline rrr::Marshal &operator<<(rrr::Marshal &m, const rusty::BTreeMap<K, V> &v) {
  // @unsafe {
    v64 v_len = v.size();
    m << v_len;
    // rusty::BTreeMap iter `operator*()` returns
    // `std::tuple<const K&, const V&>` (post-2026-04 API).
    for (typename rusty::BTreeMap<K, V>::const_iterator it = v.begin(); it != v.end();
         ++it) {
      auto kv = *it;
      m << std::get<0>(kv) << std::get<1>(kv);
    }
    return m;
  // }
}

// @unsafe
// @lifetime: (&'a, const std::map<K,V>&) -> &'a
template<class K, class V>
inline rrr::Marshal &operator<<(rrr::Marshal &m, const std::map<K, V> &v) {
  v64 v_len = v.size();
  m << v_len;
  for (typename std::map<K, V>::const_iterator it = v.begin(); it != v.end();
       ++it) {
    m << it->first << it->second;
  }
  return m;
}

// @unsafe
// @lifetime: (&'a, const rusty::HashSet<T>&) -> &'a
template<class T>
inline rrr::Marshal &operator<<(rrr::Marshal &m,
                                const rusty::HashSet<T> &v) {
  // @unsafe {
    v64 v_len = v.size();
    m << v_len;
    for (typename rusty::HashSet<T>::const_iterator it = v.begin();
         it != v.end(); ++it) {
      m << *it;
    }
    return m;
  // }
}

// @unsafe
// @lifetime: (&'a, const std::unordered_set<T>&) -> &'a
template<class T>
inline rrr::Marshal &operator<<(rrr::Marshal &m,
                                const std::unordered_set<T> &v) {
  v64 v_len = v.size();
  m << v_len;
  for (typename std::unordered_set<T>::const_iterator it = v.begin();
       it != v.end(); ++it) {
    m << *it;
  }
  return m;
}

// @unsafe
// @lifetime: (&'a, const rusty::HashMap<K,V>&) -> &'a
template<class K, class V>
inline rrr::Marshal &operator<<(rrr::Marshal &m,
                                const rusty::HashMap<K, V> &v) {
  // @unsafe {
    v64 v_len = v.size();
    m << v_len;
    // rusty::HashMap iter `operator*()` returns
    // `std::tuple<const K&, const V&>` (post-2026-04 API).
    for (typename rusty::HashMap<K, V>::const_iterator it = v.begin();
         it != v.end(); ++it) {
      auto kv = *it;
      m << std::get<0>(kv) << std::get<1>(kv);
    }
    return m;
  // }
}

// @unsafe
// @lifetime: (&'a, const std::unordered_map<K,V>&) -> &'a
template<class K, class V>
inline rrr::Marshal &operator<<(rrr::Marshal &m,
                                const std::unordered_map<K, V> &v) {
  v64 v_len = v.size();
  m << v_len;
  for (typename std::unordered_map<K, V>::const_iterator it = v.begin();
       it != v.end(); ++it) {
    m << it->first << it->second;
  }
  return m;
}

// @unsafe
// @lifetime: (&'a, i8&) -> &'a
inline rrr::Marshal &operator>>(rrr::Marshal &m, rrr::i8 &v) {
  verify(m.read(&v, sizeof(v)) == sizeof(v));
  return m;
}

// @unsafe
// @lifetime: (&'a, i16&) -> &'a
inline rrr::Marshal &operator>>(rrr::Marshal &m, rrr::i16 &v) {
  verify(m.read(&v, sizeof(v)) == sizeof(v));
  return m;
}

// @unsafe
// @lifetime: (&'a, i32&) -> &'a
inline rrr::Marshal &operator>>(rrr::Marshal &m, rrr::i32 &v) {
  verify(m.read(&v, sizeof(v)) == sizeof(v));
  return m;
}

// @unsafe
// @lifetime: (&'a, i64&) -> &'a
inline rrr::Marshal &operator>>(rrr::Marshal &m, rrr::i64 &v) {
  verify(m.read(&v, sizeof(v)) == sizeof(v));
	/*if (m.found_dep) {
		if (v != -1) {
			Log_info("valid id: %d", v);
			m.valid_id = true;
		} else {
			Log_info("invalid id: %d", v);
			m.valid_id = false;
		}
		m.found_dep = false;
	}*/
  return m;
}

// @unsafe
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

// @unsafe
// @lifetime: (&'a, v64&) -> &'a
inline rrr::Marshal &operator>>(rrr::Marshal &m, rrr::v64 &v) {
  char byte0;
  //Log_info("peeking data of %d", m.peek(byte0, 1));
  verify(m.peek(byte0, 1) == 1);
  size_t bsize = rrr::SparseInt::buf_size(byte0);
  char buf[9];
  verify(m.read(buf, bsize) == bsize);
  i64 val = rrr::SparseInt::load_i64(buf);
  v.set(val);
  return m;
}

// @unsafe
// @lifetime: (&'a, uint8_t&) -> &'a
inline rrr::Marshal &operator>>(rrr::Marshal &m, uint8_t &u) {
  verify(m.read(&u, sizeof(u)) == sizeof(u));
  return m;
}

// @unsafe
// @lifetime: (&'a, uint16_t&) -> &'a
inline rrr::Marshal &operator>>(rrr::Marshal &m, uint16_t &u) {
  verify(m.read(&u, sizeof(u)) == sizeof(u));
  return m;
}

// @unsafe
// @lifetime: (&'a, uint32_t&) -> &'a
inline rrr::Marshal &operator>>(rrr::Marshal &m, uint32_t &u) {
  verify(m.read(&u, sizeof(u)) == sizeof(u));
  return m;
}

// @unsafe
// @lifetime: (&'a, uint64_t&) -> &'a
inline rrr::Marshal &operator>>(rrr::Marshal &m, uint64_t &u) {
  verify(m.read(&u, sizeof(u)) == sizeof(u));
  return m;
}

// @unsafe
// @lifetime: (&'a, double&) -> &'a
inline rrr::Marshal &operator>>(rrr::Marshal &m, double &v) {
  verify(m.read(&v, sizeof(v)) == sizeof(v));
  return m;
}

// @unsafe
// @lifetime: (&'a, std::string&) -> &'a
inline rrr::Marshal &operator>>(rrr::Marshal &m, std::string &v) {
  v64 v_len;
  m >> v_len;
  v.resize(v_len.get());
  if (v_len.get() > 0) {
    verify(m.read(&v[0], v_len.get()) == (size_t) v_len.get());
  }
	/*if (v == "dep") {
		Log_info("dep: %s", v.c_str());
		m.found_dep = true;
	} else {
		Log_info("not dep: %s", v.c_str());
	}*/
  return m;
}

// @unsafe
// @lifetime: (&'a, std::pair<T1,T2>&) -> &'a
template<class T1, class T2>
inline rrr::Marshal &operator>>(rrr::Marshal &m, std::pair<T1, T2> &v) {
  m >> v.first;
  m >> v.second;
  return m;
}

// @unsafe
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

// @unsafe
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

// @unsafe
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

// @unsafe
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

// @unsafe
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

// @unsafe
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

// @unsafe
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

// @unsafe
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

// @unsafe
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

// @unsafe
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

// @unsafe
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
// Implementation (formerly marshal.cpp's body)
// ============================================================================
namespace rrr {


// retired the entire `#ifdef RPC_STATISTICS`
// block.  Phase 5b-8 deleted the marshal-out side; Phase 5b-11 deleted
// the only remaining caller of `stat_marshal_in` (`chunk::read_from_fd`)
// along with `Marshal::read_from_fd` / `Marshal::chnk_read_from_fd`.
// The receive path uses `FdSource` (`serializable.hpp`) instead, so
// the histogram-bucket I/O accounting (`g_marshal_in_stat[12]`,
// `g_marshal_in_stat_cumulative[12]`, `stat_marshal_report`,
// `g_marshal_stat_report_time` / `g_marshal_stat_report_interval`,
// `stat_marshal_in`) had no live producers.

/**
 * 8kb minimum chunk size.
 * NOTE: this value directly affects how many read/write syscall will be issued.
 */
const size_t Marshal::raw_bytes::min_size = 8192;

Marshal::~Marshal() {
    chunk* chnk = head_;
    while (chnk != nullptr) {
	//Log_info("wkwkakakak");
        chunk* next = chnk->next;
        delete chnk;
        chnk = next;
    }
}

size_t Marshal::content_size_slow() const {
    assert(tail_ == nullptr || tail_->next == nullptr);

    size_t sz = 0;
    chunk* chnk = head_;
    while (chnk != nullptr) {
	//Log_info("wkwkakakak");
        sz += chnk->content_size();
        chnk = chnk->next;
    }
    return sz;
}

size_t Marshal::write(const void* p, size_t n) {
    assert(tail_ == nullptr || tail_->next == nullptr);
    std::chrono::time_point<std::chrono::steady_clock> start;
    if (head_ == nullptr) {
        assert(tail_ == nullptr);
        head_ = new chunk(p, n);
        tail_ = head_;
    } else if (tail_->fully_written()) {
        tail_->next = new chunk(p, n);
        tail_ = tail_->next;
    } else {
        //if(timing) start = chrono::steady_clock::now();
        size_t n_write = tail_->write(p, n);
        /*if(timing){
	    auto end =  chrono::steady_clock::now();
	    auto duration = chrono::duration_cast<chrono::microseconds>(end-start).count();
	    Log_info("Duration of this tail write is: %d", duration);
	}*/
        // otherwise the above fully_written() should have returned true
        assert(n_write > 0);

        if (n_write < n) {
	    //Log_info("Less less less");
            const char* pc = (const char *) p;
	    //if(timing) start = chrono::steady_clock::now();
            tail_->next = new chunk(pc + n_write, n - n_write);
            /*if(timing){
	        auto end = chrono::steady_clock::now();
		auto duration = chrono::duration_cast<chrono::microseconds>(end-start).count();
		Log_info("Duration of Less less less is: %d", duration);
	    }*/
            tail_ = tail_->next;
        }
	
    }
    write_cnt_ += n;
    content_size_ += n;
    //assert(content_size_ == content_size_slow());

    return n;
}

// removed `Marshal::bypass_copying`. It
// was the implementation of the dead bypass-to-socket fast path —
// no production type ever set `bypass_to_socket_=true`, so the
// `if (rhs.bypass_to_socket_)` branch in `operator<<(MarshallDeputy)`
// (now also gone) never invoked it.

size_t Marshal::read_chnk(void* p, size_t n){
    char* pc = (char *) p;
    size_t n_read = head_->read(pc, n);
    content_size_ -= n_read;
    return n_read;
}

// @safe - Reads data from marshal buffer
// SAFETY: Internal @unsafe block handles raw pointer casting and arithmetic
size_t Marshal::read(void* p, size_t n) {
    assert(tail_ == nullptr || tail_->next == nullptr);
    assert(empty() || (head_ != nullptr && !head_->fully_read()));

    // @unsafe - raw pointer casting and arithmetic
    {
        char* pc = (char *) p;
        size_t n_read = 0;
        while (n_read < n && head_ != nullptr && head_->content_size() > 0) {
            size_t cnt = head_->read(pc + n_read, n - n_read);
            if (head_->fully_read()) {
                if (tail_ == head_) {
                    // deleted the only chunk
                    tail_ = nullptr;
                }
                chunk* chnk = head_;
                head_ = head_->next;
                //delete chnk;
            }
            if (cnt == 0) {
                // currently there's no data available, so stop
                break;
            }
            n_read += cnt;
        }
        assert(content_size_ >= n_read);
        content_size_ -= n_read;
        assert(content_size_ == content_size_slow());

        assert(n_read <= n);
        assert(tail_ == nullptr || tail_->next == nullptr);
        assert(empty() || (head_ != nullptr && !head_->fully_read()));

        return n_read;
    }
}

// removed `Marshal::read_from_fd(int)` and
// `Marshal::chnk_read_from_fd(int, size_t)`. Neither had any
// production callers in the codebase; the receive path uses
// `FdSource` (`serializable.hpp`) for direct fd reads. The
// inner `chunk::read_from_fd` they used was deleted in the same
// commit.

size_t Marshal::read_reuse_chnk(Marshal& m, size_t n){
    assert(m.content_size() >= n);   // require m.content_size() >= n > 0
    size_t n_fetch = 0;

    while (n_fetch < n) {
        // NOTE: The copied chunk is shared by 2 Marshal objects. Be careful
        //       that only one Marshal should be able to write to it! For the
        //       given 2 use cases, it works.
        // @unsafe
        chunk* chnk = m.head_->shared_copy();
        if (n_fetch + chnk->content_size() > n) {
            // only fetch enough bytes we need
            chnk->write_idx -= (n_fetch + chnk->content_size()) - n;
        }
        size_t cnt = chnk->content_size();
        assert(cnt > 0);
        n_fetch += cnt;
        verify(m.head_->discard(cnt) == cnt);
        if (head_ == nullptr) {
            head_ = tail_ = chnk;
        } else {
            tail_->next = chnk;
            tail_ = chnk;
        }
    }

    write_cnt_ += n_fetch;
    content_size_ += n_fetch;
    verify(m.content_size_ >= n_fetch);
    m.content_size_ -= n_fetch;
    return n_fetch;
}

// @safe - Transfers data between Marshal objects
// SAFETY: Internal @unsafe block wraps raw pointer operations
size_t Marshal::read_from_marshal(Marshal& m, size_t n) {
    assert(m.content_size() >= n);   // require m.content_size() >= n > 0
    size_t n_fetch = 0;

    // @unsafe - Raw pointer operations (head_, tail_, chunk*)
    {
        if ((head_ == nullptr && tail_ == nullptr) || tail_->fully_written()) {
            // efficiently copy data by only copying pointers
            while (n_fetch < n) {
                // NOTE: The copied chunk is shared by 2 Marshal objects. Be careful
                //       that only one Marshal should be able to write to it! For the
                //       given 2 use cases, it works.
                chunk* chnk = m.head_->shared_copy();
                if (n_fetch + chnk->content_size() > n) {
                    // only fetch enough bytes we need
                    chnk->write_idx -= (n_fetch + chnk->content_size()) - n;
                }
                size_t cnt = chnk->content_size();
                assert(cnt > 0);
                n_fetch += cnt;
                verify(m.head_->discard(cnt) == cnt);
                if (head_ == nullptr) {
                    head_ = tail_ = chnk;
                } else {
                    tail_->next = chnk;
                    tail_ = chnk;
                }
                if (m.head_->fully_read()) {
                    if (m.tail_ == m.head_) {
                        // deleted the only chunk
                        m.tail_ = nullptr;
                    }
                    chunk* next = m.head_->next;
                    delete m.head_;
                    m.head_ = next;
                }
            }
            write_cnt_ += n_fetch;
            content_size_ += n_fetch;
            verify(m.content_size_ >= n_fetch);
            m.content_size_ -= n_fetch;

        } else {

            // number of bytes that need to be copied
            size_t copy_n = safe_min(tail_->data->size - tail_->write_idx, n);
            char* buf = new char[copy_n];
            n_fetch = m.read(buf, copy_n);
            verify(n_fetch == copy_n);
            verify(this->write(buf, n_fetch) == n_fetch);
            delete[] buf;

            size_t leftover = n - copy_n;
            if (leftover > 0) {
                verify(tail_->fully_written());
                n_fetch += this->read_from_marshal(m, leftover);
            }
        }
        assert(n_fetch == n);
        assert(content_size_ == content_size_slow());
    }
    return n_fetch;
}


// removed `Marshal::write_to_fd(int)`. It
// had no callers anywhere in the codebase. New code uses `FdSink`
// (see `serializable.hpp`) to write archive bytes directly to a
// file descriptor without going through the legacy chunk-list
// representation.

// @unsafe - Creates bookmark for deferred writes
// SAFETY: Uses verify/new/delete and raw pointer operations
Marshal::bookmark Marshal::set_bookmark(size_t n) {
    verify(write_cnt_ == 0);

    // @unsafe
    {
        bookmark bm;
        bm.size = n;
        bm.ptr = new char*[n];
        for (size_t i = 0; i < n; i++) {
            if (head_ == nullptr) {
                head_ = new chunk;
                tail_ = head_;
            } else if (tail_->fully_written()) {
                // dropped
                // `|| tail_->is_shared_data_chunk()` — `shared_data`
                // chunks no longer exist (dead bypass-to-socket
                // fast path removed).
                tail_->next = new chunk;
                tail_ = tail_->next;
            }
            bm.ptr[i] = tail_->set_bookmark();
        }
        content_size_ += n;
        assert(content_size_ == content_size_slow());

        return bm;  // Moved out (NRVO)
    }
}



}  // namespace rrr
