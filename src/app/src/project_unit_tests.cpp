// SPDX-License-Identifier: Apache-2.0
//
// discoverTestsAndEntryPointsInFile (project_properties_dialog.hpp): the source-text recognisers
// of tests and entry points. The editor gutter uses only their entry points (main, Main) for its
// run markers. Tests are listed and run by the Unit Tests window (unit_tests_window.cpp, RUN-7)
// from ctest's output, never from this scan; a gutter test marker is RUN-10's.

#include "project_properties_dialog.hpp"
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStringList>

namespace frost::app {

namespace {

QString toQs(std::string_view sv) { return QString::fromUtf8(sv.data(), static_cast<qsizetype>(sv.size())); }

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

} // namespace frost::app
