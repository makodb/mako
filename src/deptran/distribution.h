#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cmath>
#include <cstdio>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "config.h"
#include "rrr/rrr.hpp"

namespace janus {

// Lightweight latency/sample accumulator shared by the replication client and
// Multi-Paxos diagnostics.  It intentionally has no transaction or MemDB
// dependency.
class Distribution {
 private:
  static double CurrentMsTime() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration<double, std::milli>(now).count();
  }

  double created_time_ = CurrentMsTime();
  double recent_100_sum_ = 0;

 public:
  std::vector<double> data_;

  void append(double x) {
    data_.push_back(x);
    recent_100_sum_ += x;
    if (data_.size() > 100) {
      recent_100_sum_ -= data_[data_.size() - 101];
    }
  }

  // Only append samples collected during the middle third of an experiment.
  void mid_time_append(double x, double append_time) {
    const double duration_3_times = (append_time - created_time_) * 3;
    if (duration_3_times > Config::GetConfig()->duration_ * 1000 &&
        duration_3_times < Config::GetConfig()->duration_ * 2 * 1000) {
      data_.push_back(x);
    }
  }

  void mid_time_append(double x) {
    mid_time_append(x, CurrentMsTime());
  }

  void merge(Distribution& other) {
    for (size_t i = 0; i < other.count(); ++i) {
      data_.push_back(other.data_[i]);
    }
  }

  size_t count() const { return data_.size(); }

  double recent_100_ave() const {
    if (data_.empty()) {
      return 0;
    }
    return recent_100_sum_ / std::min<size_t>(data_.size(), 100);
  }

  double pct(double percentile) {
    verify(percentile >= -1e-6 && percentile <= 100.0 + 1e-6);
    if (data_.empty()) {
      return -1;
    }
    std::sort(data_.begin(), data_.end());
    size_t pick = static_cast<size_t>(
        std::floor(data_.size() * percentile));
    if (pick == data_.size()) {
      --pick;
    }
    return data_[pick];
  }

  double pct50() { return pct(0.5); }
  double pct90() { return pct(0.9); }
  double pct99() { return pct(0.99); }

  double ave() const {
    if (data_.empty()) {
      return -1;
    }
    double sum = 0;
    for (double sample : data_) {
      sum += sample;
    }
    return sum / data_.size();
  }

  std::string statistics() {
    char buf[64];
    std::string out;
    std::snprintf(buf, sizeof(buf), "%7s%9zu", "count", count());
    out += buf;
    std::snprintf(buf, sizeof(buf), "%7s%9.2f", " 0pct", pct(0.0));
    out += buf;
    std::snprintf(buf, sizeof(buf), "%7s%9.2f", "50pct", pct(0.5));
    out += buf;
    std::snprintf(buf, sizeof(buf), "%7s%9.2f", "90pct", pct(0.9));
    out += buf;
    std::snprintf(buf, sizeof(buf), "%7s%9.2f", "99pct", pct(0.99));
    out += buf;
    std::snprintf(buf, sizeof(buf), "%7s%9.2f", "  ave", ave());
    out += buf;
    return out;
  }

  std::string distribution() {
    char buf[32];
    std::string out;
    for (int i = 0; i <= 100; i += 10) {
      std::snprintf(buf, sizeof(buf), "%9.2f", pct(i / 100.0));
      out += buf;
    }
    return out;
  }
};

// Lightweight key-frequency diagnostic used by Multi-Paxos when
// CHECK_KEY_DISTRIBUTION is enabled. It lives with the other replication
// diagnostics and has no dependency on the retired transaction command lane.
class KeyDistribution {
 private:
  std::unordered_map<key_t, int> key_count_;
  std::vector<std::pair<int, key_t>> sort_vec_;

 public:
  void Insert(key_t key) { ++key_count_[key]; }

  void Print() {
    sort_vec_.clear();
    int sum = 0;
    for (const auto& [key, occurrences] : key_count_) {
      sort_vec_.emplace_back(-occurrences, key);
      sum += occurrences;
    }
    std::sort(sort_vec_.begin(), sort_vec_.end());
    int count = 0;
    for (auto it = sort_vec_.begin();
         it != sort_vec_.end() && count <= 100; ++it, ++count) {
      Log_info("[KeyDistribution] key = {} occur = {} pct= {:.2f}",
               it->second, -it->first, -it->first * 100.0 / sum);
    }
  }
};

// Small integer-frequency summary used by the legacy RW workload diagnostics.
class Frequency {
 private:
  std::vector<int> keys_;

 public:
  void append(double value) { keys_.push_back(static_cast<int>(value)); }

  void merge(Frequency& other) {
    keys_.insert(keys_.end(), other.keys_.begin(), other.keys_.end());
  }

  std::size_t count() const { return keys_.size(); }

  std::string top_keys_pcts() const {
    std::unordered_map<int, int> counts;
    for (int key : keys_) {
      ++counts[key];
    }

    std::set<std::pair<int, int>> frequency;
    for (const auto& [key, occurrences] : counts) {
      frequency.emplace(-occurrences, key);
    }

    std::stringstream out;
    int emitted = 0;
    for (auto it = frequency.begin();
         it != frequency.end() && emitted < 10; ++it, ++emitted) {
      char buf[48];
      std::snprintf(buf, sizeof(buf), "%.6f",
                    -it->first * 100.0 / count());
      out << buf << " (" << it->second << "), ";
    }
    return out.str();
  }
};

}  // namespace janus
