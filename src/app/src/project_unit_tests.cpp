// SPDX-License-Identifier: Apache-2.0
//
// discoverTestsAndEntryPointsInFile (project_properties_dialog.hpp): the source-text recognisers
// of tests and entry points. The editor gutter uses only their entry points (main, Main) for its
// run markers. Tests are listed and run by the Unit Tests window (unit_tests_window.cpp, RUN-7)
// from ctest's output, never from this scan; a gutter test marker is RUN-10's.

#include "frost/build/cmake_service.hpp"
#include "frost/project/cmake_presets.hpp"
#include "frost/project/trust_store.hpp"
#include "frost/run/run_manager.hpp"
#include "frost/run/test_runner.hpp"

#include "project_properties_dialog.hpp"
#include "unit_tests_window.hpp"
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStringList>

#include <algorithm>
#include <format>

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

std::string unitTestsSourceKey(const UnitTestsWindowContext& context) {
    build::CMakeService* service = context.cmake ? context.cmake() : nullptr;
    if (!service)
        return context.solutionOpen && context.solutionOpen() ? "solution" : "none";
    const auto model = service->model();
    const auto profile = service->activeProfile();
    const auto trust = context.trust ? context.trust->snapshot(service->sourceDirectory()) : project::TrustSnapshot{};
    return std::format("cmake|{}|{}|{}|{}|{}|{}|{}", service->sourceDirectory(),
                       model ? model->replyIndex : std::string(), model ? model->buildDirectory : std::string(),
                       profile ? profile->name : std::string(), trust.trusted, service->profileGeneration(),
                       trust.revision);
}

UnitTestsSource unitTestsSource(const UnitTestsWindowContext& context) {
    UnitTestsSource out;
    build::CMakeService* service = context.cmake ? context.cmake() : nullptr;
    if (!service) {
        out.unavailable =
            context.solutionOpen && context.solutionOpen()
                ? QStringLiteral("Tests are listed by ctest, from a configured CMake build directory. This "
                                 "Visual Studio solution has none, so no test is listed.")
                : QStringLiteral("Open a CMake folder: the Unit Tests window lists the tests ctest finds in "
                                 "its configured build directory.");
        return out;
    }
    const auto model = service->model();
    const auto profile = service->activeProfile();
    if (!model || !profile) {
        out.unavailable = QStringLiteral("The CMake folder is not configured yet. Its tests are listed once a "
                                         "configure has written its build directory.");
        return out;
    }
    const auto trust = context.trust ? context.trust->snapshot(service->sourceDirectory()) : project::TrustSnapshot{};
    if (!trust.trusted) {
        out.unavailable = QStringLiteral("Trust this folder to list and run its tests: ctest runs the "
                                         "project's test scripts and test programs.");
        return out;
    }
    const project::CMakeTestPreset* preset = nullptr;
    auto presets = project::loadCMakePresets(*context.fs, service->sourceDirectory());
    if (presets && presets->has_value())
        preset = run::testPresetFor(**presets, *profile);
    auto setup = run::ctestSetupFor(*model, *profile, preset);
    if (!setup) {
        out.unavailable = QStringLiteral("ctest is unavailable: %1").arg(toQs(setup.error().message()));
        return out;
    }
    out.setup = std::move(*setup);
    out.trustRoot = service->sourceDirectory();
    out.trustRevision = trust.revision;
    return out;
}

std::optional<run::TestLocation> unitTestsLocationOf(const std::string& testName, const std::string& caseName,
                                                     const run::DiscoveredTest* test, const run::TestResult* result,
                                                     const std::vector<run::DoctestCase>* doctestCases) {
    (void)testName;
    if (!caseName.empty()) {
        if (result) {
            for (const run::TestResult& testCase : result->cases) {
                if (testCase.name != caseName)
                    continue;
                for (const run::TestFailure& failure : testCase.failures)
                    if (failure.location)
                        return failure.location;
                if (testCase.definition)
                    return testCase.definition;
            }
        }
        if (doctestCases) {
            const auto found = std::ranges::find(*doctestCases, caseName, &run::DoctestCase::name);
            if (found != doctestCases->end() && found->location)
                return found->location;
        }
        return std::nullopt;
    }
    if (result)
        for (const run::TestFailure& failure : result->failures)
            if (failure.location)
                return failure.location;
    if (test && test->definition)
        return test->definition;
    if (test && test->declaration)
        return test->declaration;
    return std::nullopt;
}

UnitTestsSelectionDetails unitTestsDescribeSelection(const std::string& testName, const std::string& caseName,
                                                     const run::DiscoveredTest* test, const run::TestResult* result,
                                                     const std::vector<run::DoctestCase>* doctestCases) {
    UnitTestsSelectionDetails out;
    QStringList lines;
    const auto describe = [&](const run::TestResult& r) {
        lines << QStringLiteral("Status: %1").arg(toQs(run::toString(r.outcome)));
        if (!r.reason.empty())
            lines << QStringLiteral("Reason: %1").arg(toQs(r.reason));
        if (r.duration)
            lines << QStringLiteral("Duration: %1").arg(toQs(run::formatTestDuration(r.duration)));
        if (r.interim)
            lines << QStringLiteral("(from ctest's progress; its report has not been read yet)");
        for (const run::TestFailure& failure : r.failures) {
            lines << QString() << QStringLiteral("Failure:") << toQs(failure.message);
            if (failure.location)
                out.locations.emplace_back(QStringLiteral("Failure"), *failure.location);
        }
    };
    if (!caseName.empty()) {
        lines << QStringLiteral("%1 case: %2")
                     .arg(doctestCases ? QStringLiteral("doctest") : QStringLiteral("Google Test"), toQs(caseName))
              << QStringLiteral("Runs in: %1").arg(toQs(testName));
        if (doctestCases) {
            const auto listed = std::ranges::find(*doctestCases, caseName, &run::DoctestCase::name);
            if (listed != doctestCases->end() && !listed->suite.empty())
                lines << QStringLiteral("Test suite: %1").arg(toQs(listed->suite));
            if (listed != doctestCases->end() && listed->location)
                out.locations.emplace_back(QStringLiteral("Defined"), *listed->location);
        }
        const run::TestResult* caseResult = nullptr;
        if (result)
            for (const run::TestResult& c : result->cases)
                if (c.name == caseName)
                    caseResult = &c;
        if (caseResult) {
            describe(*caseResult);
            if (caseResult->definition)
                out.locations.emplace_back(QStringLiteral("Defined"), *caseResult->definition);
        } else {
            lines << QStringLiteral("Status: no result yet");
        }
        out.text = lines.join(QLatin1Char('\n'));
        return out;
    }
    lines << QStringLiteral("Test: %1").arg(toQs(testName));
    if (test) {
        if (!test->command.empty()) {
            run::LaunchPlan plan;
            plan.program = test->command.front();
            plan.arguments.assign(test->command.begin() + 1, test->command.end());
            lines << QStringLiteral("Command: %1").arg(toQs(run::displayCommandLine(plan)));
        } else {
            lines << QStringLiteral("Command: none (ctest found no program for this test)");
        }
        if (!test->workingDirectory.empty())
            lines << QStringLiteral("Working directory: %1").arg(toQs(test->workingDirectory));
        if (!test->labels.empty()) {
            QStringList labels;
            for (const auto& label : test->labels)
                labels << toQs(label);
            lines << QStringLiteral("Labels: %1").arg(labels.join(QStringLiteral(", ")));
        }
        if (test->disabled)
            lines << QStringLiteral("DISABLED");
        if (test->definition)
            out.locations.emplace_back(QStringLiteral("Defined"), *test->definition);
        if (test->declaration)
            out.locations.emplace_back(QStringLiteral("add_test"), *test->declaration);
    }
    if (result) {
        describe(*result);
        if (!result->output.empty())
            lines << QString() << QStringLiteral("Output:") << toQs(result->output);
    } else {
        lines << QStringLiteral("Status: no result yet");
    }
    out.text = lines.join(QLatin1Char('\n'));
    return out;
}

} // namespace frost::app
