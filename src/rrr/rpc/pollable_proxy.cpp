module;

#include <rusty/arc.hpp>
#include <rusty/box.hpp>
// The generic PollableArcShim<T> block generates `rusty::is_send<T>` /
// `rusty::is_sync<T>` (primary templates at rusty/traits.hpp:49), and
// inline-rust cannot add includes — §7.27. Note this only became necessary
// when the file was regenerated: the emission is output drift (§7.18) in a
// block that was not hand-edited.
#include <rusty/traits.hpp>

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
class PollableBase;

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

// Probe-verified: `type X = rusty::Box<Trait>` lowers exactly (§7.29).
#if RUSTYCPP_RUST
type PollableProxy = rusty::Box<PollableBase>;
#endif
/*RUSTYCPP:GEN-BEGIN id=pollable_proxy.2 version=1 rust_sha256=1b1131fff81477f4873a4d285b4ed15856f71f6ce264e0821358514802df5472*/
using PollableProxy = rusty::Box<PollableBase>;
/*RUSTYCPP:GEN-END id=pollable_proxy.2*/
// `PollableArcShim<T>` — the generic Arc-holding PollableBase
// implementor (generic #[cpp_inherit], probe-verified). Requires T's
// pollable hooks to be &self/const — true for every production T after
// the interior-mutability flips; the old adapter's mut_poll()
// const_cast is gone.
#if RUSTYCPP_RUST
struct PollableArcShim<T> {
    poll_: Arc<T>,
}

#[cpp_inherit]
impl<T> PollableBase for PollableArcShim<T> {
    fn fd(&self) -> i32 {
        self.poll_.fd()
    }
    fn poll_mode(&self) -> i32 {
        self.poll_.poll_mode()
    }
    fn content_size(&mut self) -> usize {
        self.poll_.content_size()
    }
    fn handle_read(&mut self) -> bool {
        self.poll_.handle_read()
    }
    fn handle_write(&mut self) -> i32 {
        self.poll_.handle_write()
    }
    fn handle_error(&mut self) {
        self.poll_.handle_error()
    }
    fn close(&mut self) {
        self.poll_.close()
    }
    fn check_pending_write_update(&self) -> bool {
        self.poll_.check_pending_write_update()
    }
    fn is_closed(&self) -> bool {
        self.poll_.is_closed()
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=pollable_proxy.arc_shim version=1 rust_sha256=943e959c53d0a213e676eead25ffcd462759245e5a6ac4a01136c15506b38a98*/
template<typename T>
struct PollableArcShim;

template<typename T>
struct PollableArcShim : public PollableBase {
    rusty::Arc<T> poll_;
    PollableArcShim(rusty::Arc<T> poll__init) : PollableBase(), poll_(std::move(poll__init)) {}
    PollableArcShim(PollableArcShim&& other) noexcept : PollableBase(), poll_(std::move(other.poll_)) {}


    int32_t fd() const {
        return this->poll_->fd();
    }
    int32_t poll_mode() const {
        return this->poll_->poll_mode();
    }
    size_t content_size() {
        return this->poll_->content_size();
    }
    bool handle_read() {
        return this->poll_->handle_read();
    }
    int32_t handle_write() {
        return this->poll_->handle_write();
    }
    void handle_error() {
        this->poll_->handle_error();
    }
    void close() {
        this->poll_->close();
    }
    bool check_pending_write_update() const {
        return this->poll_->check_pending_write_update();
    }
    bool is_closed() const {
        return this->poll_->is_closed();
    }
    // Rust derives Send/Sync from the field types; C++ cannot see them.
    static constexpr bool is_send = rusty::is_send<T>::value && rusty::is_sync<T>::value;
    static constexpr bool is_sync = rusty::is_send<T>::value && rusty::is_sync<T>::value;
};
/*RUSTYCPP:GEN-END id=pollable_proxy.arc_shim*/

#if RUSTYCPP_RUST
fn make_pollable_proxy_from_typed_arc<T>(poll: rusty::Arc<T>) -> PollableProxy {
    rusty::make_box::<PollableArcShim<T>>(poll)
}
#endif
/*RUSTYCPP:GEN-BEGIN id=pollable_proxy.4 version=1 rust_sha256=cfbe9b339204092e4dabfdaffffc7105b63545afa6a8797228dc356e861876e2*/
template<typename T>
PollableProxy make_pollable_proxy_from_typed_arc(rusty::Arc<T> poll) {
    return rusty::make_box<PollableArcShim<T>>(std::move(poll));
}
/*RUSTYCPP:GEN-END id=pollable_proxy.4*/

}  // export namespace rrr
