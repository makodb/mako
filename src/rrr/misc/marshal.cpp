
// import std; replacement — see <std_compat.hpp> for rationale.
#include <std_compat.hpp>

// @c-compat-added
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <inttypes.h>
#include <string.h>
#include <unistd.h>
#include <rusty/arc.hpp>
#include <proxy/proxy.h>
#include <proxy/proxy_macros.h>

#include "../base/threading.hpp"  // rrr::SpinMutex


#include <sys/time.h>
#include <rusty/rc.hpp>





#include "marshal.hpp"


#include "../rrr.hpp"

// External safety annotations for atomic operations
// @external: {
//   std::__atomic_base::load: [unsafe]
//   std::__atomic_base::store: [unsafe]
//   std::__atomic_base::fetch_add: [unsafe]
//   std::__atomic_base::fetch_sub: [unsafe]
// }


using namespace std;

namespace rrr {

#ifdef RPC_STATISTICS

// -1, 0~15, 16~31, 32~63, 64~127, 128~255, 256~511, 512~1023, 1024~2047, 2048~4095, 4096~8191, 8192~
//
// Workstream N Phase 5b-8: removed `g_marshal_out_stat` /
// `g_marshal_out_stat_cumulative` arrays + `stat_marshal_out`
// function + the marshal-out report blocks. The chunk::write_to_fd
// that fed them was deleted in Phase 5b-7. The marshal-in side
// remains wired to `chunk::read_from_fd` on the receive path.
static Counter g_marshal_in_stat[12];
static Counter g_marshal_in_stat_cumulative[12];
static uint64_t g_marshal_stat_report_time = 0;
static const uint64_t g_marshal_stat_report_interval = 1000 * 1000 * 1000;

static void stat_marshal_report() {
    Log::info("* MARSHAL:     -1 0~15 16~31 32~63 64~127 128~255 256~511 512~1023 1024~2047 2048~4095 4096~8191 8192~");
    {
        ostringstream ostr;
        for (size_t i = 0; i < arraysize(g_marshal_in_stat); i++) {
            i64 v = g_marshal_in_stat[i].peek_next();
            g_marshal_in_stat_cumulative[i].next(v);
            ostr << " " << v;
            g_marshal_in_stat[i].reset();
        }
        Log::info("* MARSHAL IN: %s", ostr.str().c_str());
    }
    {
        ostringstream ostr;
        for (size_t i = 0; i < arraysize(g_marshal_in_stat); i++) {
            ostr << " " << g_marshal_in_stat_cumulative[i].peek_next();
        }
        Log::info("* MARSHAL IN (cumulative): %s", ostr.str().c_str());
    }
}

void stat_marshal_in(int fd, const void* buf, size_t nbytes, ssize_t ret) {
    if (ret == -1) {
        g_marshal_in_stat[0].next();
    } else if (ret < 16) {
        g_marshal_in_stat[1].next();
    } else if (ret < 32) {
        g_marshal_in_stat[2].next();
    } else if (ret < 64) {
        g_marshal_in_stat[3].next();
    } else if (ret < 128) {
        g_marshal_in_stat[4].next();
    } else if (ret < 256) {
        g_marshal_in_stat[5].next();
    } else if (ret < 512) {
        g_marshal_in_stat[6].next();
    } else if (ret < 1024) {
        g_marshal_in_stat[7].next();
    } else if (ret < 2048) {
        g_marshal_in_stat[8].next();
    } else if (ret < 4096) {
        g_marshal_in_stat[9].next();
    } else if (ret < 8192) {
        g_marshal_in_stat[10].next();
    } else {
        g_marshal_in_stat[11].next();
    }

    uint64_t now = base::rdtsc();
    if (now - g_marshal_stat_report_time > g_marshal_stat_report_interval) {
        stat_marshal_report();
        g_marshal_stat_report_time = now;
    }
}

#endif // RPC_STATISTICS

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
    chrono::time_point<chrono::steady_clock> start;
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

// Workstream N Phase 5b-3: removed `Marshal::bypass_copying`. It
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

// @safe - Reads from file descriptor (I/O system call)
// SAFETY: Internal @unsafe block handles I/O and raw pointer operations
size_t Marshal::read_from_fd(int fd) {
    // @unsafe - I/O system calls and raw pointer operations
    {
        assert(empty() || (head_ != nullptr && !head_->fully_read()));

        size_t n_bytes = 0;
        for (;;) {
            if (head_ == nullptr) {
                head_ = new chunk;
                tail_ = head_;
            } else if (tail_->fully_written()) {
                tail_->next = new chunk;
                tail_ = tail_->next;
            }
            int r = tail_->read_from_fd(fd);
            if (r <= 0) {
                break;
            }
            n_bytes += r;
        }
        write_cnt_ += n_bytes;
        content_size_ += n_bytes;
        assert(content_size_ == content_size_slow());

        assert(empty() || (head_ != nullptr && !head_->fully_read()));
        return n_bytes;
    }
}

// the marshal object should have a chunk allocated with necessary size
size_t Marshal::chnk_read_from_fd(int fd, size_t bytes){
    size_t read_bytes = 0;
    read_bytes += head_->read_from_fd(fd, bytes);
    content_size_ += read_bytes;
    write_cnt_ += read_bytes;
    if(read_bytes <= 0)return 0;
    return read_bytes;
}

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


// Workstream N Phase 5b-7: removed `Marshal::write_to_fd(int)`. It
// had no callers anywhere in the codebase. New code uses `FdSink`
// (see `marshal_archive.hpp`) to write archive bytes directly to a
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
                // Workstream N Phase 5b-3: dropped
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

// @safe - Thread-local factory registry copy
// SAFETY: Each thread has its own copy, no locking needed for access
thread_local MarshallDeputy::MarContainer mc_th_;
thread_local bool mc_th_initialized_ = false;
std::atomic<uint64_t> mc_version_g{0};
thread_local uint64_t mc_th_version_ = 0;

namespace {
// SpinMutex-owned global factory registry. Replaces the prior
// `std::mutex md_mutex_g` + `static MarContainer mc_` pair with a
// single rusty-style "data inside the mutex" container. Construct
// On First Use idiom avoids static initialization order fiasco.
//
// The unused `mdi_mutex_g` (declared but never locked) was retired
// alongside the migration.
rrr::SpinMutex<MarshallDeputy::MarContainer>& md_registry_locked() {
    static rrr::SpinMutex<MarshallDeputy::MarContainer> registry;
    return registry;
}
} // namespace

// @unsafe - Registers initializer with SpinMutex locking and map insertion
int MarshallDeputy::reg_initializer(int32_t cmd_type,
                                   MarInitializerFn init) {
  {
    auto guard = md_registry_locked().lock().unwrap();
    verify(!guard->contains_key(cmd_type));
    guard->insert(cmd_type, init);
  }
  // Bump version after releasing the lock so concurrent
  // get_initializer readers see the new contents on next refresh.
  mc_version_g.fetch_add(1, std::memory_order_release);
  return 0;
}

// @unsafe - Calls rrr::SpinMutex::lock, rusty::HashMap::get, std::function constructor
MarshallDeputy::MarInitializerFn
MarshallDeputy::get_initializer(int32_t type) {
  if (!mc_th_initialized_ ||
      mc_th_version_ != mc_version_g.load(std::memory_order_acquire)) {
    {
      auto guard = md_registry_locked().lock().unwrap();
      // Copy the container into thread-local storage
      mc_th_ = guard->clone();
    }
    mc_th_initialized_ = true;
    mc_th_version_ = mc_version_g.load(std::memory_order_relaxed);
  }
  auto opt = mc_th_.get(type);
  verify(opt.is_some());
  return *opt.unwrap();
}

// @unsafe - Calls initializer factory and unmarshals into proxied payload.
// @lifetime: (&'a mut) -> &'a mut
Marshal& MarshallDeputy::create_actual_object_from(Marshal& m) {
  verify(!has_marshallable());
  switch (kind_) {
    case UNKNOWN:
      verify(0);
      break;
    default:
      auto func = get_initializer(kind_);
      verify(static_cast<bool>(func));
      auto state = func();
      verify(state.kind != UNKNOWN);
      verify(state.kind == kind_);
      set_marshallable_state(std::move(state));
      break;
  }
  auto object = inner();
  verify(object != nullptr);
  // Use get() to get pointer access
  Marshallable* mut_data = object.get();
  verify(mut_data);  // Should succeed - we just created it
  mut_data->from_marshal(m);
  verify(object->kind());
  verify(kind_);
  verify(object->kind() == kind_);
  return m;
}

} // namespace rrr
