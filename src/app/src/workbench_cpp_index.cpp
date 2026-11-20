// SPDX-License-Identifier: Apache-2.0
// CPP-8 actual multi-command index, configuration-labelled navigation and explicit rebuild/invalidate.
#include "frost/app/workbench.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/io/spawn_log.hpp"
#include "frost/lang/clangd_service.hpp"
#include "frost/lang/compile_commands.hpp"
#include "frost/lang/cpp_index.hpp"
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
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <tuple>

namespace frost::app {
namespace {
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
    std::size_t completed = 0, total = 0, stored = 0;
};
struct IndexJob {
    std::string root, databaseFile, storeFile, helper, operation, symbol;
    std::uint64_t trustRevision = 0, generation = 0;
    std::shared_ptr<sched::ResourceGovernor> governor;
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
} // namespace
void Workbench::registerCppIndexActions() {
    for (const auto& [operation, label] : std::array{std::pair{"rebuild", "Rebuild C++ Configuration Index"},
                                                     std::pair{"symbols", "Find Symbol in C++ Configuration Index…"},
                                                     std::pair{"clear", "Invalidate C++ Configuration Index"}}) {
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
    IndexJob job;
    job.root = root;
    job.databaseFile = database->file;
    job.operation = operation;
    job.symbol = std::move(symbol);
    job.trustRevision = permission.revision;
    job.generation = generation;
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
                        QStringLiteral("Indexed %1 of %2 actual captured commands (serial)").arg(done).arg(total));
                },
                Qt::QueuedConnection);
    };
    auto completed = [self, dialog, root, server, generation, revision = permission.revision,
                      databaseFile = job.databaseFile, stop, operation](Result<IndexResult> result) mutable {
        if (!self)
            return;
        QMetaObject::invokeMethod(
            self.data(),
            [self, dialog, root, server, generation, revision, databaseFile, stop, operation,
             result = std::move(result)]() mutable {
                if (dialog) {
                    QObject::disconnect(dialog.data(), nullptr, self.data(), nullptr);
                    dialog->close();
                }
                if (!self || self->m_shutDown || self->m_projectRoot != root || self->m_clangd != server ||
                    self->m_cppIndexGeneration != generation)
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
                const auto contextCurrent = [self, root, server, generation, revision] {
                    if (!self || self->m_shutDown || self->m_projectRoot != root || self->m_clangd != server ||
                        self->m_cppIndexGeneration != generation)
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
                        using ViewStamp = std::tuple<QPointer<DocumentEditorView>, std::string, std::uint64_t>;
                        std::vector<ViewStamp> stamps;
                        for (auto* view : self->m_editors->uniqueDocumentViews())
                            stamps.emplace_back(view, view->path(), view->document().document().version());
                        auto done = [self, resultDialog, contextCurrent, stamps = std::move(stamps),
                                     action = std::move(action)](bool valid) {
                            if (self)
                                QMetaObject::invokeMethod(
                                    self.data(),
                                    [self, resultDialog, contextCurrent, stamps, valid, action] {
                                        if (!resultDialog || !valid || !contextCurrent() ||
                                            self->m_editors->uniqueDocumentViews().size() != stamps.size())
                                            return;
                                        for (const auto& [view, path, version] : stamps)
                                            if (!view || view->path() != path ||
                                                view->document().document().version() != version)
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
