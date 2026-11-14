// SPDX-License-Identifier: Apache-2.0
// FER-10: decode only engine-owned scene identities and actual run reports.
#include "frost/engine/engine_test_provider.hpp"
#include "frost/io/path.hpp"

#include <algorithm>
#include <limits>
#include <set>

namespace frost::engine {
namespace {
using rpc::Json;
constexpr std::size_t kEntries = 100000, kText = 1024 * 1024;
bool text(std::string_view s) { return s.size() <= kText && s.find('\0') == s.npos; }
bool asset(std::string_view s) {
    if (!s.starts_with("Assets/") || s.size() <= 7 || s.find_first_of("\\:") != s.npos || !text(s))
        return false;
    for (const char c : s)
        if (static_cast<unsigned char>(c) < 32 || c == 127)
            return false;
    for (std::size_t start = 0; start < s.size();) {
        const auto end = s.find('/', start);
        const auto part = s.substr(start, end == s.npos ? s.size() - start : end - start);
        if (part.empty() || part == "." || part == "..")
            return false;
        if (end == s.npos)
            return true;
        start = end + 1;
    }
    return false;
}
std::optional<std::uint64_t> count(const Json& j, std::string_view k) {
    const auto* n = j.find(k);
    return n && n->kind() == Json::Kind::Integer ? n->asUnsigned() : std::nullopt;
}
Result<std::string> string(const Json& j, std::string_view key, bool optional = false) {
    const auto* v = j.find(key);
    if (!v && optional)
        return std::string{};
    if (!v || !v->isString() || !text(*v->asString()))
        return fail(ErrorCode::Corrupt, "engine test report: invalid string " + std::string(key));
    return *v->asString();
}
Result<std::optional<EngineTestLocation>> location(const Json& j) {
    const auto* p = j.find("path");
    const auto* l = j.find("line");
    if (!p && !l)
        return std::nullopt;
    // Script load errors report the asset path but no known line (zero). Keep the error without
    // inventing a navigable line. Empty path/zero is the engine's fully unavailable location.
    if (p && p->isString() && (p->asString()->empty() || asset(*p->asString())) && count(j, "line") == 0)
        return std::nullopt;
    if (!p || !l || !p->isString() || !asset(*p->asString()) || !count(j, "line") || *count(j, "line") == 0)
        return fail(ErrorCode::Corrupt, "engine test report: invalid structured source location");
    return EngineTestLocation{*p->asString(), *count(j, "line")};
}
Result<std::optional<std::chrono::microseconds>> duration(const Json& j) {
    if (!j.find("durationMicros"))
        return std::nullopt;
    const auto n = count(j, "durationMicros");
    if (!n || *n > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        return fail(ErrorCode::Corrupt, "engine test report: invalid measured duration");
    return std::chrono::microseconds(static_cast<std::int64_t>(*n));
}
Result<void> appendFailure(const Json& j, std::string_view peer, EngineSceneTestResult& out, bool script) {
    if (!j.isObject() || out.failures.size() >= kEntries)
        return fail(ErrorCode::Corrupt, "engine test report: invalid failure");
    auto m = string(j, "message"), c = string(j, "testCase", script), raw = string(j, "location", script);
    auto loc = location(j);
    if (!m)
        return std::unexpected(m.error());
    if (!c)
        return std::unexpected(c.error());
    if (!raw)
        return std::unexpected(raw.error());
    if (!loc)
        return std::unexpected(loc.error());
    if (script && raw->empty()) {
        *raw = std::string(j.stringAt("path"));
        if (const auto line = count(j, "line"); line && *line > 0)
            *raw += ":" + std::to_string(*line);
    }
    out.failures.push_back({std::move(*m), std::move(*c), std::string(peer), std::move(*raw), std::move(*loc)});
    return {};
}
Result<void> appendRun(const Json& j, std::string_view peer, EngineSceneTestResult& out) {
    const auto* errors = j.find("scriptErrors");
    if (!errors || !errors->isArray() || errors->size() > kEntries || !count(j, "scriptErrorCount") ||
        *count(j, "scriptErrorCount") < errors->size())
        return fail(ErrorCode::Corrupt, "engine test report: invalid script errors");
    for (const auto& e : errors->items()) {
        auto r = appendFailure(e, peer, out, true);
        if (!r)
            return r;
    }
    if (const auto* e = j.find("error")) {
        if (!e->isString() || !text(*e->asString()))
            return fail(ErrorCode::Corrupt, "engine test report: invalid run error");
        out.failures.push_back({*e->asString(), {}, std::string(peer), {}, std::nullopt});
    }
    const auto* t = j.find("testResults");
    if (!t)
        return fail(ErrorCode::Corrupt, "engine test report: missing runtime test results");
    if (t->isNull())
        return {};
    const auto* cases = t->find("caseResults");
    const auto* failures = t->find("failures");
    if (!t->isObject() || !cases || !cases->isArray() || cases->size() > kEntries - out.cases.size() || !failures ||
        !failures->isArray() || failures->size() > kEntries || !t->boolAt("passed"))
        return fail(ErrorCode::Corrupt, "engine test report: invalid runtime test results");
    std::uint64_t passed = 0, failed = 0, skipped = 0;
    for (const auto& c : cases->items()) {
        auto name = string(c, "name"), status = string(c, "status"), skip = string(c, "skipReason", true);
        auto loc = location(c);
        auto d = duration(c);
        if (!name)
            return std::unexpected(name.error());
        if (!status)
            return std::unexpected(status.error());
        if (!skip)
            return std::unexpected(skip.error());
        if (!loc)
            return std::unexpected(loc.error());
        if (!d)
            return std::unexpected(d.error());
        EngineTestOutcome outcome;
        if (*status == "Passed") {
            outcome = EngineTestOutcome::Passed;
            ++passed;
        } else if (*status == "Failed") {
            outcome = EngineTestOutcome::Failed;
            ++failed;
        } else if (*status == "Skipped") {
            outcome = EngineTestOutcome::Skipped;
            ++skipped;
        } else
            return fail(ErrorCode::Corrupt, "engine test report: unknown case status");
        out.cases.push_back({std::move(*name), std::string(peer), std::move(*skip), outcome, std::move(*loc), *d});
    }
    if (count(*t, "cases") != cases->size() || count(*t, "passedCases") != passed ||
        count(*t, "failedCases") != failed || count(*t, "skippedCases") != skipped)
        return fail(ErrorCode::Corrupt, "engine test report: inconsistent case counts");
    // Failures outside a Test.Case are real too; never attribute them by a potentially duplicate name.
    for (const auto& f : failures->items()) {
        auto r = appendFailure(f, peer, out, false);
        if (!r)
            return r;
    }
    if (*t->boolAt("passed") && (failed || !failures->items().empty()))
        return fail(ErrorCode::Corrupt, "engine test report: contradictory case outcome");
    return {};
}
} // namespace

std::string_view toString(EngineTestOutcome outcome) noexcept {
    switch (outcome) {
    case EngineTestOutcome::Passed: return "Passed";
    case EngineTestOutcome::Failed: return "Failed";
    case EngineTestOutcome::Skipped: return "Skipped";
    }
    return "Unknown";
}
Result<EngineTestListing> parseEngineTestListing(const Json& j) {
    if (!j.isObject() || !count(j, "version"))
        return fail(ErrorCode::Corrupt, "test.list: missing integer version");
    if (count(j, "version") != 1 || j.stringAt("granularity") != "scene")
        return fail(ErrorCode::Unsupported, "test.list: unsupported version or test granularity");
    auto root = string(j, "projectPath"), file = string(j, "projectFile");
    if (!root)
        return std::unexpected(root.error());
    if (!file)
        return std::unexpected(file.error());
    if (!io::normalizePath(*root) || !io::normalizePath(*file))
        return fail(ErrorCode::Corrupt, "test.list: invalid absolute project identity");
    const auto* tests = j.find("tests");
    if (!tests || !tests->isArray() || tests->size() > kEntries || count(j, "count") != tests->size())
        return fail(ErrorCode::Corrupt, "test.list: invalid scene count");
    EngineTestListing out{std::move(*root), std::move(*file), {}};
    std::set<std::string> seen;
    for (const auto& t : tests->items()) {
        auto id = string(t, "id"), scene = string(t, "scene");
        if (!id)
            return std::unexpected(id.error());
        if (!scene)
            return std::unexpected(scene.error());
        if (*id != *scene || !asset(*scene) || !scene->ends_with(".fscene") || !seen.insert(*id).second)
            return fail(ErrorCode::Corrupt, "test.list: invalid or duplicate scene identity");
        out.tests.push_back({std::move(*id), std::move(*scene)});
    }
    return out;
}
Result<std::vector<EngineSceneTestResult>> parseEngineTestRun(const Json& j, std::span<const std::string> requested) {
    const auto* scenes = j.find("scenes");
    const auto* totals = j.find("totals");
    if (!j.isObject() || !j.boolAt("passed") || !scenes || !scenes->isArray() || scenes->size() > kEntries || !totals ||
        !totals->isObject())
        return fail(ErrorCode::Corrupt, "test.run: invalid scene report");
    if (requested.size() > kEntries)
        return fail(ErrorCode::Corrupt, "test.run: too many requested scenes");
    std::vector<std::string> commandReport;
    if (const auto* report = j.find("report")) {
        if (!report->isArray() || report->size() > kEntries)
            return fail(ErrorCode::Corrupt, "test.run: invalid report lines");
        for (const auto& line : report->items()) {
            if (!line.isString() || !text(*line.asString()))
                return fail(ErrorCode::Corrupt, "test.run: invalid report text");
            commandReport.push_back(*line.asString());
        }
    }
    std::set<std::string> expected(requested.begin(), requested.end()), seen;
    if (expected.size() != requested.size() || scenes->size() != expected.size())
        return fail(ErrorCode::Corrupt, "test.run: result differs from requested scenes");
    std::vector<EngineSceneTestResult> out;
    std::uint64_t passed = 0;
    std::size_t totalCases = 0, totalFailures = 0;
    for (const auto& s : scenes->items()) {
        auto scene = string(s, "scene");
        auto d = duration(s);
        if (!scene)
            return std::unexpected(scene.error());
        if (!d)
            return std::unexpected(d.error());
        if (!asset(*scene) || !expected.contains(*scene) || !seen.insert(*scene).second || !s.boolAt("passed"))
            return fail(ErrorCode::Corrupt, "test.run: invalid or unrequested scene");
        EngineSceneTestResult r;
        r.scene = std::move(*scene);
        r.outcome = *s.boolAt("passed") ? EngineTestOutcome::Passed : EngineTestOutcome::Failed;
        r.duration = *d;
        auto exitName = string(s, "exitName", true), exitReason = string(s, "exitReason", true);
        if (!exitName)
            return std::unexpected(exitName.error());
        if (!exitReason)
            return std::unexpected(exitReason.error());
        r.exitName = std::move(*exitName);
        r.exitReason = std::move(*exitReason);
        if (const auto* code = s.find("exitCode")) {
            if (code->kind() != Json::Kind::Integer || !code->asInteger())
                return fail(ErrorCode::Corrupt, "test.run: invalid exit code");
            r.exitCode = *code->asInteger();
        }
        if (const auto* log = s.find("logTail")) {
            if (!log->isArray() || log->size() > kEntries)
                return fail(ErrorCode::Corrupt, "test.run: invalid log tail");
            for (const auto& entry : log->items()) {
                auto level = string(entry, "level"), logger = string(entry, "logger"),
                     message = string(entry, "message");
                if (!level)
                    return std::unexpected(level.error());
                if (!logger)
                    return std::unexpected(logger.error());
                if (!message)
                    return std::unexpected(message.error());
                r.logTail.push_back({std::move(*level), std::move(*logger), std::move(*message)});
            }
        }
        passed += *s.boolAt("passed");
        const auto* peers = s.find("peers");
        if (peers && (!peers->isArray() || peers->size() > kEntries))
            return fail(ErrorCode::Corrupt, "test.run: invalid peers");
        if (peers && peers->size()) {
            std::set<std::string> names;
            for (const auto& p : peers->items()) {
                auto name = string(p, "name");
                if (!name)
                    return std::unexpected(name.error());
                if (name->empty() || !names.insert(*name).second)
                    return fail(ErrorCode::Corrupt, "test.run: duplicate peer identity");
                auto parsed = appendRun(p, *name, r);
                if (!parsed)
                    return std::unexpected(parsed.error());
            }
        } else {
            auto parsed = appendRun(s, {}, r);
            if (!parsed)
                return std::unexpected(parsed.error());
        }
        if (*s.boolAt("passed") && (!r.failures.empty() || std::ranges::any_of(r.cases, [](const auto& c) {
                return c.outcome == EngineTestOutcome::Failed;
            })))
            return fail(ErrorCode::Corrupt, "test.run: scene outcome contradicts runtime failures");
        if (r.cases.size() > kEntries - totalCases || r.failures.size() > kEntries - totalFailures)
            return fail(ErrorCode::Corrupt, "test.run: too many runtime cases or failures");
        totalCases += r.cases.size();
        totalFailures += r.failures.size();
        out.push_back(std::move(r));
    }
    if (count(*totals, "scenes") != out.size() || count(*totals, "passedScenes") != passed ||
        count(*totals, "failedScenes") != out.size() - passed)
        return fail(ErrorCode::Corrupt, "test.run: inconsistent scene totals");
    // Coverage/component requirements can fail a command even when each scene passed. The caller
    // preserves that command-level refusal; never convert its scenes to fabricated failures.
    if (*j.boolAt("passed") && passed != out.size())
        return fail(ErrorCode::Corrupt, "test.run: contradictory overall outcome");
    if (!out.empty())
        out.front().report = std::move(commandReport);
    return out;
}
} // namespace frost::engine
