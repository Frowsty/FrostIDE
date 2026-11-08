// SPDX-License-Identifier: Apache-2.0
//
// The Unit Tests window of project_properties_dialog.hpp: tests and entry points found by source
// text recognisers, every one labelled "Discovered, not run" until the test runner (RUN-7), and the
// window over them (split from project_properties_dialog.cpp, FND-12a).

#include "frost/io/path.hpp"

#include "project_properties_dialog.hpp"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSplitter>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <memory>

namespace frost::app {

namespace {

QString toQs(std::string_view sv) { return QString::fromUtf8(sv.data(), static_cast<qsizetype>(sv.size())); }

QString testRunnerUnavailable() { return QStringLiteral("Requires the test runner (RUN-7)."); }

} // namespace

std::vector<DiscoveredTestEntry> discoverTestsAndEntryPointsInFile(std::string_view filePath, std::string_view content,
                                                                   std::string_view projectName,
                                                                   std::string_view projectPath,
                                                                   std::string_view solutionName) {
    std::vector<DiscoveredTestEntry> results;
    const QString qPath = toQs(filePath);
    const bool isCs = qPath.endsWith(QStringLiteral(".cs"), Qt::CaseInsensitive);
    const QString textContent = toQs(content);
    const QStringList lines = textContent.split(QLatin1Char('\n'));

    const QRegularExpression kGTestRe(QStringLiteral(
        R"(^\s*(?:TEST|TEST_F|TEST_P|TYPED_TEST)\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*,\s*([A-Za-z_][A-Za-z0-9_]*)\s*\))"));
    const QRegularExpression kCatchRe(QStringLiteral(R"re(^\s*(?:TEST_CASE|SCENARIO)\s*\(\s*"([^"]+)")re"));
    const QRegularExpression kCsClassRe(QStringLiteral(R"(\bclass\s+([A-Za-z_][A-Za-z0-9_]*)\b)"));
    const QRegularExpression kCsAttrRe(QStringLiteral(R"(^\s*\[\s*(?:Fact|Theory|Test|TestCase|TestMethod)\b)"));
    const QRegularExpression kCsMethodRe(
        QStringLiteral(R"(\b(?:void|Task(?:<[^>]+>)?)\s+([A-Za-z_][A-Za-z0-9_]*)\s*\()"));
    const QRegularExpression kCppMainRe(QStringLiteral(R"(^\s*(?:int|void|auto)\s+main\s*\()"));
    const QRegularExpression kCsMainRe(QStringLiteral(
        R"(^\s*(?:(?:public|private|internal|static|async)\s+)*(?:void|int|Task(?:<int>)?)\s+Main\s*\()"));

    const std::string resolvedSln = solutionName.empty() ? "Solution" : std::string(solutionName);
    const std::string resolvedProj =
        projectName.empty() ? QFileInfo(qPath).dir().dirName().toStdString() : std::string(projectName);
    const std::string resolvedProjPath = std::string(projectPath);

    std::string currentCsClass = QFileInfo(qPath).completeBaseName().toStdString();
    bool pendingCsTestAttr = false;

    for (int i = 0; i < lines.size(); ++i) {
        const QString& lineText = lines[i];
        if (isCs) {
            if (const auto clsMatch = kCsClassRe.match(lineText); clsMatch.hasMatch()) {
                currentCsClass = clsMatch.captured(1).toStdString();
            }
            if (kCsAttrRe.match(lineText).hasMatch()) {
                pendingCsTestAttr = true;
            }
            if (pendingCsTestAttr) {
                if (const auto mMatch = kCsMethodRe.match(lineText); mMatch.hasMatch()) {
                    DiscoveredTestEntry entry;
                    entry.solutionName = resolvedSln;
                    entry.projectName = resolvedProj.empty() ? "Tests" : resolvedProj;
                    entry.projectPath = resolvedProjPath;
                    entry.suiteName = currentCsClass.empty() ? "Tests" : currentCsClass;
                    entry.testName = mMatch.captured(1).toStdString();
                    entry.filePath = std::string(filePath);
                    entry.line = i + 1;
                    entry.isDotNet = true;
                    entry.isEntryPoint = false;
                    results.push_back(std::move(entry));
                    pendingCsTestAttr = false;
                    continue;
                }
            }
            if (kCsMainRe.match(lineText).hasMatch()) {
                DiscoveredTestEntry entry;
                entry.solutionName = resolvedSln;
                entry.projectName = resolvedProj.empty() ? "App" : resolvedProj;
                entry.projectPath = resolvedProjPath;
                entry.suiteName = currentCsClass;
                entry.testName = "Main";
                entry.filePath = std::string(filePath);
                entry.line = i + 1;
                entry.isDotNet = true;
                entry.isEntryPoint = true;
                results.push_back(std::move(entry));
            }
        } else {
            if (const auto gtMatch = kGTestRe.match(lineText); gtMatch.hasMatch()) {
                DiscoveredTestEntry entry;
                entry.solutionName = resolvedSln;
                entry.projectName = resolvedProj.empty() ? "Tests" : resolvedProj;
                entry.projectPath = resolvedProjPath;
                entry.suiteName = gtMatch.captured(1).toStdString();
                entry.testName = gtMatch.captured(2).toStdString();
                entry.filePath = std::string(filePath);
                entry.line = i + 1;
                entry.isDotNet = false;
                entry.isEntryPoint = false;
                results.push_back(std::move(entry));
                continue;
            }
            if (const auto catchMatch = kCatchRe.match(lineText); catchMatch.hasMatch()) {
                DiscoveredTestEntry entry;
                entry.solutionName = resolvedSln;
                entry.projectName = resolvedProj.empty() ? "Tests" : resolvedProj;
                entry.projectPath = resolvedProjPath;
                entry.suiteName = "Catch2";
                entry.testName = catchMatch.captured(1).toStdString();
                entry.filePath = std::string(filePath);
                entry.line = i + 1;
                entry.isDotNet = false;
                entry.isEntryPoint = false;
                results.push_back(std::move(entry));
                continue;
            }
            if (kCppMainRe.match(lineText).hasMatch()) {
                DiscoveredTestEntry entry;
                entry.solutionName = resolvedSln;
                entry.projectName = resolvedProj.empty() ? "App" : resolvedProj;
                entry.projectPath = resolvedProjPath;
                entry.suiteName = QFileInfo(qPath).fileName().toStdString();
                entry.testName = "main";
                entry.filePath = std::string(filePath);
                entry.line = i + 1;
                entry.isDotNet = false;
                entry.isEntryPoint = true;
                results.push_back(std::move(entry));
            }
        }
    }
    return results;
}

std::vector<DiscoveredTestEntry> discoverWorkspaceUnitTests(io::IFileSystem& fs, const project::SolutionModel* solution,
                                                            std::string_view workspaceRoot) {
    std::vector<DiscoveredTestEntry> allTests;
    std::vector<std::string> visitedFiles;

    const auto scanDirectory = [&](const std::string& dirPath, std::string_view projName, std::string_view projPath,
                                   std::string_view slnName, auto& selfRef, int depth) -> void {
        if (depth > 3 || dirPath.empty())
            return;
        auto entries = fs.listDirectory(dirPath);
        if (!entries)
            return;
        for (const auto& de : *entries) {
            const QString qEntryName = toQs(de.name);
            if (de.name.empty() || de.name.starts_with('.') ||
                qEntryName.compare(QStringLiteral("bin"), Qt::CaseInsensitive) == 0 ||
                qEntryName.compare(QStringLiteral("obj"), Qt::CaseInsensitive) == 0 ||
                qEntryName.compare(QStringLiteral("build"), Qt::CaseInsensitive) == 0 ||
                qEntryName.compare(QStringLiteral("out"), Qt::CaseInsensitive) == 0 ||
                qEntryName.compare(QStringLiteral("node_modules"), Qt::CaseInsensitive) == 0 ||
                qEntryName.compare(QStringLiteral("Library"), Qt::CaseInsensitive) == 0 ||
                qEntryName.compare(QStringLiteral("Temp"), Qt::CaseInsensitive) == 0 ||
                qEntryName.compare(QStringLiteral("Logs"), Qt::CaseInsensitive) == 0 ||
                qEntryName.compare(QStringLiteral("UserSettings"), Qt::CaseInsensitive) == 0 ||
                qEntryName.compare(QStringLiteral("PackageCache"), Qt::CaseInsensitive) == 0 ||
                qEntryName.compare(QStringLiteral("MemoryCaptures"), Qt::CaseInsensitive) == 0) {
                continue;
            }
            const auto fullRes = io::joinPath(dirPath, de.name);
            if (!fullRes)
                continue;
            if (de.type == io::FileType::Directory) {
                selfRef(*fullRes, projName, projPath, slnName, selfRef, depth + 1);
                continue;
            }
            const QString qName = toQs(de.name);
            if (!qName.endsWith(QStringLiteral(".cpp"), Qt::CaseInsensitive) &&
                !qName.endsWith(QStringLiteral(".cc"), Qt::CaseInsensitive) &&
                !qName.endsWith(QStringLiteral(".cxx"), Qt::CaseInsensitive) &&
                !qName.endsWith(QStringLiteral(".h"), Qt::CaseInsensitive) &&
                !qName.endsWith(QStringLiteral(".hpp"), Qt::CaseInsensitive) &&
                !qName.endsWith(QStringLiteral(".cs"), Qt::CaseInsensitive)) {
                continue;
            }
            if (std::ranges::contains(visitedFiles, *fullRes))
                continue;
            visitedFiles.push_back(*fullRes);
            if (auto content = fs.readFile(*fullRes)) {
                auto found = discoverTestsAndEntryPointsInFile(*fullRes, *content, projName, projPath, slnName);
                for (auto& item : found) {
                    if (!item.isEntryPoint) {
                        allTests.push_back(std::move(item));
                    }
                }
            }
        }
    };

    const std::string slnName =
        (solution && !solution->name().empty())
            ? solution->name()
            : (!workspaceRoot.empty() ? QFileInfo(toQs(workspaceRoot)).fileName().toStdString() : "Solution");

    if (solution) {
        for (const auto& proj : solution->projects()) {
            if (proj.nodeKind != project::NodeKind::Project || proj.path.empty())
                continue;
            const std::string projDir(io::parentPath(proj.path));
            scanDirectory(projDir, proj.name, proj.path, slnName, scanDirectory, 0);
        }
    }
    if (!workspaceRoot.empty()) {
        scanDirectory(std::string(workspaceRoot), slnName, {}, slnName, scanDirectory, 0);
    }
    return allTests;
}

QWidget* createUnitTestsToolWindowWidget(QWidget* parent, io::IFileSystem& fs,
                                         std::function<const project::SolutionModel*()> solutionProvider,
                                         std::function<std::string()> workspaceRootProvider,
                                         std::function<void(const std::string& path, int line)> onNavigateToSource,
                                         std::function<void(const DiscoveredTestEntry& test)> onDebugTest) {
    /*
     * Discovery only (RUN-0, absorbed by FND-3). The tree lists what a source-text scan finds and
     * marks every test "Discovered, not run". There is no test runner yet: the run and debug
     * actions are disabled with the owning epic (RUN-7, then RUN-10 for debugging a single test),
     * so the window starts no process and never shows an outcome, a duration or console text.
     */
    static_cast<void>(onDebugTest);
    auto* root = new QWidget(parent);
    root->setObjectName(QStringLiteral("unitTestsToolWindow"));
    auto* outer = new QVBoxLayout(root);
    outer->setContentsMargins(6, 6, 6, 6);
    outer->setSpacing(6);

    auto tests = std::make_shared<std::vector<DiscoveredTestEntry>>();

    auto* toolbar = new QHBoxLayout();
    const auto runnerAction = [root, toolbar](const QString& label, const char* objectName) {
        auto* button = new QPushButton(label, root);
        button->setObjectName(QLatin1String(objectName));
        button->setEnabled(false);
        button->setToolTip(testRunnerUnavailable());
        toolbar->addWidget(button);
        return button;
    };
    runnerAction(QStringLiteral("Run All"), "unitTestsRunAllButton");
    runnerAction(QStringLiteral("Run Selected"), "unitTestsRunSelectedButton");
    runnerAction(QStringLiteral("Run Failed"), "unitTestsRunFailedButton");
    runnerAction(QStringLiteral("Repeat Until Failure"), "unitTestsRepeatUntilFailureButton");
    runnerAction(QStringLiteral("Debug Selected Test"), "unitTestsDebugSelectedButton");
    runnerAction(QStringLiteral("Stop"), "unitTestsStopButton");
    auto* refreshBtn = new QPushButton(QStringLiteral("Refresh"), root);
    refreshBtn->setObjectName(QStringLiteral("unitTestsRefreshButton"));
    refreshBtn->setToolTip(QStringLiteral("Scan the project sources for test declarations again."));
    toolbar->addWidget(refreshBtn);

    auto* filterEdit = new QLineEdit(root);
    filterEdit->setObjectName(QStringLiteral("unitTestsFilterEdit"));
    filterEdit->setPlaceholderText(QStringLiteral("Filter tests by suite or name..."));
    toolbar->addWidget(filterEdit, 1);
    outer->addLayout(toolbar);

    auto* summaryLabel = new QLabel(root);
    summaryLabel->setObjectName(QStringLiteral("unitTestsSummaryLabel"));
    summaryLabel->setWordWrap(true);
    outer->addWidget(summaryLabel);

    auto* splitter = new QSplitter(Qt::Horizontal, root);
    auto* tree = new QTreeWidget(splitter);
    tree->setObjectName(QStringLiteral("unitTestsTreeWidget"));
    tree->setColumnCount(2);
    tree->setHeaderLabels({
        QStringLiteral("Test Hierarchy (Solution → Project → Suite → Test)"),
        QStringLiteral("Status"),
    });
    tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    splitter->addWidget(tree);

    auto* rightPane = new QWidget(splitter);
    auto* rightLayout = new QVBoxLayout(rightPane);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(4);
    auto* detailsView = new QPlainTextEdit(rightPane);
    detailsView->setObjectName(QStringLiteral("unitTestsOutputView"));
    detailsView->setReadOnly(true);
    detailsView->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    detailsView->setPlaceholderText(QStringLiteral("Select a test to see where it is declared."));
    auto* locationLabel = new QLabel(QStringLiteral("Declaration (click to jump):"), rightPane);
    auto* locationList = new QListWidget(rightPane);
    locationList->setObjectName(QStringLiteral("unitTestsStackTraceLinks"));
    locationList->setMaximumHeight(110);
    rightLayout->addWidget(detailsView, 1);
    rightLayout->addWidget(locationLabel);
    rightLayout->addWidget(locationList);
    splitter->addWidget(rightPane);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 1);
    outer->addWidget(splitter, 1);

    const QString discoveredStatus = QStringLiteral("Discovered, not run");

    const auto showTestDetails = [=](int idx) {
        locationList->clear();
        if (idx < 0 || static_cast<std::size_t>(idx) >= tests->size()) {
            detailsView->clear();
            return;
        }
        const auto& t = (*tests)[static_cast<std::size_t>(idx)];
        detailsView->setPlainText(QStringLiteral("Test: %1.%2\nDeclared in: %3:%4\nStatus: %5. Running tests %6")
                                      .arg(toQs(t.suiteName), toQs(t.testName), toQs(t.filePath))
                                      .arg(t.line)
                                      .arg(discoveredStatus, QStringLiteral("requires the test runner (RUN-7).")));
        if (!t.filePath.empty())
            locationList->addItem(QStringLiteral("%1:%2").arg(toQs(t.filePath)).arg(t.line));
    };

    const auto rebuildTree = [=, &fs] {
        const project::SolutionModel* sln = solutionProvider ? solutionProvider() : nullptr;
        const std::string wsRoot = workspaceRootProvider ? workspaceRootProvider() : std::string{};
        *tests = discoverWorkspaceUnitTests(fs, sln, wsRoot);

        const QString filter = filterEdit->text().trimmed();
        tree->clear();
        std::map<std::string, QTreeWidgetItem*> slnNodes;
        std::map<std::string, QTreeWidgetItem*> projNodes;
        std::map<std::string, QTreeWidgetItem*> suiteNodes;
        const auto groupNode = [&](std::map<std::string, QTreeWidgetItem*>& nodes, const std::string& key,
                                   const std::string& label, QTreeWidgetItem* parentItem) {
            if (auto it = nodes.find(key); it != nodes.end())
                return it->second;
            auto* item = parentItem ? new QTreeWidgetItem(parentItem, {toQs(label), QString()})
                                    : new QTreeWidgetItem(tree, {toQs(label), QString()});
            item->setData(0, Qt::UserRole, -1);
            nodes[key] = item;
            return item;
        };

        std::size_t shown = 0;
        for (std::size_t i = 0; i < tests->size(); ++i) {
            const auto& t = (*tests)[i];
            if (!filter.isEmpty() && !toQs(t.suiteName).contains(filter, Qt::CaseInsensitive) &&
                !toQs(t.testName).contains(filter, Qt::CaseInsensitive) &&
                !toQs(t.projectName).contains(filter, Qt::CaseInsensitive)) {
                continue;
            }
            QTreeWidgetItem* slnItem = groupNode(slnNodes, t.solutionName, t.solutionName, nullptr);
            const std::string projKey = t.solutionName + "::" + t.projectName;
            QTreeWidgetItem* projItem = groupNode(projNodes, projKey, t.projectName, slnItem);
            QTreeWidgetItem* suiteItem = groupNode(suiteNodes, projKey + "::" + t.suiteName, t.suiteName, projItem);
            auto* leaf = new QTreeWidgetItem(suiteItem, {toQs(t.testName), discoveredStatus});
            leaf->setData(0, Qt::UserRole, static_cast<int>(i));
            ++shown;
        }
        tree->expandAll();
        // Counts what the scan found; there are no run results to count.
        summaryLabel->setText(QStringLiteral("%1 test(s) discovered in source, none run. Running and debugging tests "
                                             "requires the test runner (RUN-7).")
                                  .arg(shown));
        showTestDetails(tests->empty() ? -1 : 0);
    };

    QObject::connect(refreshBtn, &QPushButton::clicked, root, rebuildTree);
    QObject::connect(filterEdit, &QLineEdit::textChanged, root, [=](const QString&) { rebuildTree(); });

    QObject::connect(tree, &QTreeWidget::currentItemChanged, root, [=](QTreeWidgetItem* cur, QTreeWidgetItem*) {
        if (!cur)
            return;
        const int idx = cur->data(0, Qt::UserRole).toInt();
        if (idx >= 0)
            showTestDetails(idx);
    });

    QObject::connect(tree, &QTreeWidget::itemDoubleClicked, root, [=](QTreeWidgetItem* item, int) {
        if (!item || !onNavigateToSource)
            return;
        const int idx = item->data(0, Qt::UserRole).toInt();
        if (idx >= 0 && static_cast<std::size_t>(idx) < tests->size()) {
            const auto& t = (*tests)[static_cast<std::size_t>(idx)];
            onNavigateToSource(t.filePath, t.line);
        }
    });

    QObject::connect(locationList, &QListWidget::itemClicked, root, [=](QListWidgetItem* item) {
        if (!item || !onNavigateToSource)
            return;
        const QString text = item->text();
        const qsizetype colon = text.lastIndexOf(QLatin1Char(':'));
        if (colon > 1) {
            const QString pathPart = text.left(colon).trimmed();
            const int lineNum = text.mid(colon + 1).toInt();
            if (!pathPart.isEmpty() && lineNum > 0) {
                onNavigateToSource(pathPart.toStdString(), lineNum);
            }
        }
    });

    rebuildTree();
    return root;
}

} // namespace frost::app
