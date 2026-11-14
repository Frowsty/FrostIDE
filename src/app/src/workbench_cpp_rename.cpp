// SPDX-License-Identifier: Apache-2.0
//
// REF-4/5: fast clangd preview, asynchronous LibTooling admission, then one captured transaction.
#include "frost/app/workbench.hpp"
#include "frost/io/spawn_log.hpp"
#include "frost/lang/cpp_rename_plan.hpp"
#include "frost/lang/uri.hpp"
#include "frost/ui/editor/editor_view.hpp"

#include "language_bridge.hpp"
#include "trusted_project_launcher.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QMetaObject>
#include <QPointer>

#include <algorithm>
#include <chrono>
#include <memory>
#include <unordered_set>
#include <utility>

namespace frost::app {
struct CppRenameAdmission {
    std::string root;
    std::string databasePath;
    std::uint64_t trustRevision = 0;
    std::uint64_t generation = 0;
    std::shared_ptr<lang::ClangdService> server;
    lang::PositionEncoding encoding = lang::PositionEncoding::Utf16;
    lang::WorkspaceEdit required;
    lang::WorkspaceEdit offered;
    document::RefactoringPlan evidence;
};

namespace {
using CheckDone = std::function<void(Result<std::shared_ptr<CppRenameAdmission>>)>;

const lang::FileEdit* fileAt(const lang::WorkspaceEdit& edit, std::string_view path) {
    const auto key = lang::documentKey(path);
    for (const auto& file : edit.files)
        if (lang::documentKey(file.path) == key)
            return &file;
    return nullptr;
}

sched::Task<void> checkRenameTask(io::IFileSystem* fs, io::IProcessLauncher* launcher, project::TrustStore* trust,
                                  io::AsyncContext context, std::string helper, lang::CppRenameRequest request,
                                  std::shared_ptr<CppRenameAdmission> admission, std::stop_token stop, CheckDone done) {
    auto commands = fs->readFileSnapshot(admission->databasePath, 32U * 1024U * 1024U);
    if (!commands) {
        done(std::unexpected(commands.error()));
        co_return;
    }
    request.compilationDatabase = *commands;
    admission->evidence.dependencies.push_back({admission->databasePath, *commands});
    TrustedProjectLauncher admitted(*launcher, *trust, admission->root, admission->trustRevision, stop);
    auto result = co_await lang::requestCppRenamePlan(admitted, context, std::move(helper), request,
                                                      std::chrono::minutes(5), stop);
    if (!result) {
        done(std::unexpected(result.error()));
        co_return;
    }
    if (!result->ready()) {
        std::string reason = "C++ rename is not ready (" + result->status + ")";
        for (const auto& diagnostic : result->diagnostics)
            reason += "\n" + diagnostic;
        done(fail(ErrorCode::Unsupported, std::move(reason)));
        co_return;
    }
    std::unordered_set<std::string> matched;
    for (const auto& file : result->files) {
        const auto* required = fileAt(admission->required, file.path);
        if (!required) {
            done(fail(ErrorCode::Busy, "The C++ index preview omitted a semantic rename participant: " + file.path +
                                           ". Let indexing finish and request Rename again."));
            co_return;
        }
        auto after = lang::applyTextEdits(file.before, required->edits, admission->encoding);
        if (!after || *after != file.after) {
            done(fail(ErrorCode::Busy, "The index preview disagrees with the C++ semantic rename: " + file.path));
            co_return;
        }
        matched.insert(lang::documentKey(file.path));
    }
    for (const auto& file : admission->required.files) {
        if (!file.edits.empty() && !matched.contains(lang::documentKey(file.path))) {
            done(fail(ErrorCode::Busy, "The C++ semantic tool did not approve a preview participant: " + file.path));
            co_return;
        }
    }
    std::size_t retainedBytes = commands->size();
    for (const auto& dependency : result->dependencies) {
        if (stop.stop_requested()) {
            done(fail(ErrorCode::Cancelled, "Rename check cancelled"));
            co_return;
        }
        if (dependency.overlay) {
            const auto it = std::ranges::find_if(request.overlays, [&](const auto& overlay) {
                return lang::documentKey(overlay.path) == lang::documentKey(dependency.path);
            });
            if (it == request.overlays.end() || io::sha256Hex(it->contents) != dependency.sha256 ||
                it->contents.size() != dependency.bytes) {
                done(fail(ErrorCode::Busy, "A captured editor dependency does not match the semantic result"));
                co_return;
            }
            admission->evidence.bufferDependencies.push_back({it->path, it->contents});
        } else {
            auto bytes = fs->readFileSnapshot(dependency.path, 64U * 1024U * 1024U);
            if (!bytes || bytes->size() != dependency.bytes || io::sha256Hex(*bytes) != dependency.sha256) {
                done(fail(ErrorCode::Busy, "A C++ input changed during rename analysis: " + dependency.path));
                co_return;
            }
            retainedBytes += bytes->size();
            if (retainedBytes > 512U * 1024U * 1024U) {
                done(fail(ErrorCode::OutOfRange, "Rename dependency snapshots exceed the memory limit"));
                co_return;
            }
            admission->evidence.dependencies.push_back({dependency.path, std::move(*bytes)});
        }
    }
    for (const auto& path : result->missingDependencies) {
        const auto state = fs->status(path);
        if (state || state.error().code() != ErrorCode::NotFound) {
            done(fail(ErrorCode::Busy, "A previously absent C++ include changed: " + path));
            co_return;
        }
        admission->evidence.dependencies.push_back({path, std::nullopt});
    }
    done(std::move(admission));
}
} // namespace

void Workbench::checkCppRename(std::string path, lang::LspPosition position, std::string newName,
                               lang::WorkspaceEdit required, lang::WorkspaceEdit offered, std::stop_token cancel,
                               std::function<void(Result<void>)> done) {
    m_cppRenameStop.request_stop();
    m_cppRenameStop = std::stop_source{};
    ++m_cppRenameGeneration;
    m_cppRenameAdmission.reset();
    if (m_shutDown || !m_clangd || !m_pool || !m_reactor || m_projectRoot.empty()) {
        done(fail(ErrorCode::Busy, "Open a C++ project with a running language server before refactoring"));
        return;
    }
    const auto permission = m_services.trust.snapshot(m_projectRoot);
    if (!permission.trusted) {
        done(fail(ErrorCode::Untrusted, "Trust the project before semantic refactoring"));
        return;
    }
    m_cppRenameTrustRevision = permission.revision;
    const auto database = m_clangd->compileCommands();
    if (!database) {
        done(fail(ErrorCode::NotFound, "C++ refactoring requires a compilation database"));
        return;
    }
    const auto state = m_bridge->find(path);
    if (!state) {
        done(fail(ErrorCode::NotFound, "The renamed document is no longer open"));
        return;
    }
    const auto encoding = m_bridge->encoding();
    const auto offset = lang::fromLspPosition(state->snapshot, position, encoding);
    if (!offset) {
        done(std::unexpected(offset.error()));
        return;
    }
    auto admission = std::make_shared<CppRenameAdmission>();
    admission->root = m_projectRoot;
    admission->databasePath = database->file;
    admission->trustRevision = permission.revision;
    admission->generation = m_cppRenameGeneration;
    admission->server = m_clangd;
    admission->encoding = encoding;
    admission->required = std::move(required);
    admission->offered = std::move(offered);
    lang::CppRenameRequest request;
    request.projectRoot = m_projectRoot;
    request.path = std::move(path);
    request.offset = offset->value();
    request.newName = std::move(newName);
    const auto bundledLlvm =
        QDir(QCoreApplication::applicationDirPath())
            .filePath(QStringLiteral("llvm-%1").arg(QString::fromStdString(std::string(lang::pinnedLlvmVersion()))))
            .toStdString();
    if (const auto bundled = m_services.fs.status(bundledLlvm); bundled && bundled->type == io::FileType::Directory)
        request.llvmRoot = bundledLlvm;
    for (auto* view : m_editors->uniqueDocumentViews()) {
        if (!view->path().empty())
            request.overlays.push_back({view->path(), view->editor().document().text().text()});
    }
    const auto helper = QDir(QCoreApplication::applicationDirPath())
                            .filePath(
#ifdef _WIN32
                                QStringLiteral("frost-clang.exe")
#else
                                QStringLiteral("frost-clang")
#endif
                                    )
                            .toStdString();
    const QPointer<Workbench> self(this);
    const auto generation = m_cppRenameGeneration;
    const auto stop = m_cppRenameStop.get_token();
    const auto cancellation = std::make_shared<std::stop_callback<std::function<void()>>>(
        cancel, [source = m_cppRenameStop]() mutable { source.request_stop(); });
    auto publish = [self, generation, cancellation,
                    done = std::move(done)](Result<std::shared_ptr<CppRenameAdmission>> result) mutable {
        static_cast<void>(cancellation); // Keep the user's stop callback alive until the child is finished.
        auto* app = QCoreApplication::instance();
        if (!app)
            return;
        QMetaObject::invokeMethod(
            app,
            [self, generation, done = std::move(done), result = std::move(result)]() mutable {
                if (!self)
                    return;
                if (self->m_shutDown || self->m_cppRenameGeneration != generation) {
                    done(fail(ErrorCode::Cancelled, "A newer refactoring replaced this preview"));
                    return;
                }
                if (!result) {
                    done(std::unexpected(std::move(result.error())));
                    return;
                }
                const auto& value = **result;
                const auto current = self->m_services.trust.snapshot(value.root);
                const auto database = self->m_clangd ? self->m_clangd->compileCommands() : std::nullopt;
                if (self->m_projectRoot != value.root || self->m_clangd != value.server || !current.trusted ||
                    current.revision != value.trustRevision || !database || database->file != value.databasePath ||
                    self->m_bridge->encoding() != value.encoding) {
                    done(fail(ErrorCode::Busy, "The project or C++ configuration changed during rename analysis"));
                    return;
                }
                self->m_cppRenameAdmission = std::move(*result);
                done({});
            },
            Qt::QueuedConnection);
    };
    m_pool
        ->spawn(sched::Lane::Background,
                checkRenameTask(&m_services.fs, &m_services.launcher, &m_services.trust,
                                io::AsyncContext{*m_reactor, *m_pool}, helper, std::move(request), std::move(admission),
                                stop, std::move(publish)))
        .detach();
}

Result<void> Workbench::admitCppRename(const lang::WorkspaceEdit& selected, document::RefactoringPlan& plan) {
    const auto admission = m_cppRenameAdmission;
    if (!admission)
        return fail(ErrorCode::Busy, "Request a fresh C++ rename preview before applying");
    // Required semantic edits are one group. Only captured optional comment/string edits may be
    // excluded independently; no caller can add a new range after the AST check.
    for (const auto& file : admission->required.files) {
        if (file.edits.empty())
            continue;
        const auto* chosen = fileAt(selected, file.path);
        if (!chosen)
            return fail(ErrorCode::InvalidArgument, "C++ symbol edits must be applied as one semantic group");
        for (const auto& edit : file.edits)
            if (std::ranges::find(chosen->edits, edit) == chosen->edits.end())
                return fail(ErrorCode::InvalidArgument,
                            "A required C++ symbol edit was excluded; include the semantic group");
    }
    for (const auto& file : selected.files) {
        const auto* offered = fileAt(admission->offered, file.path);
        if (!offered || file.version != offered->version)
            return fail(ErrorCode::Busy, "The selected rename no longer matches its preview");
        for (const auto& edit : file.edits)
            if (std::ranges::find(offered->edits, edit) == offered->edits.end())
                return fail(ErrorCode::Busy, "An edit was added after the C++ rename check");
    }
    // Disk and buffer dependencies also include mutation inputs; coordinator admission checks all
    // of them before the durable decision, so optional edits cannot be based on different bytes.
    plan.dependencies.insert(plan.dependencies.end(), admission->evidence.dependencies.begin(),
                             admission->evidence.dependencies.end());
    plan.bufferDependencies.insert(plan.bufferDependencies.end(), admission->evidence.bufferDependencies.begin(),
                                   admission->evidence.bufferDependencies.end());
    const QPointer<Workbench> self(this);
    plan.revalidateContext = [self, admission]() -> Result<void> {
        if (!self || self->m_shutDown)
            return fail(ErrorCode::Cancelled, "The C++ project was closed");
        const auto permission = self->m_services.trust.snapshot(admission->root);
        const auto database = self->m_clangd ? self->m_clangd->compileCommands() : std::nullopt;
        if (self->m_cppRenameAdmission != admission || self->m_projectRoot != admission->root ||
            self->m_clangd != admission->server || !permission.trusted ||
            permission.revision != admission->trustRevision || !database || database->file != admission->databasePath ||
            self->m_bridge->encoding() != admission->encoding)
            return fail(ErrorCode::Busy, "The C++ rename context changed; request a fresh preview");
        std::unordered_set<std::string> openNames;
        std::unordered_set<std::string> openPhysicalNames;
        for (const auto& document : self->m_documents->documents()) {
            if (document->path().empty())
                continue;
            openNames.insert(lang::documentKey(document->path()));
            const auto physical = self->m_services.fs.physicalPath(document->path());
            if (physical)
                openPhysicalNames.insert(lang::documentKey(*physical));
            else if (physical.error().code() != ErrorCode::NotFound)
                return fail(ErrorCode::Busy, "Cannot establish an open document's identity: " + document->path());
        }
        for (const auto& dependency : admission->evidence.dependencies) {
            bool open = openNames.contains(lang::documentKey(dependency.path));
            if (!open && dependency.expectedBytes) {
                const auto physical = self->m_services.fs.physicalPath(dependency.path);
                if (!physical)
                    return std::unexpected(physical.error());
                open = openPhysicalNames.contains(lang::documentKey(*physical));
            }
            if (open)
                return fail(ErrorCode::Busy,
                            "A C++ input analyzed from disk is now open in an editor: " + dependency.path +
                                ". Close it or request a new rename preview with its editor snapshot.");
        }
        return {};
    };
    return {};
}
} // namespace frost::app
