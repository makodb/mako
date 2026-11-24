#pragma once
#include <atomic>
#include <memory>

namespace janus {
// Application epoch state used by raft_main_helper. Raft's election algorithm
// lives in Rust; this temporary native holder moves with the application host.
class ElectionState {
 public:
  bool running = true;
  int machine_id = -1;

  static std::shared_ptr<ElectionState> instance() {
    static auto state = std::shared_ptr<ElectionState>(new ElectionState);
    return state;
  }
  int get_epoch() const { return epoch_.load(); }
  int set_epoch(int value = -1) {
    if (value == -1) return epoch_.fetch_add(1) + 1;
    epoch_.store(value);
    return value;
  }

 private:
  ElectionState() = default;
  std::atomic<int> epoch_{0};
};
}  // namespace janus
