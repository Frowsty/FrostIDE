// SPDX-License-Identifier: Apache-2.0
// REF-6/7 production actions: captured semantic helper, atomic preview, then REF-1 transaction.
#include "frost/app/workbench.hpp"
#include "frost/io/spawn_log.hpp"
#include "frost/lang/uri.hpp"
#include "frost/ui/editor/editor_view.hpp"

#include "language_bridge.hpp"
#include "trusted_project_launcher.hpp"
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMetaObject>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProgressDialog>
#include <QPushButton>
#include <QTabWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <map>
#include <set>

namespace frost::app {
namespace {
struct Capture {
    lang::CppRefactoringRequest request;
    document::RefactoringPlan plan;
    std::string database;
    std::set<std::string> openPaths;
    std::uint64_t trustRevision = 0, generation = 0;
    std::shared_ptr<lang::ClangdService> server;
    std::stop_token stop;
    std::shared_ptr<std::stop_callback<std::function<void()>>> cancellation;
};
using Prepared = Result<std::shared_ptr<Capture>>;
sched::Task<void> prepare(io::IFileSystem* fs, io::IProcessLauncher* launcher, project::TrustStore* trust,
                          io::AsyncContext context, std::string helper, std::shared_ptr<Capture> capture,
                          std::stop_token stop, std::function<void(Prepared)> done) {
    auto database = fs->readFileSnapshot(capture->database, 32U * 1024U * 1024U);
    if (!database) {
        done(std::unexpected(database.error()));
        co_return;
    }
    capture->request.context.compilationDatabase = *database;
    capture->plan.dependencies.push_back({capture->database, *database});
    TrustedProjectLauncher admitted(*launcher, *trust, capture->request.context.projectRoot, capture->trustRevision,
                                    stop);
    auto proposal = co_await lang::requestCppRefactoringPlan(admitted, context, std::move(helper), capture->request,
                                                             std::chrono::minutes(5), stop);
    if (!proposal) {
        done(std::unexpected(proposal.error()));
        co_return;
    }
    if (!proposal->ready()) {
        std::string reason = "AST refactoring refused (" + proposal->status + ")";
        for (const auto& message : proposal->diagnostics)
            reason += "\n" + message;
        done(fail(ErrorCode::Unsupported, std::move(reason)));
        co_return;
    }
    std::size_t retained = database->size();
    for (const auto& dependency : proposal->dependencies) {
        if (stop.stop_requested()) {
            done(fail(ErrorCode::Cancelled, "Refactoring cancelled"));
            co_return;
        }
        if (dependency.overlay) {
            const auto found = std::ranges::find_if(capture->request.context.overlays, [&](const auto& overlay) {
                return lang::documentKey(overlay.path) == lang::documentKey(dependency.path);
            });
            if (found == capture->request.context.overlays.end() || found->contents.size() != dependency.bytes ||
                io::sha256Hex(found->contents) != dependency.sha256) {
                done(fail(ErrorCode::Busy, "Captured editor input differs from the AST receipt"));
                co_return;
            }
            capture->plan.bufferDependencies.push_back({found->path, found->contents});
        } else {
            auto bytes = fs->readFileSnapshot(dependency.path, 64U * 1024U * 1024U);
            if (!bytes || bytes->size() != dependency.bytes || io::sha256Hex(*bytes) != dependency.sha256) {
                done(fail(ErrorCode::Busy, "Source input changed during AST analysis: " + dependency.path));
                co_return;
            }
            retained += bytes->size();
            if (retained > 512U * 1024U * 1024U) {
                done(fail(ErrorCode::OutOfRange, "Refactoring snapshots exceed 512 MiB"));
                co_return;
            }
            capture->plan.dependencies.push_back({dependency.path, std::move(*bytes)});
        }
    }
    for (const auto& path : proposal->missingDependencies)
        capture->plan.dependencies.push_back({path, std::nullopt});
    for (const auto& path : proposal->directoryDependencies)
        capture->plan.dependencies.push_back({path, std::nullopt, true});
    for (const auto& file : proposal->files) {
        if (file.overlay)
            capture->plan.buffers.push_back({file.path, file.before, file.after});
        else
            capture->plan.disk.push_back({file.path, file.before, file.after});
    }
    done(std::move(capture));
}
QString display(std::string_view value) {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}
} // namespace

void Workbench::prepareCppRefactoring(lang::CppRefactoringRequest request, std::stop_token cancel,
                                      std::function<void(Result<document::RefactoringPlan>)> done) {
    m_cppRenameStop.request_stop();
    m_cppRenameStop = std::stop_source{};
    ++m_cppRenameGeneration;
    m_cppRenameAdmission.reset();
    if (m_shutDown || !m_clangd || !m_pool || !m_reactor || m_projectRoot.empty()) {
        done(fail(ErrorCode::Busy, "Open a C++ project with a running language server"));
        return;
    }
    const auto permission = m_services.trust.snapshot(m_projectRoot);
    const auto database = m_clangd->compileCommands();
    if (!permission.trusted || !database) {
        done(fail(ErrorCode::Untrusted, "A trusted project and actual compilation database are required"));
        return;
    }
    auto capture = std::make_shared<Capture>();
    request.context.projectRoot = m_projectRoot;
    request.context.seedTranslationUnit = request.context.path;
    request.context.overlays.clear();
    const auto bundled =
        QDir(QCoreApplication::applicationDirPath())
            .filePath(QStringLiteral("llvm-%1").arg(QString::fromStdString(std::string(lang::pinnedLlvmVersion()))))
            .toStdString();
    request.context.llvmRoot.clear();
    if (const auto status = m_services.fs.status(bundled); status && status->type == io::FileType::Directory)
        request.context.llvmRoot = bundled;
    for (auto* view : m_editors->uniqueDocumentViews()) {
        if (view->path().empty())
            continue;
        capture->openPaths.insert(lang::documentKey(view->path()));
        const auto& path = view->path();
        if (path == m_projectRoot || (path.starts_with(m_projectRoot) && path.size() > m_projectRoot.size() &&
                                      (path[m_projectRoot.size()] == '/' || path[m_projectRoot.size()] == '\\')))
            request.context.overlays.push_back({path, view->editor().document().text().text()});
    }
    capture->request = std::move(request);
    capture->plan.label = "C++ " + capture->request.operation;
    capture->database = database->file;
    capture->trustRevision = permission.revision;
    m_cppRenameTrustRevision = permission.revision;
    capture->generation = m_cppRenameGeneration;
    capture->server = m_clangd;
    capture->stop = m_cppRenameStop.get_token();
    const QPointer<Workbench> self(this);
    const auto cancellation = std::make_shared<std::stop_callback<std::function<void()>>>(
        cancel, [source = m_cppRenameStop]() mutable { source.request_stop(); });
    capture->cancellation = cancellation; // Retained through queued delivery and the accepted plan context.
    auto publish = [self, cancellation, done = std::move(done)](Prepared result) mutable {
        static_cast<void>(cancellation);
        auto* app = QCoreApplication::instance();
        if (!app)
            return;
        QMetaObject::invokeMethod(
            app,
            [self, done = std::move(done), result = std::move(result)]() mutable {
                if (!self)
                    return;
                if (!result) {
                    done(std::unexpected(result.error()));
                    return;
                }
                auto capture = std::move(*result);
                auto revalidate = [self, capture, dependencies = capture->plan.dependencies]() -> Result<void> {
                    if (!self || self->m_shutDown || capture->stop.stop_requested())
                        return fail(ErrorCode::Cancelled, "Project closed");
                    const auto permission = self->m_services.trust.snapshot(capture->request.context.projectRoot);
                    const auto database = self->m_clangd ? self->m_clangd->compileCommands() : std::nullopt;
                    if (self->m_cppRenameGeneration != capture->generation || self->m_clangd != capture->server ||
                        self->m_projectRoot != capture->request.context.projectRoot || !permission.trusted ||
                        permission.revision != capture->trustRevision || !database ||
                        database->file != capture->database)
                        return fail(ErrorCode::Busy, "Project/configuration/trust changed during AST refactoring");
                    std::set<std::string> open, physical;
                    for (const auto& doc : self->m_documents->documents()) {
                        if (doc->path().empty())
                            continue;
                        open.insert(lang::documentKey(doc->path()));
                        auto identity = self->m_services.fs.physicalPath(doc->path());
                        if (identity)
                            physical.insert(lang::documentKey(*identity));
                        else if (identity.error().code() != ErrorCode::NotFound)
                            return std::unexpected(identity.error());
                    }
                    if (open != capture->openPaths)
                        return fail(ErrorCode::Busy, "Open document layout changed; request a new preview");
                    for (const auto& dependency : dependencies) {
                        if (open.contains(lang::documentKey(dependency.path)))
                            return fail(ErrorCode::Busy, "Disk input is now open in an editor");
                        if (dependency.expectedBytes) {
                            auto identity = self->m_services.fs.physicalPath(dependency.path);
                            if (!identity)
                                return std::unexpected(identity.error());
                            if (physical.contains(lang::documentKey(*identity)))
                                return fail(ErrorCode::Busy,
                                            "Disk input aliases an open editor; request an exact snapshot");
                        }
                    }
                    return {};
                };
                auto current = revalidate();
                if (!current) {
                    done(std::unexpected(current.error()));
                    return;
                }
                // Move the plan out: its context retains capture, not a self-referential plan cycle.
                auto plan = std::move(capture->plan);
                plan.revalidateContext = std::move(revalidate);
                done(std::move(plan));
            },
            Qt::QueuedConnection);
    };
    const auto helper = QDir(QCoreApplication::applicationDirPath())
                            .filePath(
#ifdef _WIN32
                                QStringLiteral("frost-clang.exe")
#else
                                QStringLiteral("frost-clang")
#endif
                                    )
                            .toStdString();
    m_pool
        ->spawn(sched::Lane::Background,
                prepare(&m_services.fs, &m_services.launcher, &m_services.trust, io::AsyncContext{*m_reactor, *m_pool},
                        helper, capture, m_cppRenameStop.get_token(), std::move(publish)))
        .detach();
}

void Workbench::registerCppRefactoringActions() {
    for (const auto& [operation, label] : std::array{
             std::pair{"change-signature", "Change C++ Signature…"},
             std::pair{"inline-variable", "Inline C++ Variable…"}, std::pair{"inline-function", "Inline C++ Call…"},
             std::pair{"extract-variable", "Extract C++ Variable…"},
             std::pair{"extract-constant", "Extract C++ Constant…"}, std::pair{"extract-method", "Extract C++ Method…"},
             std::pair{"extract-field", "Extract C++ Field…"}}) {
        input::ActionSpec action;
        action.id = "refactor.cpp." + std::string(operation);
        action.name = label;
        action.group = "Refactor";
        action.context.all = input::ContextKeys{input::ContextKey::EditorFocus};
        action.context.none = input::ContextKeys{input::ContextKey::EditorReadOnly, input::ContextKey::DialogFocus};
        action.handler = [this, operation](const input::ActionContext&, const input::InvokeInfo&) {
            showCppRefactoring(operation);
        };
        (void)m_registry.add(std::move(action));
    }
}

void Workbench::showCppRefactoring(std::string operation) {
    // Prefer the actual clangd tweaks for their broader expression/statement/local profiles.
    if (m_language && operation == "extract-variable") {
        m_language->extractVariable();
        return;
    }
    if (m_language && operation == "extract-method") {
        m_language->extractMethod();
        return;
    }
    if (m_language && operation == "inline-variable") {
        m_language->inlineSymbol();
        return;
    }
    auto* view = m_actionHost.active();
    if (!view || view->path().empty())
        return;
    const QPointer<ui::EditorView> selectedView(view);
    const auto selectedText = view->editor().document().text();
    const auto selectedPath = view->path();
    const auto& selections = view->editor().state().selections;
    if (selections.empty())
        return;
    const auto selection = selections.all()[selections.primaryIndex()];
    lang::CppRefactoringRequest request;
    request.context.path = view->path();
    request.context.offset = selection.range().begin.value();
    request.endOffset = selection.range().end.value();
    request.context.newName = "unused";
    request.operation = operation;
    if (operation.starts_with("extract-")) {
        bool accepted = false;
        const auto name =
            QInputDialog::getText(m_window, QStringLiteral("Extract"), QStringLiteral("New C++ identifier:"),
                                  QLineEdit::Normal, {}, &accepted);
        if (!accepted)
            return;
        request.context.newName = name.toStdString();
    } else if (operation == "change-signature") {
        bool accepted = false;
        const auto order = QInputDialog::getText(
            m_window, QStringLiteral("Change signature"),
            QStringLiteral("Existing parameter indices in new order (zero based, comma separated):"), QLineEdit::Normal,
            {}, &accepted);
        if (!accepted)
            return;
        for (const auto& part : order.split(',')) {
            bool valid = false;
            const auto index = part.trimmed().toUInt(&valid);
            if (!valid) {
                QMessageBox::warning(m_window, QStringLiteral("Change signature"),
                                     QStringLiteral("Enter a complete numeric permutation."));
                return;
            }
            request.parameterOrder.push_back(index);
        }
    }
    if (!selectedView || selectedView->path() != selectedPath ||
        selectedView->editor().document().text().version() != selectedText.version()) {
        QMessageBox::warning(m_window, QStringLiteral("C++ refactoring"),
                             QStringLiteral("The selection document changed; request the action again."));
        return;
    }
    auto* progress = new QProgressDialog(QStringLiteral("Checking captured C++ configurations…"),
                                         QStringLiteral("Cancel"), 0, 0, m_window);
    progress->setAttribute(Qt::WA_DeleteOnClose);
    auto cancellation = std::make_shared<std::stop_source>();
    connect(progress, &QProgressDialog::canceled, this, [cancellation] { cancellation->request_stop(); });
    progress->show();
    const QPointer<Workbench> self(this);
    prepareCppRefactoring(
        std::move(request), cancellation->get_token(),
        [self, progress = QPointer<QProgressDialog>(progress), cancellation,
         operation](Result<document::RefactoringPlan> result) {
            if (progress) {
                QObject::disconnect(progress.data(), nullptr, self.data(), nullptr);
                progress->close();
            }
            if (!self)
                return;
            if (!result) {
                QMessageBox::warning(self->m_window, QStringLiteral("C++ refactoring"),
                                     display(result.error().message()));
                return;
            }
            auto plan = std::make_shared<document::RefactoringPlan>(std::move(*result));
            auto* dialog = new QDialog(self->m_window);
            dialog->setAttribute(Qt::WA_DeleteOnClose);
            dialog->setWindowTitle(display(plan->label));
            auto* layout = new QVBoxLayout(dialog);
            const QString profile =
                operation == "change-signature"
                    ? QStringLiteral("Profile: one defined internal non-overloaded free function; permute all existing "
                                     "arithmetic parameters and side-effect-free direct call arguments.")
                : operation == "inline-function"
                    ? QStringLiteral("Profile: one direct call to an internal arithmetic function with a single pure "
                                     "return expression; every argument must be a non-volatile value or literal.")
                : operation == "extract-field"
                    ? QStringLiteral(
                          "Profile: literal arithmetic constant in an instance-method return, in an internal "
                          "non-aggregate class. Adds a private const instance field and changes object layout.")
                    : QStringLiteral(
                          "Profile: complete arithmetic return expression; constant extraction requires a constant "
                          "expression. Stock clangd actions cover broader local variable/statement extraction.");
            auto* applicability = new QLabel(profile, dialog);
            applicability->setWordWrap(true);
            layout->addWidget(applicability);
            layout->addWidget(new QLabel(
                QStringLiteral("Scope: all captured compile configurations. Apply is one atomic operation; "
                               "generated/macro/ambiguous profiles refuse. Preview text is capped at 1 MiB per image."),
                dialog));
            auto* tabs = new QTabWidget(dialog);
            const auto image = [&](std::string_view path, std::string_view before, std::string_view after) {
                auto* text = new QPlainTextEdit(dialog);
                text->setReadOnly(true);
                text->setPlainText(QStringLiteral("BEFORE\n") + display(before.substr(0, 1024 * 1024)) +
                                   QStringLiteral("\nAFTER\n") + display(after.substr(0, 1024 * 1024)));
                tabs->addTab(text, display(path));
            };
            for (const auto& file : plan->buffers)
                image(file.path, file.expectedText, file.afterText);
            for (const auto& file : plan->disk)
                image(file.path, file.expectedBytes, file.afterBytes);
            layout->addWidget(tabs);
            auto* buttons = new QDialogButtonBox(QDialogButtonBox::Apply | QDialogButtonBox::Cancel, dialog);
            connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, dialog, [self, dialog, plan] {
                if (!self)
                    return;
                auto applied = self->applyRefactoring(*plan);
                if (!applied)
                    QMessageBox::warning(dialog, QStringLiteral("Refactoring refused"),
                                         display(applied.error().message()));
                else
                    dialog->accept();
            });
            connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
            layout->addWidget(buttons);
            dialog->resize(900, 650);
            dialog->show();
        });
}

Result<std::function<Result<void>(const lang::WorkspaceEdit&)>> Workbench::beginCppProviderRefactoring() {
    if (m_shutDown || !m_clangd || !m_bridge || m_projectRoot.empty())
        return fail(ErrorCode::Busy, "Open a C++ project before requesting a semantic tweak");
    const auto permission = m_services.trust.snapshot(m_projectRoot);
    if (!permission.trusted)
        return fail(ErrorCode::Untrusted, "Trust the project before refactoring");
    const auto server = m_clangd;
    const auto database = server->compileCommands();
    const auto encoding = m_bridge->encoding();
    const auto root = m_projectRoot;
    const auto generation = m_cppRenameGeneration;
    std::map<std::string, std::pair<std::string, std::optional<std::int64_t>>> snapshots;
    for (const auto& [path, snapshot] : m_bridge->openDocuments())
        snapshots.emplace(lang::documentKey(path), std::pair{snapshot.text(), server->documentVersion(path)});
    const QPointer<Workbench> self(this);
    auto consumed = std::make_shared<bool>(false);
    return std::function<Result<void>(const lang::WorkspaceEdit&)>{
        [self, server, database, root, permission, generation, encoding, snapshots = std::move(snapshots),
         consumed](const lang::WorkspaceEdit& edit) -> Result<void> {
            if (*consumed)
                return fail(ErrorCode::Busy, "This semantic operation already supplied its atomic edit");
            if (!self || self->m_shutDown || self->m_projectRoot != root || self->m_clangd != server ||
                self->m_cppRenameGeneration != generation || self->m_bridge->encoding() != encoding)
                return fail(ErrorCode::Busy, "C++ request context changed");
            const auto currentPermission = self->m_services.trust.snapshot(root);
            const auto currentDatabase = server->compileCommands();
            if (!currentPermission.trusted || currentPermission.revision != permission.revision ||
                currentDatabase.has_value() != database.has_value() ||
                (database && currentDatabase->file != database->file))
                return fail(ErrorCode::Busy, "C++ project admission changed");
            const auto open = self->m_bridge->openDocuments();
            if (open.size() != snapshots.size())
                return fail(ErrorCode::Busy, "Open documents changed during semantic request");
            for (const auto& [path, snapshot] : open) {
                const auto captured = snapshots.find(lang::documentKey(path));
                if (captured == snapshots.end() || captured->second.first != snapshot.text() ||
                    captured->second.second != server->documentVersion(path))
                    return fail(ErrorCode::Busy, "A request document version changed: " + path);
            }
            if (edit.empty())
                return {};
            for (const auto& file : edit.files)
                if (!snapshots.contains(lang::documentKey(file.path)))
                    return fail(ErrorCode::Unsupported, "This local clangd tweak edited an uncaptured closed file; "
                                                        "open its source before requesting the action: " +
                                                            file.path);
            *consumed = true;
            return self->m_bridge->apply(edit, "C++ Semantic Refactoring");
        }};
}

bool Workbench::previewCppRefactoring(const document::RefactoringPlan& plan) {
    QDialog dialog(m_window);
    dialog.setWindowTitle(display(plan.label));
    auto* layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel(QStringLiteral("Actual clangd WorkspaceEdit. Apply is atomic and rechecks captured "
                                                "before-images. Preview images are capped at 1 MiB."),
                                 &dialog));
    auto* tabs = new QTabWidget(&dialog);
    const auto add = [&](std::string_view path, std::string_view before, std::string_view after) {
        auto* image = new QPlainTextEdit(&dialog);
        image->setReadOnly(true);
        image->setPlainText(QStringLiteral("BEFORE\n") + display(before.substr(0, 1024 * 1024)) +
                            QStringLiteral("\nAFTER\n") + display(after.substr(0, 1024 * 1024)));
        tabs->addTab(image, display(path));
    };
    for (const auto& file : plan.buffers)
        add(file.path, file.expectedText, file.afterText);
    for (const auto& file : plan.disk)
        add(file.path, file.expectedBytes, file.afterBytes);
    layout->addWidget(tabs);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Apply | QDialogButtonBox::Cancel, &dialog);
    connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.resize(900, 650);
    return dialog.exec() == QDialog::Accepted;
}
} // namespace frost::app
