#pragma once

#include <memory>
#include <type_traits>
#include <utility>

template <typename Signature>
class FunctionRef;

template <typename R, typename... Args>
class FunctionRef<R(Args...)> {
    void* context = nullptr;
    R (*invoker)(void*, Args...) = nullptr;

public:
    FunctionRef() = default;

    template <typename F>
        requires (!std::is_same_v<std::remove_cv_t<F>, FunctionRef> &&
                  std::is_invocable_r_v<R, F&, Args...>)
    FunctionRef(F& callable) noexcept
        : context(const_cast<void*>(static_cast<const void*>(std::addressof(callable)))),
          invoker([](void* ctx, Args... args) -> R {
              return (*static_cast<F*>(ctx))(std::forward<Args>(args)...);
          }) {}

    explicit operator bool() const noexcept { return invoker != nullptr; }

    R operator()(Args... args) const {
        return invoker(context, std::forward<Args>(args)...);
    }
};
