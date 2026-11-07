// SPDX-License-Identifier: Apache-2.0
//
// clangd's code actions by name (clangd_actions.hpp): tweak ids, the Refactor This order and the
// include-cleaner fix.

#include "frost/lang/clangd_actions.hpp"

#include <algorithm>
#include <cstddef>

namespace frost::lang {
namespace {

// kTweaks' index of `id`, or kTweaks.size() for an unknown tweak.
[[nodiscard]] std::size_t tweakRank(std::string_view id) noexcept {
    for (std::size_t i = 0; i < kTweaks.size(); ++i) {
        if (kTweaks[i].id == id) {
            return i;
        }
    }
    return kTweaks.size();
}

// The tweak id an action carries, or the id kTweaks' titles name it by.
[[nodiscard]] std::string_view effectiveTweakId(const CodeActionItem& action) noexcept {
    if (const std::string_view id = tweakIdOf(action); !id.empty()) {
        return id;
    }
    if (action.commandName != kClangdApplyTweak && !action.kind.starts_with("refactor")) {
        return {};
    }
    for (const ClangdTweakInfo& tweak : kTweaks) {
        if (std::string_view(action.title).starts_with(tweak.titlePrefix)) {
            return tweak.id;
        }
    }
    return {};
}

} // namespace

std::string_view tweakIdOf(const CodeActionItem& action) noexcept {
    if (action.commandName != kClangdApplyTweak || action.commandArguments.empty()) {
        return {};
    }
    return action.commandArguments.front().stringAt("tweakID");
}

const CodeActionItem* findTweak(std::span<const CodeActionItem> actions, std::string_view id) noexcept {
    for (const CodeActionItem& action : actions) {
        if (effectiveTweakId(action) == id) {
            return &action;
        }
    }
    return nullptr;
}

std::vector<CodeActionItem> orderRefactorActions(std::vector<CodeActionItem> actions) {
    // A tweak of any kind (clangd marks Override pure virtual methods a quick fix) or a refactor.
    std::erase_if(actions, [](const CodeActionItem& action) {
        return action.commandName != kClangdApplyTweak && !action.kind.starts_with("refactor");
    });
    std::ranges::stable_sort(actions, [](const CodeActionItem& a, const CodeActionItem& b) {
        return tweakRank(effectiveTweakId(a)) < tweakRank(effectiveTweakId(b));
    });
    return actions;
}

bool isIncludeCleanerDiagnostic(const LiveDiagnostic& diagnostic) noexcept {
    return (diagnostic.source.empty() || diagnostic.source == "clangd") &&
           (diagnostic.code == "unused-includes" || diagnostic.code == "missing-includes");
}

std::optional<CodeActionItem> pickIncludeCleanerFix(std::span<const CodeActionItem> actions) {
    // clangd 23's titles (clang-tools-extra/clangd/IncludeCleaner.cpp).
    static constexpr std::array<std::string_view, 3> kAll{"fix all includes", "remove all unused includes",
                                                          "add all missing includes"};
    for (std::string_view title : kAll) {
        for (const CodeActionItem& action : actions) {
            if (action.title == title && !action.edit.empty()) {
                return action;
            }
        }
    }
    for (const CodeActionItem& action : actions) {
        const bool single = action.title == "remove #include directive" || action.title.starts_with("#include ") ||
                            action.title.starts_with("#import ");
        if (single && !action.edit.empty()) {
            return action;
        }
    }
    return std::nullopt;
}

} // namespace frost::lang
