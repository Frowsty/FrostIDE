// SPDX-License-Identifier: Apache-2.0
//
// doctest's command lines and the parsers of its XML case list, XML run report and console failure
// blocks (doctest_parsers.hpp). XML through pugixml (ADR 0033; private to this file). The option
// spelling follows doctest's own command-line reader (doctest 2.5.3, MIT, parseOption and
// parseCommaSepArgs): one or more dashes, an optional "dt-" prefix, the last occurrence wins.

#include "frost/run/doctest_parsers.hpp"

#include <pugixml.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <format>
#include <utility>

namespace frost::run {

namespace {

constexpr std::string_view kWhitespace = " \t\r\n";

std::string_view trim(std::string_view text) {
    const auto first = text.find_first_not_of(kWhitespace);
    if (first == std::string_view::npos)
        return {};
    const auto last = text.find_last_not_of(kWhitespace);
    return text.substr(first, last - first + 1);
}

std::optional<int> parseLine(std::string_view text) {
    int value = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || end != text.data() + text.size() || text.empty() || value < 0)
        return std::nullopt;
    return value;
}

std::optional<std::size_t> parseCount(std::string_view text) {
    std::size_t value = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || end != text.data() + text.size() || text.empty())
        return std::nullopt;
    return value;
}

// doctest's seconds ("0.001234") as microseconds.
std::optional<std::chrono::microseconds> parseSeconds(std::string_view text) {
    text = trim(text);
    double seconds = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), seconds);
    if (ec != std::errc{} || end != text.data() + text.size() || text.empty() || !std::isfinite(seconds) || seconds < 0)
        return std::nullopt;
    return std::chrono::microseconds(static_cast<std::int64_t>(std::llround(seconds * 1e6)));
}

// A filename/line attribute pair; nullopt without a file or for line 0 (--no-line-numbers).
std::optional<TestLocation> locationOf(const pugi::xml_node node) {
    TestLocation location;
    location.file = node.attribute("filename").as_string();
    const auto line = parseLine(node.attribute("line").as_string());
    if (location.file.empty() || !line || *line == 0)
        return std::nullopt;
    location.line = *line;
    return location;
}

// The option name of a doctest argument ("--dt-test-suite=a" -> "test-suite", prefixed), or "".
struct OptionName {
    std::string_view name{};
    bool prefixed = false;
    bool hasValue = false;
};

OptionName optionName(std::string_view argument) {
    OptionName out;
    std::size_t dashes = 0;
    while (dashes < argument.size() && argument[dashes] == '-')
        ++dashes;
    if (dashes == 0 || dashes > 2)
        return out;
    std::string_view rest = argument.substr(dashes);
    if (rest.starts_with("dt-")) {
        out.prefixed = true;
        rest.remove_prefix(3);
    }
    const auto equals = rest.find('=');
    out.hasValue = equals != std::string_view::npos;
    out.name = rest.substr(0, equals);
    return out;
}

// The options that mark a doctest command (isDoctestCommand): selection options take a value.
constexpr std::string_view kValueOptions[] = {
    "test-suite",  "test-suite-exclude",
    "test-case",   "test-case-exclude",
    "source-file", "source-file-exclude",
    "ts",          "tse",
    "tc",          "tce",
    "sf",          "sfe",
    "subcase",     "subcase-exclude",
    "reporters",
};

bool marksDoctest(const OptionName& option) {
    if (option.name.empty())
        return false;
    if (option.name == "no-intro")
        return true;
    return option.hasValue && std::ranges::contains(kValueOptions, option.name);
}

// Whether the command spells its doctest options with the "dt-" prefix (then ours do too: a
// program built with DOCTEST_CONFIG_NO_UNPREFIXED_OPTIONS reads only those).
bool usesPrefix(const std::vector<std::string>& command) {
    for (std::size_t i = 1; i < command.size(); ++i) {
        const OptionName option = optionName(command[i]);
        if (option.prefixed && marksDoctest(option))
            return true;
    }
    return false;
}

std::string option(bool prefixed, std::string_view name, std::string_view value) {
    return std::format("--{}{}={}", prefixed ? "dt-" : "", name, value);
}

std::vector<std::string> ownArguments(const std::vector<std::string>& command) {
    if (command.size() <= 1)
        return {};
    return {command.begin() + 1, command.end()};
}

// The <doctest> root of a report, or null.
pugi::xml_node doctestRoot(const pugi::xml_document& doc) { return doc.child("doctest"); }

// "CHECK( a == b )", doctest's own spelling of an assertion.
std::string assertionText(std::string_view type, std::string_view expression) {
    return std::format("{}( {} )", type, expression);
}

// The failures of one case, depth first in report order (SubCase elements nest them).
void collectFailures(const pugi::xml_node node, std::vector<TestFailure>& failures,
                     const std::vector<std::string>& subcases) {
    const auto withSubcases = [&subcases](std::string message) {
        for (const std::string& subcase : subcases)
            message += std::format("\n  in subcase: {}", subcase);
        return message;
    };
    const auto withInfo = [](const pugi::xml_node element, std::string message) {
        for (const pugi::xml_node info : element.children("Info"))
            message += std::format("\n  logged: {}", trim(info.text().as_string()));
        return message;
    };
    for (const pugi::xml_node child : node.children()) {
        const std::string_view tag = child.name();
        if (tag == "SubCase") {
            std::vector<std::string> nested = subcases;
            nested.emplace_back(child.attribute("name").as_string());
            collectFailures(child, failures, nested);
        } else if (tag == "Expression") {
            if (child.attribute("success").as_bool(true))
                continue;
            const std::string_view type = child.attribute("type").as_string();
            std::string message = assertionText(type, trim(child.child("Original").text().as_string()));
            if (const pugi::xml_node expanded = child.child("Expanded"))
                message += std::format(" is NOT correct!\n  values: {}",
                                       assertionText(type, trim(expanded.text().as_string())));
            if (const pugi::xml_node exception = child.child("Exception"))
                message += std::format(" threw: {}", trim(exception.text().as_string()));
            if (const pugi::xml_node expected = child.child("ExpectedException"))
                message += std::format("\n  expected exception: {}", trim(expected.text().as_string()));
            TestFailure failure;
            failure.message = withSubcases(withInfo(child, std::move(message)));
            failure.location = locationOf(child);
            failures.push_back(std::move(failure));
        } else if (tag == "Message") {
            const std::string_view type = child.attribute("type").as_string();
            if (type.find("ERROR") == std::string_view::npos)
                continue; // A WARNING (MESSAGE, WARN) is no failure.
            TestFailure failure;
            failure.message = withSubcases(
                withInfo(child, std::format("{}: {}", type, trim(child.child("Text").text().as_string()))));
            failure.location = locationOf(child);
            failures.push_back(std::move(failure));
        } else if (tag == "Exception") {
            TestFailure failure;
            failure.message = withSubcases(
                std::format("test case {}: {}", child.attribute("crash").as_bool() ? "CRASHED" : "THREW exception",
                            trim(child.text().as_string())));
            failures.push_back(std::move(failure)); // No location: the case's definition stands for it.
        }
    }
}

// "\x1b[0;31m" and the like (doctest's colours with --force-colors), removed.
std::string withoutColours(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\x1b' && i + 1 < text.size() && text[i + 1] == '[') {
            std::size_t end = i + 2;
            while (end < text.size() && (std::isdigit(static_cast<unsigned char>(text[end])) || text[end] == ';'))
                ++end;
            if (end < text.size() && text[end] == 'm') {
                i = end;
                continue;
            }
        }
        out += text[i];
    }
    return out;
}

// The location token doctest prints before a block or an assertion: "E:\x\a.cpp(15):" (MSVC
// style) or "a.cpp:15:" (gnu style). nullopt for anything else, and for line 0.
std::optional<TestLocation> consoleLocation(std::string_view token) {
    TestLocation location;
    if (token.ends_with("):")) {
        token.remove_suffix(2);
        const auto open = token.rfind('(');
        if (open == std::string_view::npos || open == 0)
            return std::nullopt;
        const auto line = parseLine(token.substr(open + 1));
        if (!line)
            return std::nullopt;
        location.file = std::string(token.substr(0, open));
        location.line = *line;
    } else if (token.ends_with(':')) {
        token.remove_suffix(1);
        const auto colon = token.rfind(':');
        if (colon == std::string_view::npos || colon == 0)
            return std::nullopt;
        const auto line = parseLine(token.substr(colon + 1));
        if (!line)
            return std::nullopt;
        location.file = std::string(token.substr(0, colon));
        location.line = *line;
    } else {
        return std::nullopt;
    }
    if (location.line == 0)
        return std::nullopt;
    return location;
}

bool isSeparator(std::string_view line) {
    line = trim(line);
    return line.size() >= 20 && line.find_first_not_of('=') == std::string_view::npos;
}

} // namespace

bool isDoctestCommand(const std::vector<std::string>& command) {
    for (std::size_t i = 1; i < command.size(); ++i)
        if (marksDoctest(optionName(command[i])))
            return true;
    return false;
}

std::string doctestFilterValue(const std::vector<std::string>& names) {
    std::string out;
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (i > 0)
            out += ',';
        for (const char c : names[i]) {
            if (c == ',' || c == '\\')
                out += '\\';
            out += c;
        }
    }
    return out;
}

std::vector<std::string> doctestListArguments(const std::vector<std::string>& command, std::string_view reportPath) {
    const bool prefixed = usesPrefix(command);
    std::vector<std::string> arguments = ownArguments(command);
    arguments.push_back(std::format("--{}list-test-cases", prefixed ? "dt-" : ""));
    arguments.push_back(option(prefixed, "reporters", "xml"));
    arguments.push_back(option(prefixed, "out", reportPath));
    return arguments;
}

std::vector<std::string> doctestRunArguments(const std::vector<std::string>& command,
                                             const std::vector<std::string>& cases, std::string_view reportPath) {
    const bool prefixed = usesPrefix(command);
    std::vector<std::string> arguments = ownArguments(command);
    if (!cases.empty())
        arguments.push_back(option(prefixed, "test-case", doctestFilterValue(cases)));
    arguments.push_back(option(prefixed, "reporters", "xml"));
    arguments.push_back(option(prefixed, "duration", "true"));
    arguments.push_back(option(prefixed, "no-skipped-summary", "true"));
    arguments.push_back(option(prefixed, "out", reportPath));
    return arguments;
}

Result<std::vector<DoctestCase>> parseDoctestCaseList(std::string_view report) {
    pugi::xml_document doc;
    const pugi::xml_parse_result parsed =
        doc.load_buffer(report.data(), report.size(), pugi::parse_default, pugi::encoding_utf8);
    if (!parsed)
        return fail(ErrorCode::Corrupt,
                    std::format("doctest's case list is not XML: {} at byte {}", parsed.description(), parsed.offset));
    const pugi::xml_node root = doctestRoot(doc);
    if (!root)
        return fail(ErrorCode::Corrupt, "doctest's case list has no <doctest> element (not a doctest program?)");
    const pugi::xml_node summary = root.child("OverallResultsTestCases");
    const auto unskipped = summary ? parseCount(summary.attribute("unskipped").as_string()) : std::nullopt;
    if (!unskipped)
        return fail(ErrorCode::Corrupt, "doctest's case list has no <OverallResultsTestCases unskipped=...>");
    std::vector<DoctestCase> out;
    for (const pugi::xml_node node : root.children("TestCase")) {
        DoctestCase testCase;
        testCase.name = node.attribute("name").as_string();
        testCase.suite = node.attribute("testsuite").as_string();
        testCase.location = locationOf(node);
        out.push_back(std::move(testCase));
    }
    if (out.size() != *unskipped)
        return fail(ErrorCode::Corrupt,
                    std::format("doctest's case list holds {} cases but says {}", out.size(), *unskipped));
    return out;
}

Result<DoctestReport> parseDoctestXmlReport(std::string_view report) {
    pugi::xml_document doc;
    // A report cut short does not parse to the end; pugixml keeps what it read before the error.
    const pugi::xml_parse_result parsed =
        doc.load_buffer(report.data(), report.size(), pugi::parse_default, pugi::encoding_utf8);
    const pugi::xml_node root = doctestRoot(doc);
    if (!root)
        return fail(
            ErrorCode::Corrupt,
            parsed ? std::string("doctest's report has no <doctest> element (not a doctest program?)")
                   : std::format("doctest's report is not XML: {} at byte {}", parsed.description(), parsed.offset));
    DoctestReport out;
    const auto readCase = [&out](const pugi::xml_node node, std::string_view suite) {
        if (node.attribute("skipped").as_bool())
            return; // Filtered out or doctest::skip(): it did not run, so it has no result.
        const pugi::xml_node end = node.child("OverallResultsAsserts");
        if (!end) {
            DoctestCase unfinished;
            unfinished.name = node.attribute("name").as_string();
            unfinished.suite = std::string(suite);
            unfinished.location = locationOf(node);
            out.unfinished = std::move(unfinished);
            return;
        }
        TestResult result;
        result.name = node.attribute("name").as_string();
        result.definition = locationOf(node);
        result.duration = parseSeconds(end.attribute("duration").as_string());
        collectFailures(node, result.failures, {});
        result.outcome = end.attribute("test_case_success").as_bool() ? TestOutcome::Passed : TestOutcome::Failed;
        if (result.outcome == TestOutcome::Failed)
            result.reason = std::format("{} failed assertion(s)", end.attribute("failures").as_string("0"));
        out.cases.push_back(std::move(result));
    };
    for (const pugi::xml_node child : root.children()) {
        const std::string_view tag = child.name();
        if (tag == "TestSuite") {
            const std::string_view suite = child.attribute("name").as_string();
            for (const pugi::xml_node node : child.children("TestCase"))
                readCase(node, suite);
        } else if (tag == "TestCase") {
            readCase(child, {});
        } else if (tag == "OverallResultsTestCases") {
            const auto passed = parseCount(child.attribute("successes").as_string());
            const auto failed = parseCount(child.attribute("failures").as_string());
            if (passed && failed) {
                out.complete = true;
                out.passedCases = *passed;
                out.failedCases = *failed;
            }
        }
    }
    return out;
}

std::optional<TestResult> doctestRunResult(const std::string& testName, const Result<DoctestReport>& report,
                                           const DoctestProcessEnd& end) {
    if (!report)
        return std::nullopt;
    TestResult result;
    result.name = testName;
    result.cases = report->cases;
    result.output = end.output;
    if (end.stopped) {
        if (result.cases.empty())
            return std::nullopt;
        // Only the cases that ended are results; the test as a whole did not finish.
        result.outcome =
            std::ranges::any_of(result.cases, [](const TestResult& c) { return c.outcome == TestOutcome::Failed; })
                ? TestOutcome::Failed
                : TestOutcome::NotRun;
        result.reason = "Stopped";
        return result;
    }
    result.duration = end.duration;
    if (report->complete) {
        if (report->failedCases > 0) {
            result.outcome = TestOutcome::Failed;
            result.reason =
                std::format("{} of {} case(s) failed", report->failedCases, report->failedCases + report->passedCases);
        } else if (end.exitCode != 0) {
            result.outcome = TestOutcome::Failed;
            result.reason = std::format("Exit code {}", end.exitCode);
        } else {
            result.outcome = TestOutcome::Passed;
        }
        return result;
    }
    result.outcome = TestOutcome::Failed;
    result.reason = std::format("Exit code {} before doctest finished its report", end.exitCode);
    if (report->unfinished) {
        TestResult unfinished;
        unfinished.name = report->unfinished->name;
        unfinished.definition = report->unfinished->location;
        unfinished.outcome = TestOutcome::Failed;
        unfinished.reason = "Ended before doctest reported its end";
        TestFailure failure;
        failure.message = std::format("The program ended (exit code {}) while this case ran.", end.exitCode);
        failure.location = report->unfinished->location;
        unfinished.failures.push_back(std::move(failure));
        result.cases.push_back(std::move(unfinished));
    }
    return result;
}

std::vector<TestResult> parseDoctestConsoleFailures(std::string_view output) {
    const std::string text = withoutColours(output);
    std::vector<std::string_view> lines;
    for (std::size_t start = 0; start <= text.size();) {
        const auto newline = text.find('\n', start);
        std::string_view line =
            std::string_view(text).substr(start, newline == std::string::npos ? std::string::npos : newline - start);
        if (line.ends_with('\r'))
            line.remove_suffix(1);
        lines.push_back(line);
        if (newline == std::string::npos)
            break;
        start = newline + 1;
    }

    std::vector<TestResult> out;
    std::vector<std::string> dropped; // Cases doctest marked as not failed.
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (!isSeparator(lines[i]) || i + 1 >= lines.size())
            continue;
        // The block's first line: the case's location, alone on its line.
        const auto definition = consoleLocation(trim(lines[i + 1]));
        if (!definition)
            continue;
        std::size_t at = i + 2;
        std::string name;
        for (; at < lines.size() && !trim(lines[at]).empty(); ++at) {
            const std::string_view line = lines[at];
            if (line.starts_with("TEST CASE:")) {
                name = std::string(trim(line.substr(10)));
            } else if (line.starts_with("  Scenario:") && name.empty()) {
                name = std::string(line); // doctest prints a SCENARIO's name as is, without "TEST CASE:".
            }
        }
        if (name.empty())
            continue;
        std::vector<TestFailure> failures;
        bool notFailed = false;
        // The block's body runs to the next separator.
        for (; at < lines.size() && !isSeparator(lines[at]); ++at) {
            const std::string_view line = lines[at];
            if (line.find("marking it as not failed") != std::string_view::npos) {
                notFailed = true;
                continue;
            }
            std::size_t severity = line.find(": FATAL ERROR: ");
            if (severity == std::string_view::npos)
                severity = line.find(": ERROR: ");
            if (severity == std::string_view::npos)
                continue;
            // The location token ends with its own ':' (the one before " ERROR").
            const auto location = consoleLocation(line.substr(0, severity + 1));
            if (!location)
                continue;
            TestFailure failure;
            failure.message = std::string(line.substr(severity + 2));
            failure.location = location;
            // Continuation lines ("  values: ...", "  logged: ...") up to the blank line.
            for (std::size_t more = at + 1; more < lines.size() && lines[more].starts_with("  "); ++more) {
                failure.message += '\n';
                failure.message += lines[more];
            }
            failures.push_back(std::move(failure));
        }
        i = at - 1;
        if (notFailed) {
            dropped.push_back(name);
            continue;
        }
        if (failures.empty())
            continue;
        // A case whose subcases print separate blocks is one case.
        const auto existing = std::ranges::find(out, name, &TestResult::name);
        if (existing != out.end()) {
            existing->failures.insert(existing->failures.end(), failures.begin(), failures.end());
            continue;
        }
        TestResult result;
        result.name = std::move(name);
        result.outcome = TestOutcome::Failed;
        result.reason = "Failed";
        result.definition = definition;
        result.failures = std::move(failures);
        out.push_back(std::move(result));
    }
    std::erase_if(out, [&dropped](const TestResult& r) { return std::ranges::contains(dropped, r.name); });
    return out;
}

} // namespace frost::run
