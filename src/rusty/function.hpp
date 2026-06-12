#ifndef MAKO_RUSTY_FUNCTION_HPP
#define MAKO_RUSTY_FUNCTION_HPP

#include <memory>
#include <type_traits>
#include <utility>

namespace rusty {

template<typename Signature>
class Function;

template<typename R, typename... Args>
class Function<R(Args...)> {
private:
    struct CallableBase {
        virtual ~CallableBase() = default;
        virtual R call(Args... args) = 0;
    };

    template<typename F>
    struct Callable final : CallableBase {
        F func;

        template<typename Fn>
        explicit Callable(Fn&& f) : func(std::forward<Fn>(f)) {}

        R call(Args... args) override {
            if constexpr (std::is_void_v<R>) {
                func(std::forward<Args>(args)...);
            } else {
                return func(std::forward<Args>(args)...);
            }
        }
    };

    std::unique_ptr<CallableBase> callable_;

public:
    Function() = default;
    Function(std::nullptr_t) {}

    template<typename F,
             typename = std::enable_if_t<!std::is_same_v<std::decay_t<F>, Function>>>
    Function(F&& f)
        : callable_(std::make_unique<Callable<std::decay_t<F>>>(std::forward<F>(f))) {}

    Function(Function&&) noexcept = default;
    Function& operator=(Function&&) noexcept = default;

    Function(const Function&) = delete;
    Function& operator=(const Function&) = delete;

    Function& operator=(std::nullptr_t) {
        callable_.reset();
        return *this;
    }

    explicit operator bool() const {
        return static_cast<bool>(callable_);
    }

    R operator()(Args... args) {
        if constexpr (std::is_void_v<R>) {
            callable_->call(std::forward<Args>(args)...);
        } else {
            return callable_->call(std::forward<Args>(args)...);
        }
    }
};

} // namespace rusty

#endif // MAKO_RUSTY_FUNCTION_HPP
