#pragma once

#include "__dep__.h"
#include "constants.h"
#include <vector>
#include <sstream>
#include <chrono>
namespace janus {

class View {
 public:
  int n_; // number of replicas in this view
  std::vector<int> leaders_; // leader ids (size = 1 for Raft since raft only have 1 leader)
  epoch_t view_id_; // corresponding term in Raft
  uint64_t timestamp_; // timestamp when view was created (microseconds since epoch)

  View() : n_(0), view_id_(0), timestamp_(0) {}
  
  View(int n, const std::vector<int>& leaders, epoch_t view_id) 
      : n_(n), leaders_(leaders), view_id_(view_id) {
    timestamp_ = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
  }
  
  // @unsafe
  View(int n, int leader, epoch_t view_id) 
      : n_(n), leaders_{leader}, view_id_(view_id) {
    timestamp_ = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
  }

  // @unsafe
  View& operator=(const View&) = default;
  
  bool operator==(const View& other) const {
    return n_ == other.n_ && leaders_ == other.leaders_ && view_id_ == other.view_id_;
  }
  
  bool operator!=(const View& other) const {
    return !(*this == other);
  }
  
  bool IsEmpty() const {
    return n_ == 0 && leaders_.empty();
  }
  
  int GetLeader() const {
    return leaders_.empty() ? -1 : leaders_[0];
  }
  
  // Update the leader for a specific partition (for multi-leader protocols)
  // For Raft, partition_id is ignored since there's only one leader
  void UpdateLeader(int new_leader, int partition_id = 0) {
    int old_leader = (partition_id < leaders_.size()) ? leaders_[partition_id] : -1;
    
    if (partition_id < leaders_.size()) {
      leaders_[partition_id] = new_leader;
    } else if (partition_id == 0 && !leaders_.empty()) {
      leaders_[0] = new_leader;
    } else {
      leaders_.resize(partition_id + 1, -1);
      leaders_[partition_id] = new_leader;
    }
    timestamp_ = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    
    Log_info("[VIEW] Updated leader for partition %d: %d -> %d, view_id=%lu, timestamp=%lu", 
             partition_id, old_leader, new_leader, view_id_, timestamp_);
  }
  
  // Check if view is stale based on timestamp (default: 30 seconds)
  bool IsStale(uint64_t max_age_us = 30000000) const {
    uint64_t now = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return (now - timestamp_) > max_age_us;
  }
  
  std::string ToString() const {
    std::stringstream ss;
    ss << "View{n=" << n_ << ", view_id=" << view_id_ << ", leaders=[";
    for (size_t i = 0; i < leaders_.size(); i++) {
      if (i > 0) ss << ",";
      ss << leaders_[i];
    }
    ss << "], timestamp=" << timestamp_ << "}";
    return ss.str();
  }
};

} // namespace janus