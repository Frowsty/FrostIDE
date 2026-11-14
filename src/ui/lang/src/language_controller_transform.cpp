// SPDX-License-Identifier: Apache-2.0
// REF-9/10 conversion and usage fixes come from actual provider actions, never line heuristics.
#include "frost/lang/clangd_actions.hpp"
#include "frost/ui/lang/language_controller.hpp"

#include "language_controller_internal.hpp"
namespace frost::ui::lang {
namespace fl = ::frost::lang;
void LanguageController::convertCode() { showProviderTransformations(false); }
void LanguageController::createFromUsage() { showProviderTransformations(true); }
void LanguageController::fixInScope() {
    if (capabilities().clangdTweaks && m_cppAstRefactoring)
        m_cppAstRefactoring("fix-in-scope");
    else
        unavailable(QStringLiteral("Fix in Scope requires the captured clang-tidy project integration"));
}
void LanguageController::fixPublishedDiagnostics() {
    if (capabilities().clangdTweaks && m_cppPublishedScopeFix)
        m_cppPublishedScopeFix();
    else
        unavailable(QStringLiteral("Fix in Scope requires a trusted C++ project diagnostic scope"));
}
void LanguageController::showProviderTransformations(bool creation) {
    if (!hasDocument() || !capabilities().clangdTweaks || !m_cppSemanticBegin) {
        unavailable(QStringLiteral("Open a C++ document with the semantic transaction provider"));
        return;
    }
    if (m_cppSemanticCommand) {
        unavailable(QStringLiteral("Wait for the active semantic command"));
        return;
    }
    auto captured = m_cppSemanticBegin();
    if (!captured) {
        report(captured.error(), "C++ transformations");
        return;
    }
    auto semantic = std::make_shared<SemanticApply>(std::move(*captured));
    const auto path = m_services.host.documentPath();
    fl::LspRange range{caretPosition(), caretPosition()};
    if (const auto selected = m_services.host.selection())
        range = {positionAt(selected->begin), positionAt(selected->end)};
    const auto diagnostics = m_services.diagnostics ? fl::diagnosticsAt(m_services.diagnostics(path), range)
                                                    : std::vector<fl::LiveDiagnostic>{};
    start<std::vector<fl::CodeActionItem>>(
        m_actionSlot,
        [&server = m_services.server, path, range, diagnostics](std::stop_token stop) {
            return fl::requestCodeActions(server, path, range, diagnostics, stop);
        },
        [this, semantic, creation](Result<std::vector<fl::CodeActionItem>> result) {
            if (!result) {
                report(result.error(), "C++ transformations");
                return;
            }
            if (auto current = (*semantic)({}); !current) {
                report(current.error(), "C++ transformations");
                return;
            }
            std::vector<QString> titles;
            m_choiceActions.clear();
            for (auto& action : *result) {
                const auto id = fl::tweakIdOf(action);
                const bool selected =
                    creation ? (action.kind.starts_with("quickfix") &&
                                (action.title.starts_with("Create ") || action.title.starts_with("Declare ") ||
                                 action.title.starts_with("Add declaration")))
                             : (action.kind.starts_with("refactor.rewrite") || id.starts_with("Convert") ||
                                id.starts_with("Swap") || id.starts_with("Invert") || id.starts_with("Expand") ||
                                id.starts_with("IntroduceUsing") || id == "RemoveUsingNamespace");
                if (!selected)
                    continue;
                titles.push_back(detail::qs(action.title));
                m_choiceActions.emplace_back([this, action = std::move(action), semantic]() mutable {
                    runCodeAction(std::move(action), semantic);
                });
            }
            if (titles.empty()) {
                unavailable(creation ? QStringLiteral("The provider offered no create/declaration fix at this usage")
                                     : QStringLiteral("clangd offered no applicable conversion for this selection"));
                return;
            }
            m_choice->start(creation ? QStringLiteral("Create from Usage — actual provider fixes")
                                     : QStringLiteral("C++ Conversions"),
                            titles, m_services.host.caretScreenRect());
        });
}
} // namespace frost::ui::lang
