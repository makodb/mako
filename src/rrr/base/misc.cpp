module;

#include <rusty/fn.hpp>
#include <rusty/function.hpp>

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

export module rrr.misc;

import std;
import rrr.basetypes;

export namespace rrr {

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
  return 0;
#endif
}

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

// YYYY-MM-DD HH:MM:SS.mmm; caller-supplied buffer must be at least 24 bytes.
void time_now_str(char *now);
int get_ncpu();
const char *get_exec_path();

// NOTE: \n is stripped from input
std::string getline(FILE *fp, char delim = '\n');

template <class K, class V, class Map>
inline void insert_into_map(Map &map, const K &key, const V &value) {
  map.insert(typename Map::value_type(key, value));
}

template <class Container>
typename std::reverse_iterator<typename Container::iterator>
erase(Container &l,
      typename std::reverse_iterator<typename Container::iterator> &rit) {
  typename Container::iterator it = rit.base();
  it--;
  it = l.erase(it);
  return std::reverse_iterator<typename Container::iterator>(it);
}

class Job {
public:
  virtual bool Ready() = 0;
  virtual void Work() = 0;
  virtual bool Done() = 0;
  virtual ~Job() = default;
};

class OneTimeJob : public Job {
 public:
  OneTimeJob(rusty::Function<void()> func) : func_(std::move(func)) {
  }
  bool done_{false};
  bool ready_{true};
  rusty::Function<void()> func_{};
  bool Ready() override { return ready_; }
  bool Done() override { return done_; }
  void Work() override {
    ready_ = false;
    func_();
    done_ = true;
  }
};

class FrequentJob : public Job {
public:
  uint64_t tm_last_ = 0;
  uint64_t period_ = 0;

  virtual ~FrequentJob() {}
  virtual bool Ready() override {
    uint64_t tm_now = rrr::Time::now();
    uint64_t s = tm_now - tm_last_;
    if (s > period_) {
      tm_last_ = tm_now;
      return true;
    }
    return false;
  }

  virtual bool Done() override {
    return false;
  }

  virtual uint64_t get_last_time() { return tm_last_; }

  virtual void set_period(uint64_t p) { period_ = p; }
};

} // export namespace rrr

namespace rrr {

static void make_int(char* str, int val, int digits) {
    char* p = str + digits;
    for (int i = 0; i < digits; i++) {
        int d = val % 10;
        val /= 10;
        p--;
        *p = '0' + d;
    }
}

void time_now_str(char* now) {
    time_t seconds_since_epoch = time(nullptr);
    struct tm local_calendar;
    localtime_r(&seconds_since_epoch, &local_calendar);
    make_int(now, local_calendar.tm_year + 1900, 4);
    now[4] = '-';
    make_int(now + 5, local_calendar.tm_mon + 1, 2);
    now[7] = '-';
    make_int(now + 8, local_calendar.tm_mday, 2);
    now[10] = ' ';
    make_int(now + 11, local_calendar.tm_hour, 2);
    now[13] = ':';
    make_int(now + 14, local_calendar.tm_min, 2);
    now[16] = ':';
    make_int(now + 17, local_calendar.tm_sec, 2);
    now[19] = '.';
    timeval tv;
    gettimeofday(&tv, nullptr);
    make_int(now + 20, tv.tv_usec / 1000, 3);
    now[23] = '\0';
}

int get_ncpu() {
    return sysconf(_SC_NPROCESSORS_ONLN);
}

const char* get_exec_path() {
    static char path[PATH_MAX];
    static bool ready = false;
    if (!ready) {
        char link[PATH_MAX];
        snprintf(link, sizeof(link), "/proc/%d/exe", getpid());
        int ret = readlink(link, path, sizeof(path));
        if (ret != -1) {
            path[ret] = '\0';
            ready = true;
        } else {
            return nullptr;
        }
    }
    return path;
}

std::string getline(FILE* fp, char delim) {
    char* buf = nullptr;
    size_t n = 0;
    ssize_t n_read = ::getdelim(&buf, &n, delim, fp);
    if (n_read > 0 && buf[n_read - 1] == delim) {
        n_read--;
    }
    std::string line(buf, n_read);
    free(buf);
    return line;
}

} // namespace rrr
