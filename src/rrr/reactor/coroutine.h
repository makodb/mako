#pragma once

#include <rusty/box.hpp>
#include <rusty/rc.hpp>
#include <rusty/option.hpp>
#include <rusty/cell.hpp>
#include <rusty/refcell.hpp>
#include <rusty/function.hpp>

#define USE_BOOST_COROUTINE2

#ifdef USE_BOOST_COROUTINE2
#define BOOST_COROUTINE_NO_DEPRECATION_WARNING 1
#define BOOST_COROUTINES_NO_DEPRECATION_WARNING 1
#include <boost/coroutine2/all.hpp>
#endif

#ifdef USE_BOOST_COROUTINE1
#include <boost/coroutine/symmetric_coroutine.hpp>
#endif

#include <boost/optional.hpp>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace rrr {

// Forward declaration
class Coroutine;

#ifdef USE_BOOST_COROUTINE2
typedef boost::coroutines2::coroutine<void>::pull_type boost_coro_task_t;
typedef boost::coroutines2::coroutine<void>::push_type boost_coro_yield_t;
typedef boost::coroutines2::coroutine<void()> coro_t;
#endif

#ifdef USE_BOOST_COROUTINE1
typedef boost::coroutines::symmetric_coroutine<void>::call_type boost_coro_task_t;
typedef boost::coroutines::symmetric_coroutine<void>::yield_type boost_coro_yield_t;
typedef boost::coroutines::symmetric_coroutine<void()> coro_t;
#endif

class Reactor;
class Event;
// @unsafe - Single-threaded coroutine with rusty::Rc ownership and mutable fields for interior mutability
class Coroutine {
 public:
  // Returns current coroutine with single-threaded reference counting
  // Returns None if called outside of a coroutine context
  static rusty::Option<rusty::Rc<Coroutine>> CurrentCoroutine();

  // the argument cannot be a reference because it could be declared on stack.
  // Using rusty::Function to support move-only callables (e.g., lambdas capturing rusty::Box)
  // Creates and runs coroutine with rusty::Rc ownership
  // Template wrapper to support file/line debugging parameters (Jetpack)
  template <typename Func>
  static rusty::Rc<Coroutine> CreateRun(Func&& func, const char* file = "", int64_t line = 0) {
    return CreateRunImpl(rusty::Function<void()>(std::forward<Func>(func)), file, line);
  }

  static void Sleep(uint64_t microseconds);
  static uint64_t global_id;
  uint64_t dep_id_{0};
  bool need_finalize_{false};
  uint64_t id{0};

  enum Status { INIT = 0, STARTED, PAUSED, RESUMED, FINISHED, FINALIZING, RECYCLED };

  // Interior mutability for use with rusty::Rc (const methods need to modify state)
  mutable Status status_ = INIT;
  mutable bool needs_finalize_ = false;  // Jetpack: track finalization state
  mutable rusty::Function<void()> func_{};

  // Migrated from std::unique_ptr to rusty::Box with Option for nullable semantics
  mutable rusty::Option<rusty::Box<boost_coro_task_t>> boost_coro_task_{};
  mutable boost::optional<boost_coro_yield_t&> boost_coro_yield_{};

  Coroutine() = delete;
  explicit Coroutine(rusty::Function<void()> func);
  ~Coroutine();
  // @unsafe - Uses std::bind and function pointers
  void BoostRunWrapper(boost_coro_yield_t& yield);
  // @unsafe - Uses std::bind and function pointers
  void Run() const;  // Made const for Rc compatibility
  // @unsafe - Calls boost coroutine yield
  void Yield() const;  // Made const for Rc compatibility
  // @unsafe - Resumes boost coroutine
  void Continue() const;  // Made const for Rc compatibility
  bool Finished() const;
  void DoFinalize();

  // Comparison operator for std::set<rusty::Rc<Coroutine>>
  // Compares by address (pointer identity)
  friend bool operator<(const rusty::Rc<Coroutine>& lhs, const rusty::Rc<Coroutine>& rhs) {
    return lhs.get() < rhs.get();
  }

 private:
  // @unsafe - Creates and runs a new coroutine (uses raw pointer operations)
  static rusty::Rc<Coroutine> CreateRunImpl(rusty::Function<void()> func, const char* file, int64_t line);
};

} // namespace rrr
