// SPDX-License-Identifier: Apache-2.0
//
// REF-2: selectable language-server edits with immutable before/after previews.
#pragma once

#include "frost/lang/workspace_edit.hpp"

#include <QDialog>

#include <functional>
#include <string>
#include <string_view>
#include <vector>

class QLabel;
class QPlainTextEdit;
class QPushButton;
class QTreeWidget;

namespace frost::ui::lang {

/*
 * UI-thread-only dialog. The reader returns current editor text when open, otherwise disk text,
 * and must outlive the dialog. Captures every file on construction, shows only real supplied
 * edits, and checks selected files again before accepting. No writes occur here; the caller
 * applies selectedEdit() through its transaction service after accepted(). Capturing is O(text +
 * edits); each selection refresh copies selected edits and renders the selected file. Validation
 * rereads selected files and matches entries in O(files squared + text + edits log edits).
 */
class RefactoringPreview final : public QDialog {
public:
    using ReadText = std::function<Result<std::string>(std::string_view)>;
    RefactoringPreview(::frost::lang::WorkspaceEdit edit, ::frost::lang::PositionEncoding encoding,
                       ReadText reader, QWidget* parent = nullptr);
    // Keeps file versions, original ranges and server ordering; excludes unchecked edits/files.
    [[nodiscard]] ::frost::lang::WorkspaceEdit selectedEdit() const;
    // Rechecks captured source bytes. Busy means the caller must request a fresh rename.
    [[nodiscard]] Result<void> validateSelection() const;
    void accept() override;

private:
    void refresh();
    ::frost::lang::WorkspaceEdit m_edit;
    ::frost::lang::PositionEncoding m_encoding;
    ReadText m_reader;
    std::vector<std::string> m_before;
    std::string m_error;
    QTreeWidget* m_tree = nullptr;
    QPlainTextEdit* m_original = nullptr;
    QPlainTextEdit* m_proposed = nullptr;
    QLabel* m_status = nullptr;
    QPushButton* m_apply = nullptr;
};

} // namespace frost::ui::lang
