// SPDX-License-Identifier: Apache-2.0
// Primary schemas: Catch2 v3.7.1 catch_reporter_xml.cpp; Boost.Test boost-1.85.0
// unit_test_main.ipp (DOT), xml_report_formatter.ipp and xml_log_formatter.ipp.
#include "frost/run/framework_parsers.hpp"
#include <pugixml.hpp>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <functional>
#include <map>
#include <set>
namespace frost::run {
namespace {
constexpr std::size_t kLimit = 100000;
std::string_view trim(std::string_view value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == value.npos) return {};
    return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}
std::optional<int> number(std::string_view value) {
    int result = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (error != std::errc{} || end != value.data() + value.size() || value.empty() || result < 0) return {};
    return result;
}
std::optional<TestLocation> location(pugi::xml_node node, const char* file = "filename") {
    const std::string path = node.attribute(file).as_string();
    const auto line = number(node.attribute("line").as_string());
    if (path.empty() || !line || *line == 0) return {};
    return TestLocation{path, *line};
}
std::optional<std::chrono::microseconds> seconds(std::string_view value) {
    double result = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (error != std::errc{} || end != value.data() + value.size() || !std::isfinite(result) || result < 0 || result > 1e9) return {};
    return std::chrono::microseconds(static_cast<std::int64_t>(result * 1e6));
}
Result<void> load(pugi::xml_document& document, std::string_view text) {
    if (text.size() > 64 * 1024 * 1024 || !document.load_buffer(text.data(), text.size(), pugi::parse_default))
        return fail(ErrorCode::Corrupt, "Missing, truncated or oversized framework XML");
    return {};
}
Result<std::vector<FrameworkCase>> catchList(std::string_view text) {
    pugi::xml_document document;
    if (auto parsed = load(document, text); !parsed) return std::unexpected(parsed.error());
    const auto root = document.child("MatchingTests");
    if (!root) return fail(ErrorCode::Unsupported, "Expected Catch2 3 XML MatchingTests listing");
    std::vector<FrameworkCase> result;
    std::set<std::string> names;
    for (auto node : root.children("TestCase")) {
        const std::string name = node.child_value("Name");
        if (name.empty() || !names.insert(name).second || result.size() >= kLimit)
            return fail(ErrorCode::Corrupt, "Invalid or duplicate Catch2 case name");
        FrameworkCase item{name};
        const auto source = node.child("SourceInfo");
        const auto line = number(source.child_value("Line"));
        if (line && *line > 0 && *source.child_value("File")) item.location = TestLocation{source.child_value("File"), *line};
        // Hidden tests remain discoverable and can be explicitly selected; no outcome is implied.
        result.push_back(std::move(item));
    }
    return result;
}
Result<std::vector<FrameworkCase>> boostList(std::string_view text) {
    if (!trim(text).starts_with("digraph G {") || text.size() > 32 * 1024 * 1024)
        return fail(ErrorCode::Unsupported, "Expected Boost.Test 1.85 DOT case listing");
    struct Node { FrameworkCase value; std::string parent; bool suite = false; bool root = false; };
    std::map<std::string, Node> nodes;
    std::string previous;
    std::size_t offset = 0;
    int braces = 1;
    bool firstLine = true;
    while (offset < text.size()) {
        const auto end = text.find('\n', offset);
        auto line = trim(text.substr(offset, end == text.npos ? text.size() - offset : end - offset));
        offset = end == text.npos ? text.size() : end + 1;
        if (firstLine) { firstLine = false; continue; }
        if (line.empty()) continue;
        if (braces == 0) return fail(ErrorCode::Corrupt, "Trailing data after Boost DOT tree");
        if (line == "{") { ++braces; if (nodes.contains(previous)) nodes[previous].suite = true; continue; }
        if (line == "}") { if (--braces < 0) return fail(ErrorCode::Corrupt, "Unbalanced Boost DOT tree"); continue; }
        if (!line.starts_with("tu")) return fail(ErrorCode::Corrupt, "Unexpected Boost DOT content");
        const auto bracket = line.find('[');
        const auto arrow = line.find(" -> ");
        if (arrow != line.npos) {
            if (bracket != line.npos) continue; // dependency edge, not a parent relation
            const std::string parent(line.substr(0, arrow));
            auto child = line.substr(arrow + 4);
            if (!child.ends_with(';')) return fail(ErrorCode::Corrupt, "Malformed Boost DOT edge");
            child.remove_suffix(1);
            auto found = nodes.find(std::string(child));
            if (found == nodes.end() || !found->second.parent.empty()) return fail(ErrorCode::Corrupt, "Ambiguous Boost DOT parent");
            found->second.parent = parent;
            continue;
        }
        const auto label = line.find("label=\"");
        if (bracket == line.npos || label == line.npos || !line.ends_with("\"];"))
            return fail(ErrorCode::Corrupt, "Malformed Boost DOT node");
        const std::string id(line.substr(0, bracket));
        auto value = line.substr(label + 7, line.size() - label - 10);
        if (value.find('\"') != value.npos) return fail(ErrorCode::Unsupported, "Unescaped Boost DOT label is ambiguous");
        const bool root = line.find("shape=ellipse") != line.npos;
        const auto separator = value.find('|');
        if (!root && separator == value.npos) return fail(ErrorCode::Corrupt, "Boost node lacks source record");
        Node node;
        node.root = root; node.value.name = std::string(value.substr(0, separator));
        node.value.disabled = line.find("color=yellow") != line.npos;
        if (!root) {
            auto source = value.substr(separator + 1); source = source.substr(0, source.find('|'));
            const auto open = source.rfind('(');
            if (open != source.npos && source.ends_with(')')) {
                const auto lineNumber = number(source.substr(open + 1, source.size() - open - 2));
                if (lineNumber && *lineNumber > 0 && open > 0) node.value.location = TestLocation{std::string(source.substr(0, open)), *lineNumber};
            }
        }
        if (node.value.name.empty() || nodes.size() >= kLimit || !nodes.emplace(id, std::move(node)).second)
            return fail(ErrorCode::Corrupt, "Invalid or duplicate Boost DOT node");
        previous = id;
    }
    if (braces != 0 || !trim(text).ends_with('}')) return fail(ErrorCode::Corrupt, "Truncated Boost DOT tree");
    if (std::ranges::count_if(nodes, [](const auto& item) { return item.second.root; }) != 1)
        return fail(ErrorCode::Corrupt, "Boost DOT must name exactly one module");
    for (const auto& [id, node] : nodes) {
        std::set<std::string> seen{id};
        auto current = &node;
        while (!current->root) {
            const auto parent = nodes.find(current->parent);
            if (parent == nodes.end() || !seen.insert(current->parent).second)
                return fail(ErrorCode::Corrupt, "Invalid Boost DOT parent tree");
            current = &parent->second;
        }
    }
    std::vector<FrameworkCase> result;
    std::set<std::string> unique;
    for (const auto& [id, node] : nodes) {
        if (node.root || node.suite) continue;
        auto value = node.value;
        std::set<std::string> visited{id};
        auto parent = node.parent;
        while (true) {
            const auto found = nodes.find(parent);
            if (found == nodes.end() || !visited.insert(parent).second) return fail(ErrorCode::Corrupt, "Invalid Boost DOT tree");
            if (found->second.root) break;
            value.name = found->second.value.name + "/" + value.name;
            value.disabled = value.disabled || found->second.value.disabled;
            parent = found->second.parent;
        }
        if (!unique.insert(value.name).second) return fail(ErrorCode::Corrupt, "Duplicate Boost case path");
        result.push_back(std::move(value));
    }
    if (nodes.empty()) return fail(ErrorCode::Corrupt, "Boost DOT listing has no module");
    return result;
}
}
std::string_view frameworkProfileName(FrameworkProfile profile) noexcept {
    return profile == FrameworkProfile::Catch2Xml3 ? "Catch2 3 XML" : "Boost.Test 1.85 XML";
}
Result<std::vector<FrameworkCase>> parseFrameworkList(FrameworkProfile profile, std::string_view text) {
    return profile == FrameworkProfile::Catch2Xml3 ? catchList(text) : boostList(text);
}
Result<std::vector<std::string>> frameworkArguments(FrameworkProfile profile, const std::vector<std::string>& command,
    std::span<const std::string> cases, std::string_view report, bool listing) {
    if (command.empty() || (listing && report.empty())) return fail(ErrorCode::InvalidArgument, "A framework program and report path are required");
    const bool catch2 = profile == FrameworkProfile::Catch2Xml3;
    std::vector<std::string> result;
    for (std::size_t i = 1; i < command.size(); ++i) {
        const std::string_view argument = command[i];
        if (argument == "--" || (argument.starts_with('-') && !argument.starts_with("--")))
            return fail(ErrorCode::Unsupported, "Use long framework options without a command separator");
        const auto key = argument.substr(0, argument.find('='));
        if (catch2) {
            const bool replaceValue = key == "--reporter" || key == "--out" || key == "--durations";
            const bool replaceFlag = key == "--list-tests" || key == "--list-tags" || key == "--list-reporters" || key == "--list-listeners";
            const bool preserveValue = key == "--rng-seed" || key == "--order" || key == "--name" ||
                key == "--section" || key == "--warn" || key == "--verbosity";
            const bool preserveFlag = key == "--success" || key == "--break" || key == "--nothrow" ||
                key == "--invisibles" || key == "--abort" || key == "--filenames-as-tags" || key == "--skip-benchmarks";
            if (replaceFlag) continue; // These options never consume a following test spec.
            if (replaceValue || preserveValue) {
                if (argument.find('=') == argument.npos) {
                    if (i + 1 == command.size() || command[i + 1].starts_with("--"))
                        return fail(ErrorCode::InvalidArgument, "Missing Catch2 option value");
                    if (preserveValue) { result.emplace_back(argument); result.push_back(command[i + 1]); }
                    ++i;
                } else if (preserveValue) result.emplace_back(argument);
                continue;
            }
            if (argument.starts_with("--") && !preserveFlag)
                return fail(ErrorCode::Unsupported, "Catch2 option is outside the supported direct-run profile");
            if (!argument.starts_with('-') && !cases.empty()) continue; // Original test spec, not an option value.
            result.emplace_back(argument);
        } else {
            if (!argument.starts_with("--") || argument.find('=') == argument.npos)
                return fail(ErrorCode::Unsupported, "Boost.Test direct profile requires --option=value spelling");
            const bool replace = key == "--report_format" || key == "--report_level" || key == "--report_sink" ||
                key == "--log_format" || key == "--log_sink" || key == "--log_level" || key == "--output_format" ||
                key == "--logger" || key == "--list_content" || key == "--list_labels" || key == "--build_info" ||
                key == "--color_output" || key == "--result_code" || (!cases.empty() && key == "--run_test");
            if (!replace) result.emplace_back(argument);
        }
    }
    for (const auto& name : cases) {
        if (name.empty() || name.find_first_of(catch2 ? "*?[]~\",\\\r\n" : "*,!@:+ \t\r\n") != name.npos)
            return fail(ErrorCode::Unsupported, "Framework cannot select this case literally with the supported profile");
        result.push_back(catch2 ? "\"" + name + "\"" : "--run_test=" + name);
    }
    if (report.empty()) return result; // Debug launch: exact selection, normal console output.
    if (catch2) {
        result.insert(result.end(), {"--reporter", "xml", "--out", std::string(report)});
        if (listing) result.push_back("--list-tests");
        else result.insert(result.end(), {"--durations", "yes"});
    } else if (listing) {
        result.insert(result.end(), {"--list_content=DOT", "--report_sink=" + std::string(report), "--color_output=no"});
    } else {
        // Detailed report supplies authoritative outcomes; log supplies real failure locations/timing/version.
        result.insert(result.end(), {"--report_format=XML", "--report_level=detailed", "--report_sink=" + std::string(report),
            "--log_format=XML", "--log_level=all", "--log_sink=" + std::string(report) + ".log", "--build_info=yes", "--result_code=yes", "--color_output=no"});
    }
    return result;
}
Result<TestResult> parseFrameworkRun(FrameworkProfile profile, std::string_view name, std::string_view report,
    std::string_view log, std::span<const std::string> selected, int exitCode, std::string_view output) {
    pugi::xml_document document;
    if (auto parsed = load(document, report); !parsed) return std::unexpected(parsed.error());
    TestResult result; result.name = name; result.output = output;
    const bool catch2 = profile == FrameworkProfile::Catch2Xml3;
    const auto root = document.child(catch2 ? "Catch2TestRun" : "TestResult");
    if (!root) return fail(ErrorCode::Unsupported, "Unexpected framework report profile");
    if (catch2 && (!std::string_view(root.attribute("catch2-version").as_string()).starts_with("3.") ||
        root.attribute("xml-format-version").as_int() != 3 || !root.child("OverallResultsCases")))
        return fail(ErrorCode::Unsupported, "Expected complete Catch2 3 XML schema 3 report");
    std::set<std::string> actual;
    std::vector<std::string> omittedSuites;
    std::function<Result<void>(pugi::xml_node, std::string, bool)> visit;
    visit = [&](pugi::xml_node parent, std::string prefix, bool master) -> Result<void> {
        for (auto node : parent.children()) {
            const std::string_view tag = node.name();
            if (tag == "TestSuite") {
                const std::string suiteName = node.attribute("name").as_string();
                const auto next = master ? prefix : prefix + suiteName + "/";
                if (!catch2 && !node.child("TestSuite") && !node.child("TestCase")) {
                    const std::string_view verdict = node.attribute("result").as_string();
                    if (verdict == "skipped" || verdict == "failed" || verdict == "aborted" || verdict == "timed-out") {
                        TestResult suite;
                        suite.name = master ? suiteName : prefix + suiteName;
                        suite.outcome = verdict == "skipped" ? TestOutcome::Skipped : TestOutcome::Failed;
                        suite.reason = "Boost.Test suite " + std::string(verdict) + "; no child outcomes reported";
                        result.cases.push_back(std::move(suite));
                        omittedSuites.push_back(master ? std::string{} : next);
                    }
                }
                if (auto parsed = visit(node, next, false); !parsed) return parsed;
            } else if (tag == "TestCase") {
                TestResult item; item.name = prefix + node.attribute("name").as_string();
                if (item.name.empty() || !actual.insert(item.name).second || result.cases.size() >= kLimit)
                    return fail(ErrorCode::Corrupt, "Invalid or duplicate case result");
                if (catch2) {
                    const auto overall = node.child("OverallResult");
                    if (!overall || !overall.attribute("success")) return fail(ErrorCode::Corrupt, "Unfinished Catch2 test case");
                    item.outcome = !overall.attribute("success").as_bool() ? TestOutcome::Failed :
                        overall.attribute("skips").as_int() > 0 ? TestOutcome::Skipped : TestOutcome::Passed;
                    item.duration = seconds(overall.attribute("durationInSeconds").as_string());
                    item.definition = location(node);
                    std::function<void(pugi::xml_node)> failures = [&](pugi::xml_node current) {
                        for (auto child : current.children()) {
                            const std::string_view kind = child.name();
                            if ((kind == "Expression" && std::string_view(child.attribute("success").as_string()) == "false") ||
                                kind == "Failure" || kind == "Exception" || kind == "FatalErrorCondition") {
                                const std::string message = child.child("Original") ? child.child_value("Original") : child.text().as_string();
                                item.failures.push_back(TestFailure{message, location(child)});
                            }
                            failures(child);
                        }
                    };
                    failures(node);
                } else {
                    const std::string_view verdict = node.attribute("result").as_string();
                    if (verdict == "passed") item.outcome = TestOutcome::Passed;
                    else if (verdict == "skipped") item.outcome = TestOutcome::Skipped;
                    else if (verdict == "failed" || verdict == "aborted" || verdict == "timed-out") item.outcome = TestOutcome::Failed;
                    else return fail(ErrorCode::Corrupt, "Boost report omitted its case verdict");
                    item.reason = verdict;
                }
                result.cases.push_back(std::move(item));
            }
        }
        return {};
    };
    if (auto parsed = visit(root, {}, !catch2); !parsed) return std::unexpected(parsed.error());
    if (actual.empty() && omittedSuites.empty()) return fail(ErrorCode::Corrupt, "Framework reported no executed cases");
    if (!selected.empty()) {
        const std::set<std::string> expected(selected.begin(), selected.end());
        for (const auto& name : actual) if (!expected.contains(name))
            return fail(ErrorCode::Corrupt, "Framework reported a case outside the selection");
        for (const auto& name : expected) if (!actual.contains(name) && !std::ranges::any_of(omittedSuites,
                [&](const auto& suite) { return name.starts_with(suite); }))
            return fail(ErrorCode::Corrupt, "Framework report does not account for a selected case");
    }
    if (catch2) {
        const auto totals = root.child("OverallResultsCases");
        const auto passed = number(totals.attribute("successes").as_string());
        const auto failed = number(totals.attribute("failures").as_string());
        const auto expected = number(totals.attribute("expectedFailures").as_string());
        const auto skipped = number(totals.attribute("skips").as_string());
        if (!passed || !failed || !expected || !skipped ||
            (static_cast<std::uint64_t>(*passed) + *failed + *expected + *skipped) != result.cases.size() ||
            std::ranges::count(result.cases, TestOutcome::Failed, &TestResult::outcome) != *failed ||
            std::ranges::count(result.cases, TestOutcome::Skipped, &TestResult::outcome) != *skipped)
            return fail(ErrorCode::Corrupt, "Catch2 case counters disagree with the complete case records");
    }
    if (!catch2) {
        pugi::xml_document logs;
        if (auto parsed = load(logs, log); !parsed) return std::unexpected(parsed.error());
        const auto logRoot = logs.child("TestLog");
        if (!logRoot || std::string_view(logRoot.child("BuildInfo").attribute("boost").as_string()) != "1.85.0")
            return fail(ErrorCode::Unsupported, "Expected Boost.Test 1.85.0 XML log with build information");
        std::function<void(pugi::xml_node, std::string, bool)> merge;
        merge = [&](pugi::xml_node parent, std::string prefix, bool master) {
            for (auto node : parent.children()) {
                const std::string_view tag = node.name();
                if (tag == "TestSuite") merge(node, master ? prefix : prefix + node.attribute("name").as_string() + "/", false);
                else if (tag == "TestCase") {
                    const auto found = std::ranges::find(result.cases, prefix + node.attribute("name").as_string(), &TestResult::name);
                    if (found == result.cases.end()) continue;
                    found->definition = location(node, "file");
                    if (const auto time = number(node.child_value("TestingTime"))) found->duration = std::chrono::microseconds(*time);
                    for (auto entry : node.children()) {
                        const std::string_view kind = entry.name();
                        if (kind == "Error" || kind == "FatalError" || kind == "Exception")
                            found->failures.push_back(TestFailure{entry.text().as_string(), location(entry, "file")});
                    }
                }
            }
        };
        merge(logRoot, {}, true);
    }
    result.outcome = std::ranges::any_of(result.cases, [](const auto& item) { return countsAsFailure(item.outcome); })
        ? TestOutcome::Failed : std::ranges::all_of(result.cases, [](const auto& item) { return item.outcome == TestOutcome::Skipped; })
        ? TestOutcome::Skipped : TestOutcome::Passed;
    if (!catch2) {
        const bool childFailure = result.outcome == TestOutcome::Failed;
        const auto module = root.child("TestSuite");
        const std::string_view verdict = module.attribute("result").as_string();
        if (verdict == "failed" || verdict == "aborted" || verdict == "timed-out") result.outcome = TestOutcome::Failed;
        else if (verdict == "skipped") result.outcome = TestOutcome::Skipped;
        else if (verdict != "passed") return fail(ErrorCode::Corrupt, "Boost module verdict is missing");
        if ((verdict == "passed" || verdict == "skipped") && childFailure)
            return fail(ErrorCode::Corrupt, "Boost module verdict contradicts child failures");
    }
    if (exitCode != 0 && result.outcome == TestOutcome::Passed)
        return fail(ErrorCode::Corrupt, "Framework exited unsuccessfully despite a passing report");
    return result;
}
}
