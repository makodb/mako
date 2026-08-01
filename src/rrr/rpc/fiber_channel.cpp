// rrr.fiber_channel — fiber-blocking wrapper over a
// `ChannelConnectionProxy` (formerly fiber_channel.hpp +
// fiber_channel.cpp).
//
// The channel layer's primitive is callback-driven: every backend
// (TcpConnection, in-memory channel, …) only has to implement
// `set_on_frame` / `set_on_closed` / `set_on_error`. That keeps
// backends simple but pushes callback-shaped code into every
// consumer. `FiberChannel` provides loop-shaped ergonomics on top:
//
//   FiberChannel fc(channel_proxy);
//   while (auto frame = fc.recv_frame()) {
//       handle(*frame);
//   }
//
// `recv_frame()` suspends the calling fiber until a frame arrives or
// the channel is closed. `send_frame(...)` forwards to the proxy
// (non-suspending — the proxy's outbound queue is internally
// thread-safe).
//
// Threading: must be constructed, destroyed, and `recv_frame()`-ed
// on the reactor (poll) thread. Only **one fiber** may call
// `recv_frame()` at a time; the underlying `IntEvent` is
// single-waiter. `send_frame(...)`, `close()`, and `is_closed()` are
// safe from any thread.
//
// Lifetime: the wrapper installs lambda callbacks on the proxy that
// capture `this`. The proxy is owned by the wrapper, so the lambdas
// can never outlive the wrapper. On destruction, the proxy is
// dropped, destroying the callback closures held by the underlying
// connection — so callbacks stop firing before any other state is
// torn down.
//
// Clang 22 quirk: importing `rrr.fiber_channel` into a TU that also
// `import std;`s makes clang ambiguate
// `operator new(size_t, std::align_val_t)` inside
// `std::__libcpp_allocate<std::shared_ptr<rusty::Waker>>`
// instantiations. The other rpc modules don't exhibit this; we
// found no in-module workaround. The workaround lives in
// `fiber_channel.hpp` (a 1-line `#include <memory>` shim) which
// `rrr/rrr.hpp` `#include`s *before* `import rrr.fiber_channel;`.
// That textual anchor pins libc++'s `operator new` in the global
// module ahead of the import, and the ambiguity disappears. Empirical
// — see docs/dev/srpc_module_migration_plan.md for the diagnostic.
module;

#include <cstddef>
#include <cstdint>

// `<rusty/rusty.hpp>` pre-instantiates libc++ container templates
// (e.g. `std::vector<uint8_t>::assign`, `std::deque<>::push_back`).
// Without this, clang 22 crashes CodeGen at `EmitScalarExpr` inside
// `EmitReturnStmt` when those instantiations are emitted from
// module-purview function bodies. Same trick as in other rpc
// modules.
#include <rusty/rusty.hpp>

export module rrr.fiber_channel;

import std;
import rusty;       // rusty::Vec lives in the C++20 module umbrella; the
                     // `<rusty/vec.hpp>` header alias was retired and
                     // `OwnedFrame::bytes` now needs Vec<u8> at name lookup.
import rrr.channel;
import rrr.reactor;
import rrr.threading;

// @safe - FiberChannel: fiber-blocking wrapper over a
// `ChannelConnectionProxy`. Bodies use rusty::Mutex<std::deque> for the
// inbound queue, `rusty::Cell<bool>` for the closed flag, and
// IntEvent for parking. Per-method `// @unsafe` overrides cover the
// ctor (which installs lambda callbacks through `ch_->set_on_*` —
// std::unique_ptr deref + rusty::Function ctor chain), the dtor
// (same set_on_* detach), `on_inbound_frame` (raw `const uint8_t*`
// byte arithmetic into the std::vector buffer), and `is_closed`
// (const_cast through the ChannelConnectionProxy).
export namespace rrr {

/**
 * Heap-owned copy of an inbound frame's payload. The wrapping is
 * necessary because the channel-layer `ChannelFrame::payload` is
 * only valid for the duration of the `on_frame` callback.
 *
 * Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below
 * is the source of truth; the transpiler regenerates the matching
 * `RUSTYCPP:GEN-BEGIN ... END` block. The field was a
 * `std::vector<uint8_t>` before the migration; the DSL `Vec<u8>`
 * lowers to `rusty::Vec<uint8_t>` (the transpiled rustc Vec), which
 * is move-only and doesn't expose `.assign()`. The one call site in
 * `FiberChannel::on_inbound_frame` flipped from a `.assign(p, p+n)`
 * iterator-range build to a `reserve` + `memcpy` + `set_len` triple
 * to fit the rusty::Vec API.
 */
#if RUSTYCPP_RUST
struct OwnedFrame {
    bytes: Vec<u8>,
}
#endif
/*RUSTYCPP:GEN-BEGIN id=fiber_channel.owned_frame version=1 rust_sha256=f8ed2cc17a3f8dfefed06907b9428287df01d7d0facfcd8bb77141a0a8f5e21c*/
struct OwnedFrame;

struct OwnedFrame {
    rusty::Vec<uint8_t> bytes;
    // Rust derives Send/Sync from the field types; C++ cannot see them.
    static constexpr bool is_send = true;
    static constexpr bool is_sync = true;
};
/*RUSTYCPP:GEN-END id=fiber_channel.owned_frame*/

// Free-fn implementations of the proxy-/Reactor-/IntEvent-touching
// FiberChannel methods; the DSL methods delegate to these. The private
// on_inbound_* / signal_pending_recv become pure free fns (called from the
// bind_callbacks lambdas). The custom dtor (detach callbacks before member
// teardown) maps to `impl Drop`. Defined in the impl namespace below.
struct FiberChannel;  // defined by the GEN block below
rusty::Option<OwnedFrame> fiberchannel_try_pop(FiberChannel& self);
rusty::Arc<IntEvent>      fiberchannel_make_event();
void                      fiberchannel_wait_event(FiberChannel& self);
void                      fiberchannel_arm(FiberChannel& self);
OwnedFrame                fiberchannel_owned_copy(const ChannelFrame& f);
void                      fiberchannel_signal_pending_recv(FiberChannel& self);
ChannelError              fiberchannel_send_frame(FiberChannel& self, const ChannelFrame& f);
void                      fiberchannel_close(FiberChannel& self);

// Default-init helpers for the `#[cpp_ctor]` (the DSL can't spell a default
// std::deque / empty Option inline).
inline std::deque<OwnedFrame>              fiberchannel_empty_queue() { return std::deque<OwnedFrame>{}; }
inline rusty::Option<rusty::Arc<IntEvent>> fiberchannel_null_event()  { return rusty::Option<rusty::Arc<IntEvent>>(rusty::None); }

// Fiber-blocking wrapper over a `ChannelConnectionProxy` (see file header).
// Interior state (rusty::Mutex<std::deque> inbound queue + a dedicated
// rusty::Mutex<Option<Arc<IntEvent>>> single-waiter handle + Cell<bool>
// closed flag, shared between the on_frame callback and recv_frame) is borrow-
// checked; the proxy-deref / Reactor / IntEvent / fiber-suspend bodies live
// in the `fiberchannel_*` free fns the methods delegate to. The former
// `friend`-free private fields are public in the DSL struct. The custom
// dtor (detach the 3 callbacks before other members tear down) is preserved
// via `impl Drop`. Held only via Box (pinned) / on the test stack, so the
// implicit move ctor the Drop machinery synthesizes is never invoked.
//
// @safe - delegating methods forward to the `fiberchannel_*` free fns,
// which carry their own `// @unsafe`.
#if RUSTYCPP_RUST
struct FiberChannel {
    ch_: ChannelConnectionProxy,
    queue_: rusty::Mutex<std::deque<OwnedFrame>>,
    pending_recv_event_: rusty::Mutex<rusty::Option<rusty::Arc<IntEvent>>>,
    closed_: Cell<bool>,
}

impl FiberChannel {
    #[cpp_ctor] fn new(ch: ChannelConnectionProxy) -> FiberChannel {
        FiberChannel {
            ch_: ch,
            queue_: rusty::Mutex::<std::deque<OwnedFrame>>::new(fiberchannel_empty_queue()),
            pending_recv_event_: rusty::Mutex::<rusty::Option<rusty::Arc<IntEvent>>>::new(fiberchannel_null_event()),
            closed_: Cell::new(false),
        }
    }

    fn bind_callbacks(&mut self) {
        // Callbacks capture a raw self pointer, not a handle: FiberChannel is
        // owned by its holder and the Drop impl detaches these before
        // teardown, which keeps the pointer live for their lifetime.
        let self_ptr: *mut FiberChannel = &raw mut *self;
        let ch: &mut Box<ChannelConnectionBase> = &mut self.ch_;
        ch.set_on_frame(move |f: &ChannelFrame| {
            unsafe { (*self_ptr).on_inbound_frame(f) };
        });
        ch.set_on_closed(move |reason: ChannelError| {
            unsafe { (*self_ptr).on_inbound_closed() };
        });
        // Fatal errors are followed by on_closed; non-fatal errors are
        // silently ignored at this layer.
        ch.set_on_error(move |err: ChannelError, msg: std::string_view| {
        });
    }

    // Fiber-blocking receive: drain the queue, else arm the pending
    // event and suspend until the inbound callback signals. The
    // move-out-of-deque pop, event construction, and fiber-suspending
    // wait are kernels; the loop/arming logic lives here.
    fn recv_frame(&mut self) -> rusty::Option<OwnedFrame> {
        while true {
            let popped = fiberchannel_try_pop(self);
            if popped.is_some() {
                return popped;
            }
            if self.closed_.get() {
                return rusty::None;
            }

            fiberchannel_arm(self);

            let mut armed = true;
            {
                let guard = self.queue_.lock().unwrap();
                if !(*guard).empty() || self.closed_.get() {
                    armed = false;
                }
            }
            if armed {
                fiberchannel_wait_event(self);
            }
            let mut ev_guard = self.pending_recv_event_.lock().unwrap();
            (*ev_guard) = rusty::None;
        }
        rusty::None
    }

    // Inbound-callback targets (invoked from the bind_callbacks
    // lambdas): copy the frame (byte kernel), enqueue, signal.
    fn on_inbound_frame(&mut self, f: &ChannelFrame) {
        let mut copy = fiberchannel_owned_copy(f);
        {
            let mut guard = self.queue_.lock().unwrap();
            (*guard).push_back(copy);
        }
        fiberchannel_signal_pending_recv(self);
    }

    fn on_inbound_closed(&mut self) {
        self.closed_.set(true);
        fiberchannel_signal_pending_recv(self);
    }

    fn send_frame(&mut self, f: &ChannelFrame) -> ChannelError {
        fiberchannel_send_frame(self, f)
    }

    fn close(&mut self) {
        fiberchannel_close(self)
    }

    fn is_closed(&self) -> bool {
        if self.closed_.get() {
            return true;
        }
        // The helper this replaced const_cast'd the proxy to call
        // is_closed(). The trait declares `fn is_closed(&self)` and the C++
        // base `is_closed() const`, so the cast was never needed (§7.16).
        let ch: &Box<ChannelConnectionBase> = &self.ch_;
        ch.is_closed()
    }

    fn channel_for_test(&mut self) -> &mut ChannelConnectionProxy {
        &mut self.ch_
    }
}

impl Drop for FiberChannel {
    fn drop(&mut self) {
        // Detach callbacks before the proxy destructor runs, so an
        // in-flight callback dispatch cannot race with member teardown.
        // The empty_*_callback factories are channel.cpp's hand-bridge for
        // the `{}` default ctor the DSL cannot spell (playbook §7.2).
        let ch: &mut Box<ChannelConnectionBase> = &mut self.ch_;
        ch.set_on_frame(empty_on_frame_callback());
        ch.set_on_closed(empty_on_closed_callback());
        ch.set_on_error(empty_on_error_callback());
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=fiber_channel.fiber_channel version=1 rust_sha256=950c58a923c0e19e455a812c668d3d01c596ba8505f22d977a80fbff2bb487a0*/
struct FiberChannel;

struct FiberChannel {
    ChannelConnectionProxy ch_;
    rusty::Mutex<std::deque<OwnedFrame>> queue_;
    rusty::Mutex<rusty::Option<rusty::Arc<IntEvent>>> pending_recv_event_;
    rusty::Cell<bool> closed_;
    mutable bool _rusty_forgotten = false;
    FiberChannel(ChannelConnectionProxy ch__init, rusty::Mutex<std::deque<OwnedFrame>> queue__init, rusty::Mutex<rusty::Option<rusty::Arc<IntEvent>>> pending_recv_event__init, rusty::Cell<bool> closed__init) : ch_(std::move(ch__init)), queue_(std::move(queue__init)), pending_recv_event_(std::move(pending_recv_event__init)), closed_(std::move(closed__init)) {}
    FiberChannel(const FiberChannel&) = delete;
    FiberChannel(FiberChannel&& other) noexcept : ch_(std::move(other.ch_)), queue_(std::move(other.queue_)), pending_recv_event_(std::move(other.pending_recv_event_)), closed_(std::move(other.closed_)) {
        this->_rusty_forgotten = other._rusty_forgotten;
        other._rusty_forgotten = true;
    }
    FiberChannel& operator=(const FiberChannel&) = delete;
    FiberChannel& operator=(FiberChannel&& other) noexcept {
        if (this == &other) {
            return *this;
        }
        this->~FiberChannel();
        new (this) FiberChannel(std::move(other));
        return *this;
    }
    void rusty_mark_forgotten() const noexcept { _rusty_forgotten = true; rusty::detail::mark_forgotten_if_supported(this->ch_); rusty::detail::mark_forgotten_if_supported(this->queue_); rusty::detail::mark_forgotten_if_supported(this->pending_recv_event_); rusty::detail::mark_forgotten_if_supported(this->closed_); }


    FiberChannel(ChannelConnectionProxy ch);
    void bind_callbacks();
    rusty::Option<OwnedFrame> recv_frame();
    void on_inbound_frame(const ChannelFrame& f);
    void on_inbound_closed();
    ChannelError send_frame(const ChannelFrame& f);
    void close();
    bool is_closed() const;
    ChannelConnectionProxy& channel_for_test();
    ~FiberChannel() noexcept(false);
};


FiberChannel::FiberChannel(ChannelConnectionProxy ch)
    : ch_(std::move(ch))
    , queue_(rusty::Mutex<std::deque<OwnedFrame>>::new_(fiberchannel_empty_queue()))
    , pending_recv_event_(rusty::Mutex<rusty::Option<rusty::Arc<IntEvent>>>::new_(fiberchannel_null_event()))
    , closed_(rusty::Cell<bool>::new_(false))
{}

void FiberChannel::bind_callbacks() {
    FiberChannel* self_ptr = &(*this);
    rusty::Box<ChannelConnectionBase>& ch = this->ch_;
    ch->set_on_frame([=, self_ptr = std::move(self_ptr)](const ChannelFrame& f) {
// @unsafe
{
    ((*self_ptr)).on_inbound_frame(f);
}
});
    ch->set_on_closed([=, self_ptr = std::move(self_ptr)](ChannelError reason) {
// @unsafe
{
    ((*self_ptr)).on_inbound_closed();
}
});
    ch->set_on_error([=](ChannelError err, std::string_view msg) {
});
}

rusty::Option<OwnedFrame> FiberChannel::recv_frame() {
    while (true) {
        auto popped = fiberchannel_try_pop((*this));
        if (popped.is_some()) {
            return std::move(popped);
        }
        if (this->closed_.get()) {
            return rusty::None;
        }
        fiberchannel_arm((*this));
        auto armed = true;
        {
            auto guard = this->queue_.lock().unwrap();
            if (rusty::detail::rust_not(((*guard)).empty()) || this->closed_.get()) {
                armed = false;
            }
        }
        if (armed) {
            fiberchannel_wait_event((*this));
        }
        auto ev_guard = this->pending_recv_event_.lock().unwrap();
        (*ev_guard) = rusty::None;
    }
    return rusty::None;
}

void FiberChannel::on_inbound_frame(const ChannelFrame& f) {
    auto copy = fiberchannel_owned_copy(f);
    {
        auto guard = this->queue_.lock().unwrap();
        ((*guard)).push_back(std::move(copy));
    }
    fiberchannel_signal_pending_recv((*this));
}

void FiberChannel::on_inbound_closed() {
    this->closed_.set(true);
    fiberchannel_signal_pending_recv((*this));
}

ChannelError FiberChannel::send_frame(const ChannelFrame& f) {
    return fiberchannel_send_frame((*this), f);
}

void FiberChannel::close() {
    fiberchannel_close((*this));
}

bool FiberChannel::is_closed() const {
    if (this->closed_.get()) {
        return true;
    }
    const rusty::Box<ChannelConnectionBase>& ch = this->ch_;
    return ch->is_closed();
}

ChannelConnectionProxy& FiberChannel::channel_for_test() {
    return this->ch_;
}

FiberChannel::~FiberChannel() noexcept(false) {
    if (_rusty_forgotten) { return; }
    rusty::Box<ChannelConnectionBase>& ch = this->ch_;
    ch->set_on_frame(empty_on_frame_callback());
    ch->set_on_closed(empty_on_closed_callback());
    ch->set_on_error(empty_on_error_callback());
}
/*RUSTYCPP:GEN-END id=fiber_channel.fiber_channel*/

}  // export namespace rrr

// @safe - impl namespace. Out-of-class definitions inherit their
// per-method `// @safe` / `// @unsafe` from the matching declarations
// in the export namespace above.
namespace rrr {

// Forward decl for the signal path (Arc clone + IntEvent::set
// arrow-deref; called from the DSL on_inbound_* methods).
void fiberchannel_signal_pending_recv(FiberChannel& self);

// @unsafe - `ch_->set_on_*` driven through the proxy deref + rusty::Function
// ctor chain on three captured `[self_ptr]` lambdas. `self_ptr == &self` is
// pinned because callers hold FiberChannel inside a `rusty::Box` (or on the
// test stack) for the proxy's lifetime (which the FiberChannel owns).

// @unsafe - `ch_->set_on_*({})` detach driven through the proxy deref. This
// is the former `~FiberChannel`, now reached via `impl Drop`.

// @unsafe - raw `const uint8_t*` + `memcpy` + `set_len` byte-copy
// (rusty::Vec has no `.assign(iter, iter)` so we reserve, memcpy, then
// commit the new length).
OwnedFrame fiberchannel_owned_copy(const ChannelFrame& f) {
    OwnedFrame copy;
    if (f.size > 0) {
        copy.bytes.reserve(f.size);
        std::memcpy(copy.bytes.data(), f.payload, f.size);
        copy.bytes.set_len(f.size);
    }
    return copy;
}

void fiberchannel_signal_pending_recv(FiberChannel& self) {
    // The waiter handle is shared between the reactor thread (recv_frame
    // arm/disarm) and the callback thread (this fn, which the in-memory
    // backend delivers synchronously on the SENDER's thread). Clone the
    // Arc out under the dedicated mutex so a concurrent arm/disarm can't
    // race the field read/free, then release the lock BEFORE set() (set()
    // may re-enter the reactor — holding the lock across it is a hazard).
    rusty::Option<rusty::Arc<IntEvent>> held{rusty::None};
    {
        auto guard = self.pending_recv_event_.lock().unwrap();
        held = (*guard).clone();
    }
    if (held.is_some()) {
        auto event = held.unwrap();  // owned Arc<IntEvent>, keeps it alive
        // @unsafe { IntEvent::set is not annotated @safe yet. }
        event->set(1);
    }
}

// @unsafe - Mutex + Arc make + store. Arms the single-waiter event under
// the dedicated mutex (called only on the reactor thread from recv_frame).
void fiberchannel_arm(FiberChannel& self) {
    auto guard = self.pending_recv_event_.lock().unwrap();
    (*guard) = rusty::Option<rusty::Arc<IntEvent>>(fiberchannel_make_event());
}

// @unsafe - Mutex + store None. Clears the waiter under the dedicated
// mutex after the wait completes (reactor thread only).

// @unsafe - proxy deref through `ch_->send_frame(f)`.
ChannelError fiberchannel_send_frame(FiberChannel& self, const ChannelFrame& f) {
    return self.ch_->send_frame(f);
}

// @unsafe - proxy deref through `ch_->close()`.
void fiberchannel_close(FiberChannel& self) {
    self.ch_->close();
}

// @unsafe - Mutex lock + move-out-of-deque (the DSL cannot spell a
// container front()-move); one lock covers test+move+pop.
rusty::Option<OwnedFrame> fiberchannel_try_pop(FiberChannel& self) {
    auto guard = self.queue_.lock().unwrap();
    if ((*guard).empty()) {
        return rusty::None;
    }
    OwnedFrame f = std::move((*guard).front());
    (*guard).pop_front();
    return rusty::Some(std::move(f));
}

// @unsafe - Reactor template factory + Arc hand-off.
rusty::Arc<IntEvent> fiberchannel_make_event() {
    return Reactor::create_sp_event<IntEvent>();
}

// @unsafe - fiber-suspending Event::wait through the Arc (a defensive
// Arc clone keeps the event alive across the suspend even if the field
// is reset concurrently).
void fiberchannel_wait_event(FiberChannel& self) {
    // Clone the waiter out under the mutex, then suspend OUTSIDE the lock
    // (holding the mutex across a fiber yield would deadlock the reactor).
    // The owned Arc keeps the event alive across the suspend even if a
    // signal disarms the field concurrently.
    rusty::Option<rusty::Arc<IntEvent>> held{rusty::None};
    {
        auto guard = self.pending_recv_event_.lock().unwrap();
        held = (*guard).clone();
    }
    if (held.is_some()) {
        auto event = held.unwrap();
        event->wait();
    }
}

}  // namespace rrr
