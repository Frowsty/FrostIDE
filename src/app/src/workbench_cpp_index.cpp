// SPDX-License-Identifier: Apache-2.0
// CPP-8 actual multi-command index, configuration-labelled navigation and explicit rebuild/invalidate.
#include "frost/app/workbench.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/io/spawn_log.hpp"
#include "frost/lang/clangd_service.hpp"
#include "frost/lang/compile_commands.hpp"
#include "frost/lang/cpp_index.hpp"
#include "frost/lang/cpp_modules.hpp"
#include "frost/lang/cpp_preprocessor.hpp"
#include "frost/lang/unreal_reflection.hpp"
#include "frost/sched/resource_governor.hpp"
#include "frost/ui/editor/editor_view.hpp"

#include "trusted_project_launcher.hpp"
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProgressDialog>
#include <QPushButton>
#include <QTabWidget>
#include <QTableWidget>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <map>

namespace frost::app {
namespace {
struct IndexViewStamp {
    QPointer<ui::EditorView> editor;
    std::string path;
    std::uint64_t version;
};

std::vector<IndexViewStamp> captureIndexViews(const DocumentEditorFactory& editors) {
    std::vector<IndexViewStamp> stamps;
    for (auto* view : editors.uniqueDocumentViews())
        stamps.push_back({&view->editor(), view->path(), view->document().document().version()});
    return stamps;
}

bool indexViewsCurrent(const DocumentEditorFactory& editors, const std::vector<IndexViewStamp>& stamps) {
    const auto views = editors.uniqueDocumentViews();
    if (views.size() != stamps.size())
        return false;
    return std::ranges::all_of(views, [&stamps](DocumentEditorView* view) {
        const auto stamp = std::ranges::find_if(
            stamps, [view](const IndexViewStamp& item) { return item.editor.data() == &view->editor(); });
        return stamp != stamps.end() && stamp->path == view->path() && !view->document().dirty() &&
               stamp->version == view->document().document().version();
    });
}

QString display(std::string_view bytes) {
    return QString::fromUtf8(bytes.data(), static_cast<qsizetype>(bytes.size()));
}
QString roleText(std::uint64_t roles) {
    QStringList labels;
    constexpr std::array<const char*, 21> names{
        "declaration",       "definition",        "reference",     "read",        "write",       "call",
        "dynamic dispatch",  "address taken",     "implicit",      "undefined",   "child of",    "base of",
        "overrides",         "received by",       "called by",     "extended by", "accessor of", "contained by",
        "interface type of", "specialization of", "name reference"};
    for (std::size_t bit = 0; bit < names.size(); ++bit)
        if (roles & (std::uint64_t{1} << bit))
            labels.push_back(QString::fromUtf8(names[bit]));
    return labels.join(QStringLiteral(", "));
}
struct IndexResult {
    lang::CppIndexQuery query;
    std::optional<lang::CppPreprocessorResult> preprocessor;
    std::optional<std::vector<lang::UnrealReflectionIssue>> unrealIssues;
    std::string unrealIssueError;
    std::optional<lang::CppModuleGraph> modules;
    std::map<std::string, std::string> sourceHashes;
    std::string databaseHash;
    std::shared_ptr<std::vector<lang::CppIndexUnit>> moduleInputs;
    std::size_t completed = 0, total = 0, stored = 0;
};
struct IndexJob {
    std::string root, databaseFile, storeFile, helper, operation, symbol;
    std::string scanner, privateDatabase;
    std::uint64_t trustRevision = 0, generation = 0, commandIndex = 0;
    std::shared_ptr<sched::ResourceGovernor> governor;
    bool inspectUnreal = false;
};
using Done = std::function<void(Result<IndexResult>)>;
using Progress = std::function<void(std::size_t, std::size_t)>;
sched::Task<void> indexTask(io::IFileSystem* fs, io::IProcessLauncher* launcher, project::TrustStore* trust,
                            io::AsyncContext context, IndexJob job, std::stop_token stop, Progress progress,
                            Done done) {
    TrustedProjectLauncher admitted(*launcher, *trust, job.root, job.trustRevision, stop);
    const auto permission = [&] {
        const auto current = trust->snapshot(job.root);
        return !stop.stop_requested() && current.trusted && current.revision == job.trustRevision;
    };
    if (!permission()) {
        done(fail(ErrorCode::Cancelled, "Index request no longer admitted"));
        co_return;
    }
    auto bytes = fs->readFileSnapshot(job.databaseFile, 32U * 1024U * 1024U);
    if (!bytes) {
        done(std::unexpected(bytes.error()));
        co_return;
    }
    const auto hash = io::sha256Hex(*bytes);
    auto commands = lang::parseJson(*bytes);
    if (!commands || !commands->isArray() || commands->items().empty() || commands->items().size() > 32768) {
        done(fail(ErrorCode::InvalidArgument, "Index needs a nonempty bounded compilation database"));
        co_return;
    }
    if (job.operation == "preprocess" || job.operation == "modules") {
        IndexResult result;
        result.total = commands->items().size();
        auto lease = job.governor->tryAcquireHeavyLeg(sched::HeavyLegKind::BackgroundIndex);
        if (!lease) {
            done(std::unexpected(lease.error()));
            co_return;
        }
        const auto current = [&]() -> Result<void> {
            auto database = fs->readFileSnapshot(job.databaseFile, 32U * 1024U * 1024U);
            if (!permission() || !database || *database != *bytes)
                return fail(ErrorCode::Busy, "Captured command context changed");
            return {};
        };
        if (job.operation == "preprocess") {
            lang::CppIndexRequest request;
            request.projectRoot = job.root;
            request.compilationDatabase = *bytes;
            request.configuration = "Captured CDB command " + std::to_string(job.commandIndex + 1);
            request.commandIndex = job.commandIndex;
            auto output = co_await lang::requestCppPreprocessor(admitted, context, job.helper, std::move(request),
                                                                std::chrono::minutes(5), stop);
            if (!output) {
                done(std::unexpected(output.error()));
                co_return;
            }
            auto valid = lang::validateCppIndexInputs(*fs, output->receipt, {}, stop);
            if (!valid) {
                done(std::unexpected(valid.error()));
                co_return;
            }
            if (job.inspectUnreal) {
                auto issues = lang::inspectUnrealReflection(*output);
                if (issues)
                    result.unrealIssues = std::move(*issues);
                else
                    result.unrealIssueError = issues.error().message();
            }
            result.preprocessor = std::move(*output);
        } else {
            if (auto created = fs->createDirectories(io::parentPath(job.privateDatabase)); !created) {
                done(std::unexpected(created.error()));
                co_return;
            }
            if (auto written = fs->writeFileAtomic(job.privateDatabase, *bytes); !written) {
                done(std::unexpected(written.error()));
                co_return;
            }
            auto receipt = std::make_shared<std::vector<lang::CppIndexUnit>>();
            std::size_t receiptBytes = 0;
            for (std::size_t ordinal = 0; ordinal < result.total; ++ordinal) {
                if (auto valid = current(); !valid) {
                    (void)fs->removeFile(job.privateDatabase);
                    (void)fs->removeDirectory(io::parentPath(job.privateDatabase));
                    done(std::unexpected(valid.error()));
                    co_return;
                }
                lang::CppIndexRequest input;
                input.projectRoot = job.root;
                input.compilationDatabase = *bytes;
                input.configuration = "Module scan captured command " + std::to_string(ordinal + 1);
                input.commandIndex = ordinal;
                auto captured = co_await lang::requestCppPreprocessor(admitted, context, job.helper, std::move(input),
                                                                      std::chrono::minutes(5), stop);
                if (!captured) {
                    (void)fs->removeFile(job.privateDatabase);
                    (void)fs->removeDirectory(io::parentPath(job.privateDatabase));
                    done(std::unexpected(captured.error()));
                    co_return;
                }
                const auto size = captured->receipt.data.dump().size();
                if (size > 32U * 1024U * 1024U - receiptBytes) {
                    (void)fs->removeFile(job.privateDatabase);
                    (void)fs->removeDirectory(io::parentPath(job.privateDatabase));
                    done(fail(ErrorCode::OutOfRange, "Module input receipts exceed32MiB"));
                    co_return;
                }
                receiptBytes += size;
                receipt->push_back(std::move(captured->receipt));
                progress(ordinal + 1, result.total);
            }
            lang::CppModuleScanRequest request;
            request.scanner = job.scanner;
            request.privateDatabasePath = job.privateDatabase;
            request.databaseBytes = *bytes;
            request.workingDirectory = job.root;
            request.trusted = true;
            request.stop = stop;
            request.validateCurrent = [&]() -> Result<void> {
                if (auto valid = current(); !valid)
                    return valid;
                for (const auto& unit : *receipt)
                    if (auto valid = lang::validateCppIndexInputs(*fs, unit, {}, stop); !valid)
                        return valid;
                return {};
            };
            auto output = lang::scanCppModules(admitted, *fs, request);
            (void)fs->removeFile(job.privateDatabase);
            (void)fs->removeDirectory(io::parentPath(job.privateDatabase));
            if (!output) {
                done(std::unexpected(output.error()));
                co_return;
            }
            result.modules = std::move(*output);
            for (const auto& unit : *receipt)
                for (const auto& input : unit.data.find("dependencies")->items())
                    result.sourceHashes.emplace(std::string(input.stringAt("path")),
                                                std::string(input.stringAt("sha256")));
            result.moduleInputs = std::move(receipt);
            result.databaseHash = hash;
        }
        if (auto valid = current(); !valid) {
            done(std::unexpected(valid.error()));
            co_return;
        }
        done(std::move(result));
        co_return;
    }
    if (auto created = fs->createDirectories(io::parentPath(job.storeFile)); !created) {
        done(std::unexpected(created.error()));
        co_return;
    }
    auto store = lang::CppIndexStore::open(job.storeFile);
    if (!store) {
        done(std::unexpected(store.error()));
        co_return;
    }
    IndexResult result;
    result.total = commands->items().size();
    if (job.operation == "clear") {
        auto cleared = store->clear();
        if (!cleared) {
            done(std::unexpected(cleared.error()));
            co_return;
        }
    } else if (job.operation == "rebuild") {
        auto lease = job.governor->tryAcquireHeavyLeg(sched::HeavyLegKind::BackgroundIndex);
        if (!lease) {
            done(std::unexpected(lease.error()));
            co_return;
        }
        for (std::size_t ordinal = 0; ordinal < result.total; ++ordinal) {
            auto current = fs->readFileSnapshot(job.databaseFile, 32U * 1024U * 1024U);
            if (!permission() || !current || *current != *bytes) {
                done(fail(ErrorCode::Busy, "Project admission or captured compile commands changed"));
                co_return;
            }
            lang::CppIndexRequest request;
            request.projectRoot = job.root;
            request.compilationDatabase = *bytes;
            request.configuration = "Captured CDB command " + std::to_string(ordinal + 1);
            request.commandIndex = ordinal;
            auto unit = co_await lang::requestCppIndexUnit(admitted, context, job.helper, std::move(request),
                                                           std::chrono::minutes(5), stop);
            if (!unit) {
                done(std::unexpected(unit.error()));
                co_return;
            }
            current = fs->readFileSnapshot(job.databaseFile, 32U * 1024U * 1024U);
            if (!permission() || !current || *current != *bytes) {
                done(fail(ErrorCode::Busy, "Index response is stale"));
                co_return;
            }
            if (auto published = store->publish(*unit, job.generation, stop); !published) {
                done(std::unexpected(published.error()));
                co_return;
            }
            ++result.completed;
            progress(result.completed, result.total);
        }
    } else if (job.operation == "relations") {
        auto rows = store->relations(job.symbol, hash, {}, 1000, stop);
        if (!rows) {
            done(std::unexpected(rows.error()));
            co_return;
        }
        for (const auto& receipt : rows->find("receipts")->items()) {
            auto parsed = lang::parseCppIndexUnit(receipt.dump());
            if (!parsed) {
                done(std::unexpected(parsed.error()));
                co_return;
            }
            result.query.receipts.push_back(std::move(*parsed));
        }
        result.query.staleCommands = static_cast<std::size_t>(rows->find("staleCommands")->asUnsigned().value_or(0));
        result.query.truncated = rows->boolAt("truncated").value_or(false);
        for (const auto& row : rows->find("rows")->items())
            result.query.locations.push_back(
                {std::string(row.stringAt("commandIdentity")), std::string(row.stringAt("configuration")),
                 std::string(row.stringAt("source")),
                 std::string(row.stringAt("sourceName")) + " → " + std::string(row.stringAt("targetName")), "relation",
                 std::string(row.stringAt("path")), row.find("offset")->asUnsigned().value_or(0),
                 row.find("line")->asUnsigned().value_or(0), row.find("column")->asUnsigned().value_or(0),
                 row.find("roles")->asUnsigned().value_or(0)});
    } else {
        auto rows = job.operation == "usages" ? store->occurrences(job.symbol, hash, {}, 1000, stop)
                                              : store->symbols(job.symbol, hash, {}, 200, stop);
        if (!rows) {
            done(std::unexpected(rows.error()));
            co_return;
        }
        result.query = std::move(*rows);
    }
    auto current = fs->readFileSnapshot(job.databaseFile, 32U * 1024U * 1024U);
    if (!permission() || !current || *current != *bytes) {
        done(fail(ErrorCode::Busy, "Index result context changed"));
        co_return;
    }
    auto stored = store->storedCommands(hash);
    if (!stored) {
        done(std::unexpected(stored.error()));
        co_return;
    }
    result.stored = *stored;
    done(std::move(result));
}
// A modeless row retains its query-time dependency receipt. Validation stays on the worker;
// the queued continuation separately checks the live editor/project admission before acting.
sched::Task<void> validateIndexAction(io::IFileSystem* fs, std::string databaseFile, lang::CppIndexUnit receipt,
                                      std::stop_token stop, std::function<void(bool)> done) {
    auto database = fs->readFileSnapshot(databaseFile, 32U * 1024U * 1024U);
    const bool valid = database && io::sha256Hex(*database) == receipt.databaseSha256 &&
                       lang::validateCppIndexInputs(*fs, receipt, {}, stop).has_value() && !stop.stop_requested();
    auto current = fs->readFileSnapshot(databaseFile, 32U * 1024U * 1024U);
    done(valid && current && database && *current == *database && !stop.stop_requested());
    co_return;
}
sched::Task<void> validateModuleAction(io::IFileSystem* fs, std::string databaseFile, std::string databaseHash,
                                       std::shared_ptr<std::vector<lang::CppIndexUnit>> inputs, std::stop_token stop,
                                       std::function<void(bool)> done) {
    auto database = fs->readFileSnapshot(databaseFile, 32U * 1024U * 1024U);
    bool valid = database && io::sha256Hex(*database) == databaseHash && inputs;
    if (valid)
        for (const auto& unit : *inputs)
            if (!lang::validateCppIndexInputs(*fs, unit, {}, stop)) {
                valid = false;
                break;
            }
    auto current = fs->readFileSnapshot(databaseFile, 32U * 1024U * 1024U);
    done(valid && current && database && *current == *database && !stop.stop_requested());
    co_return;
}
} // namespace
void Workbench::registerCppIndexActions() {
    for (const auto& [operation, label] :
         std::array{std::pair{"rebuild", "Rebuild C++ Configuration Index"},
                    std::pair{"symbols", "Find Symbol in C++ Configuration Index…"},
                    std::pair{"clear", "Invalidate C++ Configuration Index"},
                    std::pair{"preprocess", "C++ Preprocessor: Expanded Source, Macros and Includes..."},
                    std::pair{"modules", "Scan C++ Module Dependencies..."}}) {
        input::ActionSpec action;
        action.id = "cpp.index." + std::string(operation);
        action.name = label;
        action.group = "C++";
        action.handler = [this, operation](const input::ActionContext&, const input::InvokeInfo&) {
            showCppIndex(operation);
        };
        (void)m_registry.add(std::move(action));
    }
}
void Workbench::showCppIndex(std::string operation, std::string symbol) {
    if (m_shutDown || !m_clangd || !m_pool || !m_reactor || m_projectRoot.empty())
        return;
    const auto permission = m_services.trust.snapshot(m_projectRoot);
    const auto database = m_clangd->compileCommands();
    const QPointer<Workbench> admission(this);
    const auto root = m_projectRoot;
    const auto server = m_clangd;
    const auto requestGeneration = m_cppIndexGeneration;
    if (!permission.trusted || !database || m_options.compileCommandsCache.empty()) {
        QMessageBox::warning(
            m_window, QStringLiteral("C++ index"),
            QStringLiteral(
                "A trusted project, captured compilation database and configured private cache are required."));
        return;
    }
    for (auto* view : m_editors->uniqueDocumentViews())
        if (view->document().dirty()) {
            QMessageBox::information(m_window, QStringLiteral("C++ index"),
                                     QStringLiteral("Save open changes before indexing or querying the persistent disk "
                                                    "snapshot. Live clangd navigation remains available."));
            return;
        }
    std::uint64_t commandIndex = 0;
    if (operation == "preprocess") {
        bool accepted = false;
        const int ordinal = QInputDialog::getInt(
            m_window, QStringLiteral("C++ preprocessor"),
            QStringLiteral("Exact compilation database command ordinal (1-based):"), 1, 1, 32768, 1, &accepted);
        if (!admission || !accepted)
            return;
        commandIndex = static_cast<std::uint64_t>(ordinal - 1);
    }
    if (operation == "symbols") {
        bool accepted = false;
        const auto query = QInputDialog::getText(
            m_window, QStringLiteral("C++ configuration index"),
            QStringLiteral("Symbol name prefix (all captured commands; configuration identity is retained):"),
            QLineEdit::Normal, {}, &accepted);
        if (!admission || !accepted)
            return;
        symbol = query.toStdString();
    }
    if (!admission || m_shutDown || m_projectRoot != root || m_clangd != server ||
        m_cppIndexGeneration != requestGeneration)
        return;
    const auto currentPermission = m_services.trust.snapshot(root);
    const auto currentDatabase = m_clangd->compileCommands();
    if (!currentPermission.trusted || currentPermission.revision != permission.revision || !currentDatabase ||
        currentDatabase->file != database->file)
        return;
    for (auto* view : m_editors->uniqueDocumentViews())
        if (view->document().dirty())
            return;
    m_cppIndexStop.request_stop();
    m_cppIndexStop = std::stop_source{};
    const auto stop = m_cppIndexStop.get_token();
    const auto generation = ++m_cppIndexGeneration;
    const auto documentsCurrent = [self = QPointer<Workbench>(this), stamps = captureIndexViews(*m_editors)] {
        return self && indexViewsCurrent(*self->m_editors, stamps);
    };
    IndexJob job;
    job.root = root;
    job.databaseFile = database->file;
    job.operation = operation;
    job.inspectUnreal = m_unreal != nullptr;
    job.symbol = std::move(symbol);
    job.trustRevision = permission.revision;
    job.generation = generation;
    job.commandIndex = commandIndex;
    job.storeFile =
        io::joinPath(m_options.compileCommandsCache, "semantic-index/" + io::sha256Hex(root) + ".db").value_or("");
    job.helper = QDir(QCoreApplication::applicationDirPath())
                     .filePath(
#ifdef _WIN32
                         QStringLiteral("frost-clang.exe")
#else
                         QStringLiteral("frost-clang")
#endif
                             )
                     .toStdString();
    if (operation == "modules") {
        for (const auto& directory : lang::defaultClangdSearch().sdkDirectories) {
            const auto path = io::joinPath(directory,
#ifdef _WIN32
                                           "clang-scan-deps.exe"
#else
                                           "clang-scan-deps"
#endif
            );
            if (path && io::isAbsolutePath(*path) && m_services.fs.exists(*path).value_or(false)) {
                job.scanner = *path;
                break;
            }
        }
        job.privateDatabase =
            io::joinPath(QDir::tempPath().toStdString(),
                         "frost-module-scan-" + QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString() +
                             "/compile_commands.json")
                .value_or("");
    }
    if (!m_resourceGovernor)
        m_resourceGovernor = std::make_shared<sched::ResourceGovernor>();
    job.governor = m_resourceGovernor;
    auto* progress =
        new QProgressDialog(QStringLiteral("Reading actual C++ index data…"), QStringLiteral("Cancel"), 0, 0, m_window);
    progress->setAttribute(Qt::WA_DeleteOnClose);
    progress->show();
    connect(progress, &QProgressDialog::canceled, this, [source = m_cppIndexStop]() mutable { source.request_stop(); });
    const QPointer<Workbench> self(this);
    const QPointer<QProgressDialog> dialog(progress);
    auto update = [self, dialog, generation](std::size_t done, std::size_t total) {
        if (self)
            QMetaObject::invokeMethod(
                self.data(),
                [self, dialog, generation, done, total] {
                    if (!self || !dialog || self->m_cppIndexGeneration != generation)
                        return;
                    dialog->setLabelText(
                        QStringLiteral("Processed %1 of %2 actual captured commands (serial)").arg(done).arg(total));
                },
                Qt::QueuedConnection);
    };
    auto completed = [self, dialog, root, server, generation, revision = permission.revision,
                      databaseFile = job.databaseFile, stop, documentsCurrent,
                      operation](Result<IndexResult> result) mutable {
        if (!self)
            return;
        QMetaObject::invokeMethod(
            self.data(),
            [self, dialog, root, server, generation, revision, databaseFile, stop, documentsCurrent, operation,
             result = std::move(result)]() mutable {
                if (dialog) {
                    QObject::disconnect(dialog.data(), nullptr, self.data(), nullptr);
                    dialog->close();
                }
                if (!self || stop.stop_requested() || self->m_shutDown || self->m_projectRoot != root ||
                    self->m_clangd != server || self->m_cppIndexGeneration != generation)
                    return;
                if (!documentsCurrent())
                    return;
                const auto activeDatabase = self->m_clangd->compileCommands();
                if (!activeDatabase || activeDatabase->file != databaseFile)
                    return;
                const auto current = self->m_services.trust.snapshot(root);
                if (!current.trusted || current.revision != revision)
                    return;
                for (auto* view : self->m_editors->uniqueDocumentViews())
                    if (view->document().dirty())
                        return;
                if (!result) {
                    QMessageBox::warning(
                        self->m_window, QStringLiteral("C++ index"),
                        display(result.error().message()) +
                            QStringLiteral(
                                "\nCompleted units are atomic snapshots; the requested full rebuild is not complete."));
                    return;
                }
                if (result->preprocessor || result->modules) {
                    auto* output = new QDialog(self->m_window);
                    output->setAttribute(Qt::WA_DeleteOnClose);
                    output->setObjectName(result->modules ? QStringLiteral("cppModuleResults")
                                                          : QStringLiteral("cppPreprocessorResults"));
                    output->setWindowTitle(result->preprocessor
                                               ? QStringLiteral("C++ preprocessor - captured command snapshot")
                                               : QStringLiteral("C++ module dependencies - scanner snapshot"));
                    auto* layout = new QVBoxLayout(output);
                    auto* tabs = new QTabWidget(output);
                    const auto add = [&](QString title, const std::string& bytes) {
                        auto* text = new QPlainTextEdit(tabs);
                        text->setReadOnly(true);
                        text->setPlainText(display(bytes));
                        tabs->addTab(text, title);
                    };
                    if (result->preprocessor) {
                        layout->addWidget(new QLabel(
                            QStringLiteral(
                                "Clang -E output without line markers. Macro events retain definition tokens; expanded "
                                "source is shown separately. Includes record actual directed source-to-target edges."),
                            output));
                        add(QStringLiteral("Expanded source"), result->preprocessor->text);
                        add(QStringLiteral("Include graph"), result->preprocessor->includes.dump());
                        add(QStringLiteral("Macro events"), result->preprocessor->macros.dump());
                        if (result->unrealIssues || !result->unrealIssueError.empty()) {
                            std::string report = result->unrealIssueError;
                            if (result->unrealIssues) {
                                report =
                                    "Actual active Clang invocation tokens; only documented conflicting specifiers are "
                                    "checked. Unknown/version-specific specifiers are not rejected.\n";
                                for (const auto& issue : *result->unrealIssues)
                                    report += issue.path + ":" + std::to_string(issue.line) + ":" +
                                              std::to_string(issue.column) + ": " + issue.macro + ": " + issue.message +
                                              "\n";
                                if (result->unrealIssues->empty())
                                    report += "No supported conflicts in this captured command. This is not a complete "
                                              "UHT validation.\n";
                            }
                            add(QStringLiteral("Unreal reflection inspections"), report);
                        }
                    } else {
                        layout->addWidget(new QLabel(
                            QStringLiteral("Actual clang-scan-deps P1689 report for captured saved commands. Missing "
                                           "scanner or unsupported CDB is reported; no module build is performed."),
                            output));
                        add(QStringLiteral("Module dependency graph"), result->modules->rawP1689);
                        auto* interfaces = new QTableWidget(0, 2, tabs);
                        interfaces->setObjectName(QStringLiteral("cppModuleRelationships"));
                        interfaces->setHorizontalHeaderLabels(
                            {QStringLiteral("Module interface"), QStringLiteral("Scanner source path")});
                        interfaces->setEditTriggers(QAbstractItemView::NoEditTriggers);
                        for (const auto& rule : result->modules->rules)
                            for (const auto& provider : rule.provides)
                                if (provider.interface && result->sourceHashes.contains(provider.sourcePath)) {
                                    const int row = interfaces->rowCount();
                                    interfaces->insertRow(row);
                                    interfaces->setItem(row, 0, new QTableWidgetItem(display(provider.name)));
                                    interfaces->setItem(row, 1, new QTableWidgetItem(display(provider.sourcePath)));
                                }
                        for (const auto& rule : result->modules->rules)
                            for (const auto& dependency : rule.requiresModules) {
                                const auto targets = result->modules->interfaceTargets(dependency);
                                const int row = interfaces->rowCount();
                                interfaces->insertRow(row);
                                interfaces->setItem(
                                    row, 0,
                                    new QTableWidgetItem(display(rule.primaryOutput + " -> " + dependency.name)));
                                const auto target =
                                    targets.size() == 1 && result->sourceHashes.contains(targets.front())
                                        ? targets.front()
                                        : std::string{};
                                auto* source = new QTableWidgetItem(display(target));
                                if (target.empty())
                                    source->setToolTip(QStringLiteral(
                                        "External, uncaptured or ambiguous interface; navigation unavailable."));
                                interfaces->setItem(row, 1, source);
                            }
                        interfaces->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
                        tabs->addTab(interfaces, QStringLiteral("Imports and interfaces"));
                        auto* navigationStatus = new QLabel(output);
                        navigationStatus->setObjectName(QStringLiteral("cppModuleNavigationStatus"));
                        layout->addWidget(navigationStatus);
                        const QPointer<QDialog> owner(output);
                        connect(interfaces, &QTableWidget::cellDoubleClicked, output,
                                [self, owner, interfaces, sourceHashes = result->sourceHashes,
                                 hash = result->databaseHash, inputs = result->moduleInputs, databaseFile, stop, root,
                                 server, generation, revision, documentsCurrent](int row, int) {
                                    if (!self || !documentsCurrent() || row < 0 || row >= interfaces->rowCount())
                                        return;
                                    const auto file = interfaces->item(row, 1)->text().toStdString();
                                    const auto found = sourceHashes.find(file);
                                    if (found == sourceHashes.end())
                                        return;
                                    auto done = [self, owner, file, root, server, generation, revision, databaseFile,
                                                 stop, documentsCurrent](bool valid) {
                                        if (self)
                                            QMetaObject::invokeMethod(
                                                self.data(),
                                                [=] {
                                                    if (!self || !owner || stop.stop_requested() || self->m_shutDown ||
                                                        !documentsCurrent() || self->m_projectRoot != root ||
                                                        self->m_clangd != server ||
                                                        self->m_cppIndexGeneration != generation)
                                                        return;
                                                    const auto database = self->m_clangd->compileCommands();
                                                    if (!database || database->file != databaseFile)
                                                        return;
                                                    const auto permission = self->m_services.trust.snapshot(root);
                                                    if (!permission.trusted || permission.revision != revision)
                                                        return;
                                                    if (!valid) {
                                                        if (auto* status = owner->findChild<QLabel*>(
                                                                QStringLiteral("cppModuleNavigationStatus")))
                                                            status->setText(
                                                                QStringLiteral("Module dependency snapshot changed; "
                                                                               "rescan before navigation."));
                                                        return;
                                                    }
                                                    self->openLocation(display(file), 1, 1);
                                                },
                                                Qt::QueuedConnection);
                                    };
                                    self->m_pool
                                        ->spawn(sched::Lane::Background,
                                                validateModuleAction(&self->m_services.fs, databaseFile, hash, inputs,
                                                                     stop, std::move(done)))
                                        .detach();
                                });
                    }
                    layout->addWidget(tabs);
                    auto* close = new QDialogButtonBox(QDialogButtonBox::Close, output);
                    connect(close, &QDialogButtonBox::rejected, output, &QDialog::reject);
                    layout->addWidget(close);
                    output->resize(1000, 650);
                    output->show();
                    return;
                }
                if (operation == "rebuild" || operation == "clear") {
                    QMessageBox::information(
                        self->m_window, QStringLiteral("C++ index"),
                        operation == "clear"
                            ? QStringLiteral("Persisted configuration index invalidated.")
                            : QStringLiteral("Indexed %1/%2 actual captured commands. Each command keeps its own "
                                             "identity; uncaptured configurations are outside this scope.")
                                  .arg(result->completed)
                                  .arg(result->total));
                    return;
                }
                auto* output = new QDialog(self->m_window);
                output->setAttribute(Qt::WA_DeleteOnClose);
                output->setWindowTitle(QStringLiteral("C++ configuration index — actual snapshot results"));
                auto* layout = new QVBoxLayout(output);
                const auto contextCurrent = [self, root, server, generation, revision, databaseFile, stop,
                                             documentsCurrent] {
                    if (stop.stop_requested() || !documentsCurrent())
                        return false;
                    if (!self || self->m_shutDown || self->m_projectRoot != root || self->m_clangd != server ||
                        self->m_cppIndexGeneration != generation)
                        return false;
                    const auto database = self->m_clangd->compileCommands();
                    if (!database || database->file != databaseFile)
                        return false;
                    const auto permission = self->m_services.trust.snapshot(root);
                    if (!permission.trusted || permission.revision != revision)
                        return false;
                    for (auto* view : self->m_editors->uniqueDocumentViews())
                        if (view->document().dirty())
                            return false;
                    return true;
                };
                {
                    layout->addWidget(new QLabel(
                        QStringLiteral(
                            "%1 locations from saved snapshots; %2 stale matching snapshots excluded%3. %4/%5 captured "
                            "commands have stored snapshots. "
                            "Unindexed commands are not covered. Configuration labels identify exact CDB ordinals.")
                            .arg(result->query.locations.size())
                            .arg(result->query.staleCommands)
                            .arg(result->query.truncated ? QStringLiteral("; result limit reached") : QString{})
                            .arg(result->stored)
                            .arg(result->total),
                        output));
                    auto rows =
                        std::make_shared<std::vector<lang::CppIndexLocation>>(std::move(result->query.locations));
                    auto* table = new QTableWidget(static_cast<int>(rows->size()), 4, output);
                    table->setHorizontalHeaderLabels(
                        {QStringLiteral("Symbol"), QStringLiteral("Configuration / command"), QStringLiteral("Source"),
                         QStringLiteral("Occurrence / relation")});
                    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
                    table->setSelectionBehavior(QAbstractItemView::SelectRows);
                    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
                    for (std::size_t index = 0; index < rows->size(); ++index) {
                        const auto& row = (*rows)[index];
                        table->setItem(static_cast<int>(index), 0, new QTableWidgetItem(display(row.name)));
                        auto* identity = new QTableWidgetItem(display(row.configuration));
                        identity->setToolTip(display(row.commandIdentity));
                        table->setItem(static_cast<int>(index), 1, identity);
                        table->setItem(static_cast<int>(index), 2,
                                       new QTableWidgetItem(display(row.path) + ":" + QString::number(row.line)));
                        table->setItem(static_cast<int>(index), 3, new QTableWidgetItem(roleText(row.roles)));
                    }
                    auto receipts =
                        std::make_shared<std::vector<lang::CppIndexUnit>>(std::move(result->query.receipts));
                    const QPointer<QDialog> resultDialog(output);
                    const auto admitAction = [self, contextCurrent, receipts, databaseFile, stop, resultDialog](
                                                 const lang::CppIndexLocation& location, std::function<void()> action) {
                        if (!contextCurrent())
                            return;
                        const auto receipt = std::find_if(receipts->begin(), receipts->end(), [&](const auto& unit) {
                            return unit.commandIdentity == location.commandIdentity;
                        });
                        if (receipt == receipts->end())
                            return;
                        auto done = [self, resultDialog, contextCurrent, stamps = captureIndexViews(*self->m_editors),
                                     action = std::move(action)](bool valid) {
                            if (self)
                                QMetaObject::invokeMethod(
                                    self.data(),
                                    [self, resultDialog, contextCurrent, stamps, valid, action] {
                                        if (!resultDialog || !valid || !contextCurrent() ||
                                            !indexViewsCurrent(*self->m_editors, stamps))
                                            return;
                                        action();
                                    },
                                    Qt::QueuedConnection);
                        };
                        self->m_pool
                            ->spawn(sched::Lane::Background, validateIndexAction(&self->m_services.fs, databaseFile,
                                                                                 *receipt, stop, std::move(done)))
                            .detach();
                    };
                    connect(table, &QTableWidget::cellDoubleClicked, output,
                            [self, rows, contextCurrent, admitAction](int row, int) {
                                if (contextCurrent() && row >= 0 && static_cast<std::size_t>(row) < rows->size()) {
                                    const auto& location = (*rows)[static_cast<std::size_t>(row)];
                                    admitAction(location, [self, location] {
                                        self->openLocation(display(location.path), static_cast<int>(location.line),
                                                           static_cast<int>(location.column));
                                    });
                                }
                            });
                    layout->addWidget(table);
                    for (const auto& [mode, label] :
                         std::array{std::pair{"usages", "Show all indexed occurrences"},
                                    std::pair{"relations", "Show Clang symbol relations"}}) {
                        auto* button = new QPushButton(QString::fromUtf8(label), output);
                        layout->addWidget(button);
                        connect(button, &QPushButton::clicked, output,
                                [self, rows, table, mode, contextCurrent, admitAction] {
                                    const auto row = table->currentRow();
                                    if (contextCurrent() && row >= 0 && static_cast<std::size_t>(row) < rows->size())
                                        admitAction((*rows)[static_cast<std::size_t>(row)],
                                                    [self, mode, usr = (*rows)[static_cast<std::size_t>(row)].usr] {
                                                        self->showCppIndex(mode, usr);
                                                    });
                                });
                    }
                }
                auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, output);
                connect(buttons, &QDialogButtonBox::rejected, output, &QDialog::reject);
                layout->addWidget(buttons);
                output->resize(1000, 550);
                output->show();
            },
            Qt::QueuedConnection);
    };
    m_pool
        ->spawn(sched::Lane::Background,
                indexTask(&m_services.fs, &m_services.launcher, &m_services.trust, {*m_reactor, *m_pool},
                          std::move(job), stop, std::move(update), std::move(completed)))
        .detach();
}
} // namespace frost::app
