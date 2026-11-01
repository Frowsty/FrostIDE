# SPDX-License-Identifier: Apache-2.0
#
# Tests for tools/frost_lint.py (frost-lint v0): the clean and dirty fixtures under
# tests/tools/fixtures/lint, the C++26 exemptions, the no-CI rule, file enumeration with
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

    def test_xml(self) -> None:
        text = '<?xml version="1.0"?>\n<!-- one\n  two -->\n<a/>\n'
        self.assertEqual(self._comments(text, frost_lint.XML), [(2, "one"), (3, "two")])


if __name__ == "__main__":
    unittest.main()
