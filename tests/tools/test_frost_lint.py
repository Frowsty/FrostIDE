# SPDX-License-Identifier: Apache-2.0
#
# Tests for tools/frost_lint.py (frost-lint v0): the clean and dirty fixtures under
# tests/tools/fixtures/lint (including the qt-svg rule, whose allowed directory is in the clean
# fixture), the C++26 exemptions, the no-CI rule, file enumeration with
# and without git, REUSE glob semantics, and the comment/string lexers.

from __future__ import annotations

import contextlib
import io
import re
import subprocess
import sys
import unittest
from unittest import mock

from tools_test_support import REPO, TOOLS, TempDirTestCase, clean_env, git, run_tool

import frost_lint

_LINE = re.compile(r"^[^:\n]+:\d+: [a-z0-9-]+: .+$")


def _write(path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8", newline="\n")


class FixtureTest(TempDirTestCase):
    def test_clean_fixture_passes(self) -> None:
        repo = self.copy_fixture("lint/clean")
        result = run_tool("frost_lint.py", "--repo", str(repo))
        self.assertEqual(result.stdout, "")
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_dirty_fixture_reports_every_violation(self) -> None:
        repo = self.copy_fixture("lint/dirty")
        found = [(v.path, v.line, v.rule) for v in frost_lint.lint(repo)]
        expected = sorted(
            [("REUSE.toml", 1, "reuse"), ("budget.cpp", 4, "budget"), ("budget.cpp", 5, "budget")]
            + [("cpp26.cpp", line, "cpp26") for line in range(3, 16)]
            + [("icon.png", 1, "reuse"), ("late_spdx.cmake", 12, "spdx-header"),
               ("no_desc.py", 1, "spdx-header"), ("no_spdx.cpp", 1, "spdx-header"),
               ("notes.txt", 1, "reuse"), ("script.lua", 1, "reuse"),
               ("tests/perf/budgets.json", 1, "reuse"),
               ("tests/toolchain_probe/banned/no_header.cpp", 1, "spdx-header"),
               ("tests/toolchain_probe/banned/no_header.cpp", 1, "spdx-header")]
            + [("todo.cpp", line, "todo") for line in (3, 4, 5, 7)]
            + [("win32.cpp", line, "narrow-win32") for line in (4, 5, 6)]
            + [("qt_svg.cpp", line, "qt-svg") for line in range(3, 13)]
        )
        self.assertEqual(sorted(found), expected)

    def test_cli_output_format_and_exit_status(self) -> None:
        repo = self.copy_fixture("lint/dirty")
        result = run_tool("frost_lint.py", "--repo", str(repo))
        self.assertEqual(result.returncode, 1)
        lines = result.stdout.splitlines()
        self.assertTrue(lines)
        for line in lines:
            self.assertRegex(line, _LINE)
        self.assertIn("win32.cpp:4: narrow-win32: narrow Win32 API CreateFileA is banned", result.stdout)
        self.assertIn("qt_svg.cpp:5: qt-svg: #include of QSvgGenerator: Qt Svg is linked only by", result.stdout)

    def test_missing_repository_is_a_usage_error(self) -> None:
        result = run_tool("frost_lint.py", "--repo", str(self.tmp / "absent"))
        self.assertEqual(result.returncode, 2)

    def test_git_failure_is_an_environment_error(self) -> None:
        error = subprocess.CalledProcessError(128, ["git", "ls-files"], stderr=b"fatal: bad index file")
        stderr = io.StringIO()
        with mock.patch.object(frost_lint, "list_repo_files", side_effect=error), contextlib.redirect_stderr(stderr):
            status = frost_lint.main(["--repo", str(self.tmp)])
        self.assertEqual(status, 2)
        self.assertIn("fatal: bad index file", stderr.getvalue())

    def test_unreadable_tree_is_an_environment_error(self) -> None:
        # An OSError while listing the tree (an unreadable .gitignore, say) also exits 2.
        error = PermissionError(13, "Permission denied", str(self.tmp / "locked"))
        stderr = io.StringIO()
        with mock.patch.object(frost_lint, "list_repo_files", side_effect=error), contextlib.redirect_stderr(stderr):
            status = frost_lint.main(["--repo", str(self.tmp)])
        self.assertEqual(status, 2)
        self.assertIn("Permission denied", stderr.getvalue())

    def test_old_python_is_an_environment_error(self) -> None:
        # Pretends to be Python 3.10 before the version check runs.
        code = ("import runpy, sys; sys.version_info = (3, 10, 0, 'final', 0); "
                f"runpy.run_path({str(TOOLS / 'frost_lint.py')!r}, run_name='__main__')")
        result = subprocess.run([sys.executable, "-I", "-B", "-c", code], capture_output=True, text=True,
                                encoding="utf-8", env=clean_env(), check=False)
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertIn("needs Python 3.11 or newer", result.stderr)


class Cpp26ExemptionTest(TempDirTestCase):
    BODY = ("// SPDX-License-Identifier: Apache-2.0\n"
            "// A banned C++26 construct used by the exemption tests.\n"
            "void k() { template for (auto x : {1}) { (void)x; } }\n")

    def _cpp26(self, rel: str) -> list[frost_lint.Violation]:
        _write(self.tmp / rel, self.BODY)
        return [v for v in frost_lint.lint(self.tmp, [rel]) if v.rule == "cpp26"]

    def test_template_for_is_caught_outside_the_exempt_directories(self) -> None:
        self.assertEqual(len(self._cpp26("src/core/expansion.cpp")), 1)
        self.assertEqual(len(self._cpp26("tests/toolchain_probe/allowed/expansion.cpp")), 1)

    def test_template_for_spanning_lines_is_caught(self) -> None:
        rel = "src/core/split.cpp"
        _write(self.tmp / rel, "// SPDX-License-Identifier: Apache-2.0\n// Expansion statement split.\n"
                               "void k() {\n    template\n        for (auto x : {1}) {}\n}\n")
        self.assertEqual([v.line for v in frost_lint.lint(self.tmp, [rel]) if v.rule == "cpp26"], [4])

    def test_toolchain_probe_banned_directory_is_exempt_from_cpp26_only(self) -> None:
        self.assertEqual(self._cpp26("tests/toolchain_probe/banned/expansion.cpp"), [])
        rel = "tests/toolchain_probe/banned/unlicensed.cpp"
        _write(self.tmp / rel, "void k() { template for (auto x : {1}) {} } // TODO: x\n")
        rules = sorted(v.rule for v in frost_lint.lint(self.tmp, [rel]))
        self.assertEqual(rules, ["spdx-header", "spdx-header", "todo"])

    def test_compat_is_exempt(self) -> None:
        self.assertEqual(self._cpp26("src/compat/include/frost/compat/probe.hpp"), [])

    def test_banned_constructs_clang_accepts_are_caught(self) -> None:
        # tests/toolchain_probe/banned shows the constructs clang 23 compiles without
        # complaint; outside the exempt directory, the cpp26 rule must reject each.
        expected = {"expansion_statement.cpp": ["expansion statements (template for) are banned (PLAN.md 2.1)"],
                    "named_module.cpp": ["named modules (export module) are banned (PLAN.md 2.1)"]}
        for name, messages in expected.items():
            rel = f"src/core/{name}"
            _write(self.tmp / rel, (REPO / "tests" / "toolchain_probe" / "banned" / name).read_text(encoding="utf-8"))
            found = [v.message for v in frost_lint.lint(self.tmp, [rel])]
            self.assertEqual(found, messages, name)

    def test_conservative_contract_detection(self) -> None:
        rel = "src/core/calls.cpp"
        _write(self.tmp / rel, "// SPDX-License-Identifier: Apache-2.0\n// Calls named pre and post.\n"
                               "void a(bool r, int t) { if (r) post(t); while (r) pre(t); }\n"
                               "void b(int t) { switch (t) { case 1: post(t); } return; }\n"
                               "#define FORWARD(x) post(x)\n"
                               "int c(int x) const noexcept(true) post(r: r > 0);\n")
        self.assertEqual([v.line for v in frost_lint.lint(self.tmp, [rel]) if v.rule == "cpp26"], [6])


class ExeSearchTest(TempDirTestCase):
    HEADER = "// SPDX-License-Identifier: Apache-2.0\n// Calls for the exe-search tests.\n"

    def _exe_search(self, rel: str, body: str) -> list[tuple[int, str]]:
        _write(self.tmp / rel, self.HEADER + body)
        return sorted((v.line, v.message.split(" ", 1)[0]) for v in frost_lint.lint(self.tmp, [rel])
                      if v.rule == "exe-search")

    def test_calls_that_search_for_executables_are_caught(self) -> None:
        body = ("void a() { SearchPathW(nullptr, L\"git.exe\", nullptr, 0, nullptr, nullptr); }\n"
                "int b() { return std::system(\"make\"); }\n"
                "int c() { return ::_wsystem(L\"make\"); }\n"
                "void d(const char* const* v) { execvp(v[0], v); _wspawnvp(0, L\"x\", nullptr); }\n"
                "void e() { popen (\"ls\", \"r\"); posix_spawnp(nullptr, \"x\", nullptr, nullptr, nullptr, nullptr); }\n")
        self.assertEqual(self._exe_search("src/core/spawn.cpp", body),
                         [(3, "SearchPathW"), (4, "system"), (5, "_wsystem"), (6, "_wspawnvp"), (6, "execvp"),
                          (7, "popen"), (7, "posix_spawnp")])

    def test_lookalikes_are_not_calls(self) -> None:
        body = ("wchar_t system[260]; // SearchPathW(nullptr, ...) in a comment\n"
                "const char* s = \"system(\\\"x\\\")\";\n"
                "auto& c = std::system_category(); runner.system(1); p->popen(2); Shell::execvp(3);\n"
                "void f() { mysystem(1); SearchPathWide(2); _wexecvpx(3); }\n")
        self.assertEqual(self._exe_search("src/core/lookalikes.cpp", body), [])

    def test_startup_test_is_exempt_from_exe_search_only(self) -> None:
        rel = "tests/app/startup_test.cpp"
        _write(self.tmp / rel, self.HEADER + "void a() { SearchPathW(nullptr, nullptr, nullptr, 0, nullptr, nullptr); "
                                             "CreateFileA(nullptr); }\n")
        self.assertEqual(sorted(v.rule for v in frost_lint.lint(self.tmp, [rel])), ["narrow-win32"])
        self.assertEqual(len(self._exe_search("tests/app/other_test.cpp", "void a() { SearchPathW(); }\n")), 1)


class RepositoryRulesTest(TempDirTestCase):
    def test_github_workflows_directory_is_banned(self) -> None:
        repo = self.copy_fixture("lint/clean")
        _write(repo / ".github" / "workflows" / "ci.yml",
               "# SPDX-License-Identifier: Apache-2.0\n# A workflow that must not exist here.\non: push\n")
        rules = {v.rule for v in frost_lint.lint(repo)}
        self.assertIn("no-ci", rules)

    def test_m0_is_closed_since_its_sign_off(self) -> None:
        # M0 was signed off on 2026-11-01; its markers must be finished or moved (PLAN.md 6.6).
        violations = frost_lint.check_todos("x.cpp", [frost_lint.Comment(2, "TODO(M0): leftover")])
        self.assertEqual([(v.line, v.rule) for v in violations], [(2, "todo")])
        self.assertIn("closed", violations[0].message)

    def test_closed_milestone_markers_fail(self) -> None:
        comments = [frost_lint.Comment(3, "TODO(M3): finish"), frost_lint.Comment(4, "TODO(M4): later")]
        with mock.patch.object(frost_lint, "CLOSED_MILESTONES", frozenset({3})):
            violations = frost_lint.check_todos("x.cpp", comments)
        self.assertEqual([(v.line, v.rule) for v in violations], [(3, "todo")])
        self.assertIn("closed", violations[0].message)

    def test_budget_references_need_budgets_json(self) -> None:
        rel = "src/a.cpp"
        _write(self.tmp / rel, "// SPDX-License-Identifier: Apache-2.0\n// Budgeted code.\n// Budget: B5.\n")
        messages = [v.message for v in frost_lint.lint(self.tmp, [rel]) if v.rule == "budget"]
        self.assertEqual(len(messages), 1)
        self.assertIn("missing", messages[0])

    def test_vendored_sources_need_only_reuse_coverage(self) -> None:
        # No header and a narrow API: fine under tree-sitter / the explicitly vendored test tool
        # once REUSE.toml covers them. The latter's hash and BSD-3-Clause are also checked by
        # licensecheck (ADR 0086).
        rel = "third_party/tree-sitter/lib/src/a.c"
        _write(self.tmp / rel, "int f(void) { return CreateFileA(0); }\n")
        found = [(v.path, v.rule) for v in frost_lint.lint(self.tmp, [rel])]
        self.assertEqual(found, [(rel, "reuse")])
        tool_rel = "tools/_vendor/fastjsonschema/draft04.py"
        _write(self.tmp / tool_rel, "# TODO: upstream byte exact\n")
        self.assertEqual([(v.path, v.rule) for v in frost_lint.lint(self.tmp, [tool_rel])], [(tool_rel, "reuse")])
        _write(self.tmp / "REUSE.toml", "# SPDX-License-Identifier: Apache-2.0\n#\n# REUSE annotations for the test.\n"
               'version = 1\n\n[[annotations]]\npath = "third_party/tree-sitter/**"\n'
               'SPDX-FileCopyrightText = "Copyright (c) 2018 Max Brunsfeld"\nSPDX-License-Identifier = "MIT"\n'
               '\n[[annotations]]\npath = "tools/_vendor/fastjsonschema/**"\n'
               'SPDX-FileCopyrightText = "Copyright (c) 2018, Michal Horejsek"\n'
               'SPDX-License-Identifier = "BSD-3-Clause"\n')
        self.assertEqual(frost_lint.lint(self.tmp, [rel, tool_rel, "REUSE.toml"]), [])
        # The same file outside the vendored directories gets every rule.
        _write(self.tmp / "src/a.c", "int f(void) { return CreateFileA(0); }\n")
        rules = {v.rule for v in frost_lint.lint(self.tmp, ["src/a.c"])}
        self.assertEqual(rules, {"spdx-header", "narrow-win32"})

    def test_sidecar_without_spdx_is_reported(self) -> None:
        _write(self.tmp / "logo.svg", "<svg/>\n")
        _write(self.tmp / "logo.svg.license", "no licence here\n")
        found = [(v.path, v.rule) for v in frost_lint.lint(self.tmp, ["logo.svg", "logo.svg.license"])]
        self.assertEqual(found, [("logo.svg.license", "reuse")])


class FileListingTest(TempDirTestCase):
    def _make_tree(self, repo) -> None:
        _write(repo / ".gitignore", "# Ignore rules for the listing tests.\n/build/\n*.log\n!keep.log\nnested/\n")
        _write(repo / "src" / ".gitignore", "generated.txt\n")
        for rel in ("build/out.txt", "a.log", "keep.log", "src/generated.txt", "src/real.txt",
                    "deep/nested/x.txt", "deep/visible.txt", "top.txt"):
            _write(repo / rel, "x\n")

    EXPECTED = [".gitignore", "deep/visible.txt", "keep.log", "src/.gitignore", "src/real.txt", "top.txt"]

    def test_walk_honours_gitignore(self) -> None:
        self._make_tree(self.tmp)
        self.assertEqual(sorted(frost_lint.list_repo_files(self.tmp)), self.EXPECTED)

    def test_git_listing_includes_untracked_and_skips_ignored_and_deleted(self) -> None:
        self._make_tree(self.tmp)
        git("init", "-q", cwd=self.tmp)
        _write(self.tmp / "gone.txt", "x\n")
        git("add", "top.txt", "gone.txt", cwd=self.tmp)
        (self.tmp / "gone.txt").unlink()
        self.assertEqual(sorted(frost_lint.list_repo_files(self.tmp)), self.EXPECTED)

    def test_git_mode_lints_untracked_files_but_not_ignored_ones(self) -> None:
        repo = self.copy_fixture("lint/clean")
        git("init", "-q", cwd=repo)
        _write(repo / ".gitignore", "# SPDX-License-Identifier: Apache-2.0\n# Ignore rules for the lint test.\nignored/\n")
        _write(repo / "ignored" / "bad.cpp", "int ignored;\n")
        _write(repo / "untracked" / "bad.cpp", "int untracked;\n")
        paths = {v.path for v in frost_lint.lint(repo)}
        self.assertEqual(paths, {"untracked/bad.cpp"})


class TruthRuleTest(TempDirTestCase):
    """The FND-5 rules on the seeded fixture tests/tools/fixtures/lint/truth (ADR 0078 decision 1)."""

    TRUTH_RULES = ("fabricated-result", "result-type-registry", "synthesized-output", "parity-claim")

    def _found(self, repo) -> list[tuple[str, int, str]]:
        return sorted((v.path, v.line, v.rule) for v in frost_lint.lint(repo) if v.rule in self.TRUTH_RULES)

    def test_seeded_fabrications_fail_and_honest_code_passes(self) -> None:
        repo = self.copy_fixture("lint/truth")
        expected = sorted(
            [("README.md", 3, "parity-claim"), ("README.md", 7, "parity-claim")]
            # TestResult braced, declared with an initializer, default-constructed, from make_shared;
            # the NuGet-like catalogue literal and a produced outcome enumerator; then (FND-5 review)
            # T x(...), a T(...) temporary, an implicit braced push_back({...}), emplace_back with
            # arguments, a C array, a static_cast to the outcome enum and emplace_back().
            + [("src/app/src/fake_runner.cpp", line, "fabricated-result")
               for line in (6, 11, 12, 14, 17, 19, 27, 28, 29, 30, 31, 32, 33)]
            # The last one is split across two adjacent literals.
            + [("src/app/src/fake_runner.cpp", line, "synthesized-output") for line in (21, 22, 23, 24, 35)]
            + [("src/app/src/fake_runner.hpp", 19, "result-type-registry")]
            + [("src/ui/shell/src/about.cpp", 7, "parity-claim"), ("src/ui/shell/src/about.cpp", 11, "parity-claim")]
        )
        self.assertEqual(self._found(repo), expected)

    def test_cli_fails_on_the_seeded_fixture(self) -> None:
        repo = self.copy_fixture("lint/truth")
        result = run_tool("frost_lint.py", "--repo", str(repo))
        self.assertEqual(result.returncode, 1)
        self.assertIn("src/app/src/fake_runner.cpp:6: fabricated-result: TestResult (test outcome, declaration)",
                      result.stdout)
        self.assertIn("src/app/src/fake_runner.cpp:21: synthesized-output: string literal imitates gtest output",
                      result.stdout)

    def test_messages_name_the_allowed_parsers(self) -> None:
        rel = "src/app/src/adapter.cpp"
        _write(self.tmp / rel, "// SPDX-License-Identifier: Apache-2.0\n// A diagnostic made up in the app.\n"
                               "void f() { LiveDiagnostic d{.message = \"made up\"}; }\n")
        messages = [v.message for v in frost_lint.lint(self.tmp, [rel]) if v.rule == "fabricated-result"]
        self.assertEqual(len(messages), 1)
        self.assertIn("allowed only in src/lang/src/diagnostics.cpp", messages[0])
        # The same construction in the allowlisted parser passes.
        _write(self.tmp / "src/lang/src/diagnostics.cpp", (self.tmp / rel).read_text(encoding="utf-8"))
        self.assertEqual([v for v in frost_lint.lint(self.tmp, ["src/lang/src/diagnostics.cpp"])
                          if v.rule == "fabricated-result"], [])

    def test_tests_and_fixtures_are_exempt(self) -> None:
        body = ("// SPDX-License-Identifier: Apache-2.0\n// A test double.\n"
                "TestResult r{.passed = true}; const char* o = \"[  PASSED  ] 1 test.\";\n")
        for rel in ("tests/app/double_test.cpp", "tests/tools/fixtures/x/src/a.cpp"):
            _write(self.tmp / rel, body)
            self.assertEqual([v for v in frost_lint.lint(self.tmp, [rel]) if v.rule in self.TRUTH_RULES], [], rel)
        _write(self.tmp / "src/core/a.cpp", body)
        rules = sorted(v.rule for v in frost_lint.lint(self.tmp, ["src/core/a.cpp"]) if v.rule in self.TRUTH_RULES)
        self.assertEqual(rules, ["fabricated-result", "synthesized-output"])

    def test_csharp_strings_are_checked_too(self) -> None:
        rel = "integrations/unity/Editor/Fake.cs"
        _write(self.tmp / rel, "// SPDX-License-Identifier: Apache-2.0\n// Seeded tool output in C#.\n"
                               "class F { string s = @\"[xUnit.net 00:00:00.01] [PASS]\"; }\n")
        self.assertEqual([(v.line, v.rule) for v in frost_lint.lint(self.tmp, [rel])], [(3, "synthesized-output")])

    def test_site_allowances_cover_exact_sites_only(self) -> None:
        repo = self.copy_fixture("lint/truth")
        rel = "src/app/src/fake_runner.cpp"
        allowance = frost_lint.SiteAllowance(rel, "TestResult", "out.push_back(TestResult{.name = \"a\", .passed = false});",
                                             "test", context="TestResult second;")
        with mock.patch.object(frost_lint, "SITE_ALLOWANCES", (allowance,)):
            lines = [v.line for v in frost_lint.lint(repo, [rel]) if v.rule == "fabricated-result"]
        self.assertEqual(lines, [6, 12, 14, 17, 19, 27, 28, 29, 30, 31, 32, 33])
        # A context that is not there does not match, and the unused entry is reported as stale.
        wrong = frost_lint.SiteAllowance(rel, "TestResult", allowance.line, "test", context="never")
        with mock.patch.object(frost_lint, "SITE_ALLOWANCES", (wrong,)):
            found = [(v.line, v.message.split(" ", 2)[1]) for v in frost_lint.lint(repo, [rel])
                     if v.rule == "fabricated-result"]
        self.assertIn((11, "(test"), found)
        self.assertIn((1, "site"), found)

    def test_an_allowance_covers_exactly_its_count_of_identical_sites(self) -> None:
        # FND-5 review: one reviewed entry must not silently cover a new copy of the same line.
        rel = "src/app/src/runner.cpp"
        site = "    TestResult entry;\n    entry.name = \"x\";\n"
        _write(self.tmp / rel, "// SPDX-License-Identifier: Apache-2.0\n// Identical result sites.\n"
                               "void f() {\n" + site * 3 + "}\n")
        sites = [4, 6, 8]

        def run(count: int) -> list[tuple[int, str]]:
            entry = frost_lint.SiteAllowance(rel, "TestResult", "TestResult entry;", "test", count=count)
            with mock.patch.object(frost_lint, "SITE_ALLOWANCES", (entry,)):
                return [(v.line, v.message) for v in frost_lint.lint(self.tmp, [rel]) if v.rule == "fabricated-result"]

        self.assertEqual(run(3), [])
        two = run(2)
        self.assertEqual([line for line, _ in two], [sites[2]])
        self.assertIn("covers 2 finding(s) and they are all taken", two[0][1])
        # A count above the sites is stale too, so a removed copy cannot leave a loose entry behind.
        four = run(4)
        self.assertEqual([line for line, _ in four], [1])
        self.assertIn("only 3 of its 4 findings match", four[0][1])
        self.assertEqual(frost_lint.SiteAllowance(rel, "T", "x", "y").count, 1, "one site unless stated")

    def test_one_finding_per_line_and_type(self) -> None:
        rel = "src/app/src/one.cpp"
        # A braced declaration that is also a C array and pushed into a container of results.
        _write(self.tmp / rel, "// SPDX-License-Identifier: Apache-2.0\n// One line, several forms.\n"
                               "std::vector<TestResult> rows; void f() { TestResult a[1] = {TestResult{}}; "
                               "rows.push_back({}); }\n")
        found = [(v.line, v.rule) for v in frost_lint.lint(self.tmp, [rel]) if v.rule == "fabricated-result"]
        self.assertEqual(found, [(3, "fabricated-result")])

    def test_construction_forms_and_their_look_alikes(self) -> None:
        cases = {
            # Constructions.
            "void f() { TestResult r(name, true); }": True,
            "void f() { show(TestResult(\"a\", 1)); }": True,
            "void f() { auto r = TestResult(); }": True,
            "void f(std::vector<TestResult>& out) { out.push_back({.name = n}); }": True,
            "void f(QList<TestResult>& out) { out.emplace_back(n, true); }": True,
            "void f(std::vector<TestResult>& out) { out.emplace_back(); }": True,
            "void f(std::vector<TestResult>& out) { out.insert(out.end(), {n, true}); }": True,
            "void f(std::vector<TestResult>& out) { out = {{n, true}}; }": True,
            "TestResult kAll[] = {{\"a\", true}};": True,
            "void f(int n) { auto s = static_cast<UnitTestRunState>(n); }": True,
            "void f(int n) { auto s = std::bit_cast<TestResult>(n); }": True,
            # Look-alikes that construct nothing.
            "TestResult parse(std::string_view line);": False,
            "TestResult parse(std::string_view);": False,
            "TestResult parse(const Json& reply) const;": False,
            "TestResult make();": False,
            "TestResult run(Runner& r) { return r.next(); }": False,
            "std::function<TestResult()> later;": False,
            "using Factory = TestResult (*)(int);": False,
            "void f(std::vector<TestResult>& out, TestResult r) { out.push_back(r); out.emplace_back(std::move(r)); }":
                False,
            "void f(const Base& b) { const auto& r = static_cast<const TestResult&>(b); }": False,
            "struct TestResult { TestResult(); explicit TestResult(int code); ~TestResult(); };": False,
            "TestResult::TestResult() = default;": False,
            "void f(std::vector<TestResult>& all) { Config c{.all = {1}}; }": False,
        }
        for body, construction in cases.items():
            with self.subTest(body=body):
                rel = "src/app/src/case.cpp"
                _write(self.tmp / rel, "// SPDX-License-Identifier: Apache-2.0\n// A construction case.\n" + body + "\n")
                found = [v for v in frost_lint.lint(self.tmp, [rel]) if v.rule == "fabricated-result"]
                self.assertEqual(bool(found), construction, [v.message for v in found])

    def test_tool_output_split_across_adjacent_literals_is_found(self) -> None:
        rel = "src/app/src/split.cpp"
        # Split three ways, over two lines, and a marker made of two literals that are not adjacent.
        _write(self.tmp / rel, "// SPDX-License-Identifier: Apache-2.0\n// Split markers.\n"
                               "const char* a = \"[  PAS\" \"SED  \"\n"
                               "                \"] 3 tests.\";\n"
                               "const char* b = \"[ RUN\"; const char* c = \"      ]\";\n"
                               "const wchar_t* d = L\"tests fail\" L\"ed out of 3\";\n")
        found = [(v.line, v.rule, "split" in v.message) for v in frost_lint.lint(self.tmp, [rel])
                 if v.rule == "synthesized-output"]
        self.assertEqual(found, [(3, "synthesized-output", True), (6, "synthesized-output", True)])

    def test_stale_allowances_are_reported_only_for_linted_files(self) -> None:
        entry = frost_lint.SiteAllowance("src/gone.cpp", "TestResult", "TestResult r{};", "FND-3 harm, removed at integration")
        _write(self.tmp / "src/gone.cpp", "// SPDX-License-Identifier: Apache-2.0\n// The harm was removed here.\n")
        with mock.patch.object(frost_lint, "SITE_ALLOWANCES", (entry,)):
            stale = [v for v in frost_lint.lint(self.tmp, ["src/gone.cpp"]) if v.rule == "fabricated-result"]
            self.assertEqual(len(stale), 1)
            self.assertIn("stale site allowance", stale[0].message)
            self.assertEqual(frost_lint.lint(self.tmp, []), [])

    def test_repository_allowances_all_match(self) -> None:
        # Every entry of the real table covers exactly its count of sites in this checkout (no stale
        # entries, no extra copies), and each temporary FND-3 entry names its harm tag so the
        # integrator can find them.
        files = sorted({e.path for e in frost_lint.SITE_ALLOWANCES})
        used: dict[int, int] = {}
        for rel in files:
            self.assertTrue((REPO / rel).is_file(), rel)
            remaining = [v for v in frost_lint.lint_file(REPO, rel, None, used) if v.rule == "fabricated-result"]
            self.assertEqual(remaining, [], rel)
        self.assertEqual(frost_lint.stale_site_allowances(set(files), used), [])
        self.assertEqual(used, {i: e.count for i, e in enumerate(frost_lint.SITE_ALLOWANCES)})
        for entry in frost_lint.SITE_ALLOWANCES:
            self.assertTrue(entry.reason)

    def test_parity_claims_in_documents(self) -> None:
        cases = {
            "FrostIDE is a complete replacement for Rider.": True,
            "A drop-in replacement for CLion.": True,
            "Full Visual Studio replacement": True,
            "1:1 JetBrains Rider parity": True,
            "Rider 1:1 backlog": True,
            "complete Rider feature parity": True,
            "It is feature-complete.": True,
            "Rider parity is a goal; 1 of 191 matrix rows is green.": False,
            "Independently implemented Rider workflow parity is the target, not a claim.": False,
            "A 1:1 aspect ratio.": False,
        }
        for line, claim in cases.items():
            with self.subTest(line=line):
                found = frost_lint.check_parity_claims_in_document("README.md", line)
                self.assertEqual(bool(found), claim)


class DevPathTest(TempDirTestCase):
    """The dev-path rule (FND-10): no developer machine path in src/ or tests/ sources."""

    # Assembled here so this test file itself names no such path.
    E, D = "E" + ":", "D" + ":"

    def _rules(self, rel: str, body: str) -> list[int]:
        _write(self.tmp / rel, "// SPDX-License-Identifier: Apache-2.0\n// Paths in code.\n" + body + "\n")
        return [v.line for v in frost_lint.lint(self.tmp, [rel]) if v.rule == "dev-path"]

    def test_machine_paths_fail_in_code_strings_and_comments(self) -> None:
        cases = [
            f'const char* sdk = "{self.E}\\\\sdk";',            # an escaped C++ string
            f"// installed under {self.E}\\sdk\\llvm-23.1.4",   # a comment
            f'auto out = "{self.E}/FrostBuilds/corpora";',      # forward slashes
            f'auto engine = "{self.D}\\\\dev\\\\FrostEngine-AI-Claude";',
            'auto mount = "/mnt/' + 'e/sdk/lldb-23.1.4";',     # the WSL spelling
            f'auto lower = "{self.E.lower()}\\\\SDK";',
        ]
        for body in cases:
            with self.subTest(body=body):
                self.assertEqual(self._rules("src/lang/src/a.cpp", body), [3])
                self.assertEqual(self._rules("tests/lang/a_test.cpp", body), [3])

    def test_other_paths_and_places_pass(self) -> None:
        for body in ('auto made_up = "C:\\\\sdk";',            # a MemFs root in a unit test
                     f'auto other = "{self.E}\\\\sdkx";',       # a longer name
                     'auto env = std::getenv("FROST_SDK");',
                     'auto home = "~/sdk/env.sh";'):
            with self.subTest(body=body):
                self.assertEqual(self._rules("src/lang/src/b.cpp", body), [])
        body = f'auto sdk = "{self.E}\\\\sdk";'
        # tools/ may name the bootstrap defaults; fixtures are data.
        self.assertEqual(self._rules("tools/c.py", "# " + body), [])
        _write(self.tmp / "tests/x/fixtures/db.json", body)
        self.assertEqual([v for v in frost_lint.lint(self.tmp, ["tests/x/fixtures/db.json"]) if v.rule == "dev-path"],
                         [])


class ToolSkipTest(TempDirTestCase):
    """The tool-skip rule (FND-6): a missing pinned tool fails a test, it never skips it."""

    SKIP = "self." + "skipTest"  # Assembled, so this file's own skips are what the rule reads.

    def _lines(self, rel: str, body: str) -> list[int]:
        comment = "#" if rel.endswith(".py") else "//"
        _write(self.tmp / rel, f"{comment} SPDX-License-Identifier: Apache-2.0\n{comment} Skips.\n" + body + "\n")
        return [v.line for v in frost_lint.lint(self.tmp, [rel]) if v.rule == "tool-skip"]

    def test_skips_naming_a_pinned_tool_fail(self) -> None:
        for body in ('if (!found)\n    GTEST_SKIP() << "clangd is unavailable";',
                     'GTEST_SKIP() << "the pinned lldb-dap is not installed at \'" << path\n'
                     '             << "\'; build it";',
                     'GTEST_SKIP() << "git is not installed";',
                     'QSKIP("needs the pinned LuaLS");'):
            with self.subTest(body=body):
                self.assertEqual(self._lines("tests/lang/a_test.cpp", body), [3 if body.startswith("GTEST") or
                                                                              body.startswith("QSKIP") else 4])
        self.assertEqual(self._lines("tests/tools/test_a.py", f'{self.SKIP}("ninja not available")'), [3])
        self.assertEqual(self._lines("tests/tools/test_b.py",
                                     '@unittest.skipUnless(shutil.which("cmake"),\n    "cmake is not on PATH")'),
                         [3])

    def test_environment_skips_and_other_places_pass(self) -> None:
        for body in ('GTEST_SKIP() << "directory symbolic links unavailable: " << error.message();',
                     'GTEST_SKIP() << "MSBuild evaluation is Windows-only";',
                     'GTEST_SKIP() << "no Direct3D 11 device";',
                     '// GTEST_SKIP() << "clangd is unavailable"; would be wrong here',
                     'ASSERT_TRUE(found) << "the pinned clangd is required";'):
            with self.subTest(body=body):
                self.assertEqual(self._lines("tests/lang/b_test.cpp", body), [])
        self.assertEqual(self._lines("tests/tools/test_c.py",
                                     '@unittest.skipIf(sys.platform == "win32", "bash and build-lldb.sh run in WSL")'),
                         [])
        # Only tests/** is checked: the product itself may say "clangd is unavailable".
        self.assertEqual(self._lines("src/lang/src/c.cpp", 'GTEST_SKIP() << "clangd is unavailable";'), [])


class SkipAuditTest(TempDirTestCase):
    """frost_lint.py skips (FND-6): the skipped tests of a ctest log and their reasons."""

    @staticmethod
    def _block(index: int, name: str, output: str, reason: str) -> str:
        return (f"{index}/9 Testing: {name}\n{index}/9 Test: {name}\nCommand: \"x.exe\"\nOutput:\n"
                f"----------------------------------------------------------\n{output}<end of output>\n"
                f"Test time =   0.02 sec\n----------------------------------------------------------\n"
                f"{reason}\"{name}\" end time: Nov 07 20:21\n----------------------------------------------------------\n\n")

    def _log(self, *blocks: str) -> str:
        path = self.tmp / "LastTest.log"
        path.write_text("Start testing\n" + "".join(blocks), encoding="utf-8", newline="\r\n")
        return str(path)

    GTEST_SKIPPED = ("[ RUN      ] A.B\nE:\\x\\a_test.cpp(148): Skipped\n{reason}\n\n[  SKIPPED ] A.B (4 ms)\n"
                     "[  SKIPPED ] 1 test, listed below:\n[  SKIPPED ] A.B\n")
    BY_REGEX = "Test Pass Reason:\nSkip regular expression found in output. Regex=[\\[  SKIPPED \\]]\n"

    def test_reasons_are_listed_and_environment_skips_pass(self) -> None:
        log = self._log(
            self._block(1, "A.B", self.GTEST_SKIPPED.format(reason="symbolic links unavailable: no privilege"),
                        self.BY_REGEX),
            self._block(2, "A.Passed", "[       OK ] A.Passed (1 ms)\n", "Test Passed.\n"),
            self._block(3, "tools.x", "test_y (test_x.T.test_y) ... skipped 'Windows PowerShell 5.1'\n",
                        "Test Pass Reason:\nSkip return code found. Return code=77\n"))
        result = run_tool("frost_lint.py", "skips", "--log", log)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        lines = result.stdout.splitlines()
        self.assertIn("skipped (environment): A.B: symbolic links unavailable: no privilege", lines)
        self.assertIn("skipped (environment): tools.x: Windows PowerShell 5.1", lines)
        self.assertEqual(lines[-1], "skips ok (2 skipped, none for a missing pinned tool)")

    def test_a_skip_for_a_missing_pinned_tool_fails(self) -> None:
        log = self._log(self._block(1, "A.B", self.GTEST_SKIPPED.format(reason="clangd is unavailable"),
                                    self.BY_REGEX))
        result = run_tool("frost_lint.py", "skips", "--log", log)
        self.assertEqual(result.returncode, 1)
        self.assertIn("skipped (MISSING TOOL): A.B: clangd is unavailable", result.stdout)
        self.assertEqual(result.stdout.splitlines()[-1], "skips FAILED (1 of 1 skipped for a missing pinned tool)")

    def test_an_unreadable_log_is_not_run(self) -> None:
        result = run_tool("frost_lint.py", "skips", "--log", str(self.tmp / "missing.log"))
        self.assertEqual(result.returncode, 0)
        self.assertTrue(result.stdout.splitlines()[-1].startswith("skips not-run (cannot read "))


class RatchetTest(TempDirTestCase):
    """The source-to-test line ratchet (ADR 0078 decision 6) on scratch repositories."""

    def setUp(self) -> None:
        super().setUp()
        git("init", "-q", cwd=self.tmp)
        _write(self.tmp / "src/core/base.cpp", "// base\n" * 50)
        git("add", "-A", cwd=self.tmp)
        git("commit", "-q", "-m", "base", cwd=self.tmp)
        git("switch", "-q", "-c", "slice", cwd=self.tmp)

    def _ratchet(self, *args: str) -> subprocess.CompletedProcess:
        return run_tool("frost_lint.py", "ratchet", "--repo", str(self.tmp), "--base", "main", *args)

    def test_a_thousand_source_lines_with_ten_test_lines_fail(self) -> None:
        _write(self.tmp / "src/app/big.cpp", "int x;\n" * 1000)
        _write(self.tmp / "tests/app/big_test.cpp", "TEST(A, B) {}\n" * 10)
        git("add", "-A", cwd=self.tmp)
        git("commit", "-q", "-m", "big", cwd=self.tmp)
        result = self._ratchet()
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertEqual(result.stdout.splitlines()[-1],
                         "ratchet FAILED (src +1000 net, tests +10; 1000 source lines need 200 test lines, 20 per 100)")
        # Enough tests, committed or not, pass it.
        _write(self.tmp / "tests/app/more_test.cpp", "TEST(A, C) {}\n" * 190)
        result = self._ratchet()
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual(result.stdout.splitlines()[-1], "ratchet ok (src +1000 net, tests +200)")

    def test_fixtures_docs_and_moves_do_not_count(self) -> None:
        _write(self.tmp / "src/app/big.cpp", "int x;\n" * 600)
        _write(self.tmp / "tests/app/fixtures/data.cpp", "int y;\n" * 600)
        _write(self.tmp / "docs/notes.md", "words\n" * 600)
        result = self._ratchet()
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("src +600 net, tests +0", result.stdout)
        # Moving code is net zero: delete the base file's 50 lines and add them elsewhere.
        (self.tmp / "src/app/big.cpp").unlink()
        (self.tmp / "src/core/base.cpp").unlink()
        _write(self.tmp / "src/core/moved.cpp", "// base\n" * 50)
        result = self._ratchet()
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("ratchet ok (src +0 net, tests +0)", result.stdout)

    def test_small_slices_are_under_the_floor(self) -> None:
        _write(self.tmp / "src/app/small.cpp", "int x;\n" * (frost_lint.RATCHET_MIN_SOURCE_LINES - 1))
        result = self._ratchet()
        self.assertEqual(result.returncode, 0, result.stdout)

    def test_an_unknown_base_is_not_run(self) -> None:
        result = run_tool("frost_lint.py", "ratchet", "--repo", str(self.tmp), "--base", "nosuch")
        self.assertEqual(result.returncode, 0)
        self.assertTrue(result.stdout.splitlines()[-1].startswith("ratchet not-run (the base 'nosuch' does not resolve"))

    def test_verdict_thresholds(self) -> None:
        verdict = frost_lint.ratchet_verdict
        self.assertTrue(verdict(frost_lint.RatchetCounts(9372, 0, 1875))[0])
        self.assertFalse(verdict(frost_lint.RatchetCounts(9372, 0, 196))[0])  # commit 1c29790
        self.assertTrue(verdict(frost_lint.RatchetCounts(5000, 4900, 0))[0])  # a reformat or a split


class GlobTest(unittest.TestCase):
    def test_reuse_globs(self) -> None:
        cases = {
            "**/*.json": (["vcpkg.json", "a/b/c.json"], ["a/b.jsonx"]),
            "*.md": (["README.md"], ["docs/x.md"]),
            "tests/**/fixtures/**": (["tests/fixtures/a", "tests/x/y/fixtures/z/w"], ["tests/fixture/a"]),
            "resources/**/*.png": (["resources/a.png", "resources/x/a.png"], ["other/a.png"]),
            "LICENSE": (["LICENSE"], ["LICENSES/MIT.txt", "a/LICENSE"]),
            "a\\*b": (["a*b"], ["axb"]),
            "dir/**": (["dir/a", "dir/a/b"], ["dir", "dirx/a"]),
        }
        for pattern, (matching, other) in cases.items():
            regex = frost_lint.compile_globs([pattern])
            for path in matching:
                self.assertTrue(regex.match(path), f"{pattern} should match {path}")
            for path in other:
                self.assertFalse(regex.match(path), f"{pattern} should not match {path}")

    def test_gitignore_patterns(self) -> None:
        rules = frost_lint.parse_gitignore("", "*.pyc\n/build/\ndoc/*.txt\n**/cache\n!important.pyc\n")
        self.assertTrue(frost_lint.is_ignored("a/b/x.pyc", False, rules))
        self.assertFalse(frost_lint.is_ignored("important.pyc", False, rules))
        self.assertTrue(frost_lint.is_ignored("build", True, rules))
        self.assertFalse(frost_lint.is_ignored("src/build", True, rules))
        self.assertFalse(frost_lint.is_ignored("build", False, rules))
        self.assertTrue(frost_lint.is_ignored("doc/a.txt", False, rules))
        self.assertFalse(frost_lint.is_ignored("doc/sub/a.txt", False, rules))
        self.assertTrue(frost_lint.is_ignored("x/y/cache", True, rules))

    def test_source_table(self) -> None:
        self.assertIs(frost_lint.source_syntax("src/core/include/frost/core/build_info.hpp.in"), frost_lint.CPP)
        self.assertIs(frost_lint.source_syntax("cmake/windows-frost.manifest.in"), frost_lint.XML)
        self.assertIs(frost_lint.source_syntax("src/app/resources/windows/frost.rc.in"), frost_lint.RC)
        self.assertIs(frost_lint.source_syntax("a/CMakeLists.txt"), frost_lint.CMAKE)
        self.assertIs(frost_lint.source_syntax(".clang-format"), frost_lint.YAML)
        self.assertIs(frost_lint.source_syntax(".gitignore"), frost_lint.GIT_PATTERNS)
        self.assertIs(frost_lint.source_syntax("src/.gitattributes"), frost_lint.GIT_PATTERNS)
        self.assertIsNone(frost_lint.source_syntax("README.md"))
        self.assertIsNone(frost_lint.source_syntax("LICENSE"))
        self.assertIsNone(frost_lint.source_syntax("notes.txt"))


class LexerTest(unittest.TestCase):
    def _comments(self, text: str, syntax) -> list[tuple[int, str]]:
        return [(c.line, c.text) for c in frost_lint.scan(text, syntax).comments]

    def test_c_family(self) -> None:
        text = 'int a = 1\'000; // one\nconst char* s = "/* no */"; /* two\n * three */\nauto r = R"x(// no)x";\n'
        self.assertEqual(self._comments(text, frost_lint.CPP), [(1, "one"), (2, "two"), (3, "three")])
        code = frost_lint.scan(text, frost_lint.CPP).code
        self.assertNotIn("no", code)
        self.assertEqual(code.count("\n"), text.count("\n"))

    def test_python_triple_quotes(self) -> None:
        text = 'x = """\n# not\n"""  # yes\ny = "#no"\n'
        self.assertEqual(self._comments(text, frost_lint.PYTHON), [(3, "yes")])

    def test_single_quote_escapes_depend_on_the_language(self) -> None:
        text = "path = 'C:\\dir\\' # yes\n"
        for syntax in (frost_lint.TOML, frost_lint.YAML, frost_lint.SHELL, frost_lint.POWERSHELL):
            self.assertEqual(self._comments(text, syntax), [(1, "yes")], syntax.name)
        self.assertEqual(self._comments(text, frost_lint.PYTHON), [])

    def test_shell_hash_rules(self) -> None:
        text = 'n=${#a[@]} # yes\necho $# "#no" \'#no\'\n# also\n'
        self.assertEqual(self._comments(text, frost_lint.SHELL), [(1, "yes"), (3, "also")])

    def test_cmake_brackets(self) -> None:
        text = 'set(a [[ # no ]])\n#[[ block\ncomment ]]\nmessage("#no") # yes\n'
        self.assertEqual(self._comments(text, frost_lint.CMAKE), [(2, "block"), (3, "comment"), (4, "yes")])

    def test_powershell(self) -> None:
        text = "<# one\ntwo #>\n$a = @\"\n# no\n\"@\n$b = '#no' # yes\n"
        self.assertEqual(self._comments(text, frost_lint.POWERSHELL), [(1, "one"), (2, "two"), (6, "yes")])

    def test_tree_sitter_query(self) -> None:
        text = '; one\n((identifier) @x (#eq? @x "; no")) ; two\n"#no" @y\n'
        self.assertEqual(self._comments(text, frost_lint.TREE_SITTER_QUERY), [(1, "one"), (2, "two")])
        self.assertIs(frost_lint.source_syntax("src/syntax/queries/c/highlights.scm"), frost_lint.TREE_SITTER_QUERY)

    def test_xml(self) -> None:
        text = '<?xml version="1.0"?>\n<!-- one\n  two -->\n<a/>\n'
        self.assertEqual(self._comments(text, frost_lint.XML), [(2, "one"), (3, "two")])


if __name__ == "__main__":
    unittest.main()
