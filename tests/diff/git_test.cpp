// SPDX-License-Identifier: Apache-2.0
//
// The differential test against `git diff --histogram --no-index` (ADR 0019, D6 and D16): the
// dialect probe, every generator class at -U0 under the dialect rules (two-sided on both
// dialects), the fixtures (exact on both dialects), unified bodies at -U0/-U1/-U3, `git apply` of
// our patches, fault injection of the runner through io::ScriptedProcessLauncher, and the
// deliberate rewrite of the unified goldens. Every comparison uses LineSplit::Lf, git's model.

#include "frost/diff/unified.hpp"
#include "frost/io/scripted_process_launcher.hpp"

#include "diff/diff_test_util.hpp"
#include "diff/git_test_support.hpp"
#include "diff/support/files.hpp"
#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>

namespace frost::diff::test {
namespace {

struct ClassSpec {
    std::string_view name;
    PairClass kind;
    std::uint32_t count;
    std::uint64_t master;
    bool totalsOnly;
    NewDialectFewer measured; // git-fewer cases of exactly these cases on git 2.56 (ADR 0019).
};

constexpr ClassSpec kClasses[] = {
    {"Adversarial", PairClass::Adversarial, 2000, 0xA11CE001, false, {21, 2000}},
    {"MidFrequency", PairClass::MidFrequency, 300, 0xA11CE002, false, {9, 300}},
    {"SparseFallback", PairClass::SparseFallback, 300, 0xA11CE003, false, {14, 300}},
    {"LoneCr", PairClass::LoneCr, 300, 0xA11CE004, false, {0, 300}},
    {"CodeLike", PairClass::CodeLike, 300, 0xA11CE005, false, {0, 300}},
    {"DenseFallbackMinimal", PairClass::DenseFallback, 150, 0xA11CE006, true, {0, 150}},
};

const ClassSpec& classNamed(std::string_view name) {
    for (const ClassSpec& spec : kClasses) {
        if (spec.name == name) {
            return spec;
        }
    }
    return kClasses[0];
}

void runClass(std::string_view name) {
    auto session = startGitSession("diff-git");
    ASSERT_TRUE(session.has_value());
    const ClassSpec& spec = classNamed(name);
    const std::vector<SeededCase> cases = generateCases(spec.kind, spec.count, spec.master);
    const ClassCounts counts = compareWithGit(*session, spec.name, cases, spec.totalsOnly);
    printCounts(*session, spec.name, counts);
    EXPECT_EQ(counts.gitMore, 0u);
    if (session->dialect == GitDialect::Classic && !spec.totalsOnly) {
        EXPECT_EQ(counts.exact, counts.cases);
    }
    if (session->dialect == GitDialect::New) {
        // Two-sided on the new dialect as well: the same cases every run, so at most twice the
        // git-fewer cases measured on git 2.56 (equal-cost cases already failed one by one).
        EXPECT_LE(counts.gitFewer, spec.measured.cap(spec.count))
            << spec.name << ": " << counts.gitFewer << " cases where git changes fewer lines; " << spec.measured.count
            << " were measured on git 2.56 (ADR 0019)";
    }
    printTimes(*session, spec.name);
}

TEST(DiffGit, DialectProbe) {
    auto session = startGitSession("diff-git");
    ASSERT_TRUE(session.has_value());
    EXPECT_FALSE(session->version.empty());
}

TEST(DiffGit, Adversarial) { runClass("Adversarial"); }
TEST(DiffGit, MidFrequency) { runClass("MidFrequency"); }
TEST(DiffGit, LoneCr) { runClass("LoneCr"); }
TEST(DiffGit, CodeLike) { runClass("CodeLike"); }

TEST(DiffGit, SparseFallback) {
    // Also puts on record how many cases reach the Myers fallback (the re-validation of the
    // prototype's DP stand-in, ADR 0019 Measurements).
    runClass("SparseFallback");
}

TEST(DiffGit, DenseFallbackMinimal) {
    // A consistency check: git's Myers variant and ours both return minimal scripts here, with
    // different tie-breaks, so only the deleted + inserted totals are compared.
    runClass("DenseFallbackMinimal");
}

std::vector<SeededCase> fixtureCases(std::vector<std::string>& names) {
    std::vector<SeededCase> cases;
    const std::string root = testDataPath("fixtures/pairs");
    auto listed = listNames(root, true);
    EXPECT_TRUE(listed.has_value());
    if (!listed) {
        return cases;
    }
    names = *listed;
    for (const std::string& name : names) {
        auto a = readBinaryFile(joinUtf8(joinUtf8(root, name), "a"));
        auto b = readBinaryFile(joinUtf8(joinUtf8(root, name), "b"));
        EXPECT_TRUE(a && b) << name;
        cases.push_back(SeededCase{0, GitCase{a ? *a : std::string(), b ? *b : std::string()}});
    }
    return cases;
}

TEST(DiffGit, Fixtures) {
    auto session = startGitSession("diff-git");
    ASSERT_TRUE(session.has_value());
    std::vector<std::string> names;
    const std::vector<SeededCase> cases = fixtureCases(names);
    ASSERT_GE(cases.size(), 30u);
    // Fixtures are chosen to agree on both dialects, so they are exact everywhere.
    GitSession& s = *session;
    const GitDialect probed = s.dialect;
    s.dialect = GitDialect::Classic;
    const ClassCounts counts = compareWithGit(s, "Fixtures", cases);
    s.dialect = probed;
    printCounts(s, "Fixtures", counts);
    EXPECT_EQ(counts.exact, counts.cases);
    constexpr std::uint32_t kContexts[] = {0, 3};
    compareUnifiedBodies(s, "Fixtures", cases, counts.matched, kContexts);
}

TEST(DiffGit, UnifiedBodies) {
    auto session = startGitSession("diff-git");
    ASSERT_TRUE(session.has_value());
    for (const ClassSpec& spec : kClasses) {
        const std::vector<SeededCase> cases = generateCases(spec.kind, spec.count, spec.master);
        // The bodies of the cases whose -U0 hunks equal git's (the per-case rules are
        // DiffGit.<class>'s), all three contexts over one written batch.
        constexpr std::uint32_t kContexts[] = {0, 1, 3};
        compareUnifiedBodies(*session, spec.name, cases, {}, kContexts);
    }
    printTimes(*session, "UnifiedBodies");
}

// Our patch for every differing case, with git headers.
std::string patchFor(std::span<const SeededCase> cases, std::uint32_t context) {
    std::string patch;
    for (std::size_t k = 0; k < cases.size(); ++k) {
        if (cases[k].pair.a == cases[k].pair.b) {
            continue;
        }
        const LineSequence a = makeSequence(cases[k].pair.a, LineSplit::Lf);
        const LineSequence b = makeSequence(cases[k].pair.b, LineSplit::Lf);
        auto diff = diffLines(a, b);
        EXPECT_TRUE(diff.has_value());
        const std::string name = caseName(k);
        UnifiedOptions options;
        options.context = context;
        options.oldHeader = "a/" + name;
        options.newHeader = "b/" + name;
        patch += std::format("diff --git a/{0} b/{0}\n", name);
        patch += renderUnified(a, b, *diff, options);
    }
    return patch;
}

/*
 * Whether git apply --unidiff-zero is known to misapply the -U0 patch of a case: a pure deletion of
 * A's last line when that line has no break and lines precede it. git apply then drops the break
 * of the line before as well; git 2.43 and 2.56 do the same with git's own, byte-identical -U0
 * patch (ADR 0019, Deviations). Our POSIX applier covers these cases in every apply property test.
 */
bool unidiffZeroMisapplies(const GitCase& pair) {
    const LineSequence a = makeSequence(pair.a, LineSplit::Lf);
    const LineSequence b = makeSequence(pair.b, LineSplit::Lf);
    auto diff = diffLines(a, b);
    if (!diff || diff->hunks.empty() || a.size() == 0 || a.hasBreak(LineIndex{a.size() - 1})) {
        return false;
    }
    const Hunk& last = diff->hunks.back();
    return last.b.empty() && last.a.end.value() == a.size() && last.a.begin.value() > 0;
}

TEST(DiffGit, GitApplyAcceptsOurPatches) {
    auto session = startGitSession("diff-git");
    ASSERT_TRUE(session.has_value());
    for (const ClassSpec& spec : kClasses) {
        const std::vector<SeededCase> cases =
            generateCases(spec.kind, std::min<std::uint32_t>(spec.count, 300), spec.master);
        std::vector<GitCase> pairs;
        for (const SeededCase& item : cases) {
            pairs.push_back(item.pair);
        }
        for (const std::uint32_t context : {3u, 0u}) {
            auto files = session->runner->apply(pairs, patchFor(cases, context), context == 0);
            ASSERT_TRUE(files.has_value()) << spec.name << " -U" << context << ": " << files.error().describe();
            ASSERT_EQ(files->size(), cases.size());
            std::uint32_t skipped = 0;
            for (std::size_t k = 0; k < cases.size(); ++k) {
                if (context == 0 && unidiffZeroMisapplies(cases[k].pair)) {
                    ++skipped;
                    continue;
                }
                EXPECT_EQ((*files)[k], cases[k].pair.b)
                    << spec.name << " -U" << context << " seed " << cases[k].seed
                    << "\nA: " << printable(cases[k].pair.a)
                    << "\nour patch: " << printable(patchFor(std::span(cases).subspan(k, 1), context), 4000);
            }
            std::printf("[ git  ] %s -U%u: git apply%s gave B for %zu cases; %u not compared (a deleted incomplete "
                        "last line, which git apply --unidiff-zero misapplies)\n",
                        std::string(spec.name).c_str(), context, context == 0 ? " --unidiff-zero" : "",
                        cases.size() - skipped, skipped);
        }
    }
    printTimes(*session, "GitApplyAcceptsOurPatches");
}

// --- Fault injection of the oracle harness -------------------------------------------------------

#ifdef _WIN32
constexpr std::string_view kFakeGit = "C:\\fake\\bin\\git.exe";
#else
constexpr std::string_view kFakeGit = "/fake/bin/git";
#endif

// The canaries' classic answer, in git's output format.
std::string canaryOutput(const std::vector<DialectGolden>& canaries, bool mixed) {
    std::vector<std::string> bodies;
    for (std::size_t k = 0; k < canaries.size(); ++k) {
        const LineSequence a = makeSequence(canaries[k].a, LineSplit::Lf);
        const LineSequence b = makeSequence(canaries[k].b, LineSplit::Lf);
        auto diff = diffLines(a, b);
        EXPECT_TRUE(diff.has_value());
        if (mixed && k == 0) {
            // A hunk list that is neither dialect's: the whole input replaced.
            LineDiff whole;
            whole.hunks.push_back(
                Hunk{LineRange{LineIndex{0}, LineIndex{a.size()}}, LineRange{LineIndex{0}, LineIndex{b.size()}}});
            bodies.push_back(renderUnified(a, b, whole, UnifiedOptions{0, {}, {}}));
        } else {
            bodies.push_back(renderUnified(a, b, *diff, UnifiedOptions{0, {}, {}}));
        }
    }
    return gitStyleOutput(bodies);
}

struct Fault {
    std::string_view name;
    io::ScriptedProcess casesScript;
    bool mixedCanaries = false;
    ErrorCode code;
    std::string_view message; // A substring of the error.
};

TEST(DiffGit, RunnerFaults) {
    auto canaries = loadDialectGoldens(testDataPath("goldens/histogram/dialect"));
    ASSERT_TRUE(canaries.has_value());
    const std::vector<GitCase> cases = {{"a\nb\n", "a\nc\n"}, {"same\n", "same\n"}, {"x\n", "y\n"}};
    const std::string good =
        gitStyleOutput(std::vector<std::string>{"@@ -2 +2 @@\n-b\n+c\n", "", "@@ -1 +1 @@\n-x\n+y\n"});
    const auto script = [](int exitCode, std::string out, std::string err) {
        io::ScriptedProcess process;
        process.exitCode = exitCode;
        process.stdoutBytes = std::move(out);
        process.stderrBytes = std::move(err);
        return process;
    };
    io::ScriptedProcess hung;
    hung.neverExits = true;
    io::ScriptedProcess missing;
    missing.launchError = Error(ErrorCode::NotFound, "no such file");
    const Fault faults[] = {
        {"success", script(1, good, ""), false, ErrorCode::Unknown, ""},
        {"exit 128", script(128, "", "fatal: not a git repository\n"), false, ErrorCode::InvalidArgument, "exited 128"},
        {"could not access", script(1, "", "error: Could not access 'nonexist'\n"), false, ErrorCode::InvalidArgument,
         "Could not access"},
        {"stderr on success", script(0, good, "warning: something\n"), false, ErrorCode::InvalidArgument, "warning"},
        {"malformed @@",
         script(1, gitStyleOutput(std::vector<std::string>{"@@ -x +2 @@\n-b\n+c\n", "", "@@ -1 +1 @@\n-x\n+y\n"}), ""),
         false, ErrorCode::InvalidArgument, "malformed hunk header"},
        {"missing section", script(1, gitStyleOutput(std::vector<std::string>{"@@ -2 +2 @@\n-b\n+c\n"}), ""), false,
         ErrorCode::InvalidArgument, "no section for differing case c000002"},
        {"identical section",
         script(1,
                gitStyleOutput(std::vector<std::string>{"@@ -2 +2 @@\n-b\n+c\n", "@@ -1 +1 @@\n-same\n+same\n",
                                                        "@@ -1 +1 @@\n-x\n+y\n"}),
                ""),
         false, ErrorCode::InvalidArgument, "byte-identical case c000001"},
        {"unknown name", script(1, good + "diff --git a/a/c999999 b/b/c999999\n@@ -1 +1 @@\n-p\n+q\n", ""), false,
         ErrorCode::InvalidArgument, "unknown case name"},
        {"duplicate name", script(1, good + "diff --git a/a/c000000 b/b/c000000\n@@ -1 +1 @@\n-p\n+q\n", ""), false,
         ErrorCode::InvalidArgument, "duplicate section"},
        {"mixed canaries", script(1, good, ""), true, ErrorCode::InvalidArgument, "unknown git dialect"},
        {"hung git", hung, false, ErrorCode::Timeout, ""},
        {"launch error", missing, false, ErrorCode::NotFound, "git not found"},
    };
    for (const Fault& fault : faults) {
        frost::test::ScratchDir scratch("diff-git-faults");
        io::ScriptedProcessLauncher launcher;
        GitRunner runner(launcher, std::string(kFakeGit), scratch.path());
        ASSERT_TRUE(runner.prepare().has_value());
        const bool launchFault = fault.casesScript.launchError.has_value();
        if (launchFault) {
            launcher.expect(fault.casesScript); // The very first launch (version) fails.
            auto version = runner.version();
            ASSERT_FALSE(version.has_value()) << fault.name;
            EXPECT_EQ(version.error().code(), fault.code) << fault.name;
            EXPECT_NE(version.error().describe().find(fault.message), std::string::npos)
                << fault.name << ": " << version.error().describe();
            continue;
        }
        launcher.expect(script(0, "git version 9.99.0 (scripted)\n", ""));
        launcher.expect(script(1, canaryOutput(*canaries, fault.mixedCanaries), ""));
        launcher.expect(fault.casesScript);
        auto version = runner.version();
        ASSERT_TRUE(version.has_value()) << fault.name;
        EXPECT_EQ(*version, "git version 9.99.0 (scripted)");
        std::string report;
        auto dialect = runner.probeDialect(*canaries, report);
        if (fault.mixedCanaries) {
            ASSERT_FALSE(dialect.has_value()) << fault.name;
            EXPECT_NE(dialect.error().describe().find(fault.message), std::string::npos) << dialect.error().describe();
            EXPECT_NE(dialect.error().describe().find("9.99.0"), std::string::npos);
            continue;
        }
        ASSERT_TRUE(dialect.has_value()) << fault.name << ": " << dialect.error().describe();
        EXPECT_EQ(*dialect, GitDialect::Classic);
        auto sections = runner.diff(cases, 0);
        if (fault.code == ErrorCode::Unknown) {
            ASSERT_TRUE(sections.has_value()) << sections.error().describe();
            EXPECT_EQ(formatHunks((*sections)[0].hunks), "[(1,2,1,2)]");
            EXPECT_FALSE((*sections)[1].present);
            EXPECT_EQ((*sections)[2].body, "@@ -1 +1 @@\n-x\n+y\n");
        } else {
            ASSERT_FALSE(sections.has_value()) << fault.name;
            EXPECT_EQ(sections.error().code(), fault.code) << fault.name << ": " << sections.error().describe();
            EXPECT_NE(sections.error().describe().find(fault.message), std::string::npos)
                << fault.name << ": " << sections.error().describe();
        }
        // Every launch used the absolute executable and the pinned environment.
        for (const io::LaunchRecord& record : launcher.launches()) {
            EXPECT_EQ(record.spec.executable, kFakeGit);
            bool attrNoSystem = false;
            for (const auto& [variable, value] : record.spec.environment) {
                attrNoSystem = attrNoSystem || (variable == "GIT_ATTR_NOSYSTEM" && value == "1");
            }
            EXPECT_TRUE(attrNoSystem);
        }
    }
}

TEST(DiffGit, WriteUnifiedGoldens) {
    if (!updateGoldens()) {
        std::printf("[ git  ] set FROST_DIFF_UPDATE_GOLDENS=1 (with a Classic-dialect git) to rewrite "
                    "tests/diff/goldens/unified\n");
        return;
    }
    auto session = startGitSession("diff-git");
    ASSERT_TRUE(session.has_value());
    ASSERT_EQ(session->dialect, GitDialect::Classic)
        << "the unified goldens are git " << session->version << "'s, which is not a Classic-dialect git";
    std::vector<std::string> names;
    const std::vector<SeededCase> cases = fixtureCases(names);
    std::vector<GitCase> pairs;
    for (const SeededCase& item : cases) {
        pairs.push_back(item.pair);
    }
    const std::string dir = testDataPath("goldens/unified");
    ASSERT_TRUE(makeDirectories(dir).has_value());
    for (const std::uint32_t context : {0u, 3u}) {
        auto sections = session->runner->diff(pairs, context);
        ASSERT_TRUE(sections.has_value()) << sections.error().describe();
        for (std::size_t k = 0; k < names.size(); ++k) {
            ASSERT_TRUE(writeBinaryFile(joinUtf8(dir, std::format("{}.u{}", names[k], context)), (*sections)[k].body)
                            .has_value());
        }
    }
    ADD_FAILURE() << "rewrote " << names.size() << " unified goldens from " << session->version
                  << "; review and commit them";
}

} // namespace
} // namespace frost::diff::test
