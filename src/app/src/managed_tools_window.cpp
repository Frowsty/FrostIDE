// SPDX-License-Identifier: Apache-2.0
#include "managed_tools_window.hpp"

#include "frost/build/managed_evaluation.hpp"
#include "frost/build/managed_packages.hpp"
#include "frost/build/run_session.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/project/trust_store.hpp"
#include "frost/run/dotnet_tests.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QSplitter>
#include <QTabWidget>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTextDocument>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>
#include <mutex>
#include <set>
#include <thread>

namespace frost::app {
namespace {
QString q(std::string_view text) { return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())); }
class ManagedToolsWidget final : public QWidget {
public:
    ManagedToolsWidget(QWidget* parent, ManagedToolsContext context, bool tests)
        : QWidget(parent), m_context(std::move(context)), m_tests(tests) {
        setObjectName(tests ? "dotnetTestsWidget" : "managedPackagesWidget");
        auto* outer = new QVBoxLayout(this);
        m_form = new QWidget(this);
        auto* form = new QFormLayout(m_form);
        m_project = new QComboBox(m_form);
        m_project->setEditable(true);
        m_project->setObjectName("managedProjectPath");
        if (m_context.projects)
            for (const auto& path : m_context.projects())
                m_project->addItem(q(path));
        auto* browse = new QPushButton(tr("Choose project…"), m_form);
        auto* projectRow = new QHBoxLayout();
        projectRow->addWidget(m_project, 1);
        projectRow->addWidget(browse);
        form->addRow(tr("Project"), projectRow);
        connect(browse, &QPushButton::clicked, this, [this] {
            const auto path = QFileDialog::getOpenFileName(this, tr("Select SDK C# project"),
                                                           m_context.root ? q(m_context.root()) : QString(),
                                                           tr("C# projects (*.csproj)"));
            if (!path.isEmpty())
                m_project->setEditText(path);
        });
        m_framework = new QLineEdit(m_form);
        m_framework->setObjectName("managedTargetFramework");
        m_framework->setPlaceholderText(tr("Explicit evaluated framework, e.g. net10.0"));
        form->addRow(tr("Target framework"), m_framework);
        m_configuration = new QComboBox(m_form);
        m_configuration->setEditable(true);
        m_configuration->addItems({"Debug", "Release"});
        form->addRow(tr("Configuration"), m_configuration);
        if (tests) {
            m_platform = new QComboBox(m_form);
            m_platform->addItems({"VSTest", "Microsoft.Testing.Platform"});
            form->addRow(tr("Project test runner"), m_platform);
            m_build = new QCheckBox(tr("Build before discovery/run (restore separately)"), m_form);
            m_build->setChecked(true);
            form->addRow(m_build);
        } else {
            m_search = new QLineEdit(m_form);
            m_search->setObjectName("managedPackageSearch");
            form->addRow(tr("Package ID / search"), m_search);
            m_version = new QLineEdit(m_form);
            form->addRow(tr("Version / range"), m_version);
            m_source = new QLineEdit(m_form);
            m_source->setPlaceholderText(tr("Empty: configured NuGet sources and credential providers"));
            form->addRow(tr("Source override"), m_source);
            m_prerelease = new QCheckBox(tr("Include prerelease"), m_form);
            form->addRow(m_prerelease);
            m_helper = new QLineEdit(
                QDir(QCoreApplication::applicationDirPath()).filePath("managed-nuget/NuGetPlanner.dll"), m_form);
            form->addRow(tr("Package edit tool"), m_helper);
        }
        auto* buttons = new QHBoxLayout();
        const auto button = [&](const QString& label, auto handler) {
            auto* item = new QPushButton(label, m_form);
            buttons->addWidget(item);
            connect(item, &QPushButton::clicked, this, handler);
        };
        if (tests) {
            button(tr("Discover"), [this] { test(true, {}); });
            button(tr("Run All"), [this] { test(false, {}); });
            button(tr("Run Selected Methods"), [this] {
                std::vector<std::string> names;
                std::set<int> rows;
                for (const auto* item : m_table->selectedItems())
                    rows.insert(item->row());
                for (const int row : rows) {
                    const auto name = m_table->item(row, 0)->data(Qt::UserRole).toString().toStdString();
                    if (name.empty()) {
                        error("This result has no exact selectable adapter identity");
                        return;
                    }
                    names.push_back(name);
                }
                if (names.empty()) {
                    error("Select at least one discovered test");
                    return;
                }
                test(false, std::move(names));
            });
            button(tr("Run Failed Methods"), [this] {
                if (m_failed.empty()) {
                    error("There are no failed adapter identities to rerun");
                    return;
                }
                test(false, m_failed);
            });
        } else {
            button(tr("Search"), [this] { packages(build::PackageQuery::Search); });
            button(tr("Versions"), [this] { packages(build::PackageQuery::Search, true); });
            button(tr("Installed"), [this] { packages(build::PackageQuery::Installed); });
            button(tr("Updates"), [this] { packages(build::PackageQuery::Outdated); });
            button(tr("Vulnerable"), [this] { packages(build::PackageQuery::Vulnerable); });
            button(tr("Deprecated"), [this] { packages(build::PackageQuery::Deprecated); });
            button(tr("Restore"), [this] { packages(build::PackageQuery::Restore); });
            button(tr("Install…"), [this] { edit("install"); });
            button(tr("Update…"), [this] { edit("update"); });
            button(tr("Remove…"), [this] { edit("remove"); });
        }
        form->addRow(buttons);
        outer->addWidget(m_form);
        m_stop = new QPushButton(tr("Stop"), this);
        m_stop->setEnabled(false);
        outer->addWidget(m_stop);
        connect(m_stop, &QPushButton::clicked, this, [this] {
            if (m_job)
                m_job->stop();
            if (m_preparing)
                m_snapshot.request_stop();
        });
        m_status = new QLabel(tr("Choose an explicit SDK project and framework. Actions execute project code only in a "
                                 "trusted workspace."),
                              this);
        m_status->setWordWrap(true);
        outer->addWidget(m_status);
        m_table = new QTableWidget(this);
        m_table->setColumnCount(tests ? 3 : 5);
        m_table->setHorizontalHeaderLabels(
            tests ? QStringList{"Test", "Outcome", "Duration"}
                  : QStringList{"Package", "Version", "Framework / source", "Requested / latest", "Status"});
        m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
        m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
        outer->addWidget(m_table, 1);
        m_log = new QPlainTextEdit(this);
        m_log->setReadOnly(true);
        m_log->document()->setMaximumBlockCount(2000);
        outer->addWidget(m_log, 1);
        connect(m_table, &QTableWidget::itemSelectionChanged, this, [this] {
            const int row = m_table->currentRow();
            if (m_tests && row >= 0 && static_cast<size_t>(row) < m_results.size()) {
                QString text = q(m_results[static_cast<size_t>(row)].result.output);
                for (const auto& failure : m_results[static_cast<size_t>(row)].result.failures)
                    text += "\n" + q(failure.message);
                m_log->setPlainText(text);
            } else if (!m_tests && row >= 0) {
                m_search->setText(m_table->item(row, 0)->text());
                const auto offered = m_table->item(row, 1)->data(Qt::UserRole).toString();
                m_version->setText(offered.isEmpty() ? m_table->item(row, 1)->text() : offered);
            }
        });
        connect(m_table, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
            if (m_context.openLocation && m_tests && row >= 0 && static_cast<size_t>(row) < m_results.size())
                for (const auto& failure : m_results[static_cast<size_t>(row)].result.failures)
                    if (failure.location) {
                        m_context.openLocation(failure.location->file, failure.location->line);
                        break;
                    }
        });
        const auto invalidate = [this] {
            if (!m_job && !m_preparing) {
                m_table->setRowCount(0);
                m_results.clear();
                m_failed.clear();
            }
        };
        connect(m_project, &QComboBox::currentTextChanged, this, invalidate);
        connect(m_framework, &QLineEdit::textChanged, this, invalidate);
        connect(m_configuration, &QComboBox::currentTextChanged, this, invalidate);
        if (m_platform)
            connect(m_platform, &QComboBox::currentIndexChanged, this, [invalidate](int) { invalidate(); });
        auto* timer = new QTimer(this);
        timer->setInterval(100);
        connect(timer, &QTimer::timeout, this, [this] {
            if ((m_job || m_preparing) && !current()) {
                if (m_job)
                    m_job->stop();
                if (m_preparing)
                    m_snapshot.request_stop();
                m_status->setText(tr("Workspace or trust changed; cancelling the managed operation."));
            }
        });
        timer->start();
    }
    ~ManagedToolsWidget() override {
        if (m_job)
            m_job->stop();
        m_job.reset();
        m_snapshot.request_stop();
        if (m_snapshot.joinable())
            m_snapshot.join();
    }

private:
    void error(std::string_view message) {
        m_status->setText(q(message));
        m_workflow = false;
        if (!m_job && !m_preparing) {
            m_form->setEnabled(true);
            m_stop->setEnabled(false);
        }
    }
    build::ManagedProjectRequest request() const {
        return {m_context.dotnet ? m_context.dotnet() : std::string(),
                m_project->currentText().toStdString(),
                m_configuration->currentText().toStdString(),
                m_framework->text().trimmed().toStdString(),
                {}};
    }
    bool current() const {
        if (!m_context.root || !m_context.trust || m_context.root() != m_root ||
            (m_context.generation && m_context.generation() != m_generation) ||
            (m_context.dotnet && m_context.dotnet() != m_request.dotnet))
            return false;
        const auto trust = m_context.trust->snapshot(m_root);
        return trust.trusted && trust.revision == m_trustRevision;
    }
    bool begin() {
        if (m_job || m_preparing || m_workflow) {
            error("A managed operation is already running");
            return false;
        }
        if (!m_context.fs || !m_context.launcher || !m_context.trust || !m_context.root) {
            error("Managed workspace services are unavailable");
            return false;
        }
        m_root = m_context.root();
        m_generation = m_context.generation ? m_context.generation() : 0;
        const auto trust = m_context.trust->snapshot(m_root);
        m_trustRevision = trust.revision;
        if (!trust.trusted) {
            error("Trust the workspace before running managed tools");
            return false;
        }
        if (m_context.prepare) {
            const auto saved = m_context.prepare();
            if (!saved) {
                error(saved.error().message());
                return false;
            }
        }
        m_request = request();
        if (!current()) {
            error("Workspace changed during Save All");
            return false;
        }
        auto physicalRoot = m_context.fs->canonicalPath(m_root);
        auto physicalProject = m_context.fs->canonicalPath(m_request.project);
        if (!physicalRoot || !physicalProject) {
            error("Cannot resolve managed project ownership");
            return false;
        }
        const auto relative = QDir(q(*physicalRoot)).relativeFilePath(q(*physicalProject));
        if (relative == ".." || relative.startsWith("../") || QDir::isAbsolutePath(relative) ||
            !m_context.trust->isTrusted(*physicalProject)) {
            error("Select a project physically inside the trusted workspace");
            return false;
        }
        if (m_request.targetFramework.empty()) {
            error("Select an explicit target framework");
            return false;
        }
        m_workflow = true;
        return true;
    }
    void launch(io::ProcessSpec spec, std::function<void(const build::BuildResult&, std::string)> done,
                std::string input = {}) {
        if (!current()) {
            error("Workspace or trust changed");
            return;
        }
        m_form->setEnabled(false);
        m_stop->setEnabled(true);
        m_log->clear();
        struct Output {
            std::mutex mutex;
            std::string text;
            bool overflow = false;
            size_t logBytes = 0;
        };
        auto output = std::make_shared<Output>();
        QPointer<ManagedToolsWidget> self(this);
        build::RunSessionCallbacks callbacks;
        callbacks.onOutput = [self, output](build::RunStream stream, std::string_view chunk) {
            bool publish = false, stop = false;
            {
                std::lock_guard lock(output->mutex);
                if (!output->overflow) {
                    if (chunk.size() > 4 * 1024 * 1024 - output->logBytes) {
                        output->overflow = true;
                        stop = true;
                    } else {
                        output->logBytes += chunk.size();
                        publish = true;
                        if (stream == build::RunStream::Stdout)
                            output->text += chunk;
                    }
                }
            }
            if (self && stop)
                QMetaObject::invokeMethod(self.data(), [self] {
                    if (self && self->m_job)
                        self->m_job->stop();
                });
            if (self && publish)
                QMetaObject::invokeMethod(self.data(), [self, text = std::string(chunk)] {
                    if (self)
                        self->m_log->appendPlainText(q(text));
                });
        };
        callbacks.onExited = [self, output, done = std::move(done)](const build::RunExit& exit) {
            build::BuildResult result;
            result.success =
                exit.exitCode == 0 && !exit.stopped && !exit.waitFailed && !exit.cleanupFailed && !exit.decodingFailed;
            result.cancelled = exit.stopped;
            result.exitCode = exit.exitCode;
            result.duration = exit.duration;
            std::string text;
            bool overflow;
            {
                std::lock_guard lock(output->mutex);
                text = output->text;
                overflow = output->overflow;
            }
            if (self)
                QMetaObject::invokeMethod(
                    self, [self, result, text = std::move(text), overflow, done = std::move(done)]() mutable {
                        if (!self)
                            return;
                        self->m_job.reset();
                        if (!self->current()) {
                            self->error("Discarded stale managed tool output");
                            return;
                        }
                        if (overflow) {
                            self->error("Managed tool output exceeded its 4 MiB budget");
                            return;
                        }
                        if (result.cancelled) {
                            self->error("Managed operation cancelled");
                            return;
                        }
                        done(result, std::move(text));
                    });
        };
        if (!input.empty())
            spec.stdinMode = io::Stdio::Pipe;
        auto started = build::RunSession::start(*m_context.launcher, spec, std::move(callbacks), systemClock(),
                                                {.terminateDescendantsOnExit = true, .rejectInvalidUtf8 = true});
        if (!started) {
            m_form->setEnabled(true);
            m_stop->setEnabled(false);
            error(started.error().message());
            return;
        }
        m_job = std::move(*started);
        if (!input.empty()) {
            const auto sent = m_job->writeInput(std::move(input));
            if (!sent) {
                m_job->stop();
                error(sent.error().message());
            }
            m_job->closeInput();
        }
    }
    void evaluate(std::function<void(build::ManagedProjectEvaluation)> next) {
        auto spec = build::managedProjectEvaluationSpec(m_request);
        if (!spec) {
            error(spec.error().message());
            return;
        }
        launch(*spec, [this, next = std::move(next)](const auto& exit, std::string output) {
            if (!exit.success) {
                error("MSBuild evaluation failed; see tool output");
                return;
            }
            auto model = build::parseManagedProjectEvaluation(m_request, output);
            if (!model) {
                error(model.error().message());
                return;
            }
            next(std::move(*model));
        });
    }
    void packages(build::PackageQuery query, bool exact = false) {
        if (!begin())
            return;
        build::ManagedPackageRequest options;
        options.project = m_request;
        options.query = query;
        options.search = m_search->text().trimmed().toStdString();
        if (query == build::PackageQuery::Search || query == build::PackageQuery::Restore ||
            query == build::PackageQuery::Outdated || query == build::PackageQuery::Deprecated)
            options.source = m_source->text().trimmed().toStdString();
        options.prerelease = m_prerelease->isChecked();
        options.exactMatch = exact;
        auto spec = build::managedPackageSpec(options);
        if (!spec) {
            error(spec.error().message());
            return;
        }
        launch(*spec, [this, query](const auto& exit, std::string output) {
            if (query == build::PackageQuery::Restore) {
                error(exit.success ? "NuGet restore completed. Refresh package data and project evaluation."
                                   : "NuGet restore failed; see output");
                return;
            }
            auto report = build::parseManagedPackageReport(output, query);
            if (!report) {
                error(report.error().message());
                return;
            }
            if (query != build::PackageQuery::Search) {
                const auto project = m_context.fs->canonicalPath(report->projectPath),
                           expected = m_context.fs->canonicalPath(m_request.project);
                if (!project || !expected || *project != *expected) {
                    error("NuGet returned a different project identity");
                    return;
                }
            }
            m_table->setRowCount(0);
            for (const auto& package : report->packages) {
                const int row = m_table->rowCount();
                m_table->insertRow(row);
                const QStringList fields{q(package.id), q(package.version),
                                         q(package.framework.empty() ? package.source : package.framework),
                                         q(package.requested + (package.latest.empty() ? "" : " / " + package.latest)),
                                         q(package.status + (package.transitive ? " (transitive)" : ""))};
                for (int col = 0; col < fields.size(); ++col)
                    m_table->setItem(row, col, new QTableWidgetItem(fields[col]));
                m_table->item(row, 1)->setData(Qt::UserRole,
                                               q(package.latest.empty() ? package.version : package.latest));
            }
            std::string status = exit.success ? "Package query completed." : "Package query reported errors.";
            for (const auto& issue : report->problems)
                status += "\n" + issue;
            error(status);
        });
    }
    void test(bool discover, std::vector<std::string> selected) {
        if (!begin())
            return;
        const bool build = m_build->isChecked();
        const auto platform = m_platform->currentIndex() == 0 ? run::DotnetTestPlatform::VSTest
                                                              : run::DotnetTestPlatform::MicrosoftTestingPlatform;
        evaluate([this, discover, selected = std::move(selected), build, platform](const auto& model) {
            const auto fact = model.properties.find("IsTestProject");
            if (fact == model.properties.end() || fact->second != "true") {
                error("The selected framework does not evaluate IsTestProject=true");
                return;
            }
            const auto reportOwner = std::make_shared<QTemporaryDir>(QDir::tempPath() + "/frost-dotnet-tests-XXXXXX");
            const auto report = reportOwner->path();
            if (!reportOwner->isValid()) {
                error("Cannot create private test report directory");
                return;
            }
            run::DotnetTestRequest options;
            options.project = m_request;
            options.reportDirectory = report.toStdString();
            options.discover = discover;
            options.selected = selected;
            options.build = build;
            options.platform = platform;
            auto spec = run::dotnetTestSpec(options);
            if (!spec) {
                reportOwner->remove();
                error(spec.error().message());
                return;
            }
            launch(*spec, [this, discover, report, reportOwner](const auto& exit, std::string output) {
                if (discover) {
                    reportOwner->remove();
                    if (!exit.success) {
                        error("dotnet test discovery failed; see output");
                        return;
                    }
                    auto names = run::parseDotnetTestDiscovery(output);
                    if (!names) {
                        error(names.error().message());
                        return;
                    }
                    m_results.clear();
                    m_table->setRowCount(0);
                    for (const auto& name : *names) {
                        const int row = m_table->rowCount();
                        m_table->insertRow(row);
                        auto* item = new QTableWidgetItem(q(name));
                        item->setData(Qt::UserRole, QString());
                        m_table->setItem(row, 0, item);
                        m_table->setItem(row, 1, new QTableWidgetItem(tr("Discovered")));
                    }
                    error("Adapter discovery completed. No test results have been inferred.");
                    return;
                }
                auto bytes = m_context.fs->readFile((report + "/results.trx").toStdString());
                reportOwner->remove();
                if (!bytes) {
                    error("The test process produced no TRX report; inspect tool output (MTP requires its TRX "
                          "extension).");
                    return;
                }
                auto results = run::parseDotnetTrx(*bytes);
                if (!results) {
                    error(results.error().message());
                    return;
                }
                m_results = std::move(*results);
                m_failed.clear();
                m_table->setRowCount(0);
                for (const auto& item : m_results) {
                    const int row = m_table->rowCount();
                    m_table->insertRow(row);
                    auto* name = new QTableWidgetItem(q(item.result.name));
                    name->setData(Qt::UserRole, q(item.fullyQualifiedName));
                    m_table->setItem(row, 0, name);
                    m_table->setItem(row, 1, new QTableWidgetItem(q(run::toString(item.result.outcome))));
                    m_table->setItem(row, 2, new QTableWidgetItem(q(run::formatTestDuration(item.result.duration))));
                    if (run::countsAsFailure(item.result.outcome) && !item.fullyQualifiedName.empty())
                        m_failed.push_back(item.fullyQualifiedName);
                }
                error(exit.success
                          ? "Test process completed; displayed outcomes come from TRX."
                          : "Test process exited with failure; displayed outcomes come from its real TRX report.");
            });
        });
    }
    void snapshotDependencies(std::set<std::string> paths,
                              std::function<void(Result<std::vector<document::ReadDependency>>)> done) {
        if (paths.size() > 4096) {
            error("Too many MSBuild import dependencies");
            return;
        }
        m_preparing = true;
        m_form->setEnabled(false);
        m_stop->setEnabled(true);
        QPointer<ManagedToolsWidget> self(this);
        auto* fs = m_context.fs;
        m_snapshot =
            std::jthread([self, fs, paths = std::move(paths), done = std::move(done)](std::stop_token stop) mutable {
                auto result = [&]() -> Result<std::vector<document::ReadDependency>> {
                    std::vector<document::ReadDependency> dependencies;
                    size_t bytes = 0;
                    for (const auto& path : paths) {
                        if (stop.stop_requested())
                            return fail(ErrorCode::Cancelled, "MSBuild dependency capture cancelled");
                        auto contents = fs->readFile(path);
                        if (!contents)
                            return std::unexpected(contents.error());
                        if (contents->size() > 4 * 1024 * 1024 || bytes + contents->size() > 32 * 1024 * 1024)
                            return fail(ErrorCode::OutOfRange, "MSBuild import snapshot budget exceeded");
                        bytes += contents->size();
                        dependencies.push_back({path, *contents, false});
                    }
                    return dependencies;
                }();
                if (self)
                    QMetaObject::invokeMethod(
                        self.data(), [self, result = std::move(result), done = std::move(done)]() mutable {
                            if (!self)
                                return;
                            self->m_preparing = false;
                            if (!self->current() || self->m_snapshot.get_stop_token().stop_requested()) {
                                self->error("Discarded stale or cancelled dependency snapshots");
                                return;
                            }
                            done(std::move(result));
                        });
            });
    }
    void edit(std::string operation);
    ManagedToolsContext m_context;
    bool m_tests;
    QWidget* m_form = nullptr;
    QComboBox *m_project = nullptr, *m_configuration = nullptr, *m_platform = nullptr;
    QLineEdit *m_framework = nullptr, *m_search = nullptr, *m_version = nullptr, *m_source = nullptr,
              *m_helper = nullptr;
    QCheckBox *m_prerelease = nullptr, *m_build = nullptr;
    QPushButton* m_stop = nullptr;
    QLabel* m_status = nullptr;
    QTableWidget* m_table = nullptr;
    QPlainTextEdit* m_log = nullptr;
    std::unique_ptr<build::RunSession> m_job;
    std::jthread m_snapshot;
    bool m_preparing = false, m_workflow = false;
    build::ManagedProjectRequest m_request;
    std::string m_root;
    std::uint64_t m_trustRevision = 0, m_generation = 0;
    std::vector<run::DotnetTestResult> m_results;
    std::vector<std::string> m_failed;
};
void ManagedToolsWidget::edit(std::string operation) {
    if (!begin())
        return;
    if (!m_context.apply) {
        error("Native project edit transactions are unavailable");
        return;
    }
    const auto package = m_search->text().trimmed().toStdString(), version = m_version->text().trimmed().toStdString();
    const auto helper = m_helper->text().trimmed().toStdString();
    if (!io::isAbsolutePath(helper)) {
        error("Configure an absolute installed package edit tool path");
        return;
    }
    evaluate([this, operation = std::move(operation), package, version,
              helper](const build::ManagedProjectEvaluation& model) {
        const auto property = [&](std::string_view key) {
            const auto it = model.properties.find(key);
            return it == model.properties.end() ? std::string() : it->second;
        };
        const auto sdk = property("MSBuildToolsPath");
        if (!io::isAbsolutePath(sdk)) {
            error("MSBuild did not report an installed SDK directory");
            return;
        }
        const auto root = m_context.fs->canonicalPath(m_root), project = m_context.fs->canonicalPath(model.projectFile);
        if (!root || !project) {
            error("Cannot resolve evaluated project identity");
            return;
        }
        const auto references = model.items.find("PackageReference");
        if (references != model.items.end())
            for (const auto& item : references->second)
                if (q(item.identity).compare(q(package), Qt::CaseInsensitive) == 0) {
                    const auto owner = item.metadata.find("DefiningProjectFullPath");
                    if (owner == item.metadata.end()) {
                        error("Package reference has no evaluated defining project");
                        return;
                    }
                    const auto physical = m_context.fs->canonicalPath(owner->second);
                    if (!physical || *physical != *project) {
                        error("This package is declared by an import; select its owning definition before editing");
                        return;
                    }
                }
        std::string central;
        if (property("ManagePackageVersionsCentrally") == "true") {
            central = property("DirectoryPackagesPropsPath");
            if (!io::isAbsolutePath(central)) {
                error("MSBuild did not identify the central package version file");
                return;
            }
            const auto canonical = m_context.fs->canonicalPath(central);
            if (!canonical) {
                error("Cannot resolve central package definition");
                return;
            }
            central = *canonical;
            const auto relative = QDir(q(*root)).relativeFilePath(q(central));
            if (relative == ".." || relative.startsWith("../") || QDir::isAbsolutePath(relative)) {
                error("The shared central package definition leaves this workspace");
                return;
            }
            if (const auto versions = model.items.find("PackageVersion"); versions != model.items.end())
                for (const auto& item : versions->second)
                    if (q(item.identity).compare(q(package), Qt::CaseInsensitive) == 0) {
                        const auto owner = item.metadata.find("DefiningProjectFullPath");
                        if (owner == item.metadata.end()) {
                            error("Central package has no evaluated owner");
                            return;
                        }
                        const auto physical = m_context.fs->canonicalPath(owner->second);
                        if (!physical || *physical != central) {
                            error("Central package version is owned by an import; a scoped ownership edit is required");
                            return;
                        }
                    }
        }
        std::set<std::string> paths{*project};
        if (!central.empty())
            paths.insert(central);
        const auto imports = QString::fromStdString(property("MSBuildAllProjects")).split(';', Qt::SkipEmptyParts);
        for (const auto& path : imports)
            paths.insert(path.toStdString());
        snapshotDependencies(std::move(paths), [this, root = *root, project = *project, central, sdk, helper, operation,
                                                package, version, baseline = model](
                                                   Result<std::vector<document::ReadDependency>> captured) {
            if (!captured) {
                error(captured.error().message());
                return;
            }
            auto dependencies = std::move(*captured);
            evaluate([this, root, project, central, sdk, helper, operation, package, version,
                      dependencies = std::move(dependencies),
                      baseline](const build::ManagedProjectEvaluation& refreshed) mutable {
                if (refreshed.items != baseline.items || refreshed.properties != baseline.properties) {
                    error("Managed project evaluation changed during dependency capture; request a fresh package "
                          "proposal");
                    return;
                }
                QJsonObject payload{{"trusted", true},
                                    {"root", q(root)},
                                    {"project", q(project)},
                                    {"package", q(package)},
                                    {"version", q(version)},
                                    {"operation", q(operation)},
                                    {"centralFile", q(central)},
                                    {"configuration", q(m_request.configuration)},
                                    {"targetFramework", q(m_request.targetFramework)}};
                auto spec = build::managedProjectQuerySpec(m_request);
                if (!spec) {
                    error(spec.error().message());
                    return;
                }
                spec->arguments = {helper, "--sdk", sdk};
                const auto selectedProject = project, selectedCentral = central;
                launch(
                    *spec,
                    [this, selectedProject, selectedCentral, dependencies = std::move(dependencies), operation,
                     package](const auto& exit, std::string output) {
                        if (!exit.success) {
                            error("Package edit proposal failed; inspect tool output");
                            return;
                        }
                        QJsonParseError parseError;
                        const auto json = QJsonDocument::fromJson(QByteArray::fromStdString(output), &parseError);
                        if (parseError.error != QJsonParseError::NoError || !json.isObject()) {
                            error("Invalid package edit proposal JSON");
                            return;
                        }
                        const auto object = json.object();
                        if (object.value("version").toInt() != 1 ||
                            object.value("project").toString() != q(selectedProject) ||
                            object.value("package").toString() != q(package) ||
                            object.value("operation").toString() != q(operation) || !object.value("files").isArray()) {
                            error("Package proposal identity changed");
                            return;
                        }
                        document::RefactoringPlan plan;
                        plan.label = operation + " NuGet package " + package;
                        plan.dependencies = dependencies;
                        std::set<std::string> seen;
                        for (const auto row : object.value("files").toArray()) {
                            if (!row.isObject()) {
                                error("Malformed package proposal participant");
                                return;
                            }
                            const auto file = row.toObject();
                            const auto path = file.value("path").toString().toStdString();
                            if ((path != selectedProject && path != selectedCentral) || !seen.insert(path).second) {
                                error("Package proposal modified an unowned definition");
                                return;
                            }
                            const auto before = QByteArray::fromBase64Encoding(
                                           file.value("beforeBase64").toString().toLatin1(),
                                           QByteArray::AbortOnBase64DecodingErrors),
                                       after = QByteArray::fromBase64Encoding(
                                           file.value("afterBase64").toString().toLatin1(),
                                           QByteArray::AbortOnBase64DecodingErrors);
                            if (!before || !after || before.decoded.size() > 2 * 1024 * 1024 ||
                                after.decoded.size() > 2 * 1024 * 1024) {
                                error("Invalid package proposal file images");
                                return;
                            }
                            const auto expected =
                                std::find_if(dependencies.begin(), dependencies.end(),
                                             [&](const auto& dependency) { return dependency.path == path; });
                            if (expected == dependencies.end() || !expected->expectedBytes ||
                                *expected->expectedBytes != before.decoded.toStdString()) {
                                error("Package definition changed after evaluation");
                                return;
                            }
                            plan.disk.push_back({path, before.decoded.toStdString(), after.decoded.toStdString()});
                        }
                        if (plan.disk.empty()) {
                            error("The package definition already matches this proposal");
                            return;
                        }
                        QPointer<ManagedToolsWidget> self(this);
                        const auto root = m_root;
                        const auto revision = m_trustRevision, generation = m_generation;
                        plan.revalidateContext = [self, root, revision, generation]() -> Result<void> {
                            if (!self || !self->current() || self->m_root != root ||
                                self->m_trustRevision != revision || self->m_generation != generation)
                                return fail(ErrorCode::Cancelled, "Package preview context changed");
                            return {};
                        };
                        QDialog preview;
                        preview.setWindowModality(Qt::ApplicationModal);
                        preview.setWindowTitle(tr("Preview NuGet package edit"));
                        preview.resize(900, 600);
                        auto* layout = new QVBoxLayout(&preview);
                        layout->addWidget(
                            new QLabel(tr("All shown definition changes form one native transaction. Central package "
                                          "versions can affect other projects. Restore is a separate action."),
                                       &preview));
                        auto* tabs = new QTabWidget(&preview);
                        layout->addWidget(tabs, 1);
                        for (const auto& file : plan.disk) {
                            auto* split = new QSplitter(tabs);
                            auto* before = new QPlainTextEdit(q(file.expectedBytes), split);
                            before->setReadOnly(true);
                            auto* after = new QPlainTextEdit(q(file.afterBytes), split);
                            after->setReadOnly(true);
                            tabs->addTab(split, q(file.path));
                        }
                        auto* buttons =
                            new QDialogButtonBox(QDialogButtonBox::Apply | QDialogButtonBox::Cancel, &preview);
                        layout->addWidget(buttons);
                        connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, &preview,
                                &QDialog::accept);
                        connect(buttons, &QDialogButtonBox::rejected, &preview, &QDialog::reject);
                        const auto decision = preview.exec();
                        if (!self)
                            return;
                        if (decision != QDialog::Accepted) {
                            error("Package preview cancelled");
                            return;
                        }
                        if (!plan.revalidateContext()) {
                            error("Package preview context changed; request a fresh proposal");
                            return;
                        }
                        const auto apply = m_context.apply;
                        const auto applied = apply(plan);
                        if (!self)
                            return;
                        if (!applied) {
                            error(applied.error().message());
                            return;
                        }
                        error(
                            applied->status == document::RefactoringStatus::PendingRecovery
                                ? "Package transaction is pending native recovery; use Resume before further edits."
                                : "Package definition transaction applied. Global Undo/Redo retains the exact images. "
                                  "Restore to resolve packages.");
                    },
                    QJsonDocument(payload).toJson(QJsonDocument::Compact).toStdString() + "\n");
            });
        });
    });
}

} // namespace
QWidget* createManagedPackagesWidget(QWidget* parent, ManagedToolsContext context) {
    return new ManagedToolsWidget(parent, std::move(context), false);
}
QWidget* createDotnetTestsWidget(QWidget* parent, ManagedToolsContext context) {
    return new ManagedToolsWidget(parent, std::move(context), true);
}
} // namespace frost::app
