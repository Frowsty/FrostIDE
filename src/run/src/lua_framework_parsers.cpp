// SPDX-License-Identifier: Apache-2.0
// Busted v2.2.0 CLI/filter_loader/junit; LuaUnit LUAUNIT_V3_5 CLI/JUnitOutput.
#include "frost/run/lua_framework_parsers.hpp"

#include "frost/io/path.hpp"

#include <pugixml.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <set>
namespace frost::run {
namespace {
constexpr std::size_t limit = 100000;
std::optional<int> integer(std::string_view s) {
    int n{};
    auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), n);
    if (s.empty() || ec != std::errc{} || end != s.data() + s.size() || n < 0)
        return {};
    return n;
}
Result<void> load(pugi::xml_document& doc, std::string_view s) {
    if (s.size() > 64 * 1024 * 1024 || !doc.load_buffer(s.data(), s.size()))
        return fail(ErrorCode::Corrupt, "Missing, oversized or truncated Lua framework XML");
    return {};
}
std::string quoted(std::string_view s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        if (c == '\\' || c == '\"') {
            out += '\\';
            out += static_cast<char>(c);
        } else if (c < 32 || c == 127) {
            out += '\\';
            out += static_cast<char>('0' + c / 100);
            out += static_cast<char>('0' + (c / 10) % 10);
            out += static_cast<char>('0' + c % 10);
        } else
            out += static_cast<char>(c);
    }
    return out + '\"';
}
std::optional<TestLocation> source(std::string_view text) {
    const auto colon = text.rfind(':');
    if (colon == text.npos)
        return {};
    auto line = integer(text.substr(colon + 1));
    if (!line || !*line || !colon || text.starts_with("..."))
        return {};
    return TestLocation{std::string(text.substr(0, colon)), *line};
}
} // namespace
std::string luaExactPattern(std::string_view name) {
    std::string result = "^";
    for (char c : name) {
        if (std::string_view("^$()%.[]*+-?").find(c) != std::string_view::npos)
            result += '%';
        result += c;
    }
    return result + '$';
}
Result<RunConfiguration> luaFrameworkConfiguration(LuaTestProfile profile, RunConfiguration config,
                                                   std::span<const std::string> cases, std::string_view report,
                                                   bool listing, std::string_view bridge) {
    if (config.type != RunType::LuaScript)
        return fail(ErrorCode::Unsupported, "Lua tests require an explicit Lua Script configuration");
    if (auto valid = validateRunConfiguration(config); !valid)
        return std::unexpected(valid.error());
    if ((!report.empty() || listing) && !io::isAbsolutePath(report))
        return fail(ErrorCode::InvalidArgument, "Lua report path must be absolute");
    std::set<std::string> unique;
    for (const auto& name : cases)
        if (name.empty() || name.find_first_of("\r\n") != name.npos || !unique.insert(name).second)
            return fail(ErrorCode::InvalidArgument, "Empty, multiline or duplicate Lua case selection");
    // Fail explicitly instead of changing an existing user's report/list/re-execution semantics.
    // Busted .busted settings remain active, but explicit CLI selection/reporter values win.
    for (const auto& arg : config.arguments) {
        const auto key = arg.substr(0, arg.find('='));
        if (profile == LuaTestProfile::Busted220) {
            if (key == "-o" || key == "--output" || key == "-Xoutput" || key == "--list" || key == "-l" ||
                key == "--lua" || key == "--repeat" || key == "--version" || key == "--filter" || key == "--name" ||
                key == "--")
                return fail(ErrorCode::Unsupported,
                            "Remove Busted output/list/runtime/name controls from the Lua test configuration");
        } else if (key == "-o" || key == "--output" || key == "-n" || key == "--name" ||
                   (!cases.empty() && (key == "-p" || key == "--pattern" || key == "-x" || key == "--exclude")))
            return fail(ErrorCode::Unsupported,
                        "Remove conflicting LuaUnit report/selection controls from the configuration");
    }
    if (profile == LuaTestProfile::Busted220) {
        if (report.find(',') != report.npos)
            return fail(ErrorCode::Unsupported, "Busted Xoutput cannot represent a report path containing a comma");
        config.arguments.insert(config.arguments.end(), {"--ignore-lua", "--repeat=1"});
        for (const auto& name : cases)
            config.arguments.push_back("--filter=" + luaExactPattern(name));
        if (listing)
            config.arguments.push_back("--list");
        else if (!report.empty())
            config.arguments.insert(config.arguments.end(), {"--output=junit", "-Xoutput", std::string(report)});
    } else {
        for (const auto& name : cases)
            config.arguments.insert(config.arguments.end(), {"--pattern", luaExactPattern(name)});
        if (!listing) {
            auto at = config.interpreterArguments.end();
            if (!config.interpreterArguments.empty() && config.interpreterArguments.back() == "--")
                --at;
            config.interpreterArguments.insert(
                at, {"-e", "assert(require('luaunit').VERSION == '3.5', 'LuaUnit 3.5 required')"});
        }
        if (listing) {
            if (!io::isAbsolutePath(bridge))
                return fail(ErrorCode::InvalidArgument, "LuaUnit discovery bridge must be absolute");
            // Insert BEFORE a trailing --: Lua executes the bootstrap before the original script,
            // preserving arg[0], positional script arguments and the module-search environment.
            auto at = config.interpreterArguments.end();
            if (!config.interpreterArguments.empty() && config.interpreterArguments.back() == "--")
                --at;
            config.interpreterArguments.insert(at, {"-e", "dofile(" + quoted(bridge) + ")(" + quoted(report) + ")"});
        } else if (!report.empty())
            config.arguments.insert(config.arguments.end(), {"--output", "junit", "--name", std::string(report)});
    }
    return config;
}
Result<std::vector<FrameworkCase>> parseLuaFrameworkList(LuaTestProfile profile, std::string_view text) {
    std::vector<FrameworkCase> result;
    std::set<std::string> names;
    if (text.size() > 32 * 1024 * 1024)
        return fail(ErrorCode::Corrupt, "Oversized Lua listing");
    if (profile == LuaTestProfile::LuaUnit35) {
        pugi::xml_document doc;
        if (auto r = load(doc, text); !r)
            return std::unexpected(r.error());
        auto root = doc.child("LuaUnitDiscovery");
        if (!root || std::string_view(root.attribute("version").as_string()) != "3.5" ||
            std::string_view(root.attribute("hook").as_string()) != "internalRunSuiteByInstances")
            return fail(ErrorCode::Unsupported, "Unsupported or unobserved LuaUnit discovery hook");
        for (auto node : root.children()) {
            if (std::string_view(node.name()) != "TestCase")
                return fail(ErrorCode::Corrupt, "Unexpected LuaUnit discovery record");
            FrameworkCase item{node.attribute("name").as_string()};
            auto line = integer(node.attribute("line").as_string());
            if (line && *line && *node.attribute("file").as_string())
                item.location = TestLocation{node.attribute("file").as_string(), *line};
            result.push_back(std::move(item));
        }
    } else {
        // This is the actual --list grammar, not terminal pass/fail prose. A colon in a name
        // cannot consume its source prefix: use the first ': ' after a numeric source line.
        while (!text.empty()) {
            auto end = text.find('\n');
            auto line = text.substr(0, end);
            text = end == text.npos ? std::string_view{} : text.substr(end + 1);
            if (line.ends_with('\r'))
                line.remove_suffix(1);
            if (line.empty())
                continue;
            auto split = line.find(": ");
            if (split == line.npos)
                return fail(ErrorCode::Corrupt, "Ambiguous Busted --list record (source/line required)");
            auto loc = source(line.substr(0, split));
            // Truncated Lua short_src remains a valid name record but is not a navigable path.
            auto pos = line.substr(0, split).rfind(':');
            if (pos == line.npos || !integer(line.substr(pos + 1, split - pos - 1)))
                return fail(ErrorCode::Corrupt, "Invalid Busted source record");
            result.push_back(FrameworkCase{std::string(line.substr(split + 2)), loc});
        }
    }
    for (const auto& item : result)
        if (item.name.empty() || item.name.find_first_of("\r\n") != item.name.npos || !names.insert(item.name).second ||
            names.size() > limit)
            return fail(ErrorCode::Corrupt, "Invalid or duplicate Lua case name");
    return result;
}
Result<TestResult> parseLuaFrameworkRun(LuaTestProfile profile, std::string_view name, std::string_view xml,
                                        std::span<const std::string> selected, int exitCode, std::string_view output) {
    pugi::xml_document doc;
    if (auto r = load(doc, xml); !r)
        return std::unexpected(r.error());
    auto root = doc.child("testsuites");
    auto suite = root.child("testsuite");
    if (!root || !suite || suite.next_sibling("testsuite"))
        return fail(ErrorCode::Corrupt, "Expected one complete Lua framework JUnit suite");
    if (profile == LuaTestProfile::LuaUnit35) {
        bool version = false;
        for (auto p : suite.child("properties").children("property"))
            if (std::string_view(p.attribute("name").as_string()) == "LuaUnit Version")
                version = std::string_view(p.attribute("value").as_string()) == "3.5";
        if (!version)
            return fail(ErrorCode::Unsupported, "Expected LuaUnit 3.5 JUnit");
    } else if (std::string_view(suite.attribute("name").as_string()) != "Run 1 of 1")
        return fail(ErrorCode::Unsupported, "Expected Busted 2.2 single-run JUnit");
    TestResult result;
    result.name = name;
    result.output = output;
    result.outcome = TestOutcome::Passed;
    std::set<std::string> names;
    int failures = 0, errors = 0, skips = 0;
    for (auto node : suite.children("testcase")) {
        TestResult item;
        item.name = node.attribute("name").as_string();
        if (profile == LuaTestProfile::LuaUnit35) {
            // LuaUnit writes node.testName = prettyFuncName (already class-qualified).
        } else
            item.definition = source(node.attribute("classname").as_string());
        if (item.name.empty() || !names.insert(item.name).second || names.size() > limit)
            return fail(ErrorCode::Corrupt, "Invalid duplicate Lua JUnit case");
        double duration{};
        std::string_view value = node.attribute("time").as_string();
        auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), duration);
        if (value.empty() || ec != std::errc{} || end != value.data() + value.size() || !std::isfinite(duration) ||
            duration < 0 || duration > 1e9)
            return fail(ErrorCode::Corrupt, "Invalid Lua JUnit case duration");
        item.duration = std::chrono::microseconds(static_cast<std::int64_t>(duration * 1e6));
        item.outcome = TestOutcome::Passed;
        int verdicts = 0;
        for (auto child : node.children()) {
            std::string_view tag = child.name();
            if (tag == "failure" || tag == "error" || tag == "skipped") {
                ++verdicts;
                item.reason = child.attribute("type").as_string();
                if (!item.reason.empty())
                    item.reason += '\n';
                item.reason += child.text().as_string();
                if (tag == "skipped") {
                    ++skips;
                    item.outcome = TestOutcome::Skipped;
                } else {
                    tag == "failure" ? ++failures : ++errors;
                    item.outcome = TestOutcome::Failed;
                    item.failures.push_back(TestFailure{item.reason, item.definition});
                }
            }
        }
        if (verdicts > 1)
            return fail(ErrorCode::Corrupt, "Contradictory Lua case verdicts");
        if (item.outcome == TestOutcome::Failed)
            result.outcome = TestOutcome::Failed;
        result.cases.push_back(std::move(item));
    }
    auto count = integer(suite.attribute("tests").as_string());
    auto f = integer(suite.attribute("failures").as_string());
    auto e = integer(suite.attribute("errors").as_string());
    auto s = integer(suite.attribute(profile == LuaTestProfile::Busted220 ? "skip" : "skipped").as_string());
    if (!count || !f || !e || !s || *count != static_cast<int>(names.size()) || *f != failures || *e != errors ||
        *s != skips || suite.child("error") || suite.child("failure") || names.empty())
        return fail(ErrorCode::Corrupt, "Incomplete Lua framework case/counter accounting");
    if (!selected.empty() && names != std::set<std::string>(selected.begin(), selected.end()))
        return fail(ErrorCode::Corrupt, "Lua framework report differs from exact case selection");
    if ((exitCode == 0) != (failures + errors == 0))
        return fail(ErrorCode::Corrupt, "Lua process exit disagrees with JUnit verdicts");
    if (skips == static_cast<int>(names.size()))
        result.outcome = TestOutcome::Skipped;
    return result;
}
} // namespace frost::run
