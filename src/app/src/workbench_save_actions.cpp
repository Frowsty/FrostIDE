// SPDX-License-Identifier: Apache-2.0
//
// The workbench's actions on save (REF-12, frost/lang/save_actions.hpp): before a C or C++ document
// is written, "Optimize includes on save" applies clangd's include-cleaner fix and "Reformat code
// on save" applies clangd's formatting (clang-format with the file's .clang-format), each as one
// undo step in the document's editor. Both settings are off by default, so saving is unchanged
// unless the user turns them on (globally or for the project).
/*
 * Truth rule (ADR 0078): every edit comes from clangd's answer. Without a running clangd, without a
 * timely answer (kSaveActionTimeout), or with diagnostics that describe an older text than the one
 * being saved (the include cleaner), the action is skipped and a warning notification says why;
 * the file is then saved as it is. A failed action never blocks the save.
 *
 * Threads: GUI thread only. The requests run on the workbench's pool; the GUI thread waits for
 * each one at most kSaveActionTimeout (saving is synchronous for Ctrl+S, Save All and the close
 * prompt). The wait is part of the save and is not hidden from the stall watchdog.
 */

#include "frost/app/workbench.hpp"
#include "frost/core/log.hpp"
#include "frost/lang/clangd.hpp"
#include "frost/lang/save_actions.hpp"
#include "frost/settings/editorconfig.hpp"
#include "frost/settings/keys.hpp"
#include "frost/ui/tr.hpp"

#include "language_bridge.hpp"
#include <QFileInfo>

#include <chrono>
#include <string>
#include <utility>
#include <vector>

namespace frost::app {

namespace {

constexpr std::string_view kLog = "workbench";
// How long a save waits for each clangd answer before saving the file unchanged.
constexpr std::chrono::milliseconds kSaveActionTimeout{3000};

QString text(const char* source) { return ui::qtr("frost::app::Workbench", source); }

QString qs(std::string_view utf8) { return QString::fromUtf8(utf8.data(), static_cast<qsizetype>(utf8.size())); }

QString displayName(std::string_view path) { return QFileInfo(qs(path)).fileName(); }

} // namespace

void Workbench::runActionsOnSave(document::FileDocument& document) {
    if (document.untitled() || !isCFamilySource(document.path())) {
        return;
    }
    const auto snapshot = m_services.settings.snapshot();
    bool format = snapshot->get(settings::key::editorFormatOnSave);
    bool optimize = snapshot->get(settings::key::editorOptimizeImportsOnSave);
    // The project's settings (and .editorconfig) for this file override the application's.
    if (auto file = settings::settingsForFile(snapshot, m_services.fs, document.path()); file) {
        format = file->get(settings::key::editorFormatOnSave);
        optimize = file->get(settings::key::editorOptimizeImportsOnSave);
    }
    if (!format && !optimize) {
        return;
    }
    DocumentEditorView* view = m_editors ? m_editors->find(document) : nullptr;
    if (view == nullptr || view->editor().isReadOnly()) {
        return;
    }
    const std::string path = document.path();
    const QString name = displayName(path);
    if (!m_clangd || m_clangd->state() != lang::ClangdState::Running) {
        notify(text("Actions on save skipped for %1").arg(name),
               text("clangd is not running, so the file was saved without reformatting or optimizing includes."),
               ui::NotificationKind::Warning);
        return;
    }
    lang::ClangdService& clangd = *m_clangd;
    const auto skipped = [&](const QString& action, const Error& error) {
        log::warn(kLog, "{} on save of {}: {}", action.toStdString(), path, error.describe());
        notify(text("%1 on save skipped for %2").arg(action, name), qs(error.message()), ui::NotificationKind::Warning);
    };

    if (optimize) {
        const text::Snapshot current = view->editor().editor().text();
        // clangd's last publish must be for the text being saved; a file it found nothing in has no
        // entry in the store, and then there is nothing to remove.
        const std::optional<std::int64_t> latest = clangd.documentVersion(path);
        const std::optional<std::int64_t> diagnosed = clangd.diagnosedVersion(path);
        const std::optional<lang::FileDiagnostics> diagnostics = clangd.diagnostics().forFile(path);
        if (!latest || diagnosed != latest || (diagnostics && diagnostics->stale)) {
            skipped(text("Optimize includes"),
                    Error(ErrorCode::Busy, "clangd's diagnostics do not describe this text yet; save again once "
                                           "the analysis finishes"));
        } else if (diagnostics) {
            auto edits = lang::waitForResult<std::vector<lang::ByteEdit>>(
                *m_pool,
                [&clangd, path, current, list = diagnostics->diagnostics](std::stop_token token) {
                    return lang::includeCleanerEditsForSave(clangd, path, current, list, std::move(token));
                },
                kSaveActionTimeout);
            if (!edits) {
                skipped(text("Optimize includes"), edits.error());
            } else if (!edits->empty()) {
                if (auto applied = m_bridge->applyEdits(path, *edits, "Optimize Includes on Save"); !applied) {
                    skipped(text("Optimize includes"), applied.error());
                }
            }
        }
    }

    if (format) {
        const text::Snapshot current = view->editor().editor().text();
        const auto& editorSettings = view->editor().settings();
        const lang::FormattingOptions options{.tabSize = static_cast<std::uint32_t>(editorSettings.insertSpaces
                                                                                        ? editorSettings.indentSize
                                                                                        : editorSettings.tabSize),
                                              .insertSpaces = editorSettings.insertSpaces};
        auto edits = lang::waitForResult<std::vector<lang::ByteEdit>>(
            *m_pool,
            [&clangd, path, current, options](std::stop_token token) {
                return lang::formatEditsForSave(clangd, path, current, options, std::move(token));
            },
            kSaveActionTimeout);
        if (!edits) {
            skipped(text("Reformat code"), edits.error());
        } else if (!edits->empty()) {
            if (auto applied = m_bridge->applyEdits(path, *edits, "Reformat Code on Save"); !applied) {
                skipped(text("Reformat code"), applied.error());
            }
        }
    }
}

} // namespace frost::app
