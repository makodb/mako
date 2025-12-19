#include "counter.h"
#include "util.h"
#include "lockguard.h"
#include "silo_runtime.h"

using namespace std;
using namespace util;
using namespace private_;

// =========================================================================
// Helper functions for runtime-prefixed counter names
// =========================================================================

// @unsafe: dereferences raw pointer from SiloRuntime::Current()
string private_::get_runtime_prefixed_name(const string &name) {
  SiloRuntime* runtime = SiloRuntime::Current();
  if (runtime && runtime->id() >= 0) {
    // Multi-runtime mode: prefix with "rt<id>."
    return "rt" + to_string(runtime->id()) + "." + name;
  }
  // Single-runtime or no runtime: use name as-is
  return name;
}

// @unsafe: string manipulation
string private_::strip_runtime_prefix(const string &name) {
  // Check if name starts with "rt<digits>."
  if (name.size() >= 4 && name.substr(0, 2) == "rt") {
    size_t dot_pos = name.find('.');
    if (dot_pos != string::npos && dot_pos > 2) {
      // Verify digits between "rt" and "."
      bool all_digits = true;
      for (size_t i = 2; i < dot_pos; i++) {
        if (!isdigit(name[i])) {
          all_digits = false;
          break;
        }
      }
      if (all_digits) {
        return name.substr(dot_pos + 1);
      }
    }
  }
  return name;
}

// @unsafe: string manipulation
int private_::get_runtime_id_from_name(const string &prefixed_name) {
  // Check if name starts with "rt<digits>."
  if (prefixed_name.size() >= 4 && prefixed_name.substr(0, 2) == "rt") {
    size_t dot_pos = prefixed_name.find('.');
    if (dot_pos != string::npos && dot_pos > 2) {
      // Extract and parse digits between "rt" and "."
      string id_str = prefixed_name.substr(2, dot_pos - 2);
      bool all_digits = true;
      for (char c : id_str) {
        if (!isdigit(c)) {
          all_digits = false;
          break;
        }
      }
      if (all_digits) {
        return stoi(id_str);
      }
    }
  }
  return -1;  // No prefix
}

// @unsafe
map<string, event_ctx *> &
event_ctx::event_counters()
{
  static map<string, event_ctx *> s_counters;
  return s_counters;
}

// @unsafe
spinlock &
event_ctx::event_counters_lock()
{
  static spinlock s_lock;
  return s_lock;
}

// @unsafe: uses std::max
void
event_ctx::stat(counter_data &d)
{
  for (size_t i = 0; i < coreid::NMaxCores; i++)
    d.count_ += counts_[i];
  if (avg_tag_) {
    d.type_ = counter_data::TYPE_AGG;
    uint64_t m = 0;
    for (size_t i = 0; i < coreid::NMaxCores; i++) {
      m = max(m, static_cast<event_ctx_avg *>(this)->highs_[i]);
    }
    uint64_t s = 0;
    for (size_t i = 0; i < coreid::NMaxCores; i++)
      s += static_cast<event_ctx_avg *>(this)->sums_[i];
    d.sum_ = s;
    d.max_ = m;
  }
}

// @unsafe: uses lock_guard
map<string, counter_data>
event_counter::get_all_counters()
{
  map<string, counter_data> ret;
  const map<string, event_ctx *> &evts = event_ctx::event_counters();
  spinlock &l = event_ctx::event_counters_lock();
  ::lock_guard<spinlock> sl(l);
  for (auto &p : evts) {
    counter_data d;
    p.second->stat(d);
    if (d.type_ == counter_data::TYPE_AGG)
      ret[p.first].type_ = counter_data::TYPE_AGG;
    ret[p.first] += d;
  }
  return ret;
}

// @unsafe
map<string, counter_data>
event_counter::get_counters_for_runtime(int runtime_id)
{
  map<string, counter_data> ret;
  const map<string, event_ctx *> &evts = event_ctx::event_counters();
  spinlock &l = event_ctx::event_counters_lock();
  ::lock_guard<spinlock> sl(l);

  string prefix = "rt" + to_string(runtime_id) + ".";

  for (auto &p : evts) {
    int counter_runtime_id = get_runtime_id_from_name(p.first);

    // Include counters that match the runtime ID, or unprefixed counters if runtime_id is -1
    if (counter_runtime_id == runtime_id ||
        (runtime_id == -1 && counter_runtime_id == -1)) {
      counter_data d;
      p.second->stat(d);
      // Strip prefix when returning
      string stripped_name = strip_runtime_prefix(p.first);
      if (d.type_ == counter_data::TYPE_AGG)
        ret[stripped_name].type_ = counter_data::TYPE_AGG;
      ret[stripped_name] += d;
    }
  }
  return ret;
}

// @unsafe
void
event_counter::reset_all_counters()
{
  const map<string, event_ctx *> &evts = event_ctx::event_counters();
  spinlock &l = event_ctx::event_counters_lock();
  ::lock_guard<spinlock> sl(l);
  for (auto &p : evts)
    for (size_t i = 0; i < coreid::NMaxCores; i++) {
      p.second->counts_[i] = 0;
      if (p.second->avg_tag_) {
        static_cast<event_ctx_avg *>(p.second)->sums_[i] = 0;
        static_cast<event_ctx_avg *>(p.second)->highs_[i] = 0;
      }
    }
}

// @unsafe
void
event_counter::reset_counters_for_runtime(int runtime_id)
{
  const map<string, event_ctx *> &evts = event_ctx::event_counters();
  spinlock &l = event_ctx::event_counters_lock();
  ::lock_guard<spinlock> sl(l);
  for (auto &p : evts) {
    int counter_runtime_id = get_runtime_id_from_name(p.first);
    // Only reset counters that match the runtime ID
    if (counter_runtime_id == runtime_id ||
        (runtime_id == -1 && counter_runtime_id == -1)) {
      for (size_t i = 0; i < coreid::NMaxCores; i++) {
        p.second->counts_[i] = 0;
        if (p.second->avg_tag_) {
          static_cast<event_ctx_avg *>(p.second)->sums_[i] = 0;
          static_cast<event_ctx_avg *>(p.second)->highs_[i] = 0;
        }
      }
    }
  }
}

// @unsafe
bool
event_counter::stat(const string &name, counter_data &d)
{
  const map<string, event_ctx *> &evts = event_ctx::event_counters();
  spinlock &l = event_ctx::event_counters_lock();
  event_ctx *ctx = nullptr;
  {
    ::lock_guard<spinlock> sl(l);
    auto it = evts.find(name);
    if (it != evts.end()) // @safe: operator!= is safe
      ctx = it->second;
  }
  if (!ctx)
    return false;
  ctx->stat(d);
  return true;
}

#ifdef ENABLE_EVENT_COUNTERS
// @unsafe: calls unsafe get_runtime_prefixed_name
event_counter::event_counter(const string &name)
  : ctx_(get_runtime_prefixed_name(name), false)
{
  spinlock &l = event_ctx::event_counters_lock();
  map<string, event_ctx *> &evts = event_ctx::event_counters();
  ::lock_guard<spinlock> sl(l);
  evts[ctx_.obj()->name_] = ctx_.obj();
}

// @unsafe: calls unsafe get_runtime_prefixed_name
event_avg_counter::event_avg_counter(const string &name)
  : ctx_(get_runtime_prefixed_name(name))
{
  spinlock &l = event_ctx::event_counters_lock();
  map<string, event_ctx *> &evts = event_ctx::event_counters();
  ::lock_guard<spinlock> sl(l);
  evts[ctx_.obj()->name_] = ctx_.obj();
}
#else
// @safe
event_counter::event_counter(const string &name)
{
}

// @safe
event_avg_counter::event_avg_counter(const string &name)
{
}
#endif