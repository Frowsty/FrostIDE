// SPDX-License-Identifier: Apache-2.0
//
// The test model's names and the duration format (test_model.hpp). Builds no result: the parsers
// in ctest_parsers.cpp and gtest_parsers.cpp do.

#include "frost/run/test_model.hpp"

#include "frost/build/cmake_toolchain.hpp"
#include "frost/io/path.hpp"
#include "frost/run/doctest_parsers.hpp"
#include "frost/run/gtest_parsers.hpp"
#include "frost/run/test_runner.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <format>
#include <set>
#include <tuple>

namespace frost::run {

std::string_view toString(TestOutcome outcome) noexcept {
    switch (outcome) {
    case TestOutcome::NotRun: return "Not run";
    case TestOutcome::Passed: return "Passed";
    case TestOutcome::Failed: return "Failed";
    case TestOutcome::Skipped: return "Skipped";
    case TestOutcome::Disabled: return "Disabled";
    }
    return "Not run";
}

bool countsAsFailure(TestOutcome outcome) noexcept {
    return outcome == TestOutcome::Failed || outcome == TestOutcome::NotRun;
}

std::string formatTestDuration(std::optional<std::chrono::microseconds> duration) {
    if (!duration)
        return {};
    const auto us = duration->count();
    if (us < 1000)
        return "< 1 ms";
    if (us < 1'000'000)
        return std::format("{} ms", us / 1000);
    if (us < 60'000'000)
        return std::format("{:.2f} s", static_cast<double>(us) / 1e6);
    const auto seconds = us / 1'000'000;
    return std::format("{} min {:02} s", seconds / 60, seconds % 60);
}

Result<CTestSetup> ctestSetupFor(const project::CMakeProject& model, const project::CMakeProfile& profile,
                                 const project::CMakeTestPreset* preset) {
    if (!model.cmakeVersion.empty() && !build::versionAtLeast(model.cmakeVersion, 3, 21))
        return fail(ErrorCode::Unsupported,
                    std::format("ctest {} cannot write a JUnit report; the test runner needs CMake 3.21 or later",
                                model.cmakeVersion));
    CTestSetup setup;
    if (auto ctest = model.cacheValue("CMAKE_CTEST_COMMAND"); ctest && !ctest->empty()) {
        setup.ctest = *ctest;
    } else if (auto cmake = model.cacheValue("CMAKE_COMMAND"); cmake && !cmake->empty()) {
        const std::string_view cmakePath = *cmake;
        const auto slash = cmakePath.find_last_of("/\\");
        const std::string_view name = slash == std::string_view::npos ? cmakePath : cmakePath.substr(slash + 1);
        const bool exe = name.size() > 4 && (name.ends_with(".exe") || name.ends_with(".EXE"));
        setup.ctest = std::string(cmakePath.substr(0, slash == std::string_view::npos ? 0 : slash + 1)) + "ctest" +
                      (exe ? ".exe" : "");
    } else {
        return fail(ErrorCode::NotFound, "the CMake cache names no ctest (CMAKE_CTEST_COMMAND)");
    }
    auto ctest = io::normalizePath(setup.ctest);
    if (!ctest || !io::isAbsolutePath(*ctest))
        return fail(ErrorCode::InvalidArgument, std::format("ctest '{}' is not an absolute path", setup.ctest));
    setup.ctest = std::move(*ctest);
    setup.ctestVersion = model.cmakeVersion;
    setup.buildDirectory = model.buildDirectory;
    if (!io::isAbsolutePath(setup.buildDirectory))
        return fail(ErrorCode::InvalidArgument, "the CMake model has no absolute build directory");
    if (model.multiConfig)
        setup.configuration = model.configuration.empty() ? profile.effectiveConfiguration() : model.configuration;
    setup.environment = profile.buildEnvironment ? *profile.buildEnvironment : profile.environment;
    if (preset) {
        setup.testPreset = preset->name;
        if (!preset->configuration.empty())
            setup.configuration = preset->configuration;
        setup.includeName = preset->includeName;
        setup.excludeName = preset->excludeName;
        setup.includeLabel = preset->includeLabel;
        setup.excludeLabel = preset->excludeLabel;
        setup.jobs = preset->jobs.value_or(0);
        setup.timeout = preset->timeout;
        setup.stopOnFailure = preset->stopOnFailure;
        setup.environment = preset->environment;
    }
    return setup;
}

const project::CMakeTestPreset* testPresetFor(const project::CMakePresets& presets,
                                              const project::CMakeProfile& profile) {
    if (profile.configurePreset.empty())
        return nullptr;
    const std::vector<const project::CMakeTestPreset*> candidates = presets.testPresetsFor(profile.configurePreset);
    const project::CMakeTestPreset* first = nullptr;
    for (const project::CMakeTestPreset* preset : candidates) {
        if (!preset || preset->hidden || !preset->enabled)
            continue;
        if (preset->name == profile.buildPreset || preset->name == profile.configurePreset)
            return preset;
        if (!first)
            first = preset;
    }
    return first;
}

namespace {

char lowerAscii(char c) noexcept { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

std::string_view trimView(std::string_view text) noexcept {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos)
        return {};
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

bool isDoctestReporterArg(std::string_view arg) noexcept {
    std::size_t dashes = 0;
    while (dashes < arg.size() && arg[dashes] == '-')
        ++dashes;
    if (dashes == 0)
        return false;
    std::string_view rest = arg.substr(dashes);
    if (rest.starts_with("dt-")) {
        rest.remove_prefix(3);
    }
    const auto eq = rest.find('=');
    const std::string_view key = rest.substr(0, eq);
    return key == "reporters" || key == "out" || key == "list-test-cases";
}

std::string normalizedPathKey(std::string_view path, std::string_view buildDirectory) {
    std::string resolved(path);
    if (!resolved.empty() && !io::isAbsolutePath(resolved) && !buildDirectory.empty())
        resolved = io::joinPath(buildDirectory, resolved).value_or(resolved);
    std::string norm = io::normalizePath(resolved).value_or(resolved);
    for (char& c : norm)
        if (c == '\\')
            c = '/';
#ifdef _WIN32
    for (char& c : norm)
        c = lowerAscii(c);
#endif
    return norm;
}

bool sameNormalizedPath(std::string_view a, std::string_view b, std::string_view buildDirectory) {
    if (a.empty() || b.empty())
        return false;
    return normalizedPathKey(a, buildDirectory) == normalizedPathKey(b, buildDirectory);
}

struct SourceGTest {
    std::string suite;
    std::string name;
    int line = 0;
};

struct SourceDoctest {
    std::string name;
    int line = 0;
};

bool isIdentChar(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

bool exactGTestFilterForSource(std::string_view filter, std::string_view suite, std::string_view name) noexcept {
    if (filter.find_first_of("*?") != std::string_view::npos)
        return false;
    const std::size_t separator = filter.find('.');
    if (separator == std::string_view::npos)
        return false;
    const std::string_view filteredSuite = filter.substr(0, separator);
    const std::string_view filteredName = filter.substr(separator + 1);
    const bool suiteMatch =
        filteredSuite == suite ||
        (filteredSuite.size() > suite.size() && filteredSuite.starts_with(suite) && filteredSuite[suite.size()] == '/');
    const bool nameMatch = filteredName == name || (filteredName.size() > name.size() &&
                                                    filteredName.starts_with(name) && filteredName[name.size()] == '/');
    return suiteMatch && nameMatch;
}

void scanSourceTests(std::string_view content, std::vector<SourceGTest>& gtests, std::vector<SourceDoctest>& doctests) {
    int lineNum = 0;
    while (!content.empty()) {
        const auto nl = content.find('\n');
        std::string_view line = trimView(content.substr(0, nl));
        content = nl == std::string_view::npos ? std::string_view{} : content.substr(nl + 1);
        ++lineNum;
        for (const std::string_view macro : {"TEST_F", "TEST_P", "TYPED_TEST", "TEST"}) {
            if (!line.starts_with(macro))
                continue;
            std::string_view rest = trimView(line.substr(macro.size()));
            if (!rest.starts_with('('))
                continue;
            rest = trimView(rest.substr(1));
            std::size_t sEnd = 0;
            while (sEnd < rest.size() && isIdentChar(rest[sEnd]))
                ++sEnd;
            if (sEnd == 0)
                continue;
            const std::string_view suite = rest.substr(0, sEnd);
            rest = trimView(rest.substr(sEnd));
            if (!rest.starts_with(','))
                continue;
            rest = trimView(rest.substr(1));
            std::size_t nEnd = 0;
            while (nEnd < rest.size() && isIdentChar(rest[nEnd]))
                ++nEnd;
            if (nEnd == 0)
                continue;
            const std::string_view name = rest.substr(0, nEnd);
            rest = trimView(rest.substr(nEnd));
            if (rest.starts_with(')'))
                gtests.push_back(SourceGTest{std::string(suite), std::string(name), lineNum});
            break;
        }
        for (const std::string_view macro : {"TEST_CASE", "SCENARIO"}) {
            if (!line.starts_with(macro))
                continue;
            std::string_view rest = trimView(line.substr(macro.size()));
            if (!rest.starts_with('('))
                continue;
            rest = trimView(rest.substr(1));
            if (!rest.starts_with('"'))
                continue;
            rest.remove_prefix(1);
            std::string parsed;
            bool closed = false;
            for (std::size_t i = 0; i < rest.size(); ++i) {
                if (rest[i] == '\\' && i + 1 < rest.size()) {
                    parsed += rest[++i];
                } else if (rest[i] == '"') {
                    closed = true;
                    break;
                } else {
                    parsed += rest[i];
                }
            }
            if (closed && !parsed.empty())
                doctests.push_back(SourceDoctest{std::move(parsed), lineNum});
            break;
        }
    }
}

} // namespace

Result<SingleTestLaunch> buildSingleTestLaunch(const CTestSetup& setup, const DiscoveredTest& test,
                                               std::string_view caseName, TestCaseFramework framework) {
    if (framework == TestCaseFramework::Busted || framework == TestCaseFramework::LuaUnit)
        return fail(ErrorCode::Unsupported, "Lua framework launches require the selected Lua configuration, not a CTest command");
    if (!io::isAbsolutePath(setup.buildDirectory))
        return fail(ErrorCode::InvalidArgument,
                    std::format("the build directory '{}' is not an absolute path", setup.buildDirectory));
    if (test.command.empty() || !io::isAbsolutePath(test.command.front()))
        return fail(ErrorCode::InvalidArgument, std::format("the test '{}' has no program to launch", test.name));

    SingleTestLaunch launch;
    launch.program = test.command.front();
    launch.workingDirectory = !test.workingDirectory.empty() && io::isAbsolutePath(test.workingDirectory)
                                  ? test.workingDirectory
                                  : setup.buildDirectory;
    launch.environment = setup.environment;
    for (const std::string& entry : test.environment) {
        const auto equals = entry.find('=');
        if (equals == std::string::npos || equals == 0)
            continue;
        launch.environment.emplace_back(entry.substr(0, equals), entry.substr(equals + 1));
    }
    launch.displayName =
        caseName.empty() || caseName == test.name ? test.name : std::format("{} ({})", caseName, test.name);

    const std::span<const std::string> allArgs(test.command);
    const auto baseArgs = allArgs.size() > 1 ? allArgs.subspan(1) : std::span<const std::string>{};
    if (framework == TestCaseFramework::Doctest && !caseName.empty()) {
        auto selected = doctestCaseSelectionCommand(test.command, {std::string(caseName)});
        if (!selected)
            return std::unexpected(std::move(selected.error()));
        for (std::size_t i = 1; i < selected->size(); ++i) {
            if (!isDoctestReporterArg((*selected)[i]))
                launch.arguments.push_back((*selected)[i]);
        }
    } else if ((framework == TestCaseFramework::Catch2 || framework == TestCaseFramework::BoostTest) &&
               !caseName.empty()) {
        const std::array<std::string, 1> cases{std::string(caseName)};
        auto arguments = frameworkArguments(framework == TestCaseFramework::Catch2 ? FrameworkProfile::Catch2Xml3
                                                                                   : FrameworkProfile::BoostTest185,
                                            test.command, cases, {}, false);
        if (!arguments)
            return std::unexpected(arguments.error());
        launch.arguments = std::move(*arguments);
        if (framework == TestCaseFramework::BoostTest)
            launch.environment.emplace_back("BOOST_TEST_RUN_FILTERS", std::nullopt);
    } else if (framework == TestCaseFramework::GoogleTest && !caseName.empty()) {
        const std::array<std::string, 1> cases{std::string(caseName)};
        launch.arguments = gtestRunArguments(baseArgs, cases, {});
    } else {
        launch.arguments.assign(baseArgs.begin(), baseArgs.end());
    }
    return launch;
}

std::vector<GutterTestCase>
resolveGutterTestCasesInFile(std::string_view filePath, std::string_view content, std::span<const DiscoveredTest> tests,
                             std::span<const std::pair<std::string, std::vector<DoctestCase>>> doctestCases,
                             std::span<const std::pair<std::string, std::vector<GTestCase>>> gtestCases,
                             std::string_view buildDirectory) {
    std::vector<GutterTestCase> out;
    if (tests.empty() || filePath.empty())
        return out;

    std::vector<SourceGTest> sourceGTests;
    std::vector<SourceDoctest> sourceDoctests;
    scanSourceTests(content, sourceGTests, sourceDoctests);
    std::set<int> usedLines;

    // 1. Doctest cases must have been listed by the real doctest executable. Source text only
    // supplies a location; it never creates a runnable result on its own.
    for (const auto& [testName, listed] : doctestCases) {
        const auto testIt = std::ranges::find(tests, testName, &DiscoveredTest::name);
        if (testIt == tests.end() || testIt->command.empty())
            continue;
        for (const DoctestCase& dc : listed) {
            int matchedLine = 0;
            for (const SourceDoctest& sd : sourceDoctests) {
                if ((sd.name == dc.name || "  Scenario: " + sd.name == dc.name) && dc.location &&
                    sameNormalizedPath(dc.location->file, filePath, buildDirectory)) {
                    matchedLine = sd.line;
                    break;
                }
            }
            if (matchedLine <= 0 && dc.location && dc.location->line > 0 &&
                sameNormalizedPath(dc.location->file, filePath, buildDirectory)) {
                matchedLine = dc.location->line;
            }
            if (matchedLine <= 0 || !usedLines.insert(matchedLine).second)
                continue;
            GutterTestCase item;
            item.test = *testIt;
            item.caseName = dc.name;
            item.framework = TestCaseFramework::Doctest;
            item.line = matchedLine;
            item.displayName = dc.name;
            out.push_back(std::move(item));
        }
    }
    // 2. Google Test cases from source declarations matched against cases listed by the real
    // executable. A ctest-discovered exact filter can also provide the identity when its program
    // has not been explicitly listed in the window.

    for (const SourceGTest& sg : sourceGTests) {
        if (usedLines.contains(sg.line))
            continue;
        std::vector<std::pair<const DiscoveredTest*, std::string>> matched;
        for (const auto& [testName, listed] : gtestCases) {
            const auto testIt = std::ranges::find(tests, testName, &DiscoveredTest::name);
            if (testIt == tests.end() || testIt->command.empty())
                continue;
            for (const GTestCase& gc : listed) {
                const bool exact = gc.suite == sg.suite && gc.name == sg.name;
                const bool valueParam = (gc.suite == sg.suite || gc.suite.ends_with("/" + sg.suite)) &&
                                        (gc.name == sg.name || gc.name.starts_with(sg.name + "/"));
                const bool typedParam = gc.suite.starts_with(sg.suite + "/") && gc.name == sg.name;
                if (!(exact || valueParam || typedParam))
                    continue;
                if (std::ranges::none_of(matched, [&](const auto& item) {
                        return item.first->name == testIt->name && item.second == gc.fullName();
                    }))
                    matched.emplace_back(&*testIt, gc.fullName());
            }
        }
        if (matched.empty()) {
            for (const DiscoveredTest& test : tests) {
                if (test.command.empty() || test.gtestFilter.empty())
                    continue;
                // A negative filter is not a single test identity. The caller may select exact
                // tokens from a positive multi-case filter, but never a wildcard/exclusion.
                const std::size_t negative = test.gtestFilter.find('-');
                if (negative != std::string::npos)
                    continue;
                std::string_view remaining(test.gtestFilter);
                while (!remaining.empty()) {
                    const std::size_t separator = remaining.find(':');
                    const std::string_view filter = remaining.substr(0, separator);
                    if (exactGTestFilterForSource(filter, sg.suite, sg.name))
                        matched.emplace_back(&test, std::string(filter));
                    if (separator == std::string_view::npos)
                        break;
                    remaining.remove_prefix(separator + 1);
                }
            }
        }
        if (matched.empty())
            continue;
        for (const auto& [test, caseName] : matched) {
            GutterTestCase item;
            item.test = *test;
            item.caseName = caseName;
            item.framework = TestCaseFramework::GoogleTest;
            item.line = sg.line;
            item.displayName = caseName;
            out.push_back(std::move(item));
        }
        usedLines.insert(sg.line);
    }

    // 3. Any ctest test with DEF_SOURCE_LINE in this file not already covered by source scanning.
    // A positive gtest filter may contain several exact tokens. Give each token its own gutter
    // action so clicking one declaration cannot accidentally run its neighbours.
    for (const DiscoveredTest& t : tests) {
        if (t.command.empty() || !t.definition || t.definition->line <= 0 ||
            !sameNormalizedPath(t.definition->file, filePath, buildDirectory) || usedLines.contains(t.definition->line))
            continue;
        if (t.gtestFilter.empty()) {
            if (!usedLines.insert(t.definition->line).second)
                continue;
            GutterTestCase item;
            item.test = t;
            item.framework = TestCaseFramework::CTestWhole;
            item.line = t.definition->line;
            item.displayName = t.name;
            out.push_back(std::move(item));
            continue;
        }
        if (t.gtestFilter.find('-') != std::string::npos)
            continue;
        std::string_view remaining(t.gtestFilter);
        while (!remaining.empty()) {
            const std::size_t separator = remaining.find(':');
            const std::string_view filter = remaining.substr(0, separator);
            const bool exact =
                filter.find_first_of("*?") == std::string_view::npos && filter.find('.') != std::string_view::npos;
            if (exact) {
                GutterTestCase item;
                item.test = t;
                item.caseName = std::string(filter);
                item.framework = TestCaseFramework::GoogleTest;
                item.line = t.definition->line;
                item.displayName = item.caseName;
                out.push_back(std::move(item));
            }
            if (separator == std::string_view::npos)
                break;
            remaining.remove_prefix(separator + 1);
        }
        usedLines.insert(t.definition->line);
    }

    std::ranges::sort(out, [](const GutterTestCase& a, const GutterTestCase& b) {
        return std::tie(a.line, a.displayName, a.test.name) < std::tie(b.line, b.displayName, b.test.name);
    });
    return out;
}

} // namespace frost::run
