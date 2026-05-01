#pragma once

// Thin wrapper struct that backs a `std::function<Sig>`-style callback
// with `rusty::Arc<rusty::Function<Sig>>` storage.  Used wherever rrr
// stores user callables that must be:
//
//   * default-constructible (the unset state),
//   * copyable (so they can sit inside copyable aggregates and ride
//     through copy-out-of-lock invocation patterns),
//   * implicitly constructible from any compatible callable (lambdas,
//     std::function, etc., to keep the existing call surface),
//   * checkable via `operator bool` (`true` iff a callable is set),
//   * invocable via `operator()` (forwarded to the inner Function).
//
// `rusty::Function<Sig>` is move-only by design (matching Rust's
// `Box<dyn FnMut/Fn>` semantics) which would otherwise cripple the
// places where rrr historically copies a std::function (e.g.
// in_memory channel's `peer_on_frame = guard->b_on_frame;` snapshot,
// FutureAttr's propagation through generated `rcc_rpc.h` proxy stubs,
// the Vec<callback>-clone drains in `callbacks.hpp`).  Wrapping the
// move-only Function in an `Arc<>` makes the callback type copyable
// again — a copy is a cheap atomic refcount bump and the inner
// Function is shared, not duplicated.
//
// This header sits at the rrr base layer (no rpc/channel dependency)
// so it can be re-used by both the channel-tier callback typedefs in
// `rpc/channel.hpp` and the alock-tier callback typedefs in
// `misc/alock.hpp` — and by FutureAttr in `rpc/client.hpp`, which
// already gets the wrapper transitively through `channel.hpp`.

#include <std_compat.hpp>

#include <type_traits>
#include <utility>

#include <rusty/arc.hpp>
#include <rusty/function.hpp>

namespace rrr {
namespace detail {

template<typename Sig>
struct CallbackWrapper {
    rusty::Arc<rusty::Function<Sig>> inner;

    // Default ctor: Arc holds an empty Function (the unset state).
    // `operator bool` returns false in this state.
    CallbackWrapper()
        : inner(rusty::Arc<rusty::Function<Sig>>::make()) {}

    // Implicit construct from any callable that fits the const-call
    // signature; mirrors the prior `std::function<Sig>(callable)`
    // convenience.
    template<typename Callable,
             typename = std::enable_if_t<
                 !std::is_same_v<std::decay_t<Callable>, CallbackWrapper> &&
                 std::is_constructible_v<rusty::Function<Sig>, Callable&&>
             >>
    CallbackWrapper(Callable&& c)
        : inner(rusty::Arc<rusty::Function<Sig>>::make(std::forward<Callable>(c))) {}

    // Copy/move follow Arc semantics (Arc is copyable; copy = atomic
    // refcount bump, the inner Function is shared, not duplicated).
    CallbackWrapper(const CallbackWrapper&) = default;
    CallbackWrapper(CallbackWrapper&&) noexcept = default;
    CallbackWrapper& operator=(const CallbackWrapper&) = default;
    CallbackWrapper& operator=(CallbackWrapper&&) noexcept = default;

    // Truthy iff the inner Function is set (not the default-empty
    // state).  Delegates to rusty::Function::operator bool.
    explicit operator bool() const noexcept {
        return static_cast<bool>(*inner);
    }

    // Forward call to the inner Function.  Calling an unset wrapper
    // aborts (matching rusty::Function::operator() on an empty
    // Function), so callers must guard with `if (cb)`.
    template<typename... Args>
    auto operator()(Args&&... args) const
        -> decltype((*inner)(std::forward<Args>(args)...)) {
        return (*inner)(std::forward<Args>(args)...);
    }
};

}  // namespace detail
}  // namespace rrr
