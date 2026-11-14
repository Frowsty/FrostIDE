// SPDX-License-Identifier: Apache-2.0
//
// UX-1 keymap coverage: every action id a built-in keymap (IntelliJ, Visual Studio) binds on a
// supported platform (Windows, Linux) resolves to an action the workbench registers, so no
// shortcut is silently dead. The ids still missing are listed in kKnownGaps, owned by UX-3; the
// list may only shrink: the test fails when a new binding has no action, when a listed id gets
// registered (remove it from the list) and when a listed id is no longer bound anywhere. The
// registry is the real one of a WorkbenchRig (offscreen, a scratch work directory, no project).

#include "frost/input/action.hpp"
#include "frost/input/keymap.hpp"

#include "app/workbench_rig.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(_WIN32) && (defined(__SANITIZE_ADDRESS__) || __has_feature(address_sanitizer))
#define FROST_WINDOWS_ASAN_IMPORT_TEST 1
#include <windows.h>
#endif

namespace {

/*
 * Bound ids with no registered action yet, sorted. Owner: UX-3 (implement the action or drop the
 * binding). Never add to this list: a new binding needs its action in the same change.
 */
constexpr std::array<std::string_view, 0> kKnownGaps{};
// UX-3 closed all 52 gaps: no built-in keymap binding may lack a registered action.
static_assert(kKnownGaps.empty(), "kKnownGaps must stay empty: register the action in the same change");

// The supported platforms (macOS is unsupported until a test leg exists).
constexpr std::array kPlatforms{frost::input::KeymapPlatform::Windows, frost::input::KeymapPlatform::Linux};

// Every id bound to at least one sequence, with the keymaps that bind it ("intellij/windows", ...).
std::map<std::string, std::set<std::string>> boundIds() {
    std::map<std::string, std::set<std::string>> out;
    for (const std::string_view id : frost::input::builtinKeymapIds()) {
        auto keymap = frost::input::builtinKeymap(id);
        EXPECT_TRUE(keymap.has_value()) << id << ": " << keymap.error().describe();
        if (!keymap) {
            continue;
        }
        for (const frost::input::KeymapPlatform platform : kPlatforms) {
            const std::string where =
                std::string(id) + (platform == frost::input::KeymapPlatform::Windows ? "/windows" : "/linux");
            for (const frost::input::Binding& binding : keymap->bindings(platform)) {
                if (!binding.sequences.empty()) {
                    out[binding.actionId].insert(where);
                }
            }
        }
    }
    return out;
}

#if defined(FROST_WINDOWS_ASAN_IMPORT_TEST)
// This test executable imports Qt DLLs, whose CRT startup can allocate before ASan hooks
// exist, so it checks that the ASan runtime import comes first (frost_prepend_asan_runtime).
TEST(KeymapCoverage, AddressSanitizerRuntimeIsFirstImport) {
    const auto* image = reinterpret_cast<const unsigned char*>(GetModuleHandleW(nullptr));
    ASSERT_NE(image, nullptr);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
    ASSERT_EQ(dos->e_magic, IMAGE_DOS_SIGNATURE);
    const auto* headers = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image + dos->e_lfanew);
    ASSERT_EQ(headers->Signature, IMAGE_NT_SIGNATURE);
    const auto& imports = headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    ASSERT_NE(imports.VirtualAddress, 0U);
    const auto* first = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(image + imports.VirtualAddress);
    ASSERT_NE(first->Name, 0U);
    EXPECT_STREQ(reinterpret_cast<const char*>(image + first->Name), "clang_rt.asan_dynamic-x86_64.dll");
}
#endif

TEST(KeymapCoverage, EveryBuiltinBindingResolvesToARegisteredAction) {
    ASSERT_TRUE(std::ranges::is_sorted(kKnownGaps));
    ASSERT_EQ(std::ranges::adjacent_find(kKnownGaps), kKnownGaps.end());
    frost::app::test::WorkDir dir("keymap-coverage");
    // The product's defaults register the Git actions (no project is opened, so git never runs);
    // clangd stays off, its actions are registered without a server.
    frost::app::test::RigOptions options;
    options.workbench.git = true;
    frost::app::test::WorkbenchRig rig(dir, std::move(options));
    const frost::input::ActionRegistry& registry = rig.workbench->registry();
    const auto bound = boundIds();
    ASSERT_FALSE(bound.empty());

    std::vector<std::string> unregistered;
    for (const auto& [id, keymaps] : bound) {
        const bool registered = registry.find(id).has_value();
        const bool known = std::ranges::binary_search(kKnownGaps, std::string_view(id));
        if (!registered && !known) {
            std::string where;
            for (const std::string& keymap : keymaps) {
                where += (where.empty() ? "" : ", ") + keymap;
            }
            ADD_FAILURE() << id << " is bound (" << where << ") but no action is registered";
        }
        if (!registered) {
            unregistered.push_back(id);
        }
    }
    for (const std::string_view gap : kKnownGaps) {
        EXPECT_FALSE(registry.find(gap).has_value())
            << gap << " is registered now: remove it from kKnownGaps (the list only shrinks)";
        EXPECT_TRUE(bound.contains(std::string(gap)))
            << gap << " is no longer bound by any built-in keymap: remove it from kKnownGaps";
    }
    // The current list, for the failure output when it has to change.
    std::string list;
    for (const std::string& id : unregistered) {
        list += "    \"" + id + "\",\n";
    }
    EXPECT_EQ(unregistered.size(), kKnownGaps.size())
        << "unregistered bound ids (" << unregistered.size() << " of " << bound.size() << "):\n"
        << list;
}

} // namespace
