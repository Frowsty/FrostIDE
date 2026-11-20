// SPDX-License-Identifier: Apache-2.0
// Owned Roslyn semantic groups enter one trust/source-pinned REF1 preview and global Undo transaction.
#include "frost/app/workbench.hpp"
#include "frost/io/path.hpp"
#include "frost/lang/managed_service.hpp"
#include "frost/lang/uri.hpp"
#include "frost/text/text_file.hpp"

#include "language_bridge.hpp"
#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QTabWidget>
#include <QVBoxLayout>

#include <map>
#include <set>
namespace frost::app {
namespace {
bool within(std::string_view path, std::string_view root) {
    const auto key = lang::documentKey(path), base = lang::documentKey(root);
    return key == base ||
           (key.size() > base.size() && key.starts_with(base) && (key[base.size()] == '/' || key[base.size()] == '\\'));
}
bool previewManaged(const document::RefactoringPlan& plan, QWidget* parent) {
    QPointer<QDialog> dialog = new QDialog(parent);
    dialog->setWindowTitle(QString::fromUtf8(plan.label));
    auto* layout = new QVBoxLayout(dialog);
    layout->addWidget(new QLabel(
        QStringLiteral(
            "Complete compiler proposal. Apply rechecks captured inputs and commits one global Undo operation."),
        dialog));
    auto* tabs = new QTabWidget(dialog);
    const auto add = [&](std::string_view path, std::string_view before, std::string_view after) {
        auto* image = new QPlainTextEdit(dialog);
        image->setReadOnly(true);
        image->setPlainText(
            QStringLiteral("BEFORE\n") + QString::fromUtf8(before.data(), static_cast<qsizetype>(before.size())) +
            QStringLiteral("\nAFTER\n") + QString::fromUtf8(after.data(), static_cast<qsizetype>(after.size())));
        tabs->addTab(image, QString::fromUtf8(path.data(), static_cast<qsizetype>(path.size())));
    };
    for (const auto& file : plan.buffers)
        add(file.path, file.expectedText, file.afterText);
    for (const auto& file : plan.disk)
        add(file.path, file.expectedBytes, file.afterBytes);
    for (const auto& file : plan.creates)
        add(file.path, "", file.afterBytes);
    for (const auto& file : plan.renames)
        add(file.from + " -> " + file.to, file.expectedBytes, file.expectedBytes);
    layout->addWidget(tabs);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Apply | QDialogButtonBox::Cancel, dialog);
    QObject::connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog->resize(900, 650);
    const auto accepted = dialog->exec() == QDialog::Accepted;
    if (!dialog)
        return false;
    delete dialog.data();
    return accepted;
}
} // namespace
Result<void> LanguageBridge::applyManagedProposal(const lang::WorkspaceEdit& edit, std::string_view label) {
    auto& workbench = m_workbench;
    if (!edit.managed || !workbench.m_managed || !isManagedSource(documentPath()))
        return fail(ErrorCode::Unsupported, "Managed proposals need the active owned C# service");
    if (auto targets = validateRefactoringTargets(edit); !targets)
        return targets;
    const QPointer<Workbench> owner(&workbench);
    const auto root = workbench.m_projectRoot;
    const auto trust = workbench.m_services.trust.snapshot(root);
    const auto server = workbench.m_managed;
    const auto restarts = server->restarts();
    const auto evaluation = workbench.m_managedEvaluation;
    if (!trust.trusted || server->state() != lang::LuaState::Running)
        return fail(ErrorCode::Untrusted, "Trust the evaluated C# workspace before refactoring");
    auto& fs = workbench.m_services.fs;
    const auto physicalRoot = fs.physicalPath(root);
    if (!physicalRoot)
        return std::unexpected(physicalRoot.error());
    document::RefactoringPlan plan;
    plan.label = std::string(label);
    std::map<std::string, std::string> images, disk;
    std::map<std::string, std::pair<const document::FileDocument*, std::uint64_t>> stamps;
    std::map<std::string, std::string> physical;
    std::set<std::string> layout;
    for (const auto& doc : workbench.m_documents->documents())
        if (!doc->path().empty())
            layout.insert(lang::documentKey(doc->path()));
    for (const auto& input : edit.managed->inputs) {
        const auto key = lang::documentKey(input.path);
        auto resolved = fs.physicalPath(input.path);
        if (!resolved || !within(*resolved, *physicalRoot))
            return fail(ErrorCode::PermissionDenied, "Managed source escapes the physical workspace");
        if (!images.emplace(key, input.text).second)
            return fail(ErrorCode::InvalidArgument, "Duplicate managed source receipt");
        physical.emplace(input.path, *resolved);
        if (auto* doc = workbench.m_documents->find(input.path)) {
            const auto text = doc->document().text();
            if (text.text() != input.text)
                return fail(ErrorCode::Busy, "Managed source changed; request a new action");
            stamps.emplace(input.path, std::pair{doc, static_cast<std::uint64_t>(text.version())});
            plan.bufferDependencies.push_back({input.path, input.text});
        } else {
            auto bytes = fs.readFileSnapshot(input.path, 4U * 1024U * 1024U);
            if (!bytes)
                return std::unexpected(bytes.error());
            auto decoded = text::decodeTextFile(*bytes);
            if (!decoded || decoded->buffer.current().text() != input.text)
                return fail(ErrorCode::Busy, "Managed disk input changed; request a new action");
            plan.dependencies.push_back({input.path, *bytes});
            disk.emplace(key, std::move(*bytes));
        }
    }
    std::set<std::string> mutations;
    for (const auto& file : edit.files) {
        const auto key = lang::documentKey(file.path);
        const auto image = images.find(key);
        if (image == images.end() || !mutations.insert(key).second)
            return fail(ErrorCode::InvalidArgument, "Uncaptured or duplicate managed edit");
        auto after = lang::applyTextEdits(image->second, file.edits, lang::PositionEncoding::Utf16);
        if (!after)
            return std::unexpected(after.error());
        if (auto* doc = workbench.m_documents->find(file.path)) {
            if (file.version && server->documentVersion(file.path) != file.version)
                return fail(ErrorCode::Busy, "Managed source version changed");
            plan.buffers.push_back({doc->path(), image->second, *after, document::BufferPersistence::Automatic});
        } else {
            auto decoded = text::decodeTextFile(disk.at(key));
            auto buffer = text::TextBuffer::fromUtf8(std::move(*after));
            if (!decoded || !buffer)
                return fail(ErrorCode::Corrupt, "Managed source cannot round-trip its encoding");
            auto encoded = text::encodeForSave(buffer->current(), decoded->encoding);
            if (!encoded)
                return std::unexpected(encoded.error());
            plan.disk.push_back({file.path, disk.at(key), std::move(*encoded)});
        }
    }
    const auto endpoint = [&](std::string_view path) -> Result<void> {
        auto parent = fs.physicalPath(io::parentPath(path));
        if (!parent || !within(*parent, *physicalRoot))
            return fail(ErrorCode::PermissionDenied, "Managed endpoint escapes workspace");
        physical.emplace(std::string(io::parentPath(path)), *parent);
        if (workbench.m_documents->find(path))
            return fail(ErrorCode::AlreadyExists, "Managed endpoint is already open");
        return {};
    };
    for (const auto& creation : edit.managed->creates) {
        if (!mutations.insert(lang::documentKey(creation.path)).second)
            return fail(ErrorCode::InvalidArgument, "Overlapping managed endpoint");
        if (auto admitted = endpoint(creation.path); !admitted)
            return admitted;
        plan.creates.push_back({creation.path, creation.text});
    }
    for (const auto& move : edit.managed->renames) {
        const auto source = lang::documentKey(move.from), target = lang::documentKey(move.to);
        if (source == target || !images.contains(source) || !mutations.insert(source).second ||
            !mutations.insert(target).second)
            return fail(ErrorCode::Unsupported, "Overlapping or case-only managed moves are unavailable");
        if (auto admitted = endpoint(move.to); !admitted)
            return admitted;
        auto bytes = fs.readFileSnapshot(move.from, 4U * 1024U * 1024U);
        if (!bytes)
            return std::unexpected(bytes.error());
        plan.renames.push_back({move.from, move.to, *bytes});
    }
    plan.revalidateContext = [owner, root, trust, server, restarts, evaluation, layout, physical, stamps,
                              physicalRoot = *physicalRoot]() -> Result<void> {
        if (!owner || owner->m_shutDown || owner->m_projectRoot != root || owner->m_managed != server ||
            server->restarts() != restarts || server->state() != lang::LuaState::Running ||
            owner->m_managedEvaluation != evaluation)
            return fail(ErrorCode::Busy, "Managed semantic context changed during preview");
        const auto current = owner->m_services.trust.snapshot(root);
        if (!current.trusted || current.revision != trust.revision)
            return fail(ErrorCode::Untrusted, "Project trust changed during managed preview");
        std::set<std::string> now;
        for (const auto& doc : owner->m_documents->documents())
            if (!doc->path().empty())
                now.insert(lang::documentKey(doc->path()));
        if (now != layout)
            return fail(ErrorCode::Busy, "Managed document layout changed during preview");
        auto currentRoot = owner->m_services.fs.physicalPath(root);
        if (!currentRoot || lang::documentKey(*currentRoot) != lang::documentKey(physicalRoot))
            return fail(ErrorCode::Busy, "Managed workspace identity changed");
        for (const auto& [path, expected] : physical) {
            auto currentPath = owner->m_services.fs.physicalPath(path);
            if (!currentPath || lang::documentKey(*currentPath) != lang::documentKey(expected))
                return fail(ErrorCode::Busy, "Managed physical input changed");
        }
        for (const auto& [path, stamp] : stamps) {
            auto* doc = owner->m_documents->find(path);
            if (!doc || doc != stamp.first || doc->document().text().version() != stamp.second)
                return fail(ErrorCode::Busy, "Managed buffer identity/version changed");
        }
        return {};
    };
    if (auto admitted = plan.revalidateContext(); !admitted)
        return admitted;
    if (!previewManaged(plan, workbench.m_window) || !owner)
        return fail(ErrorCode::Cancelled, "Managed refactoring cancelled");
    auto applied = owner->applyRefactoring(plan);
    if (!applied)
        return std::unexpected(applied.error());
    return {};
}
} // namespace frost::app
