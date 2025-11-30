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

//#include <experimental/coroutine>

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
// @unsafe - Single-threaded coroutine with rusty::Rc ownership and mutable fields for interior mutability
class Coroutine {
 public:
  // Returns current coroutine with single-threaded reference counting
  // Returns None if called outside of a coroutine context
  static rusty::Option<rusty::Rc<Coroutine>> CurrentCoroutine();
  // the argument cannot be a reference because it could be declared on stack.
  // Using rusty::Function to support move-only callables (e.g., lambdas capturing rusty::Box)
  // Creates and runs coroutine with rusty::Rc ownership
  static rusty::Rc<Coroutine> CreateRun(rusty::Function<void()> func);

  enum Status {INIT=0, STARTED, PAUSED, RESUMED, FINISHED, RECYCLED};

  // Interior mutability for use with rusty::Rc (const methods need to modify state)
  mutable Status status_ = INIT;
  mutable rusty::Function<void()> func_{};

  // Migrated from std::unique_ptr to rusty::Box with Option for nullable semantics
  mutable rusty::Option<rusty::Box<boost_coro_task_t>> boost_coro_task_{};
  mutable boost::optional<boost_coro_yield_t&> boost_coro_yield_{};

  Coroutine() = delete;
  Coroutine(rusty::Function<void()> func);
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

  // Comparison operator for std::set<rusty::Rc<Coroutine>>
  // Compares by address (pointer identity)
  friend bool operator<(const rusty::Rc<Coroutine>& lhs, const rusty::Rc<Coroutine>& rhs) {
    return lhs.get() < rhs.get();
  }
};

} // namespace rrr
