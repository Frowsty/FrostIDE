// SPDX-License-Identifier: Apache-2.0
//
// Parsers of ctest's test list, JUnit report and progress lines (ctest_parsers.hpp). JSON through
// frost::rpc's yyjson reader, XML through pugixml (ADR 0033; private to this file).

#include "frost/run/ctest_parsers.hpp"

#include "frost/rpc/json.hpp"

#include <pugixml.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
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

std::optional<std::size_t> parseSize(std::string_view text) {
    std::size_t value = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || end != text.data() + text.size() || text.empty())
        return std::nullopt;
    return value;
}

// "file:line" (DEF_SOURCE_LINE): split at the last ':' so a drive letter stays in the file.
std::optional<TestLocation> parseFileLine(std::string_view text) {
    const auto colon = text.rfind(':');
    if (colon == std::string_view::npos || colon == 0)
        return std::nullopt;
    const auto line = parseSize(text.substr(colon + 1));
    if (!line || *line == 0)
        return std::nullopt;
    TestLocation location;
    location.file = std::string(text.substr(0, colon));
    location.line = static_cast<int>(*line);
    return location;
}

// Seconds as ctest prints them ("0.0147974", "1") as microseconds.
std::optional<std::chrono::microseconds> parseSeconds(std::string_view text) {
    text = trim(text);
    double seconds = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), seconds);
    if (ec != std::errc{} || end != text.data() + text.size() || text.empty() || !std::isfinite(seconds) || seconds < 0)
        return std::nullopt;
    return std::chrono::microseconds(static_cast<std::int64_t>(std::llround(seconds * 1e6)));
}

// The innermost frame of backtrace node `index`: the node itself when it has a line.
std::optional<TestLocation> backtraceLocation(const rpc::Json& graph, std::int64_t index) {
    const rpc::Json* nodes = graph.find("nodes");
    const rpc::Json* files = graph.find("files");
    if (!nodes || !files || !nodes->isArray() || !files->isArray() || index < 0 ||
        static_cast<std::size_t>(index) >= nodes->size())
        return std::nullopt;
    const rpc::Json& node = nodes->items()[static_cast<std::size_t>(index)];
    const auto file = node.integerAt("file");
    const auto line = node.integerAt("line");
    if (!file || !line || *file < 0 || static_cast<std::size_t>(*file) >= files->size() || *line <= 0)
        return std::nullopt;
    const std::string* path = files->items()[static_cast<std::size_t>(*file)].asString();
    if (!path)
        return std::nullopt;
    TestLocation location;
    location.file = *path;
    location.line = static_cast<int>(*line);
    return location;
}

} // namespace

Result<std::vector<DiscoveredTest>> parseCTestShowOnly(std::string_view output) {
    // The object starts at the first line that opens it.
    std::size_t start = 0;
    if (!output.starts_with('{')) {
        start = output.find("\n{");
        if (start == std::string_view::npos)
            return fail(ErrorCode::Corrupt, "ctest printed no test list (no JSON object in its output)");
        ++start;
    }
    auto parsed = rpc::parseJson(output.substr(start));
    if (!parsed)
        return fail(ErrorCode::Corrupt, std::format("ctest's test list is not JSON: {}", parsed.error().describe()));
    const rpc::Json& root = *parsed;
    if (root.stringAt("kind") != "ctestInfo")
        return fail(ErrorCode::Corrupt, "ctest's test list is not a ctestInfo object");
    const rpc::Json* version = root.find("version");
    if (!version || version->integerAt("major") != 1)
        return fail(ErrorCode::Corrupt, "ctest's test list has a json-v1 version other than 1");
    const rpc::Json* tests = root.find("tests");
    if (!tests || !tests->isArray())
        return fail(ErrorCode::Corrupt, "ctest's test list has no tests array");
    const rpc::Json* graph = root.find("backtraceGraph");

    std::vector<DiscoveredTest> out;
    out.reserve(tests->size());
    for (const rpc::Json& item : tests->items()) {
        const std::string* name = item.find("name") ? item.find("name")->asString() : nullptr;
        if (!name || name->empty())
            return fail(ErrorCode::Corrupt, std::format("ctest's test #{} has no name", out.size() + 1));
        DiscoveredTest test;
        test.name = *name;
        test.number = out.size() + 1;
        if (const rpc::Json* command = item.find("command"); command && command->isArray()) {
            for (const rpc::Json& argument : command->items()) {
                if (const std::string* text = argument.asString())
                    test.command.push_back(*text);
            }
        }
        for (std::size_t i = 1; i < test.command.size(); ++i) {
            constexpr std::string_view kFilter = "--gtest_filter=";
            if (test.command[i].starts_with(kFilter))
                test.gtestFilter = test.command[i].substr(kFilter.size());
        }
        if (const rpc::Json* properties = item.find("properties"); properties && properties->isArray()) {
            for (const rpc::Json& property : properties->items()) {
                const std::string_view propertyName = property.stringAt("name");
                const rpc::Json* value = property.find("value");
                if (!value)
                    continue;
                if (propertyName == "WORKING_DIRECTORY") {
                    if (const std::string* text = value->asString())
                        test.workingDirectory = *text;
                } else if (propertyName == "LABELS") {
                    if (value->isArray()) {
                        for (const rpc::Json& label : value->items())
                            if (const std::string* text = label.asString())
                                test.labels.push_back(*text);
                    } else if (const std::string* text = value->asString()) {
                        test.labels.push_back(*text);
                    }
                } else if (propertyName == "ENVIRONMENT") {
                    if (value->isArray()) {
                        for (const rpc::Json& entry : value->items())
                            if (const std::string* text = entry.asString())
                                test.environment.push_back(*text);
                    } else if (const std::string* text = value->asString()) {
                        test.environment.push_back(*text);
                    }
                } else if (propertyName == "DISABLED") {
                    test.disabled = value->asBool().value_or(false);
                } else if (propertyName == "DEF_SOURCE_LINE") {
                    if (const std::string* text = value->asString())
                        test.definition = parseFileLine(*text);
                }
            }
        }
        if (graph) {
            if (const auto backtrace = item.integerAt("backtrace"))
                test.declaration = backtraceLocation(*graph, *backtrace);
        }
        out.push_back(std::move(test));
    }
    return out;
}

Result<std::vector<TestResult>> parseCTestJUnit(std::string_view report) {
    pugi::xml_document doc;
    const pugi::xml_parse_result parsed =
        doc.load_buffer(report.data(), report.size(), pugi::parse_default, pugi::encoding_utf8);
    if (!parsed)
        return fail(ErrorCode::Corrupt,
                    std::format("ctest's JUnit report is not XML: {} at byte {}", parsed.description(), parsed.offset));
    const pugi::xml_node suite = doc.child("testsuite");
    if (!suite)
        return fail(ErrorCode::Corrupt, "ctest's JUnit report has no <testsuite>");

    std::vector<TestResult> out;
    for (const pugi::xml_node testcase : suite.children("testcase")) {
        const std::string_view status = testcase.attribute("status").as_string();
        TestResult result;
        result.name = testcase.attribute("name").as_string();
        if (result.name.empty())
            continue;
        result.duration = parseSeconds(testcase.attribute("time").as_string());
        result.output = testcase.child("system-out").text().as_string();
        if (status == "run") {
            result.outcome = TestOutcome::Passed;
        } else if (status == "fail") {
            result.outcome = TestOutcome::Failed;
            result.reason = testcase.child("failure").attribute("message").as_string();
        } else if (status == "disabled") {
            result.outcome = TestOutcome::Disabled;
            result.reason = "Disabled";
            // ctest writes "Disabled" as the output of a disabled test; it printed nothing.
            if (trim(result.output) == "Disabled")
                result.output.clear();
        } else if (status == "notrun") {
            result.reason = testcase.child("skipped").attribute("message").as_string();
            if (result.reason.starts_with("SKIP_"))
                result.outcome = TestOutcome::Skipped;
            // Otherwise NotRun (the default): ctest could not start the test.
        } else {
            continue; // A status this parser does not know: no result rather than a guessed one.
        }
        out.push_back(std::move(result));
    }
    return out;
}

std::optional<CTestProgressLine> parseCTestProgressLine(std::string_view line) {
    if (line.ends_with('\r'))
        line.remove_suffix(1);
    const std::string_view text = line.substr(std::min(line.find_first_not_of(' '), line.size()));
    // "Start  3: name"
    if (text.starts_with("Start ")) {
        const std::string_view rest = text.substr(6);
        const auto digits = rest.find_first_not_of(' ');
        const auto colon = rest.find(": ");
        if (digits == std::string_view::npos || colon == std::string_view::npos || colon < digits)
            return std::nullopt;
        const auto number = parseSize(rest.substr(digits, colon - digits));
        if (!number)
            return std::nullopt;
        CTestProgressLine out;
        out.kind = CTestProgressLine::Kind::Started;
        out.number = *number;
        out.text = std::string(rest.substr(colon + 2));
        return out;
    }
    // "3/7 Test  #3: name ..."
    const auto slash = text.find('/');
    if (slash == std::string_view::npos || slash == 0 || !parseSize(text.substr(0, slash)))
        return std::nullopt;
    const auto test = text.find(" Test ", slash);
    if (test == std::string_view::npos || !parseSize(text.substr(slash + 1, test - slash - 1)))
        return std::nullopt;
    const auto hash = text.find('#', test);
    const auto colon = text.find(": ", test);
    if (hash == std::string_view::npos || colon == std::string_view::npos || colon < hash ||
        !trim(text.substr(test + 6, hash - test - 6)).empty())
        return std::nullopt;
    const auto number = parseSize(text.substr(hash + 1, colon - hash - 1));
    if (!number)
        return std::nullopt;
    CTestProgressLine out;
    out.kind = CTestProgressLine::Kind::Finished;
    out.number = *number;
    out.text = std::string(text.substr(colon + 2));
    return out;
}

std::optional<TestResult> ctestProgressResult(const CTestProgressLine& line, std::string_view name) {
    if (line.kind != CTestProgressLine::Kind::Finished || name.empty())
        return std::nullopt;
    /*
     * ctest pads "name " with dots to its name width (30 by default, plus 4) and cuts a longer name
     * to that width, so the line holds the whole name or, for a long one, at least its first
     * kMinimumShownName characters directly followed by the status.
     */
    constexpr std::size_t kMinimumShownName = 16;
    const std::string_view text = line.text;
    std::size_t shown = 0;
    while (shown < text.size() && shown < name.size() && text[shown] == name[shown])
        ++shown;
    if (shown < name.size() &&
        (shown < kMinimumShownName || shown == text.size() || (text[shown] != ' ' && text[shown] != '*')))
        return std::nullopt;
    std::string_view rest = text.substr(shown);
    // The dots that pad the name (none when the name is long), then the status.
    const auto status = rest.find_first_not_of(" .");
    if (status == std::string_view::npos || (status == 0 && rest[0] != '*'))
        return std::nullopt;
    rest = rest.substr(status);
    // "<status>   0.02 sec"
    if (!rest.ends_with(" sec"))
        return std::nullopt;
    rest.remove_suffix(4);
    const auto timeStart = rest.find_last_of(' ');
    if (timeStart == std::string_view::npos)
        return std::nullopt;
    const auto duration = parseSeconds(rest.substr(timeStart + 1));
    if (!duration)
        return std::nullopt;
    const std::string_view verdict = trim(rest.substr(0, timeStart));

    TestResult result;
    result.name = std::string(name);
    result.duration = duration;
    result.interim = true;
    if (verdict == "Passed") {
        result.outcome = TestOutcome::Passed;
    } else if (verdict.starts_with("***")) {
        const std::string_view reason = trim(verdict.substr(3));
        result.reason = std::string(reason);
        if (reason == "Skipped")
            result.outcome = TestOutcome::Skipped;
        else if (reason == "Not Run (Disabled)")
            result.outcome = TestOutcome::Disabled;
        else if (reason.starts_with("Not Run"))
            result.outcome = TestOutcome::NotRun;
        else
            result.outcome = TestOutcome::Failed;
    } else {
        return std::nullopt;
    }
    return result;
}

} // namespace frost::run
