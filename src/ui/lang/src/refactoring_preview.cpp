// SPDX-License-Identifier: Apache-2.0
//
// REF-2 preview selection and stale-source refusal; application belongs to the transaction owner.
#include "frost/ui/lang/refactoring_preview.hpp"

#include <QDialogButtonBox>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSplitter>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <cstddef>
#include <utility>

namespace frost::ui::lang {
namespace fl = ::frost::lang;
namespace {
QString display(std::string_view text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}
} // namespace

RefactoringPreview::RefactoringPreview(fl::WorkspaceEdit edit, fl::PositionEncoding encoding,
                                     ReadText reader, QWidget* parent)
    : QDialog(parent), m_edit(std::move(edit)), m_encoding(encoding), m_reader(std::move(reader)) {
    setObjectName(QStringLiteral("refactoringPreview"));
    setWindowTitle(tr("Refactoring Preview"));
    resize(1000, 680);
    auto* layout = new QVBoxLayout(this);
    m_tree = new QTreeWidget(this);
    m_tree->setObjectName(QStringLiteral("refactoringEdits"));
    m_tree->setHeaderLabels({tr("Include / Location"), tr("Replacement")});
    layout->addWidget(m_tree, 1);
    auto* panes = new QSplitter(Qt::Horizontal, this);
    const auto pane = [&](const QString& title, const QString& name) {
        auto* container = new QWidget(panes);
        auto* column = new QVBoxLayout(container);
        column->addWidget(new QLabel(title, container));
        auto* text = new QPlainTextEdit(container);
        text->setObjectName(name);
        text->setReadOnly(true);
        text->setLineWrapMode(QPlainTextEdit::NoWrap);
        column->addWidget(text);
        panes->addWidget(container);
        return text;
    };
    m_original = pane(tr("Before"), QStringLiteral("refactoringBefore"));
    m_proposed = pane(tr("After selected edits"), QStringLiteral("refactoringAfter"));
    layout->addWidget(panes, 2);
    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    layout->addWidget(m_status);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Apply | QDialogButtonBox::Cancel, this);
    m_apply = buttons->button(QDialogButtonBox::Apply);
    m_apply->setObjectName(QStringLiteral("applyRefactoring"));
    connect(m_apply, &QPushButton::clicked, this, &RefactoringPreview::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    m_before.reserve(m_edit.files.size());
    for (const auto& file : m_edit.files) {
        auto content = m_reader(file.path);
        if (!content && m_error.empty()) {
            m_error = file.path + ": " + std::string(content.error().message());
        }
        m_before.push_back(content ? std::move(*content) : std::string{});
        auto* row = new QTreeWidgetItem(m_tree, {display(file.path)});
        row->setFlags(row->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsAutoTristate);
        for (const auto& change : file.edits) {
            auto* child = new QTreeWidgetItem(row, {
                tr("Line %1, column %2").arg(change.range.start.line + 1).arg(change.range.start.character + 1),
                display(change.newText)});
            child->setFlags(child->flags() | Qt::ItemIsUserCheckable);
            child->setCheckState(0, Qt::Checked);
        }
        row->setExpanded(true);
    }
    connect(m_tree, &QTreeWidget::itemChanged, this, [this] { refresh(); });
    connect(m_tree, &QTreeWidget::currentItemChanged, this, [this] { refresh(); });
    if (m_tree->topLevelItemCount() > 0) {
        m_tree->setCurrentItem(m_tree->topLevelItem(0));
    }
    refresh();
}

fl::WorkspaceEdit RefactoringPreview::selectedEdit() const {
    fl::WorkspaceEdit selected;
    for (std::size_t i = 0; i < m_edit.files.size(); ++i) {
        auto file = m_edit.files[i];
        file.edits.clear();
        const auto* row = m_tree->topLevelItem(static_cast<int>(i));
        for (std::size_t j = 0; j < m_edit.files[i].edits.size(); ++j) {
            if (row->child(static_cast<int>(j))->checkState(0) == Qt::Checked) {
                file.edits.push_back(m_edit.files[i].edits[j]);
            }
        }
        if (!file.edits.empty()) {
            selected.files.push_back(std::move(file));
        }
    }
    return selected;
}

Result<void> RefactoringPreview::validateSelection() const {
    if (!m_error.empty()) {
        return fail(ErrorCode::InvalidArgument, m_error);
    }
    const auto selected = selectedEdit();
    if (selected.empty()) {
        return fail(ErrorCode::InvalidArgument, "Select at least one edit");
    }
    for (std::size_t i = 0; i < m_edit.files.size(); ++i) {
        const auto* row = m_tree->topLevelItem(static_cast<int>(i));
        if (row->checkState(0) == Qt::Unchecked) {
            continue;
        }
        auto current = m_reader(m_edit.files[i].path);
        if (!current) {
            return std::unexpected(current.error().withContext(m_edit.files[i].path));
        }
        if (*current != m_before[i]) {
            return fail(ErrorCode::Busy, "Source changed while preview was open: " + m_edit.files[i].path +
                                             ". Request a new refactoring preview.");
        }
    }
    for (const auto& file : selected.files) {
        for (std::size_t i = 0; i < m_edit.files.size(); ++i) {
            if (m_edit.files[i].path == file.path) {
                auto proposed = fl::applyTextEdits(m_before[i], file.edits, m_encoding);
                if (!proposed) {
                    return std::unexpected(proposed.error().withContext(file.path));
                }
                break;
            }
        }
    }
    return {};
}

void RefactoringPreview::accept() {
    if (auto valid = validateSelection(); !valid) {
        m_status->setText(display(valid.error().message()));
        return;
    }
    QDialog::accept();
}

void RefactoringPreview::refresh() {
    const auto selected = selectedEdit();
    m_apply->setEnabled(m_error.empty() && !selected.empty());
    m_status->setText(m_error.empty() ? tr("%1 selected edits in %2 files")
                         .arg(static_cast<qulonglong>(selected.editCount()))
                         .arg(static_cast<qulonglong>(selected.files.size())) : display(m_error));
    auto* row = m_tree->currentItem();
    if (!row) {
        return;
    }
    if (row->parent()) {
        row = row->parent();
    }
    const auto index = static_cast<std::size_t>(m_tree->indexOfTopLevelItem(row));
    m_original->setPlainText(display(m_before[index]));
    m_proposed->setPlainText(display(m_before[index]));
    for (const auto& file : selected.files) {
        if (file.path != m_edit.files[index].path) {
            continue;
        }
        auto after = fl::applyTextEdits(m_before[index], file.edits, m_encoding);
        if (!after) {
            m_status->setText(display(after.error().message()));
            m_apply->setEnabled(false);
        } else {
            m_proposed->setPlainText(display(*after));
        }
        break;
    }
}

} // namespace frost::ui::lang
