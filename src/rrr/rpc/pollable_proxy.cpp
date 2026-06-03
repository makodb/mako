module;

#include <rusty/arc.hpp>
#include <rusty/box.hpp>

export module rrr.pollable_proxy;

import std;

// @safe - `PollableBase` trait + thin Arc<T>-wrapping adapter. The
// adapter's `mut_poll()` helper does a const_cast<T&> through
// rusty::Arc<T>::get() — that one method carries an explicit
// `// @unsafe` override below; everything else is pure delegation.
//
// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block is
// the source of truth. The transpiler emits the matching abstract
// C++ class (`class PollableBase`) at namespace scope — the `pub trait`
// visibility tells it not to wrap in an anonymous namespace, so
// downstream TUs (`rrr.reactor`, `rrr.tcp_channel`, …) can name
// `PollableBase` through the import graph. This is the first real
// trait migration in rrr — exercising the `pub trait` → namespace-
// scope-class codegen fix that landed on rusty-cpp main as 591aca7.
//
// Trait name kept as `PollableBase` (not the Rust-idiomatic
// `Pollable`) to avoid a name collision with the unrelated
// `rrr::Pollable` interface that `rrr.epoll_wrapper` exports in
// the same namespace; the two are structurally identical but live
// in separate modules and serve different roles, and renaming
// either is out of scope for this migration.
export namespace rrr {

#if RUSTYCPP_RUST
pub trait PollableBase {
    fn fd(&self) -> i32;
    fn poll_mode(&self) -> i32;
    fn content_size(&mut self) -> usize;
    fn handle_read(&mut self) -> bool;
    fn handle_write(&mut self) -> i32;
    fn handle_error(&mut self);
    fn close(&mut self);
    fn check_pending_write_update(&self) -> bool;
    fn is_closed(&self) -> bool;
}
#endif
/*RUSTYCPP:GEN-BEGIN id=pollable.0 version=1 rust_sha256=58f4fd8299bef8518306a38e8d93c412bfb6de8a9c7150f5c46259aeee31ed7a*/
class PollableBase {
public:
    virtual ~PollableBase() noexcept(false) {}
    virtual int32_t fd() const = 0;
    virtual int32_t poll_mode() const = 0;
    virtual size_t content_size() = 0;
    virtual bool handle_read() = 0;
    virtual int32_t handle_write() = 0;
    virtual void handle_error() = 0;
    virtual void close() = 0;
    virtual bool check_pending_write_update() const = 0;
    virtual bool is_closed() const = 0;
    PollableBase(const PollableBase&) = delete;
    PollableBase& operator=(const PollableBase&) = delete;
    PollableBase(PollableBase&&) = delete;
    PollableBase& operator=(PollableBase&&) = delete;
protected:
    PollableBase() = default;
};

template <class U> class PollableBaseAdapter;
template <class U> class PollableBaseAdapterRef;
template <class U> class PollableBaseAdapterRefMut;
/*RUSTYCPP:GEN-END id=pollable.0*/

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
  // @unsafe - const_cast through Arc::get() returning T*; lifts the
  // const-ness so the adapter can invoke non-const Pollable hooks
  // (handle_read/write/error, content_size, close). Callers guarantee
  // single-threaded access via the poll thread.
  T& mut_poll() { return const_cast<T&>(*poll_.get()); }
  rusty::Arc<T> poll_;
};

template <typename T>
inline PollableProxy make_pollable_proxy_from_typed_arc(rusty::Arc<T> poll) {
  return rusty::make_box<PollableTypedArcAdapter<T>>(std::move(poll));
}

}  // export namespace rrr
