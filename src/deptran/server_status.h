#ifndef SERVER_STATUS_H_
#define SERVER_STATUS_H_

#include <rusty/mutex.hpp>
#include <rusty/condvar.hpp>
#include <rusty/arc.hpp>

#include "srpc/srpc.hpp"

namespace janus {

/**
 * Shared server status that can be held by both ServerControlServiceImpl
 * and the caller (e.g., ServerWorker). This decouples status management
 * from the service, allowing callers to set_ready() without needing
 * a reference to the service.
 */
class ServerStatus {
 public:
  enum class Status { INIT, RUN, STOP };

 private:
  struct State {
    Status status = Status::INIT;
  };

  // mutable because Mutex provides interior mutability (safe to lock from const)
  mutable rusty::Mutex<State> state_;
  mutable rusty::Condvar cond_;

 public:
  ServerStatus() : state_(State{}), cond_() {}

  // Non-copyable, non-movable (shared via Arc)
  ServerStatus(const ServerStatus&) = delete;
  ServerStatus& operator=(const ServerStatus&) = delete;
  ServerStatus(ServerStatus&&) = delete;
  ServerStatus& operator=(ServerStatus&&) = delete;

  // All methods are const because Arc<T> dereferences to const T
  // Interior mutability is provided by Mutex

  void set_ready() const {
    auto guard = state_.lock().unwrap();
    guard->status = Status::RUN;
  }

  void set_shutdown() const {
    {
      auto guard = state_.lock().unwrap();
      guard->status = Status::STOP;
    }
    cond_.notify_all();
  }

  bool is_ready() const {
    auto guard = state_.lock().unwrap();
    return guard->status == Status::RUN;
  }

  bool is_stopped() const {
    auto guard = state_.lock().unwrap();
    return guard->status == Status::STOP;
  }

  void wait_for_shutdown() const {
    auto guard = state_.lock().unwrap();
    guard = cond_.wait_while(std::move(guard),
        [](State& s) { return s.status != Status::STOP; }).unwrap();
  }
};

}  // namespace janus

#endif  // SERVER_STATUS_H_
