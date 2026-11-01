// SPDX-License-Identifier: Apache-2.0
//
// Test support: installs one of frost::storage's white-box test hooks (src/storage/src/
// test_hooks.hpp) for the lifetime of a scope.
#pragma once

#include "test_hooks.hpp"

namespace frost::storage::test {

// Installs `function(context)` on `hook` until destruction. The hook must not be installed already.
class HookGuard {
public:
    HookGuard(detail::TestHook& hook, void (*function)(void*), void* context) : m_hook(hook) {
        m_hook.install(function, context);
    }
    ~HookGuard() { m_hook.clear(); }
    HookGuard(const HookGuard&) = delete ("installs one hook");
    HookGuard& operator=(const HookGuard&) = delete ("installs one hook");

private:
    detail::TestHook& m_hook;
};

} // namespace frost::storage::test
