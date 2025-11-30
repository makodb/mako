#pragma once

#include <cstdio>
#include <cinttypes>
#include <string>
#include <functional>

#include "basetypes.hpp"

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

namespace rrr {

// @unsafe - Uses inline assembly to read timestamp counter
// SAFETY: rdtsc instruction is safe to execute, reads CPU timestamp counter
inline uint64_t rdtsc() {
  uint32_t hi, lo;
  __asm__ __volatile__ ("rdtsc" : "=a"(lo), "=d"(hi));
  return (((uint64_t) hi) << 32) | ((uint64_t) lo);
}

// @safe - Pure computation with no memory operations
template<class T, class T1, class T2>
inline T clamp(const T& v, const T1& lower, const T2& upper) {
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
void time_now_str(char* now);

// @safe - Queries system configuration
int get_ncpu();

// @safe - Returns static buffer with executable path
const char* get_exec_path();

// NOTE: \n is stripped from input
// @unsafe - Uses raw FILE* and allocates with getdelim
// SAFETY: FILE* must be valid and open
std::string getline(FILE* fp, char delim = '\n');

// This template function declaration is used in defining arraysize.
// Note that the function doesn't need an implementation, as we only
// use its type.
template<typename T, size_t N>
char (& ArraySizeHelper(T (& array)[N]))[N];

// That gcc wants both of these prototypes seems mysterious. VC, for
// its part, can't decide which to use (another mystery). Matching of
// template overloads: the final frontier.
#ifndef COMPILER_MSVC
template<typename T, size_t N>
char (& ArraySizeHelper(const T (& array)[N]))[N];
#endif

#define arraysize(array) (sizeof(base::ArraySizeHelper(array)))

// @unsafe - Calls std::map::insert (external unsafe)
// SAFETY: Standard container insertion, caller ensures map is valid
template<class K, class V, class Map>
inline void insert_into_map(Map& map, const K& key, const V& value) {
  map.insert(typename Map::value_type(key, value));
}

// @unsafe - Calls std::reverse_iterator::base and container::erase (external unsafe)
// SAFETY: Standard container and iterator operations, caller ensures validity
template<class Container>
typename std::reverse_iterator<typename Container::iterator>
erase(Container& l,
      typename std::reverse_iterator<typename Container::iterator>& rit) {
  typename Container::iterator it = rit.base();
  it--;
  it = l.erase(it);
  return std::reverse_iterator<typename Container::iterator>(it);
}

// @safe - Abstract interface with no implementation
class Job {
 public:
  virtual bool Ready() = 0;
  virtual void Work() = 0;
  virtual bool Done() = 0;
  virtual ~Job(){};
};

// @safe - Simple job wrapper with safe state management
class OneTimeJob : public Job {
 public:
  // @safe
  OneTimeJob(std::function<void()> func) : func_(func) {
  }
  bool done_{false};
  std::function<void()> func_{};
  // @safe
  bool Ready() override {
    return true;
  }
  // @safe
  bool Done() override {
    return done_;
  }
  // @unsafe - Calls std::function::operator() (external unsafe)
  // SAFETY: Executes user-provided function, caller ensures validity
  void Work() override {
    func_();
    done_ = true;
  }
  virtual ~OneTimeJob(){};
};

// @safe - Periodic job with safe time tracking
class FrequentJob : public Job {
 public:
  uint64_t tm_last_ = 0;
  uint64_t period_ = 0;

  virtual ~FrequentJob() {}
  // @safe
  virtual bool Ready() override {
    uint64_t tm_now = rrr::Time::now();
    uint64_t s = tm_now - tm_last_;
    if (s > period_) {
      tm_last_ = tm_now;
      return true;
    }
    return false;
  }

  // @safe
  virtual bool Done() override {
    // never done.
    return false;
  }

  // @safe
  virtual uint64_t get_last_time() {
    return tm_last_;
  }

  // @safe
  virtual void set_period(uint64_t p) {
    period_ = p;
  }
};

} // namespace base
