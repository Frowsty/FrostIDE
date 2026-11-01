// SPDX-License-Identifier: Apache-2.0
//
// frost::function_ref<R(Args...)>: a non-owning, trivially copyable reference
// to any callable, modelled on C++26 std::function_ref (P0792).
//
// Polyfill: neither MSVC STL 14.44 nor libstdc++ 14 ships std::function_ref.
// Removal condition: both supported standard libraries define
// __cpp_lib_function_ref (then this header aliases the standard type).
//
// Lifetime: function_ref never owns its target. The referenced callable must
// outlive every call; binding a temporary lambda and storing the function_ref
// past the full-expression dangles. Pass function_ref as a parameter, never
// store it in a member that outlives the call.
#pragma once

#include "frost/compat/features.hpp"

#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

#if FROST_HAS_STD_FUNCTION_REF

namespace frost {
using std::function_ref;
} // namespace frost

#else

namespace frost {

template<class Signature>
class function_ref;

// The thunks call std::invoke_r<R>, which (like std::function_ref) discards the
// target's result when R is void and converts it to R otherwise.
template<class R, class... Args>
class function_ref<R(Args...)> {
public:
    // Binds a function pointer. Precondition: `function` is not null (as for
    // std::function_ref); a null pointer is undefined behaviour at call time. The
    // pointer itself is stored, so it may be a temporary.
    template<class F>
        requires std::is_function_v<F> && std::is_invocable_r_v<R, F*, Args...>
    function_ref(F* function) noexcept // NOLINT(google-explicit-constructor): implicit like std::function_ref
        : m_bound{.function = reinterpret_cast<void (*)()>(function)}, m_thunk([](Bound bound, Args... args) -> R {
              return std::invoke_r<R>(reinterpret_cast<F*>(bound.function), std::forward<Args>(args)...);
          }) {}

    // Binds any other callable object by address; the object must outlive the function_ref.
    template<class F>
        requires(!std::is_same_v<std::remove_cvref_t<F>, function_ref> &&
                 !std::is_member_pointer_v<std::remove_cvref_t<F>> && !std::is_function_v<std::remove_reference_t<F>> &&
                 std::is_invocable_r_v<R, std::remove_reference_t<F>&, Args...>)
    // Implicit like std::function_ref; it binds the callable by address, so it never forwards it.
    // NOLINTNEXTLINE(google-explicit-constructor, cppcoreguidelines-missing-std-forward): like std::function_ref
    function_ref(F&& callable) noexcept
        : m_bound{.object = const_cast<void*>(static_cast<const void*>(std::addressof(callable)))},
          m_thunk([](Bound bound, Args... args) -> R {
              using Object = std::remove_reference_t<F>;
              return std::invoke_r<R>(*static_cast<Object*>(bound.object), std::forward<Args>(args)...);
          }) {}

    function_ref(const function_ref&) noexcept = default;
    function_ref& operator=(const function_ref&) noexcept = default;

    // Invokes the bound callable. O(1): one indirect call.
    R operator()(Args... args) const { return m_thunk(m_bound, std::forward<Args>(args)...); }

private:
    // Object pointers and function pointers may differ in size, so both have a slot.
    union Bound {
        void* object;
        void (*function)();
    };

    Bound m_bound;
    R (*m_thunk)(Bound, Args...);
};

} // namespace frost

#endif
