// rrr.alock — asynchronous queued lock with timeout support (formerly
// alock.hpp + alock.cpp).
//
// ALock is NOT a thread lock — it's an async lock built for two-phase
// locking in rococo. The lock callbacks may fire on different threads,
// so callers must ensure their own thread-safety. Storage for
// callbacks goes through the `Arc<Function const>`-backed
// `detail::CallbackWrapper` aliases declared inline below.
module;

#include <cstddef>
#include <cstdint>

#include <stdint.h>
#include <stdlib.h>

#include <rusty/once.hpp>
#include <rusty/rusty.hpp>

export module rrr.alock;

import std;
import rusty;
import rrr.alarm;
import rrr.basetypes;
import rrr.callback_wrapper;
import rrr.dball;
import rrr.debugging;
import rrr.logging;
import rrr.misc;
import rrr.reactor;
import rrr.threading;

// @safe - ALock async queued lock + WaitDieALock / WoundDieALock /
// TimeoutALock variants + ALockGroup. The big request-queue methods
// (vlock, abort, wait_die, wound_die, lock_all, sanity_check,
// read_acquire-over-vec) carry per-method `// @unsafe` because they
// iterate raw `std::list<lock_req_t>` iterators, invoke external
// callbacks, dispatch through DragonBall heap pointers, and (in
// ALockGroup) keep raw `ALock*` BTreeMap keys — the
// Phase 3 ALock* → Weak<ALock> refactor stays blocked. The trivial
// accessors (cas_status, get_status, set_status, ctors, get_next_id,
// write_acquire/read_acquire scalar overload) inherit class @safe.
// ===========================================================================
// Class declarations (from former alock.hpp)
// ===========================================================================
export namespace rrr {

// ALock callback shapes — Arc<Function<...const>>-backed wrappers from
// `base/callback_wrapper.hpp`.  The wrapper hides the move-only inner
// rusty::Function behind a copyable handle so `lock_req_t` (which is
// stored by value in `std::list<lock_req_t> requests_` and is itself
// copyable) keeps its prior copy semantics — a lock_req_t copy is now
// a few Arc refcount bumps instead of a deep std::function copy.  Same
// wrapper type the channel-tier callbacks (`OnFrameCallback` etc.) and
// FutureAttr::callback use.
using ALockLockedCallback = detail::CallbackWrapper<void(uint64_t) const>;
using ALockNotifyCallback = detail::CallbackWrapper<void() const>;
using ALockWoundCallback  = detail::CallbackWrapper<int() const>;

// 0.2 seconds in microseconds. Authored as inline Rust DSL: the
// `#if RUSTYCPP_RUST` block is the source of truth; the transpiler
// regenerates the matching `/*RUSTYCPP:GEN-BEGIN ... END*/` block.
#if RUSTYCPP_RUST
const ALOCK_TIMEOUT: u64 = 200000;
#endif
/*RUSTYCPP:GEN-BEGIN id=alock.1 version=1 rust_sha256=033f8628c9cb1c27e3a0f10d4a53bb94f18496edb243244d60c74306c686109a*/
constexpr uint64_t ALOCK_TIMEOUT = static_cast<uint64_t>(200000);
/*RUSTYCPP:GEN-END id=alock.1*/

// @safe - see file header.
class ALock {
 public:
  enum type_t { RLOCK, WLOCK };
 private:
  uint64_t next_id_ = 1;
  uint64_t owner_{0};

 protected:
  enum status_t { FREE, WLOCKED, RLOCKED };
  status_t status_ = FREE;
  // how many are holding read locks;
  int64_t n_rlock_ = 0; // -1 WLOCKED, 0 FREE, >0 RLOCKED numbers;
  uint64_t get_next_id() {
    return next_id_++;
  }

  // @unsafe - Pure virtual function for lock implementation
  virtual uint64_t vlock(uint64_t owner,
                         const ALockLockedCallback &yes_callback,
                         const ALockNotifyCallback &no_callback,
                         type_t type,
                         uint64_t priority, // lower value has higher priority
                         const ALockWoundCallback &wound_callback) = 0;

  bool done_;

 public:
  ALock();

  // @unsafe - Constructs ALock callback wrapper and calls virtual vlock
  virtual uint64_t lock(uint64_t owner,
                        const ALockNotifyCallback &yes_callback,
                        const ALockNotifyCallback &no_callback,
                        type_t type = WLOCK,
                        int64_t priority = 0, // lower value has higher priority
                        const ALockWoundCallback &wound_callback = [] ()->int {return 0;}) {
    // @unsafe {
    ALockLockedCallback _yes_callback
        = [yes_callback](uint64_t id) {
          yes_callback();
        };
    // }
    return vlock(owner,
                 _yes_callback,
                 no_callback,
                 type,
                 priority,
                 wound_callback);
  }

  /**
   *
   * @return 0 for failed, >0 for a lock id.
   */
  virtual uint64_t lock_sync(uint64_t owner = 0,
                        type_t type = WLOCK,
                        uint64_t priority = 0);

  // Overload with wound_callback for jetpack compatibility
  virtual uint64_t lock_sync(uint64_t owner,
                        type_t type,
                        uint64_t priority,
                        const ALockWoundCallback& wound_callback);

  virtual void disable_wound(uint64_t req_id);
  virtual void abort(uint64_t id) = 0;
  virtual ~ALock();
};

// @safe - see file header.
class WaitDieALock: public ALock {
 protected:
  struct lock_req_t {
    typedef enum {
      WAIT,
      LOCK
    } lock_req_status_t;
    uint64_t id;
    int64_t priority;
    type_t type;
    ALockLockedCallback yes_callback;
    ALockNotifyCallback no_callback;
    lock_req_status_t status;

    lock_req_t() : id(0), priority(0), type(WLOCK), status(WAIT),
                   yes_callback(), no_callback() {
    }

    lock_req_t(uint64_t _id,
               int64_t _priority,
               type_t _type,
               const ALockLockedCallback &_yes_callback,
               const ALockNotifyCallback &_no_callback,
               lock_req_status_t _status = WAIT) :
        id(_id),
        priority(_priority),
        type(_type),
        yes_callback(_yes_callback),
        no_callback(_no_callback),
        status(_status) {
    }

    lock_req_t(const lock_req_t &lock_req)
        : id(lock_req.id), priority(lock_req.priority), type(lock_req.type),
          yes_callback(lock_req.yes_callback),
          no_callback(lock_req.no_callback),
          status(lock_req.status) {
    }

    lock_req_t &operator=(const lock_req_t &lock_req) {
      id = lock_req.id;
      priority = lock_req.priority;
      type = lock_req.type;
      yes_callback = lock_req.yes_callback;
      no_callback = lock_req.no_callback;
      status = lock_req.status;
      return *this;
    }
  };

  typedef enum {
    WD_WAIT,
    WD_DIE
  } wd_status_t;
  // alock carve-out (2026-05-01): the wait-die lock-waiter queue
  // stays `std::list` (NOT `rusty::VecDeque`).  alock.cpp relies on
  // three standard linked-list properties that VecDeque can't supply
  // without a semantic-level refactor:
  //   - reverse iteration (`requests_.rbegin/rend` at alock.cpp:309/329)
  //   - iterator-stable erase-during-iterate (`requests_.erase(it)`
  //     at alock.cpp:230/257/270/437/459/471 — returns the next iter
  //     and leaves all others valid)
  //   - pointer stability across re-entrant `lock()` calls from inside
  //     a yes_callback (read_acquire collects `lock_req_t*` snapshots
  //     and would see those invalidated by a VecDeque ring-buffer
  //     reallocation triggered by a recursive lock())
  // Same precedent as the LRU caches in rpc/idempotency.hpp and
  // rpc/completion_tracker.hpp.  See docs/TODO-srpc.md L2c-alock for
  // the full rationale.
  std::list<lock_req_t> requests_;

  uint64_t n_r_in_queue_;
  uint64_t n_w_in_queue_;

  wd_status_t wait_die(type_t type, int64_t priority);

  void write_acquire(lock_req_t &lock_req) {
    verify(lock_req.type == WLOCK && lock_req.status == lock_req_t::WAIT);
    status_ = WLOCKED;
    lock_req.status = lock_req_t::LOCK;
    lock_req.yes_callback(lock_req.id);
  }
  void read_acquire(lock_req_t &lock_req) {
    verify(lock_req.type == RLOCK && lock_req.status == lock_req_t::WAIT);
    if (n_rlock_ == 0)
      status_ = RLOCKED;
    n_rlock_++;
    lock_req.status = lock_req_t::LOCK;
    lock_req.yes_callback(lock_req.id);
  }
  void read_acquire(const rusty::Vec<lock_req_t *> &lock_reqs) {
    if (lock_reqs.size() == 0) {
      return;
    }
    if (n_rlock_ == 0)
      status_ = RLOCKED;
    n_rlock_ += lock_reqs.size();
    rusty::Vec<std::pair<ALockLockedCallback, uint64_t> > to_calls;
    to_calls.reserve(lock_reqs.size());
    for (auto* req : lock_reqs) {
      verify(req->type == RLOCK && req->status == lock_req_t::WAIT);
      req->status = lock_req_t::LOCK;
      to_calls.push(
          std::pair<ALockLockedCallback, uint64_t>(
              req->yes_callback,
              req->id));
    }
    for (auto& cb : to_calls) {
      cb.first(cb.second);
    }
  }

  virtual uint64_t vlock(uint64_t owner,
                         const ALockLockedCallback &yes_callback,
                         const ALockNotifyCallback &no_callback,
                         type_t type,
                         uint64_t priority,
                         const ALockWoundCallback &) override;

  void sanity_check() {
    bool acquired_check = false;
    int64_t big = std::numeric_limits<int64_t>::max();
    int64_t tmp_big = -1;
    int64_t n_r_locked = 0;
    if (status_ == FREE)
      verify(requests_.size() == 0);
    int64_t num_w = 0, num_r = 0;
    std::list<lock_req_t>::iterator it = requests_.begin();
    for (; it != requests_.end(); it++) {
      if (!acquired_check) {
        if (status_ == WLOCKED) {
          acquired_check = true;
          verify(it == requests_.begin());
          verify(it->type == WLOCK);
          verify(it->status == lock_req_t::LOCK);
        }
        else {
          verify(status_ == RLOCKED);
          if (it->type == RLOCK) {
            verify(it->status == lock_req_t::LOCK);
            n_r_locked++;
          }
          else {
            verify(it->status == lock_req_t::WAIT);
            acquired_check = true;
            verify(n_r_locked == n_rlock_);
          }
        }
      }
      else {
        verify(it->status == lock_req_t::WAIT);
      }

      verify(big > it->priority);
      if (it->type == RLOCK) {
        num_r++;
        if (tmp_big == -1) {
          tmp_big = it->priority;
        }
        else {
          if (tmp_big > it->priority)
            tmp_big = it->priority;
        }
      }
      else {
        num_w++;
        if (tmp_big == -1) { // no R between two Ws
          big = it->priority;
        }
        else {
          big = tmp_big;
          verify(big > it->priority);
          tmp_big = -1;
        }
      }
    }
    verify(num_w == n_w_in_queue_);
    verify(num_r == n_r_in_queue_);
  }

 public:
  WaitDieALock() : ALock(), n_r_in_queue_(0), n_w_in_queue_(0),
                   requests_() {
  }

  virtual ~WaitDieALock();

  virtual void abort(uint64_t id) override;
};

// @safe - see file header.
class WoundDieALock: public ALock {
 protected:
  struct lock_req_t {
    typedef enum {
      WAIT,
      LOCK
    } lock_req_status_t;
    uint64_t id;
    int64_t priority;
    type_t type;
    ALockLockedCallback yes_callback;
    ALockNotifyCallback no_callback;
    ALockWoundCallback wound_callback;
    lock_req_status_t status;

    lock_req_t() : id(0), priority(0), type(WLOCK), status(WAIT),
                   yes_callback(), no_callback(), wound_callback() {
    }

    lock_req_t(uint64_t _id,
               int64_t _priority,
               type_t _type,
               const ALockLockedCallback &_yes_callback,
               const ALockNotifyCallback &_no_callback,
               const ALockWoundCallback &_wound_callback,
               lock_req_status_t _status = WAIT) :
        id(_id),
        priority(_priority),
        type(_type),
        yes_callback(_yes_callback),
        no_callback(_no_callback),
        wound_callback(_wound_callback),
        status(_status) {
    }

    lock_req_t(const lock_req_t &lock_req)
        : id(lock_req.id), priority(lock_req.priority), type(lock_req.type),
          yes_callback(lock_req.yes_callback),
          no_callback(lock_req.no_callback),
          wound_callback(lock_req.wound_callback), status(lock_req.status) {
    }

    lock_req_t &operator=(const lock_req_t &lock_req) {
      id = lock_req.id;
      priority = lock_req.priority;
      type = lock_req.type;
      yes_callback = lock_req.yes_callback;
      no_callback = lock_req.no_callback;
      wound_callback = lock_req.wound_callback;
      status = lock_req.status;
      return *this;
    }
  };

  // alock carve-out (2026-05-01) — see the WaitDieALock comment
  // above for full rationale.  Same iterator-stability / reverse-
  // iteration / pointer-stability requirements apply here.
  std::list<lock_req_t> requests_;

  void wound_die(type_t type, int64_t priority);

  // return 0: wound succ
  // return 1: unwoundable
  int wound(lock_req_t &lock_req) {
    if (lock_req.status == lock_req_t::WAIT) { // waiting, use no callback
      lock_req.no_callback();
      return 0;
    } else { // locked, use wound callback
      int ret = lock_req.wound_callback();
      if (ret == 0) { // wound succ, reset alock status
        if (lock_req.type == WLOCK) {
          verify(status_ == WLOCKED);
          status_ = FREE;
        }
        else {
          verify(status_ == RLOCKED);
          n_rlock_--;
          if (n_rlock_ == 0)
            status_ = FREE;
        }
      }
      return ret;
    }
  }

  void write_acquire(lock_req_t &lock_req) {
    verify(lock_req.type == WLOCK && lock_req.status == lock_req_t::WAIT);
    status_ = WLOCKED;
    lock_req.status = lock_req_t::LOCK;
    lock_req.yes_callback(lock_req.id);
  }
  void read_acquire(lock_req_t &lock_req) {
    verify(lock_req.type == RLOCK && lock_req.status == lock_req_t::WAIT);
    if (n_rlock_ == 0)
      status_ = RLOCKED;
    n_rlock_++;
    lock_req.status = lock_req_t::LOCK;
    lock_req.yes_callback(lock_req.id);
  }
  void read_acquire(const rusty::Vec<lock_req_t *> &lock_reqs) {
    if (lock_reqs.size() == 0) {
      return;
    }
    if (n_rlock_ == 0)
      status_ = RLOCKED;
    n_rlock_ += lock_reqs.size();
    rusty::Vec<std::pair<ALockLockedCallback, uint64_t> > to_calls;
    to_calls.reserve(lock_reqs.size());
    for (auto* req : lock_reqs) {
      verify(req->type == RLOCK && req->status == lock_req_t::WAIT);
      req->status = lock_req_t::LOCK;
      to_calls.push(
          std::pair<ALockLockedCallback, uint64_t>(
              req->yes_callback,
              req->id));
    }
    for (auto& cb : to_calls) {
      cb.first(cb.second);
    }
  }

  virtual uint64_t vlock(uint64_t owner,
                         const ALockLockedCallback &yes_callback,
                         const ALockNotifyCallback &no_callback,
                         type_t type,
                         uint64_t priority,
                         const ALockWoundCallback &wound_callback) override;

  void sanity_check() {
    bool acquired_check = false;
    int64_t small = std::numeric_limits<int64_t>::min();
    int64_t tmp_small = -1;
    int64_t n_r_locked = 0;
    if (status_ == FREE)
      verify(requests_.size() == 0);
    std::list<lock_req_t>::iterator it = requests_.begin();
    for (; it != requests_.end(); it++) {
      if (!acquired_check) {
        if (status_ == WLOCKED) {
          acquired_check = true;
          verify(it == requests_.begin());
          verify(it->type == WLOCK);
          verify(it->status == lock_req_t::LOCK);
        }
        else {
          verify(status_ == RLOCKED);
          if (it->type == RLOCK) {
            verify(it->status == lock_req_t::LOCK);
            n_r_locked++;
          }
          else {
            verify(it->status == lock_req_t::WAIT);
            acquired_check = true;
            verify(n_r_locked == n_rlock_);
          }
        }
      }
      else {
        verify(it->status == lock_req_t::WAIT);

        // didn't check if wound callback is revoked or not
        verify(small < it->priority);
        if (it->type == RLOCK) {
          if (tmp_small < it->priority)
            tmp_small = it->priority;
        }
        else {
          if (tmp_small == -1) { // no R between two Ws
            small = it->priority;
          }
          else {
            small = tmp_small;
            verify(small < it->priority);
            tmp_small = -1;
          }
        }
      }
    }
  }

 public:
  WoundDieALock() :
      ALock(), requests_() {
  }

  virtual ~WoundDieALock();

  virtual void abort(uint64_t id) override;
};

// @safe - see file header.
class TimeoutALock: public ALock {
 protected:
  virtual uint64_t vlock(uint64_t owner,
                         const ALockLockedCallback &yes_callback,
                         const ALockNotifyCallback &no_callback,
                         type_t type,
                         uint64_t priority,
                         const ALockWoundCallback &) override;

 public:
  enum mode_t { TIMEOUT, PROMPT };

  class ALockReq {
   public:
    /**
     * WAIT->RLOCK->UNLOCK
     * WAIT->WLOCK->UNLOCK
     * WAIT->ABORT
     * WAIT->TIMEOUT
     * TODO: RLOCK -> WLOCK
     */
    enum status_t { WAIT, LOCK, UNLOCK, ABORT, TIMEOUT };

    uint64_t id_;
    uint64_t alarm_id_ = 0;
    type_t type_;
    uint64_t timeout_ = 0;

    ALockLockedCallback yes_callback_;
    ALockNotifyCallback no_callback_;

    uint64_t time_;
    status_t status_;

//        std::mutex mtx_;

    ALockReq(uint64_t id, type_t type)
        : id_(id), type_(type), time_(), timeout_(),
          status_(WAIT), yes_callback_(), no_callback_() { }


    ALockReq(uint64_t id,
             type_t type,
             const ALockLockedCallback &yes_cb,
             const ALockNotifyCallback &no_cb,
             uint64_t tm_out) :
        id_(id),
        type_(type),
        yes_callback_(yes_cb),
        no_callback_(no_cb),
        time_(tm_out),
        status_(WAIT) {
    }

    // @safe
    status_t get_status() {
      //            std::lock_guard<std::mutex> guard(mtx_);
      return status_;
    }

    void set_status(status_t s) {
      //            std::lock_guard<std::mutex> guard(mtx_);
      status_ = s;
    }

    bool cas_status(status_t c, status_t s) {
      //            std::lock_guard<std::mutex> guard(mtx_);
      bool ret = (status_ == c);
      if (ret) {
        status_ = s;
      }
      return ret;
    }
  };

 public:

  // @safe - Rust-idiomatic singleton accessor (mirrors
  // `std::sync::OnceLock<Alarm>` + `get_or_init`).
  //
  // Replaces the C++11 function-local static (Meyers singleton).
  // Same laziness, same thread-safe first-call init, but with a
  // direct mapping to Rust's `OnceLock<T>` for future DSL migration.
  //
  // `Alarm::Alarm()` sets `period_ = 50000` on the FrequentJob base,
  // so the lambda has to construct fresh rather than relying on a
  // default field initializer alone.
  static Alarm &get_alarm_s() {
    static rusty::OnceCell<Alarm> inst;
    inst.get_or_init([]() -> Alarm { return Alarm{}; });
    return *inst.get_mut();
  }

  uint64_t id_locked_ = 0;

  // the prior `std::mutex lock_;` field never
  // had any uncommented lock/unlock site — every reference to `lock_`
  // in alock.{hpp,cpp} was inside `//`-prefixed dead code (see git
  // blame for the commented-out `lock_.lock()` / `lock_.unlock()`
  // pairs that used to wrap `requests_` mutations in an earlier
  // version).  Field removed; no consumer (test_alock or otherwise)
  // accessed it directly.  If concurrency over `requests_` is ever
  // re-introduced, follow the L7-request_queue / L7-inmemory_*
  // pattern: wrap the protected fields in `SpinMutex<Inner>` rather
  // than re-adding a separate std::mutex.
  //
  // alock carve-out (2026-05-01) — `std::list<ALockReq>` stays for
  // the same iterator-stability reasons as the WaitDieALock variant
  // (see that class's comment above for full rationale).
  std::list<ALockReq> requests_;
  //    uint64_t tm_last_ = 0;
  uint64_t tm_wait_;

  //    std::list<ALockReq> rreqs_;
  //    std::list<ALockReq> wreqs_;

  TimeoutALock(uint64_t time_wait = ALOCK_TIMEOUT) :
      ALock(),
      tm_wait_(time_wait) {
  }

  virtual ~TimeoutALock();

  //void safe_check() {
  //auto tm_now = rrr::Time::now(false);
  //if (tm_last_ != 0 && tm_now - tm_last_ > 5 * 1000 * 1000) {
  //    verify(0);
  //}
  //}

  void do_timeout(ALockReq &req) {
    // this should be try lock.

    auto func = req.no_callback_;
    bool call = req.cas_status(ALockReq::WAIT, ALockReq::TIMEOUT);
    if (call) {
      func();
    }
  }


  /**
   * return: how many i locked.
   */
  uint32_t lock_all(rusty::Vec<ALockReq *> &lock_reqs);

  /**
   *
   */
  virtual void abort(uint64_t id) override;

};

// @safe - see file header. ALockGroup keeps raw `ALock*` BTreeMap
// keys (the Phase 3 → Weak<ALock> refactor stays blocked), so
// methods that iterate those maps or take a raw `ALock*` parameter
// carry per-method `// @unsafe`.
class ALockGroup {
 public:

  enum status_t { INIT, WAIT, LOCK, TIMEOUT, UNLOCK };

  // both `std::recursive_mutex mtx_locks_` and
  // `std::mutex mtx_` were dead — every uncommented use site of
  // either was already inside `//` comments (see git blame for the
  // historical `mtx_locks_.lock()` and `mtx_.lock_guard` blocks that
  // were commented out before this point).  Removed.  Same forward-
  // looking guidance as TimeoutALock above: future re-introduction
  // of concurrency should use SpinMutex<Inner>, not a separate
  // std::mutex.

  rusty::BTreeMap<ALock *, uint64_t> locked_;
  rusty::BTreeMap<ALock *, ALock::type_t> tolock_;

  uint64_t priority_;
  ALockWoundCallback wound_callback_;


  // INIT->WAIT->LOCK->UNLOCK
  // INIT->WAIT->TIMEOUT
  // TODO: LOCK->WAIT->LOCK->WAIT->LOCK
  // TODO: LOCK->WAIT->TIMEOUT
  status_t status_;

  ALockNotifyCallback yes_callback_;
  ALockNotifyCallback no_callback_;

  uint64_t n_locked_ = 0;
  uint64_t n_tolock_ = 0;

  DragonBall *db_;

  ALockGroup(int64_t priority = 0,
             const ALockWoundCallback &wound_callback
             = ALockWoundCallback()) :
      priority_(priority),
      wound_callback_(wound_callback),
      status_(INIT) {
  }

  bool cas_status(status_t c, status_t s) {
    //        std::lock_guard<std::mutex> guard(mtx_);
    if (status_ == c) {
      status_ = s;
      return true;
    }
    return false;
  }

  void set_status(status_t s) {
    //        std::lock_guard<std::mutex> guard(mtx_);
    status_ = s;
  }

  // @safe
  status_t get_status() {
    //        std::lock_guard<std::mutex> guard(mtx_);
    return status_;
  }

  // @unsafe
  void add(ALock *alock, ALock::type_t type = ALock::WLOCK) {


    auto status = get_status();
    if (status == INIT ||
        status == LOCK) {

      //	    mtx_locks_.lock();
      //	    tolock_.insert(std::pair<ALock*, uint64_t>(&alock, 0));
      //	    tolock_.insert(std::pair<ALock*, ALock::type_t>(&alock, type));
      tolock_.insert(alock, type);
      //	    mtx_locks_.unlock();
    } else {
      verify(0);
    }
  }

  void abort_all_locked() {
    //        mtx_locks_.lock();
    // rusty::BTreeMap iter `operator*()` returns
    // `std::tuple<const K&, V&>` (post-2026-04 API). Use structured
    // bindings to keep the same `alock`/`areq_id` names.
    for (auto&& [alock, areq_id] : locked_) {
      if (areq_id != 0) {
        alock->abort(areq_id);
      }
    }
    //        mtx_locks_.unlock();
  }

  // After calling this, this group can be freed.
  void abort_all() {

    if (cas_status(LOCK, UNLOCK)) {
      abort_all_locked();
    } else {
      // TODO: what if this still waiting!!!???
      verify(0);
    }
  }

  void lock_all(const ALockNotifyCallback &yes_cb,
                const ALockNotifyCallback &no_cb);

  void unlock_all() {

    verify(cas_status(LOCK, UNLOCK));
    // abort all the locks.
    this->abort_all_locked();
  }

  ~ALockGroup() {

  }

};

}  // export namespace rrr

// ===========================================================================
// Implementation (from former alock.cpp)
// ===========================================================================
// @safe - impl namespace. Out-of-class definitions of vlock / abort /
// wound_die / lock_all and the two ALock::lock_sync overloads all
// carry per-method `// @unsafe` because they iterate raw
// `std::list<lock_req_t>` iterators, dispatch external callbacks,
// and (in ALockGroup) traverse raw `ALock*` BTreeMap keys.
namespace rrr {

ALock::ALock()
    : status_(FREE),
      n_rlock_(0),
      done_(false) {}

ALock::~ALock() = default;


// @unsafe - Creates ALock callback wrappers from lambdas
uint64_t ALock::lock_sync(uint64_t owner,
                     type_t type,
                     uint64_t priority) {

  IntEvent& proceed = Reactor::create_event<IntEvent>(); // init 0, 1 as ready
  uint64_t ret_id = 0;
  // @unsafe {
  ALockLockedCallback _yes_callback
      = [&proceed, &ret_id](uint64_t id) {
        ret_id = id;
        verify(id > 0);
        proceed.set(1);
      };
  ALockNotifyCallback _no_callback
      = [&]() {
        proceed.set(1);
      };
  ALockWoundCallback _wound_callback
      = [&]() {
//        proceed.set(1); // TODO why this caused problem???
        return 0;
      };
  // }
  vlock(owner,
        _yes_callback,
        _no_callback,
        type,
        priority,
        _wound_callback);
  proceed.wait();
  return ret_id;
}

// Overload with wound_callback for jetpack compatibility
uint64_t ALock::lock_sync(uint64_t owner,
                     type_t type,
                     uint64_t priority,
                     const ALockWoundCallback& wound_callback) {

  IntEvent& proceed = Reactor::create_event<IntEvent>();
  uint64_t ret_id = 0;
  ALockLockedCallback _yes_callback
      = [&proceed, &ret_id](uint64_t id) {
        ret_id = id;
        verify(id > 0);
        proceed.set(1);
      };
  ALockNotifyCallback _no_callback
      = [&]() {
        proceed.set(1);
      };
  vlock(owner,
        _yes_callback,
        _no_callback,
        type,
        priority,
        wound_callback);
  proceed.wait();
  return ret_id;
}

void ALock::disable_wound(uint64_t lock_req_id) {
  // TODO
}

// @unsafe
WaitDieALock::~WaitDieALock() {
    verify(!done_);
    done_ = true;
    auto it = requests_.begin();
    verify(status_ != RLOCKED);
    for (; it != requests_.end(); it++) {
        if (it->status == lock_req_t::WAIT)
            it->no_callback();
    }
    requests_.clear();
}

uint64_t WaitDieALock::vlock(uint64_t owner,
                             const ALockLockedCallback &yes_callback,
                             const ALockNotifyCallback& no_callback,
                             type_t type,
                             uint64_t priority,
                             const ALockWoundCallback &) {
    uint64_t id = get_next_id();
    if (done_) {
        no_callback();
        return id;
    }

    if (status_ == FREE
        || (status_ == RLOCKED && type == RLOCK && n_w_in_queue_ == 0)) {
        // acquire lock
        requests_.emplace_back(id, priority, type, yes_callback, no_callback);
        // XXX can we omit yes_callback || no_callback here ???

        if (type == RLOCK) {
            n_r_in_queue_++;
            read_acquire(requests_.back());
        }
        else {
            n_w_in_queue_++;
            write_acquire(requests_.back());
        }
    }
    else {
        wd_status_t wd = wait_die(type, priority);
        if (wd == WD_WAIT) { // wait
            requests_.emplace_back(id, priority, type, yes_callback,
                    no_callback);
            if (type == RLOCK) {
                n_r_in_queue_++;
            }
            else {
                n_w_in_queue_++;
            }
        }
        else { // die
            no_callback();
        }
    }

    //sanity_check();
    return id;
}

WaitDieALock::wd_status_t WaitDieALock::wait_die(type_t type, int64_t priority) {
    switch (type) {
        case WLOCK: // in order to wait, the coming request needs to have
                    // larger priority (less priority value) than all reqs in
                    // the queue
        {
            auto rit = requests_.rbegin();
            for (; rit != requests_.rend(); rit++) {
                if (priority >= rit->priority) // can't use > here or deadlock
                    return WD_DIE;
                if (rit->type == WLOCK)  // since requests_ are ordered by
                                        // priority for write locks
                    break;
            }
            return WD_WAIT;
        }
        case RLOCK:
        {
            auto rit = requests_.rbegin();
            for (; rit != requests_.rend(); rit++) {
                if (rit->type == WLOCK) { // check tha write req with largest priority
                    if (priority < rit->priority)
                        return WD_WAIT;
                    else
                        return WD_DIE;
                }
            }
            verify(0); // should be able to acquire the lock, no need to wait or die
        }
        default:
            verify(0);
    }
}

// @unsafe - takes address-of (`&lock_req`) on stored `std::list`
// elements to pass into write_acquire / read_acquire helpers.
void WaitDieALock::abort(uint64_t id) {
    if (done_) {
        return;
    }

    int64_t n_w_before_this = 0;
    std::list<lock_req_t>::iterator it = requests_.begin();
    for (; it != requests_.end(); it++)
        if (it->id == id)
            break;
        else if (it->type == WLOCK)
            n_w_before_this++;

    if (it == requests_.end())
        return; // no request found matching the given id

    if (it->status == lock_req_t::WAIT) { // abort waiting request
        lock_req_t aborted_lock_req(*it);
        std::list<lock_req_t>::iterator next_it = requests_.erase(it);
        if (aborted_lock_req.type == RLOCK) {
            n_r_in_queue_--;
        }
        else {
            n_w_in_queue_--;
            if (n_w_before_this == 0) { // alock must be read locked
                                        // needs to approve all following read
                                        // requests till next write request
                verify(status_ == RLOCKED);
                rusty::Vec<lock_req_t *> lock_reqs;
                for (; next_it != requests_.end(); next_it++) {
                    if (next_it->type == RLOCK) {
                        lock_reqs.push(&(*next_it));
                    }
                    else
                        break;
                }
                read_acquire(lock_reqs);
            }
        }
        aborted_lock_req.no_callback();
    }
    else { // unlock
        if (it->type == RLOCK) { // unlock a read lock
            n_r_in_queue_--;
            n_rlock_--;
            std::list<lock_req_t>::iterator next_it = requests_.erase(it);
            if (n_rlock_ == 0) {
                if (next_it == requests_.end()) { // empty queue
                    status_ = FREE;
                    verify(requests_.size() == 0);
                }
                else {
                    write_acquire(*next_it);
                }
            }
        }
        else { // unlock a write lock
            n_w_in_queue_--;
            std::list<lock_req_t>::iterator next_it = requests_.erase(it);
            if (next_it == requests_.end()) { // empty queue
                status_ = FREE;
                verify(requests_.size() == 0);
            }
            else if (next_it->type == WLOCK) { // acquire next write lock
                write_acquire(*next_it);
            }
            else { // acquire read locks
                rusty::Vec<lock_req_t *> lock_reqs;
                for (; next_it != requests_.end(); next_it++) {
                    if (next_it->type == WLOCK)
                        break;
                    lock_reqs.push(&(*next_it));
                }
                read_acquire(lock_reqs);
                verify(status_ == RLOCKED);
            }
        }
    }
    //sanity_check();
}

WoundDieALock::~WoundDieALock() {
    verify(!done_);
    done_ = true;
    std::list<lock_req_t>::iterator it = requests_.begin();
    verify(status_ != RLOCKED);
    for (; it != requests_.end(); it++) {
        if (it->status == lock_req_t::WAIT)
            it->no_callback();
    }
    requests_.clear();
}

void WoundDieALock::wound_die(type_t type, int64_t priority) {
    switch (type) {
        case WLOCK:
        {
            std::list<lock_req_t>::reverse_iterator rit = requests_.rbegin();
            while (rit != requests_.rend()) {
                if (rit->priority >= priority) { // try wound it
                    int ret = wound(*rit);
                    if (0 == ret) { // wounded successfully
                        rit = erase(requests_, rit);
                        continue;
                    }
                }
                else {
                    if (rit->type == WLOCK) {
                        break;
                    }
                }
                rit++;
            }
            break;
        }
        case RLOCK:
        {
            std::list<lock_req_t>::reverse_iterator rit = requests_.rbegin();
            while (rit != requests_.rend()) {
                if (rit->priority < priority) {
                    break;
                }
                if (rit->type == WLOCK) { // try wound write lock
                    int ret = wound(*rit);
                    if (0 == ret) { // wounded successfully
                        rit = erase(requests_, rit);
                        continue;
                    }
                }
                rit++;
            }
            break;
        }
    }
}

uint64_t WoundDieALock::vlock(uint64_t owner,
                              const ALockLockedCallback &yes_callback,
                              const ALockNotifyCallback& no_callback,
                              type_t type,
                              uint64_t priority,
                              const ALockWoundCallback &wound_callback) {

    uint64_t id = get_next_id();

    if (done_) {
        no_callback();
        return id;
    }

    wound_die(type, priority);

    requests_.emplace_back(id,
            priority,
            type,
            yes_callback,
            no_callback,
            wound_callback);

    lock_req_t &front = requests_.front();
    switch (status_) {
        case FREE:
        {
            if (front.type == WLOCK) {
                write_acquire(front);
            }
            else {
                rusty::Vec<lock_req_t *> lock_reqs;
                std::list<lock_req_t>::iterator it = requests_.begin();
                for (; it != requests_.end(); it++)
                    if (it->type == RLOCK)
                        lock_reqs.push(&(*it));
                    else
                        break;
                read_acquire(lock_reqs);
            }
            break;
        }
        case RLOCKED:
        {
            verify(front.type == RLOCK && front.status == lock_req_t::LOCK);
            bool new_acquired = false;
            rusty::Vec<lock_req_t *> lock_reqs;
            std::list<lock_req_t>::iterator it = requests_.begin();
            for (; it != requests_.end(); it++) {
                if (it->status == lock_req_t::LOCK) {
                    verify(it->type == RLOCK && new_acquired == false);
                }
                else if (it->type == RLOCK) {
                    lock_reqs.push(&(*it));
                    new_acquired = true;
                }
                else
                    break;
            }
            read_acquire(lock_reqs);
            break;
        }
        case WLOCKED:
            verify(front.type == WLOCK && front.status == lock_req_t::LOCK);
            break;
        default:
            verify(0);
    }

    //sanity_check();
    return id;
}

// @unsafe - takes address-of (`&lock_req`) on stored `std::list`
// elements to pass into write_acquire / read_acquire helpers.
void WoundDieALock::abort(uint64_t id) {
    if (done_)
        return;

    int64_t n_w_before_this = 0;
    std::list<lock_req_t>::iterator it = requests_.begin();
    for (; it != requests_.end(); it++)
        if (it->id == id)
            break;
        else if (it->type == WLOCK)
            n_w_before_this++;

    if (it == requests_.end())
        return; // no request found matching the given id

    if (it->status == lock_req_t::WAIT) { // abort waiting request
        lock_req_t aborted_lock_req(*it);
        std::list<lock_req_t>::iterator next_it = requests_.erase(it);
        if (aborted_lock_req.type == WLOCK) {
            if (n_w_before_this == 0) { // alock must be read locked
                                        // needs to approve all following read
                                        // requests till next write request
                verify(status_ == RLOCKED);
                rusty::Vec<lock_req_t *> lock_reqs;
                for (; next_it != requests_.end(); next_it++) {
                    if (next_it->type == RLOCK) {
                        lock_reqs.push(&(*next_it));
                    }
                    else
                        break;
                }
                read_acquire(lock_reqs);
            }
        }
        aborted_lock_req.no_callback();
    }
    else { // unlock
        if (it->type == RLOCK) { // unlock a read lock
            n_rlock_--;
            std::list<lock_req_t>::iterator next_it = requests_.erase(it);
            if (n_rlock_ == 0) {
                if (next_it == requests_.end()) { // empty queue
                    status_ = FREE;
                    verify(requests_.size() == 0);
                }
                else {
                    write_acquire(*next_it);
                }
            }
        }
        else { // unlock a write lock
            std::list<lock_req_t>::iterator next_it = requests_.erase(it);
            if (next_it == requests_.end()) { // empty queue
                status_ = FREE;
                verify(requests_.size() == 0);
            }
            else if (next_it->type == WLOCK) { // acquire next write lock
                write_acquire(*next_it);
            }
            else { // acquire read locks
                rusty::Vec<lock_req_t *> lock_reqs;
                for (; next_it != requests_.end(); next_it++) {
                    if (next_it->type == WLOCK)
                        break;
                    lock_reqs.push(&(*next_it));
                }
                read_acquire(lock_reqs);
                verify(status_ == RLOCKED);
            }
        }
    }
    //sanity_check();
}

uint64_t TimeoutALock::vlock(uint64_t owner,
                             const ALockLockedCallback& yes_callback,
                             const ALockNotifyCallback& no_callback,
                             type_t type,
                             uint64_t priority,
                             const ALockWoundCallback& wound_callback) {


    //        safe_check();
    //        lock_.lock();

    auto id = get_next_id();

    //if (RandomGenerator::rand(1, 10000) <= 9900) {
    //	yes_callback(id);
    //} else {
    //	no_callback();
    //}
    //return id;

    // first push the request into the lock queue.
    requests_.emplace_back(id,
            type,
            yes_callback,
            no_callback,
            0);
    //    requests_.emplace_back(id, type);
    ALockReq &req = requests_.back();


    bool lockable = (status_ == FREE) ||
        (status_ == RLOCKED && type == RLOCK);



    if (lockable) {
        // then the lock can be
        // locked successfully.

        //	yes_callback(id);
        //	return id;

        if (type == RLOCK) {
            //	    yes_callback(id);
            //	    return id;

            status_ = RLOCKED;
            n_rlock_ ++;
        } else {
            //	    yes_callback(id);
            //	    return id;

            status_ = WLOCKED;
        }


        req.set_status(ALockReq::LOCK);



    } else if (tm_wait_ > 0) {
        // status is RLOCKED, type is WLOCK
        // or status is WLOCKED.
        req.set_status(ALockReq::WAIT);
        uint64_t tm_now = rrr::Time::now(false);
        uint64_t tm_out = tm_now + tm_wait_;
        auto& alarm = get_alarm_s();
        // @unsafe - Lambda captures reference to req
        // @unsafe {
        req.alarm_id_ = alarm.add(tm_out, [this, &req] () {
                this->do_timeout(req);
                });
        // }
    } else {
        // tm_wait_ = 0
        // do nothing, no callback after release the lock_;
        requests_.pop_back();
    }

    //        lock_.unlock();

    // because req might be freed already, cannot use req object.
    if (lockable) {
        //Log_info("lock req yes: %p", &req);
        yes_callback(id);
    } else if (tm_wait_ == 0) {

        no_callback();
    } else {
        // do nothing.
    }
    return id;
}

// @unsafe - takes address-of (`&req`) on stored `std::list` elements
// + raw `ALockReq*` Vec parameter (collects pointers into the
// `requests_` list).
uint32_t TimeoutALock::lock_all(rusty::Vec<ALockReq*>& lock_reqs) {
    verify(status_ == FREE && n_rlock_ == 0);

    // find next lock. if next lock is read lock, find all
    // lockable read lock requests.

    auto &alarm = get_alarm_s();
    auto it = requests_.begin();
    uint32_t n_lock = 0;

    for (; it != requests_.end(); it++) {
        auto& next_req = *it;
        if (next_req.cas_status(ALockReq::WAIT, ALockReq::LOCK)) {

            alarm.remove(next_req.alarm_id_);
            n_lock++;

            //Log_info("lock req yes: %p", &next_req);
            lock_reqs.push(&next_req);

            if (next_req.type_== RLOCK) {
                status_ = RLOCKED;
                n_rlock_++;
            } else {
                status_ = WLOCKED;
            }

            it++;
            //                tm_last_ = rrr::Time::now(false);
            break;
        }
    }

    for (; status_ == RLOCKED && it != requests_.end(); it++) {
        auto& next_req = *it;
        if (next_req.type_ == RLOCK
                && next_req.cas_status(ALockReq::WAIT, ALockReq::LOCK)) {

            alarm.remove(next_req.alarm_id_);
            n_lock++;
            n_rlock_ ++;
            //Log_info("lock req yes: %p", &next_req);
            lock_reqs.push(&next_req);
        }
    }
    return n_lock;
}

void TimeoutALock::abort(uint64_t id) {

    //status_ = FREE;
    //return;

    //safe_check();
    //	std::lock_guard<std::mutex> guard(lock_);
    //        lock_.lock();

    // find the lock request in the queue.
    auto it = requests_.begin();
    for (; it != requests_.end(); it++) {
        if ((*it).id_ == id) {
            break;
        }
    }

    // it's ok if not found.
    // maybe the caller called multiple times.
    if (it == requests_.end()) {
        // maybe this has been aborted before.
        //            lock_.unlock();
        return;
    }

    // found the request, different actions based on
    // the current state of the lock.
    rusty::Vec<ALockReq*> lock_reqs;

    ALockReq& req = *it;
    if (req.cas_status(ALockReq::LOCK, ALockReq::UNLOCK)) {
        // this is currently locked.
        // it cannot be in the timeout queue.
        auto type = req.type_;
        if (type == RLOCK) {
            n_rlock_--;
        }
        if (n_rlock_ == 0) {
            status_ = FREE;
            //                tm_last_ = 0;
        }
        requests_.erase(it);

        if (status_ == FREE) {
            // @unsafe { lock_all is @unsafe (raw `ALockReq*` Vec). }
            { lock_all(lock_reqs); }
        }
    } else if (req.cas_status(ALockReq::WAIT, ALockReq::ABORT)) {
        // cancel timeout.
        // the timeout function might be called.
        get_alarm_s().remove(req.alarm_id_);
        requests_.erase(it);
    } else if (req.get_status() == ALockReq::TIMEOUT) {
        requests_.erase(it);
    } else if (req.get_status() == ALockReq::UNLOCK) {
        verify(0);
    } else if (req.get_status() == ALockReq::ABORT) {
        verify(0);
    } else {
        verify(0);
    }
    //        lock_.unlock();

    for (auto& r: lock_reqs) {
        r->yes_callback_(r->id_);
    }

    return;
}


// @unsafe - Uses Arc<Function const>-backed callback wrappers + Vec
TimeoutALock::~TimeoutALock() {
    //    return;

    // free all the lockes and trigger timeout for those waiting.
    // @unsafe {
    rusty::Vec<ALockNotifyCallback> tocall;
    // }
    //        lock_.lock();
    auto& alarm = get_alarm_s();
    auto it = requests_.begin();
    while ((it = requests_.begin()) != requests_.end()) {
        auto& req = *it;
        if (req.cas_status(ALockReq::LOCK, ALockReq::UNLOCK)) {
            // FIXME: Be careful for this !!!
            //verify(0);
        } else if (req.cas_status(ALockReq::WAIT, ALockReq::TIMEOUT)) {
            alarm.remove(req.alarm_id_);
            req.no_callback_();
        } else if (req.get_status() == ALockReq::TIMEOUT) {
        } else {
            verify(0);
        }
        requests_.erase(it);
    }

    //        lock_.unlock();
}


void ALockGroup::lock_all(const ALockNotifyCallback& yes_cb,
        const ALockNotifyCallback& no_cb) {
//    verify(cas_status(INIT, WAIT) || cas_status(LOCK, WAIT));
//
//    yes_callback_ = yes_cb;
//    no_callback_ = no_cb;
//
//    db_ = new DragonBall(tolock_.size(), [this] () {
//            if (this->cas_status(WAIT, LOCK)) {
//            this->yes_callback_();
//            } else {
//            verify(this->get_status() == TIMEOUT);
//            this->no_callback_();
//            this->abort_all_locked();
//            }
//            });
//
//    decltype(tolock_) tmp;
//
//    swap(tmp, tolock_);
//
//    for(auto &p: tmp) {
//        auto &alock = p.first;
//        auto &type = p.second;

//        auto y_cb = [this, alock] (uint64_t id) {
//            //		this->mtx_locks_.lock();
//            this->locked_[alock] = id;
//            //		this->mtx_locks_.unlock();
//            this->db_->trigger();
//        };
//
//        auto n_cb = [this] () {
//            this->set_status(TIMEOUT);
//            this->db_->trigger();
//        };

//        auto _wound_callback = [this, alock] () -> int {
//            int ret = wound_callback_();
//            if (ret == 0)
//                locked_.erase(alock);
//            return ret;
//        };

        /*auto areq_id = */
//        alock->lock(0, y_cb, n_cb, type, priority_, _wound_callback);
        //            alocks_[alock] = areq_id;
//    }
    //        mtx_locks_.unlock();
}


}  // namespace rrr
