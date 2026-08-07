module;

#include <rusty/arc.hpp>
#include <rusty/function.hpp>

export module rrr.callback_wrapper;

import std;

// @safe - thin wrapper around `rusty::Arc<F>` (F = the callable type).
// Every method just forwards into rusty types whose `// @safe`
// annotations are already in the rusty-cpp library. No raw pointers,
// syscalls, or Marshal chains.
export namespace rrr {
// @safe - see file header.
namespace detail {

template<typename F>
struct CallbackWrapper {
    rusty::Arc<F> inner;

    CallbackWrapper()
        : inner(rusty::Arc<F>::make()) {}

    template<typename Callable,
             typename = std::enable_if_t<
                 !std::is_same_v<std::decay_t<Callable>, CallbackWrapper> &&
                 std::is_constructible_v<F, Callable&&>
             >>
    CallbackWrapper(Callable&& c)
        : inner(rusty::Arc<F>::make(std::forward<Callable>(c))) {}

    // The four copy/move special members were explicitly `= default`ed
    // here; they are redundant and have been removed. There is no
    // user-declared destructor and a constructor TEMPLATE is never a
    // copy/move constructor, so all four are implicitly generated, and
    // the move pair stays noexcept because rusty::Arc's move ctor and
    // move assignment are themselves noexcept. Machine-checked: a
    // side-by-side harness over the real `rusty::Function<void(int) const>`
    // signature static_asserts identical is_copy/move_constructible,
    // is_copy/move_assignable, the nothrow forms, is_default_constructible
    // and is_trivially_destructible.
    //
    // CAVEAT if you ever add a destructor to this struct: a user-declared
    // destructor SUPPRESSES the implicit move operations, which would
    // silently degrade the live `*guard = std::move(cb)` sites in
    // tcp_channel.cpp from moves to copies. The explicit defaults used to
    // protect against that. Re-add them if a destructor ever appears.

    explicit operator bool() const noexcept {
        return static_cast<bool>(*inner);
    }

    template<typename... Args>
    auto operator()(Args&&... args) const
        -> decltype((*inner)(std::forward<Args>(args)...)) {
        return (*inner)(std::forward<Args>(args)...);
    }
};

}  // namespace detail
}  // export namespace rrr
