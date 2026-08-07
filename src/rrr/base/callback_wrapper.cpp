module;

#include <rusty/arc.hpp>
#include <rusty/function.hpp>

export module rrr.callback_wrapper;

import std;

// @safe - thin wrapper around `rusty::Arc<rusty::Function<Sig>>`.
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

    CallbackWrapper(const CallbackWrapper&) = default;
    CallbackWrapper(CallbackWrapper&&) noexcept = default;
    CallbackWrapper& operator=(const CallbackWrapper&) = default;
    CallbackWrapper& operator=(CallbackWrapper&&) noexcept = default;

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
