#pragma once

// import std; replacement — see <std_compat.hpp> for rationale.
#include <std_compat.hpp>

// @c-compat-added
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <string>



// External safety annotations for system functions used in this module
// @external: {
//   localtime_r: [safe, (const time_t*, struct tm*) -> struct tm*]
//   time: [safe, (time_t*) -> time_t]
//   sysconf: [safe, (int) -> long]
//   getpid: [safe, () -> pid_t]
//   readlink: [safe, (const char*, char*, size_t) -> ssize_t]
//   snprintf: [safe, (char*, size_t, const char*, ...) -> int]
//   getdelim: [unsafe, (char**, size_t*, int, FILE*) -> ssize_t]
//   free: [unsafe, (void*) -> void]
// }




#include <rusty/fn.hpp>

#include "basetypes.hpp"

namespace rrr {

// @unsafe - Uses inline assembly to read timestamp counter
// SAFETY: rdtsc instruction is safe to execute, reads CPU timestamp counter
inline uint64_t rdtsc() {
#if defined(__i386__) || defined(__x86_64__)
  uint32_t hi, lo;
  __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
  return (((uint64_t)hi) << 32) | ((uint64_t)lo);
#elif defined(__aarch64__)
  uint64_t val;
  __asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(val));
  return val;
#else
  return 0; // Fallback or #error "Unsupported architecture"
#endif
}

// @safe - Pure computation with no memory operations
template <class T, class T1, class T2>
inline T clamp(const T &v, const T1 &lower, const T2 &upper) {
  if (v < lower) {
    return lower;
  }
  if (v > upper) {
    return upper;
  }
  return v;
}

// YYYY-MM-DD HH:MM:SS.mmm, 24 bytes required for now
#define TIME_NOW_STR_SIZE 24
// @unsafe - Writes directly to provided buffer
// SAFETY: Caller must ensure buffer has at least TIME_NOW_STR_SIZE bytes
void time_now_str(char *now);

// @safe - Queries system configuration
int get_ncpu();

// @safe - Returns static buffer with executable path
const char *get_exec_path();

// NOTE: \n is stripped from input
// @unsafe - Uses raw FILE* and allocates with getdelim
// SAFETY: FILE* must be valid and open
std::string getline(FILE *fp, char delim = '\n');

// This template function declaration is used in defining arraysize.
// Note that the function doesn't need an implementation, as we only
// use its type.
template <typename T, size_t N> char (&ArraySizeHelper(T (&array)[N]))[N];

// That gcc wants both of these prototypes seems mysterious. VC, for
// its part, can't decide which to use (another mystery). Matching of
// template overloads: the final frontier.
#ifndef COMPILER_MSVC
template <typename T, size_t N> char (&ArraySizeHelper(const T (&array)[N]))[N];
#endif

#define arraysize(array) (sizeof(base::ArraySizeHelper(array)))

// @unsafe - Calls Map::insert (template; external unsafe regardless of concrete container)
// SAFETY: Standard container insertion, caller ensures map is valid
template <class K, class V, class Map>
inline void insert_into_map(Map &map, const K &key, const V &value) {
  map.insert(typename Map::value_type(key, value));
}

// @unsafe - Calls std::reverse_iterator::base and container::erase (external
// unsafe) SAFETY: Standard container and iterator operations, caller ensures
// validity
template <class Container>
typename std::reverse_iterator<typename Container::iterator>
erase(Container &l,
      typename std::reverse_iterator<typename Container::iterator> &rit) {
  typename Container::iterator it = rit.base();
  it--;
  it = l.erase(it);
  return std::reverse_iterator<typename Container::iterator>(it);
}

// @interface
class Job {
public:
  virtual bool Ready() = 0;
  virtual void Work() = 0;
  virtual bool Done() = 0;
  virtual ~Job() = default;
};

// @unsafe - Inherits from @interface Job (rusty-cpp namespace resolution bug
// workaround)
class OneTimeJob : public Job {
 public:
  // @safe - Takes ownership of the callable; rusty::Function is move-only.
  OneTimeJob(rusty::Function<void()> func) : func_(std::move(func)) {
  }
  bool done_{false};
  bool ready_{true};
  rusty::Function<void()> func_{};
  // Interface method - inherits @unsafe from Job
  bool Ready() override { return ready_; }
  // Interface method - inherits @unsafe from Job
  bool Done() override { return done_; }
  // Interface method - inherits @unsafe from Job
  // Calls rusty::Function::operator() (external unsafe)
  // SAFETY: Executes user-provided function, caller ensures validity
  void Work() override {
    ready_ = false;
    func_();
    done_ = true;
  }
  // No user-declared destructor: Job's `virtual ~Job() = default;`
  // covers polymorphic deletion, and omitting our own destructor
  // allows the implicit move constructor / move assignment to be
  // synthesized — required since `func_` is move-only
  // (`rusty::Function` is non-copyable).
};

// @unsafe - Inherits from @interface Job (rusty-cpp namespace resolution bug
// workaround)
class FrequentJob : public Job {
public:
  uint64_t tm_last_ = 0;
  uint64_t period_ = 0;

  virtual ~FrequentJob() {}
  // Interface method - inherits @unsafe from Job
  virtual bool Ready() override {
    // Time::now is @unsafe
    uint64_t tm_now = rrr::Time::now();
    uint64_t s = tm_now - tm_last_;
    if (s > period_) {
      tm_last_ = tm_now;
      return true;
    }
    return false;
  }

  // Interface method - inherits @unsafe from Job
  virtual bool Done() override {
    // never done.
    return false;
  }

  // @safe
  virtual uint64_t get_last_time() { return tm_last_; }

  // @safe
  virtual void set_period(uint64_t p) { period_ = p; }
};

} // namespace rrr
