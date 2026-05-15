module;

#include <rusty/arc.hpp>
#include <rusty/box.hpp>

export module rrr.pollable_proxy;

import std;
import rrr.epoll_wrapper;

export namespace rrr {

class PollableBase {
 public:
  virtual ~PollableBase() = default;

  virtual int fd() const = 0;
  virtual int poll_mode() const = 0;
  virtual size_t content_size() = 0;
  virtual bool handle_read() = 0;
  virtual int handle_write() = 0;
  virtual void handle_error() = 0;
  virtual void close() = 0;
  virtual bool check_pending_write_update() const = 0;
  virtual bool is_closed() const = 0;
};

using PollableProxy = rusty::Box<PollableBase>;

template <typename T>
class PollableTypedArcAdapter : public PollableBase {
 public:
  explicit PollableTypedArcAdapter(rusty::Arc<T> poll) : poll_(std::move(poll)) {}

  int fd() const override { return poll_->fd(); }
  int poll_mode() const override { return poll_->poll_mode(); }
  size_t content_size() override { return mut_poll().content_size(); }
  bool handle_read() override { return mut_poll().handle_read(); }
  int handle_write() override { return mut_poll().handle_write(); }
  void handle_error() override { mut_poll().handle_error(); }
  void close() override { mut_poll().close(); }
  bool check_pending_write_update() const override { return poll_->check_pending_write_update(); }
  bool is_closed() const override { return poll_->is_closed(); }

 private:
  T& mut_poll() { return const_cast<T&>(*poll_.get()); }
  rusty::Arc<T> poll_;
};

template <typename T>
inline PollableProxy make_pollable_proxy_from_typed_arc(rusty::Arc<T> poll) {
  return rusty::make_box<PollableTypedArcAdapter<T>>(std::move(poll));
}

}  // export namespace rrr
