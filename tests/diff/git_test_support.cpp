// SPDX-License-Identifier: Apache-2.0
//
// The git differential harness (git_test_support.hpp).

#include "diff/git_test_support.hpp"

#include "frost/diff/unified.hpp"
#include "frost/io/process.hpp"
#include "frost/io/utf.hpp"

#include "diff/diff_test_util.hpp"
#include "diff/support/goldens.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <format>

#ifdef _WIN32
#include <windows.h>
#endif

namespace frost::diff::test {
namespace {

std::string environmentUtf8(const wchar_t* wideName, const char* name) {
#ifdef _WIN32
    (void)name;
    const DWORD needed = GetEnvironmentVariableW(wideName, nullptr, 0);
    if (needed == 0) {
        return {};
    }
    std::wstring value(needed, L'\0');
    const DWORD length = GetEnvironmentVariableW(wideName, value.data(), needed);
    if (length == 0 || length >= needed) {
        return {};
    }
    value.resize(length);
    auto utf8 = io::fromWide(value);
    return utf8 ? std::move(*utf8) : std::string();
#else
    (void)wideName;
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string();
#endif
}

std::uint64_t changedLines(const HunkList& hunks) {
    std::uint64_t total = 0;
    for (const HunkTuple& h : hunks) {
        total += (h[1] - h[0]) + (h[3] - h[2]);
    }
    return total;
}

} // namespace

std::string gitExecutable() {
    std::string fromEnvironment = environmentUtf8(L"FROST_GIT", "FROST_GIT");
    if (!fromEnvironment.empty()) {
        return fromEnvironment;
    }
    return std::string(FROST_DIFF_GIT);
}

std::optional<GitSession> startGitSession(std::string_view tag) {
    const std::string git = gitExecutable();
    if (git.empty()) {
        ADD_FAILURE() << kGitNotFound;
        return std::nullopt;
    }
    GitSession session;
    session.scratch = std::make_unique<frost::test::ScratchDir>(tag);
    if (session.scratch->path().empty()) {
        return std::nullopt;
    }
    session.runner = std::make_unique<GitRunner>(io::realProcessLauncher(), git, session.scratch->path());
    if (auto prepared = session.runner->prepare(); !prepared) {
        ADD_FAILURE() << prepared.error().describe();
        return std::nullopt;
    }
    auto version = session.runner->version();
    if (!version) {
        ADD_FAILURE() << version.error().describe();
        return std::nullopt;
    }
    session.version = *version;
    auto canaries = loadDialectGoldens(testDataPath("goldens/histogram/dialect"));
    if (!canaries) {
        ADD_FAILURE() << canaries.error().describe();
        return std::nullopt;
    }
    std::string report;
    auto dialect = session.runner->probeDialect(*canaries, report);
    if (!dialect) {
        ADD_FAILURE() << dialect.error().describe();
        return std::nullopt;
    }
    session.dialect = *dialect;
    std::printf("[ git  ] %s at %s: %s dialect\n", session.version.c_str(), git.c_str(),
                std::string(dialectName(session.dialect)).c_str());
    return session;
}

SeededCase caseFromSeed(PairClass kind, std::uint64_t seed, SizeClass size) {
    SplitMix64 local(seed);
    TextPair pair = generatePair(kind, local, size);
    return SeededCase{seed, GitCase{std::move(pair.a), std::move(pair.b)}};
}

std::vector<SeededCase> generateCases(PairClass kind, std::uint32_t count, std::uint64_t master, SizeClass size) {
    SplitMix64 rng(master);
    std::vector<SeededCase> cases;
    cases.reserve(count);
    for (std::uint32_t c = 0; c < count; ++c) {
        cases.push_back(caseFromSeed(kind, rng.next(), size));
    }
    return cases;
}

ClassCounts compareWithGit(GitSession& session, std::string_view name, const std::vector<SeededCase>& cases,
                           bool totalsOnly) {
    ClassCounts counts;
    counts.cases = static_cast<std::uint32_t>(cases.size());
    counts.matched.assign(cases.size(), false);
    std::vector<GitCase> pairs;
    pairs.reserve(cases.size());
    for (const SeededCase& item : cases) {
        pairs.push_back(item.pair);
    }
    auto sections = session.runner->diff(pairs, 0);
    if (!sections) {
        ADD_FAILURE() << name << ": " << sections.error().describe();
        return counts;
    }
    for (std::size_t k = 0; k < cases.size(); ++k) {
        const LineSequence a = makeSequence(cases[k].pair.a, LineSplit::Lf);
        const LineSequence b = makeSequence(cases[k].pair.b, LineSplit::Lf);
        auto diff = diffLines(a, b);
        if (!diff || diff->degraded) {
            ADD_FAILURE() << name << " seed " << cases[k].seed << ": "
                          << (diff ? std::string("degraded") : diff.error().describe());
            continue;
        }
        counts.fallbacks += diff->fallbacks;
        counts.fallbackCases += diff->fallbacks > 0 ? 1 : 0;
        const HunkList ours = hunkTuples(*diff);
        const HunkList& git = (*sections)[k].hunks;
        if (ours == git) {
            ++counts.exact;
            counts.matched[k] = true;
            continue;
        }
        const std::uint64_t ourLines = changedLines(ours);
        const std::uint64_t gitLines = changedLines(git);
        const bool classic = session.dialect == GitDialect::Classic;
        std::string_view verdict;
        bool failure = false;
        if (gitLines > ourLines) {
            ++counts.gitMore;
            verdict = "git changes MORE lines";
            failure = true;
        } else if (gitLines < ourLines) {
            ++counts.gitFewer;
            verdict = "git changes fewer lines";
            failure = classic; // On New the caller caps the number of these cases.
        } else {
            ++counts.equalCost;
            verdict = "equal cost, different hunks";
            failure = !totalsOnly; // Neither dialect makes equal-cost cases outside dense fallbacks.
        }
        if (failure) {
            ADD_FAILURE() << name << " seed " << cases[k].seed << " (" << dialectName(session.dialect)
                          << " dialect): " << verdict << "\n  ours " << formatHunks(ours) << "\n  git  "
                          << formatHunks(git) << "\n  a " << printable(cases[k].pair.a, 300) << "\n  b "
                          << printable(cases[k].pair.b, 300);
        } else if (!totalsOnly) {
            std::printf("[ git  ] %s seed %llu: %s; ours %s, git %s\n", std::string(name).c_str(),
                        static_cast<unsigned long long>(cases[k].seed), std::string(verdict).c_str(),
                        formatHunks(ours).c_str(), formatHunks(git).c_str());
        }
    }
    return counts;
}

void compareUnifiedBodies(GitSession& session, std::string_view name, const std::vector<SeededCase>& cases,
                          const std::vector<bool>& matched, std::span<const std::uint32_t> contexts) {
    std::vector<GitCase> pairs;
    for (const SeededCase& item : cases) {
        pairs.push_back(item.pair);
    }
    auto runs = session.runner->diffContexts(pairs, contexts);
    if (!runs) {
        ADD_FAILURE() << name << ": " << runs.error().describe();
        return;
    }
    std::vector<bool> exact = matched;
    if (exact.empty()) {
        // Derived from the -U0 run: the cases whose hunks equal git's.
        ASSERT_TRUE(!contexts.empty() && contexts[0] == 0) << "matched may be empty only with -U0 first";
        exact.assign(cases.size(), false);
        for (std::size_t k = 0; k < cases.size(); ++k) {
            auto diff =
                diffLines(makeSequence(cases[k].pair.a, LineSplit::Lf), makeSequence(cases[k].pair.b, LineSplit::Lf));
            exact[k] = diff.has_value() && hunkTuples(*diff) == (*runs)[0][k].hunks;
        }
    }
    for (std::size_t c = 0; c < contexts.size(); ++c) {
        const std::vector<GitSection>& sections = (*runs)[c];
        std::uint32_t compared = 0;
        for (std::size_t k = 0; k < cases.size(); ++k) {
            if (!exact[k]) {
                continue;
            }
            const LineSequence a = makeSequence(cases[k].pair.a, LineSplit::Lf);
            const LineSequence b = makeSequence(cases[k].pair.b, LineSplit::Lf);
            auto diff = diffLines(a, b);
            ASSERT_TRUE(diff.has_value());
            UnifiedOptions options;
            options.context = contexts[c];
            const std::string ours = renderUnified(a, b, *diff, options);
            if (ours != sections[k].body) {
                ADD_FAILURE() << name << " seed " << cases[k].seed << " -U" << contexts[c] << ": bodies differ\n  ours "
                              << printable(ours, 600) << "\n  git  " << printable(sections[k].body, 600);
            }
            ++compared;
        }
        std::printf("[ git  ] %s -U%u: %u bodies compared\n", std::string(name).c_str(), contexts[c], compared);
    }
}

void printTimes(const GitSession& session, std::string_view test) {
    const double write = std::chrono::duration<double, std::milli>(session.runner->writeTime()).count();
    const double git = std::chrono::duration<double, std::milli>(session.runner->gitTime()).count();
    std::printf("[ git  ] %s: writing batch files %.0f ms, waiting for git %.0f ms (file-writing share %.0f%%)\n",
                std::string(test).c_str(), write, git, write + git > 0 ? 100.0 * write / (write + git) : 0.0);
}

void printCounts(const GitSession& session, std::string_view name, const ClassCounts& counts) {
    std::printf("[ git  ] %s vs %s (%s): %u cases, %u exact, %u git-fewer, %u equal-cost, %u git-more; "
                "%u cases with fallback (%u regions)\n",
                std::string(name).c_str(), session.version.c_str(), std::string(dialectName(session.dialect)).c_str(),
                counts.cases, counts.exact, counts.gitFewer, counts.equalCost, counts.gitMore, counts.fallbackCases,
                counts.fallbacks);
}

} // namespace frost::diff::test
