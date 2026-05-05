#pragma once

// import std; replacement — see <std_compat.hpp> for rationale.
#include <std_compat.hpp>

// @c-compat-added
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <rusty/rusty.hpp>

#include <inttypes.h>
#include <string.h>
#include <unistd.h>

#include <rusty/arc.hpp>
#include <rusty/fn.hpp>

#ifdef RR
#pragma push_macro("RR")
#undef RR
#define RRR_RESTORE_RR_MACRO 1
#endif
#include <proxy/proxy.h>
#include <proxy/proxy_macros.h>
#ifdef RRR_RESTORE_RR_MACRO
#pragma pop_macro("RR")
#undef RRR_RESTORE_RR_MACRO
#endif

// External safety annotations for pure functions
// @external: {
//   std::min: [safe]
// }




#include "../base/all.hpp"

// Workstream N Phase 4d-prep: pull in `serializable.hpp` for
// `SerializableProxy` / `SerializableFacade` definitions used by
// `MarshallDeputy(shared_ptr<T>)` and `set_marshallable<T>` — the
// templates dispatch transparently to the `wrap_typed_marshallable`
// bridge overload (declared below) for migrated Serializable types
// (any non-Marshallable T) — call sites need no updates.
//
// L6-pivot (2026-05-01): the `SerializableConcept<T>` constraint was
// dropped from the bridge overloads here; templates now dispatch on
// `!std::is_base_of_v<Marshallable, T>` alone and trust the proxy
// library to reject wrong-shaped T at instantiation / runtime.  See
// `serializable.hpp` for the retirement rationale.
//
// `serializable.hpp` forward-declares `class Marshal` rather than
// including this file, so this is acyclic. The archive's MarshalSink/
// MarshalSource use `Marshal*` only; method bodies live in the .cpp.
#include "serializable.hpp"


namespace rrr {

// @safe - Wrapper for std::min (pure function, no side effects)
template<typename T>
inline T safe_min(const T& a, const T& b) {
  // @unsafe
  { return std::min(a, b); }
}

// Workstream N Phase 5b-11: removed the entire `RPC_STATISTICS` block
// and `stat_marshal_in` declaration. After Phase 5b-7/5b-8 deleted
// the marshal-out side, the marshal-in side became dead too once
// Phase 5b-11 confirmed `Marshal::read_from_fd` /
// `Marshal::chnk_read_from_fd` / `chunk::read_from_fd` had no
// production callers anywhere in the codebase. The receive path
// uses `FdSource` (`serializable.hpp`) instead.

// not thread safe, for better performance
class Marshal;


class Marshal: public NoCopy {
private:
  // Migrated from RefCounted to std::shared_ptr for automatic reference counting
  // Workstream N Phase 5b-3: removed `marshallable_entity`,
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
    // Workstream N Phase 5b-3: removed `chunk(MarshallDeputy, sz)`
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
      // Workstream N Phase 5b-3: removed
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

    // Workstream N Phase 5b-3: removed `is_shared_data_chunk()` —
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

    // Workstream N Phase 5b-7: removed `chunk::write_to_fd(int)` —
    // its only caller was `Marshal::write_to_fd(int)` which went
    // away in the same commit (no production callers).

    // Workstream N Phase 5b-11: removed `chunk::read_from_fd(int,
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

  // Workstream N Phase 5b-11: removed `read_from_fd(int)` and
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

  // Workstream N Phase 5b-7: removed `write_to_fd(int)`. It had no
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

  // Workstream N Phase 5b-3: removed `bypass_copying` — the dead
  // bypass-to-socket fast path that no production type ever
  // enabled (no caller set `bypass_to_socket_=true`).
};

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
    // L9: rusty::BTreeMap iter `operator*()` returns
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
    // L9: rusty::HashMap iter `operator*()` returns
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

// L10f-2 step 5 (2026-05-05): Marshal& operators for MarshallDeputy
// retired with the class.  janus::Command (SerializableEnvelope<
// MakoCommands>) has its own Marshal& archive operators in
// serializable_envelope.hpp.

} // namespace rrr
