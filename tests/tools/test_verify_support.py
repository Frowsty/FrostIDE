# SPDX-License-Identifier: Apache-2.0
#
# Tests for tools/verify_support.py (ADR 0027): the changed-file list over real git repositories in
# temporary directories (the repository's own index is never written), the TSan module selection
# (a table, properties, and an oracle built from the frost:: DEPS in the CMake files), the drvfs PATH
# filter, the compile-database and `ninja -t deps` readers over fixtures shaped like the real Windows
# and Linux trees, tidy planning, finding parsing, the --header-filter regex, the parallel runner with
# a fake clang-tidy, and finally the pinned clang-tidy itself: the checked-in .clang-tidy files load,
# name only existing checks, give every exclusion a reason line, and a runtime fixture reports exactly
# the findings it should. Under ctest, it also checks that every test of its own build tree carries
# exactly one mod.* label.

from __future__ import annotations

import json
import os
import random
import re
import shutil
import subprocess
import sys
import textwrap
import time
import unittest
from pathlib import Path

from tools_test_support import FIXTURES, REPO, TempDirTestCase, git, run_tool

import verify_support as vs

VERIFY = FIXTURES / "verify"
WINDOWS = sys.platform == "win32"


def _clang_tidy() -> str | None:
    """The pinned clang-tidy (FROST_LLVM_ROOT, set by env.ps1/env.sh), else one on PATH, else None."""
    root = os.environ.get("FROST_LLVM_ROOT")
    if root:
        exe = Path(root) / "bin" / ("clang-tidy.exe" if WINDOWS else "clang-tidy")
        if exe.is_file():
            return str(exe)
    found = shutil.which("clang-tidy")
    return os.path.abspath(found) if found else None


def _require(tool: str | None, name: str) -> str:
    """`tool`, or a skip when the SDK is not set up at all, or a failure when it is but `tool` is missing."""
    if tool:
        return tool
    if os.environ.get("FROST_LLVM_ROOT") or os.environ.get("FROST_SDK"):
        raise AssertionError(f"{name} is missing although the FrostIDE SDK environment is set")
    raise unittest.SkipTest(f"{name} not available (no FrostIDE SDK environment)")


def _fixture_text(name: str, root: Path, outside: Path | None = None) -> str:
    """A verify fixture with @ROOT@/@ROOTW@ (and @OUTSIDE@/@OUTSIDEW@) replaced by `root` (and `outside`)."""
    text = (VERIFY / name).read_text(encoding="utf-8")
    pairs = [("@ROOTW@", root), ("@ROOT@", root)]
    if outside is not None:
        pairs = [("@OUTSIDEW@", outside), ("@OUTSIDE@", outside)] + pairs
    for marker, path in pairs:
        if marker.endswith("W@"):
            # Backslash form inside a JSON string: each backslash escaped.
            text = text.replace(marker, json.dumps(str(path).replace("/", "\\"))[1:-1])
        else:
            text = text.replace(marker, str(path).replace("\\", "/"))
    return text


# ---------------------------------------------------------------------------------------------
# Changed files
# ---------------------------------------------------------------------------------------------


class ChangedFilesTest(TempDirTestCase):
    """changed_files and the `changed` command over real repositories."""

    def setUp(self) -> None:
        super().setUp()
        self.repo = self.tmp / "repo"
        self.repo.mkdir()
        git("init", "-q", cwd=self.repo)
        git("config", "core.autocrlf", "false", cwd=self.repo)
        self.write("README.md", "readme\n")
        self.write("src/core/a.cpp", "int a;\n")
        self.write("src/core/gone.cpp", "int g;\n")
        self.write("src/core/moved.cpp", "int m;\n")
        self.write(".gitignore", "ignored/\n*.tmp\n")
        git("add", "-A", cwd=self.repo)
        git("commit", "-q", "-m", "base", cwd=self.repo)
        git("checkout", "-q", "-b", "feature", cwd=self.repo)

    def write(self, rel: str, text: str) -> Path:
        path = self.repo / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(text.encode("utf-8"))
        return path

    def changed(self, base: str = "main", **kwargs) -> list[str]:
        return vs.changed_files(vs.GitContext(self.repo, **kwargs), base)

    def test_lists_committed_staged_unstaged_and_untracked_changes_but_not_deletions(self):
        self.write("src/core/committed.cpp", "int c;\n")
        git("add", "src/core/committed.cpp", cwd=self.repo)
        git("commit", "-q", "-m", "c", cwd=self.repo)
        self.write("src/core/staged.hpp", "#pragma once\n")
        git("add", "src/core/staged.hpp", cwd=self.repo)
        self.write("src/core/a.cpp", "int a2;\n")
        self.write("docs/untracked.md", "new\n")
        self.write("ignored/x.cpp", "int i;\n")
        self.write("src/core/scratch.tmp", "t\n")
        git("rm", "-q", "src/core/gone.cpp", cwd=self.repo)
        git("mv", "src/core/moved.cpp", "src/core/renamed.cpp", cwd=self.repo)
        self.assertEqual(self.changed(), ["docs/untracked.md", "src/core/a.cpp", "src/core/committed.cpp",
                                          "src/core/renamed.cpp", "src/core/staged.hpp"])

    def test_nothing_changed_gives_an_empty_list(self):
        self.assertEqual(self.changed(), [])

    def test_a_file_changed_back_to_the_merge_base_is_not_listed(self):
        # Changed in a branch commit, then back in the working tree; and an edit undone before any commit.
        self.write("src/core/a.cpp", "int a2;\n")
        git("commit", "-q", "-am", "edit", cwd=self.repo)
        self.write("src/core/a.cpp", "int a;\n")
        self.write("README.md", "changed\n")
        self.write("README.md", "readme\n")
        self.assertEqual(self.changed(), [])

    def test_uses_the_merge_base_not_the_tip_of_base(self):
        self.write("src/core/feature.cpp", "int f;\n")
        git("add", "-A", cwd=self.repo)
        git("commit", "-q", "-m", "f", cwd=self.repo)
        git("checkout", "-q", "main", cwd=self.repo)
        self.write("src/core/main_only.cpp", "int m;\n")
        git("add", "-A", cwd=self.repo)
        git("commit", "-q", "-m", "m", cwd=self.repo)
        git("checkout", "-q", "feature", cwd=self.repo)
        # main moved on; its new file is not a change of this branch.
        self.assertEqual(self.changed(), ["src/core/feature.cpp"])

    def test_base_head_lists_only_the_working_tree_changes(self):
        self.write("src/core/feature.cpp", "int f;\n")
        git("add", "-A", cwd=self.repo)
        git("commit", "-q", "-m", "f", cwd=self.repo)
        self.write("src/core/a.cpp", "int a3;\n")
        self.assertEqual(self.changed("HEAD"), ["src/core/a.cpp"])

    def test_falls_back_to_origin_base(self):
        main = git("rev-parse", "main", cwd=self.repo).strip()
        git("update-ref", "refs/remotes/origin/trunk", main, cwd=self.repo)
        self.write("src/core/a.cpp", "int a4;\n")
        self.assertEqual(self.changed("trunk"), ["src/core/a.cpp"])

    def test_unresolvable_base_and_missing_merge_base_are_errors(self):
        with self.assertRaisesRegex(vs.VerifyError, "does not resolve"):
            self.changed("no-such-branch")
        git("checkout", "-q", "--orphan", "orphan", cwd=self.repo)
        git("commit", "-q", "-m", "orphan", cwd=self.repo)
        with self.assertRaisesRegex(vs.VerifyError, "no merge base"):
            self.changed()

    def test_the_repository_index_keeps_its_bytes_and_mtime(self):
        # A tracked file with new stat data but the same contents is "stat-dirty": git diff would refresh
        # the index entry and, given the chance, write the index back.
        index = self.repo / ".git" / "index"
        path = self.repo / "src" / "core" / "a.cpp"
        future = time.time() + 120
        os.utime(path, (future, future))
        old = index.stat().st_mtime_ns
        os.utime(index, ns=(old - 10**9, old - 10**9))
        before_bytes, before_mtime = index.read_bytes(), index.stat().st_mtime_ns
        self.assertEqual(self.changed(), [])
        self.assertEqual(index.read_bytes(), before_bytes)
        self.assertEqual(index.stat().st_mtime_ns, before_mtime)

    def test_explicit_git_dir_and_work_tree_reach_a_worktree(self):
        worktree = self.tmp / "wt"
        git("worktree", "add", "-q", "-b", "wt-branch", str(worktree), cwd=self.repo)
        (worktree / "src" / "core" / "a.cpp").write_bytes(b"int wt;\n")
        git_dir = Path(git("rev-parse", "--absolute-git-dir", cwd=worktree).strip())
        plain = vs.changed_files(vs.GitContext(worktree), "main")
        explicit = vs.changed_files(vs.GitContext(self.tmp, git_dir=git_dir, work_tree=worktree), "main")
        self.assertEqual(plain, ["src/core/a.cpp"])
        self.assertEqual(explicit, plain)

    def test_cli_writes_utf8_lf_atomically_and_creates_parents(self):
        self.write("src/core/ä 测.cpp", "int u;\n")
        self.write("src/core/a.cpp", "int a5;\n")
        out = self.tmp / "new" / "dir" / "changed.txt"
        result = run_tool("verify_support.py", "changed", "--repo", str(self.repo), "--out", str(out))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(out.read_bytes(), "src/core/a.cpp\nsrc/core/ä 测.cpp\n".encode("utf-8"))
        self.assertEqual([p.name for p in out.parent.iterdir()], ["changed.txt"])
        self.assertEqual(vs.read_changed(out), ["src/core/a.cpp", "src/core/ä 测.cpp"])

    def test_cli_failure_exits_2_with_the_reason_last(self):
        result = run_tool("verify_support.py", "changed", "--repo", str(self.repo), "--base", "nope")
        self.assertEqual(result.returncode, 2)
        self.assertIn("does not resolve", result.stderr.strip().splitlines()[-1])
        result = run_tool("verify_support.py", "changed", "--repo", str(self.tmp / "not-a-repo"))
        self.assertEqual(result.returncode, 2)

    @unittest.skipIf(WINDOWS, "Windows file names cannot contain a line break")
    def test_a_path_with_a_line_break_is_an_error(self):
        self.write("odd\nname.cpp", "int o;\n")
        with self.assertRaisesRegex(vs.VerifyError, "line break"):
            self.changed()

    def test_validate_changed(self):
        self.assertEqual(vs.validate_changed(["a/b.cpp\r", "", "c d/e.hpp", "x/.clang-tidy"]),
                         ["a/b.cpp", "c d/e.hpp", "x/.clang-tidy"])
        for bad in ("/abs/x.cpp", "C:/x.cpp", "c:x.cpp", "a\\b.cpp", "../x.cpp", "a/../../x.cpp"):
            with self.subTest(entry=bad), self.assertRaises(vs.VerifyError):
                vs.validate_changed([bad])
        missing = self.tmp / "missing.txt"
        with self.assertRaisesRegex(vs.VerifyError, "cannot read"):
            vs.read_changed(missing)


# ---------------------------------------------------------------------------------------------
# TSan selection
# ---------------------------------------------------------------------------------------------

_ALL = ["io", "rpc", "sched", "text"]


def _cmake_calls(text: str) -> list[tuple[str, list[str]]]:
    """(command, arguments) of each CMake command call in `text`, comments removed (enough for our files)."""
    text = re.sub(r"#[^\n]*", "", text)
    return [(m.group(1).lower(), m.group(2).split()) for m in re.finditer(r"(\w+)\s*\(([^()]*)\)", text)]


def _deps_graph() -> tuple[dict[str, set[str]], dict[str, set[str]]]:
    """
    The frost:: dependency graph from the CMake files: target -> direct frost targets it links, and
    tests/<module> -> the targets defined there (tests/text/support's library included under text).
    """
    keywords = re.compile(r"^[A-Z_]+$")
    graph: dict[str, set[str]] = {}
    test_targets: dict[str, set[str]] = {}
    files = list((REPO / "src").glob("*/CMakeLists.txt")) + list((REPO / "tests").glob("*/CMakeLists.txt")) + \
        list((REPO / "tests").glob("*/support/CMakeLists.txt"))
    for path in files:
        rel = path.relative_to(REPO).as_posix().split("/")
        for command, args in _cmake_calls(path.read_text(encoding="utf-8")):
            if not args:
                continue
            if command == "frost_add_library":
                name = f"frost::{args[0]}"
            elif command in ("frost_add_test", "add_library", "add_executable", "target_link_libraries"):
                name = args[0]
            else:
                continue
            node = graph.setdefault(name, set())
            if rel[0] == "tests" and command != "target_link_libraries":
                test_targets.setdefault(rel[1], set()).add(name)
            if command == "target_link_libraries":
                deps = [a for a in args[1:] if not keywords.match(a)]
            else:
                deps, take = [], False
                for arg in args[1:]:
                    if keywords.match(arg):
                        take = arg == "DEPS"
                    elif take:
                        deps.append(arg)
            node.update(d for d in deps if d.startswith(("frost::", "frost_")))
    return graph, test_targets


def _reaches(graph: dict[str, set[str]], start: str) -> set[str]:
    seen, stack = set(), [start]
    while stack:
        for dep in graph.get(stack.pop(), ()):
            if dep not in seen:
                seen.add(dep)
                stack.append(dep)
    return seen


class TsanSelectionTest(unittest.TestCase):
    """tsan_modules and tsan_label_regex."""

    def test_table(self):
        cases = {
            "src/io/src/process_posix.cpp": ["io", "text"],
            "tests/io/process_test.cpp": ["io", "text"],
            "src/sched/include/frost/sched/pool.hpp": ["io", "sched", "text"],
            "src/text/src/piece_tree.cpp": ["text"],
            "tests/text/support/text_ops.cpp": ["text"],
            "src/rpc/src/channel.cpp": ["rpc"],
            "src/core/src/error.cpp": ["io", "sched", "text"],
            "tests/core/error_test.cpp": ["io", "sched", "text"],
            "src/compat/include/frost/compat/function_ref.hpp": ["io", "sched", "text"],
            "tests/support/test_env.hpp": _ALL,
            "cmake/FrostLayers.cmake": _ALL,
            "cmake/toolchains/linux-clang.cmake": _ALL,
            "CMakeLists.txt": _ALL,
            "src/CMakeLists.txt": _ALL,
            "tests/CMakeLists.txt": _ALL,
            "CMakePresets.json": _ALL,
            "vcpkg.json": _ALL,
            "sdk/sdk.lock.json": _ALL,
            "triplets/x64-linux-tsan.cmake": _ALL,
            "src/app/src/main.cpp": [],
            "tests/app/startup_test.cpp": [],
            "tests/tools/test_verify_support.py": [],
            "tools/verify.sh": [],
            "docs/progress.md": [],
            "src/textual/x.cpp": [],
            "src/io2/x.cpp": [],
            "src/io": [],
            "tests/sched": [],
            "cmake": [],
            "tests/supportx/a.hpp": [],
            "./src/io/src/x.cpp": ["io", "text"],
            "src\\sched\\src\\x.cpp": ["io", "sched", "text"],
            "src//text//src/x.cpp": ["text"],
        }
        for path, expected in cases.items():
            with self.subTest(path=path):
                self.assertEqual(vs.tsan_modules([path]), expected)
        self.assertEqual(vs.tsan_modules(None), _ALL)
        self.assertEqual(vs.tsan_modules([]), [])

    def test_properties(self):
        rng = random.Random(27)
        parts = ["src", "tests", "cmake", "triplets", "support", "io", "sched", "text", "rpc", "core", "compat",
                 "app", "tools", "docs", "x.cpp", "CMakeLists.txt", "a.hpp", "textual", "."]
        pool = ["/".join(rng.choice(parts) for _ in range(rng.randint(1, 4))) for _ in range(300)]
        for _ in range(500):
            a = rng.sample(pool, rng.randint(0, 5))
            b = rng.sample(pool, rng.randint(0, 5))
            ma, mb, mab = vs.tsan_modules(a), vs.tsan_modules(b), vs.tsan_modules(a + b)
            self.assertLessEqual(set(mab), set(_ALL))
            self.assertEqual(mab, sorted(mab))
            # The selection of a union is the union of the selections (so order and grouping never matter).
            self.assertEqual(mab, sorted(set(ma) | set(mb)))
            # Separator and "./" spellings select the same.
            self.assertEqual(vs.tsan_modules(["./" + p.replace("/", "\\") for p in a]), ma)

    def test_selection_matches_the_cmake_deps_oracle(self):
        graph, test_targets = _deps_graph()
        existing = [m for m in _ALL if m in test_targets]
        self.assertTrue({"io", "sched", "text"} <= set(existing), test_targets)
        # A src/ module affects exactly the TSan modules whose test targets link it, directly or not.
        for module_dir in sorted(p.name for p in (REPO / "src").iterdir() if (p / "CMakeLists.txt").is_file()):
            target = f"frost::{module_dir}"
            expected = sorted(m for m in existing
                              if any(target in _reaches(graph, t) | {t} for t in test_targets[m]))
            with self.subTest(module=module_dir):
                self.assertEqual(vs.tsan_modules([f"src/{module_dir}/src/x.cpp"]), expected)
        # A tests/ module's own tests always run (its dependents may run too).
        for module in existing:
            with self.subTest(tests=module):
                self.assertIn(module, vs.tsan_modules([f"tests/{module}/x_test.cpp"]))

    def test_label_regex(self):
        self.assertEqual(vs.tsan_label_regex([]), "")
        regex = vs.tsan_label_regex(["text", "io"])
        self.assertEqual(regex, r"^mod\.(io|text)$")
        for label, hit in (("mod.io", True), ("mod.text", True), ("mod.iox", False), ("xmod.io", False),
                           ("mod.textual", False), ("modxio", False), ("mod.sched", False)):
            with self.subTest(label=label):
                self.assertEqual(bool(re.search(regex, label)), hit)

    def test_cli(self):
        with self._list(["src/core/src/error.cpp", "docs/x.md"]) as path:
            result = run_tool("verify_support.py", "tsan", "--changed", str(path))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.splitlines(), ["io sched text", r"^mod\.(io|sched|text)$"])
        with self._list(["docs/x.md"]) as path:
            result = run_tool("verify_support.py", "tsan", "--changed", str(path))
        self.assertEqual((result.returncode, result.stdout), (0, ""))
        result = run_tool("verify_support.py", "tsan", "--all")
        self.assertEqual(result.stdout.splitlines(), ["io rpc sched text", r"^mod\.(io|rpc|sched|text)$"])
        with self._list(["../escape.cpp"]) as path:
            result = run_tool("verify_support.py", "tsan", "--changed", str(path))
        self.assertEqual(result.returncode, 2)

    class _list:
        def __init__(self, paths: list[str]) -> None:
            import tempfile
            self._dir = tempfile.TemporaryDirectory(prefix="frost-tsan-")
            self.path = Path(self._dir.name) / "changed.txt"
            self.path.write_text("".join(f"{p}\n" for p in paths), encoding="utf-8")

        def __enter__(self) -> Path:
            return self.path

        def __exit__(self, *exc) -> None:
            self._dir.cleanup()


# ---------------------------------------------------------------------------------------------
# PATH filter
# ---------------------------------------------------------------------------------------------


class StripPathTest(unittest.TestCase):
    """strip_drvfs and the strip-path command."""

    def test_table(self):
        value = ":".join([
            "/home/u/sdk/llvm/bin", "/mnt/c/Windows/system32", "/usr/bin", "", "/mnt/e/sdk/tool",
            "/mnt/c", "/mnt/wslg/runtime", "/mnt/cd/x", "/mnt/C/Upper", "/mnt/e/sdkx/bin", "/opt/mnt/c/x",
            "/mnt/d/",
        ])
        out, dropped = vs.strip_drvfs(value, ["/mnt/e/sdk/"])
        self.assertEqual(out.split(":"), ["/home/u/sdk/llvm/bin", "/usr/bin", "", "/mnt/e/sdk/tool",
                                          "/mnt/wslg/runtime", "/mnt/cd/x", "/mnt/C/Upper", "/opt/mnt/c/x"])
        self.assertEqual(dropped, 4)
        self.assertEqual(vs.strip_drvfs("", []), ("", 0))
        self.assertEqual(vs.strip_drvfs("/mnt/e/sdk", ["/mnt/e/sdk"]), ("/mnt/e/sdk", 0))

    def test_properties(self):
        rng = random.Random(4)
        entries = ["/usr/bin", "/mnt/c/Windows", "/mnt/e/sdk/bin", "/mnt/e/sdk", "/mnt/wslg", "", "/bin",
                   "/mnt/z", "/mnt/e/dev", "/home/u/sdk/bin"]
        for _ in range(500):
            items = [rng.choice(entries) for _ in range(rng.randint(0, 8))]
            keep = rng.choice([[], ["/mnt/e/sdk"], ["/home/u/sdk"]])
            out, dropped = vs.strip_drvfs(":".join(items), keep)
            # The oracle: every entry in order, except a drive mount entry outside every kept directory.
            expected = [e for e in items if not (re.match(r"^/mnt/[a-z](/|$)", e) and
                                                 not any(e == k or e.startswith(k + "/") for k in keep))]
            self.assertEqual(out, ":".join(expected))
            self.assertEqual(dropped, len(items) - len(expected) if items else 0)
            self.assertEqual(vs.strip_drvfs(out, keep), (out, 0))

    def test_cli(self):
        result = run_tool("verify_support.py", "strip-path", "--keep", "/mnt/e/sdk",
                          "/usr/bin:/mnt/c/Windows:/mnt/e/sdk/x")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "/usr/bin:/mnt/e/sdk/x\n")
        self.assertEqual(result.stderr.strip(), "dropped 1")


# ---------------------------------------------------------------------------------------------
# Compile database and ninja deps
# ---------------------------------------------------------------------------------------------


class CompileDbTest(TempDirTestCase):
    """load_compile_db over fixtures shaped like the real Ninja Multi-Config databases."""

    def load(self, name: str, config: str = "Debug") -> tuple[Path, dict[str, vs.CompileEntry]]:
        root = self.tmp / "repo"
        path = self.tmp / "db.json"
        path.write_text(_fixture_text(name, root, self.tmp / "outside"), encoding="utf-8")
        return root, vs.load_compile_db(path, config)

    def rels(self, root: Path, db: dict[str, vs.CompileEntry]) -> list[str]:
        return sorted(os.path.relpath(key, vs._norm(root)).replace("\\", "/") for key in db)

    def test_windows_debug_entries_deduplicated(self):
        root, db = self.load("compile_db_windows.json")
        self.assertEqual(self.rels(root, db), [
            "../outside/gtest/gtest-all.cc", "build/win-clangcl/gen/generated.cpp", "src/core/src/assert.cpp",
            "src/core/src/error.cpp", "src/core/src/no_output.cpp", "src/io/src/process_win.cpp",
            "src/sched/src/pool.cpp", "tests/core/assert_test.cpp", "tests/core/error_test.cpp",
            "tests/toolchain_probe/banned/reflection.cpp"])
        # The first Debug entry of a file wins (Ninja Multi-Config lists each TU once per configuration).
        assert_entry = db[vs._norm(root / "src/core/src/assert.cpp")]
        self.assertNotIn("SECOND_COPY", assert_entry.raw["command"])
        self.assertEqual(assert_entry.file, str(root / "src/core/src/assert.cpp").replace("\\", "/"))

    def test_other_configurations(self):
        root, db = self.load("compile_db_windows.json", "Release")
        self.assertEqual(self.rels(root, db), ["src/core/src/assert.cpp", "src/core/src/release_only.cpp"])
        _, db = self.load("compile_db_windows.json", "MinSizeRel")
        self.assertEqual(db, {})

    def test_linux_debug_entries(self):
        root, db = self.load("compile_db_linux.json")
        self.assertEqual(self.rels(root, db), ["src/core/src/assert.cpp", "src/io/src/process_posix.cpp",
                                               "tests/io/fixtures/sample.cpp", "tests/toolchain_probe/probe.c"])

    def test_unreadable_database_is_an_error(self):
        with self.assertRaisesRegex(vs.VerifyError, "cannot read the compile database"):
            vs.load_compile_db(self.tmp / "missing.json", "Debug")
        bad = self.tmp / "bad.json"
        bad.write_text("[{", encoding="utf-8")
        with self.assertRaisesRegex(vs.VerifyError, "cannot read the compile database"):
            vs.load_compile_db(bad, "Debug")


class NinjaDepsTest(TempDirTestCase):
    """parse_ninja_deps over fixtures, and _ninja_deps against the real ninja."""

    def test_windows_and_linux_shapes(self):
        root = self.tmp / "repo"
        for name, build, obj in (("ninja_deps_windows.txt", "build/win-clangcl", ".obj"),
                                 ("ninja_deps_linux.txt", "build/linux-clang", ".o")):
            with self.subTest(fixture=name):
                build_dir = root / build
                deps = vs.parse_ninja_deps(_fixture_text(name, root), build_dir)
                key = vs._norm(build_dir / f"src/core/CMakeFiles/frost_core.dir/Debug/src/assert.cpp{obj}")
                self.assertIn(vs._norm(root / "src/core/include/frost/core/assert.hpp"), deps[key])
                self.assertEqual(len(deps[key]), 4)
                # A relative dependency is relative to the build directory; STALE entries count too.
                io_obj = "process_win.cpp" if obj == ".obj" else "process_posix.cpp"
                io_key = vs._norm(build_dir / f"src/io/CMakeFiles/frost_io.dir/Debug/src/{io_obj}{obj}")
                self.assertIn(vs._norm(root / "src/io/src/process_internal.hpp"), deps[io_key])

    def test_lines_outside_an_entry_are_ignored(self):
        text = "    /stray/dep.h\nobj.o: #deps 1, deps mtime 1 (VALID)\n    a.h\n\n    /stray/again.h\n"
        deps = vs.parse_ninja_deps(text, self.tmp)
        self.assertEqual(deps, {vs._norm(self.tmp / "obj.o"): {vs._norm(self.tmp / "a.h")}})

    def test_real_ninja_deps(self):
        ninja = shutil.which("ninja")
        if ninja is None and (os.environ.get("FROST_SDK") or os.environ.get("FROST_LLVM_ROOT")):
            self.fail("ninja is missing although the FrostIDE SDK environment is set")
        if ninja is None:
            self.skipTest("ninja not available")
        root = self.tmp / "repo"
        build = _real_build(root, {"src/x/x.cpp": ["src/x/x.hpp"], "src/x/clean.cpp": []})
        deps = vs._ninja_deps(os.path.abspath(ninja), build, "Debug")
        self.assertEqual(deps[vs._norm(build / "obj" / "x.cpp.o")],
                         {vs._norm(root / "src/x/x.cpp"), vs._norm(root / "src/x/x.hpp")})
        with self.assertRaisesRegex(vs.VerifyError, "ninja -t deps failed"):
            vs._ninja_deps(os.path.abspath(ninja), self.tmp / "no-build", "Debug")


# A compiler stand-in for the real-ninja tests: writes the object and a gcc-style depfile naming the
# source and the extra headers given after it. Arguments: out depfile source [header...] (relative).
_FAKE_CC = textwrap.dedent("""\
    import sys
    out, depfile, *inputs = sys.argv[1:]
    open(out, "w").close()
    with open(depfile, "w") as handle:
        handle.write(out + ": " + " ".join(inputs) + "\\n")
""")


def _real_build(root: Path, sources: dict[str, list[str]], ninja: str | None = None) -> Path:
    """
    Writes `sources` (and their headers) under `root`, a build-Debug.ninja that "compiles" them with
    _FAKE_CC into <root>/build/obj, and a compile database for them, then runs ninja so `ninja -t deps`
    has entries. Returns the build directory.
    """
    build = root / "build"
    (build / "obj").mkdir(parents=True, exist_ok=True)
    fake = build / "fake_cc.py"
    fake.write_text(_FAKE_CC, encoding="utf-8")
    lines = ["ninja_required_version = 1.10", "rule cc",
             f'  command = "{sys.executable}" -B -S "{fake}" $out $out.d $in $headers',
             "  depfile = $out.d", "  deps = gcc"]
    db = []
    for source, headers in sources.items():
        for rel in [source, *headers]:
            path = root / rel
            if not path.exists():
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("int x;\n", encoding="utf-8")
        obj = f"obj/{Path(source).name}.o"
        lines += [f"build {obj}: cc ../{source}", "  headers = " + " ".join(f"../{h}" for h in headers)]
        db.append({"directory": str(build).replace("\\", "/"),
                   "command": f'clang++ -DCMAKE_INTDIR=\\"Debug\\" -std=c++23 -c "{root / source}"',
                   "file": str(root / source).replace("\\", "/"), "output": str(build / obj).replace("\\", "/")})
    (build / "build-Debug.ninja").write_text("\n".join(lines) + "\n", encoding="utf-8")
    (build / "compile_commands.json").write_text(json.dumps(db, indent=1), encoding="utf-8")
    exe = ninja or shutil.which("ninja")
    if exe:
        subprocess.run([exe, "-C", str(build), "-f", "build-Debug.ninja"], check=True, capture_output=True)
    return build


# ---------------------------------------------------------------------------------------------
# Planning
# ---------------------------------------------------------------------------------------------


class PlanTest(TempDirTestCase):
    """plan_tidy over the Windows-shaped fixtures (the logic is the same on Linux)."""

    def setUp(self) -> None:
        super().setUp()
        self.root = self.tmp / "repo"
        db_file = self.tmp / "db.json"
        db_file.write_text(_fixture_text("compile_db_windows.json", self.root, self.tmp / "outside"), encoding="utf-8")
        self.db = vs.load_compile_db(db_file, "Debug")
        self.build = self.root / "build" / "win-clangcl"
        self.deps = vs.parse_ninja_deps(_fixture_text("ninja_deps_windows.txt", self.root), self.build)

    def plan(self, changed: list[str] | None, **kwargs) -> vs.TidyPlan:
        return vs.plan_tidy(self.root, self.db, self.deps, changed, db_dir="DB", os_name="windows", **kwargs)

    def test_changed_sources_headers_and_skips(self):
        plan = self.plan([
            "src/core/src/error.cpp",
            "src/core/src/release_only.cpp",           # not a Debug TU here
            "src/core/include/frost/core/assert.hpp",   # same-stem includer: assert.cpp
            "src/compat/include/frost/compat/function_ref.hpp",  # no stem or area match: first by path
            "src/io/src/process_internal.hpp",          # relative ninja dependency
            "tests/support/test_env.hpp",
            "tests/support/gtest_only.hpp",             # only a TU outside the repository includes it
            "src/core/include/frost/core/probe_only.hpp",  # only an excluded TU includes it
            "tests/toolchain_probe/banned/reflection.cpp",
            "tests/io/fixtures/data.cpp",
            "build/win-clangcl/gen/generated.cpp",
            "docs/progress.md",
            "tools/verify_support.py",
        ])
        self.assertEqual([u.rel for u in plan.units], [
            "src/core/src/assert.cpp", "src/core/src/error.cpp", "src/io/src/process_win.cpp",
            "tests/core/assert_test.cpp"])
        self.assertEqual({u.db_dir for u in plan.units}, {"DB"})
        self.assertEqual(plan.covered, {
            "src/core/include/frost/core/assert.hpp": "src/core/src/assert.cpp",
            "src/compat/include/frost/compat/function_ref.hpp": "src/core/src/assert.cpp",
            "src/io/src/process_internal.hpp": "src/io/src/process_win.cpp",
            "tests/support/test_env.hpp": "tests/core/assert_test.cpp",
        })
        self.assertEqual(plan.skipped, [
            ("src/core/src/release_only.cpp", "not in the Debug compile database on windows"),
            ("tests/support/gtest_only.hpp", "no TU includes it"),
            ("src/core/include/frost/core/probe_only.hpp", "no TU includes it"),
        ])
        self.assertFalse(plan.all_files)
        self.assertIsNone(plan.all_reason)

    def test_a_header_and_its_source_make_one_unit(self):
        plan = self.plan(["src/core/include/frost/core/error.hpp", "src/core/src/error.cpp"])
        self.assertEqual([u.rel for u in plan.units], ["src/core/src/error.cpp"])

    def test_includer_preference(self):
        root = vs._norm(self.root)
        cands = [vs._norm(self.root / p) for p in ("tests/a/z.cpp", "src/m/b.cpp", "src/m/other.cpp", "src/n/h.cpp")]
        choose = vs._choose_includer
        self.assertEqual(choose("src/q/h.hpp", cands, root), cands[3])        # same stem
        self.assertEqual(choose("src/m/include/x.hpp", cands, root), cands[1])  # same src/<m>, first by path
        self.assertEqual(choose("src/zz/x.hpp", cands, root), cands[1])       # neither: first by path

    def test_no_c_or_cpp_changes(self):
        plan = self.plan(["docs/x.md", "tools/verify.ps1"])
        self.assertEqual((plan.units, plan.covered, plan.skipped, plan.all_files), ([], {}, [], False))

    def test_all_files(self):
        expected = ["src/core/src/assert.cpp", "src/core/src/error.cpp", "src/core/src/no_output.cpp",
                    "src/io/src/process_win.cpp", "src/sched/src/pool.cpp", "tests/core/assert_test.cpp",
                    "tests/core/error_test.cpp"]
        plan = self.plan(None)
        self.assertEqual([u.rel for u in plan.units], expected)
        self.assertTrue(plan.all_files)
        self.assertIsNone(plan.all_reason)
        # A relative database file is resolved against its entry's directory.
        pool = next(u for u in plan.units if u.rel == "src/sched/src/pool.cpp")
        self.assertTrue(os.path.isabs(pool.file))
        self.assertEqual(vs._norm(pool.file), vs._norm(self.root / "src/sched/src/pool.cpp"))

    def test_config_changes_switch_to_all_files(self):
        for path in ("src/app/.clang-tidy", ".clang-tidy", "sdk/sdk.lock.json", "cmake/FrostCompilerOptions.cmake",
                     "cmake/toolchains/win-clangcl.cmake", "CMakePresets.json"):
            with self.subTest(path=path):
                plan = self.plan(["src/core/src/error.cpp", path])
                self.assertEqual(len(plan.units), 7)
                self.assertTrue(plan.all_files)
                self.assertEqual(plan.all_reason, f"{path} changed")
        plan = self.plan(["cmake/FrostLayers.cmake", "tests/x/.clang-tidy.bak"])
        self.assertFalse(plan.all_files)

    def test_extra_database_adds_only_its_own_tus(self):
        extra_key = vs._norm(self.root / "tests/fuzz/fuzzer.cpp")
        extra = {
            extra_key: vs.CompileEntry(str(self.root / "tests/fuzz/fuzzer.cpp"), "X", None, extra_key, {}),
        }
        plan = self.plan(None, extra_db=extra, extra_db_dir="FUZZ")
        self.assertEqual(len(plan.units), 8)
        fuzz = [u for u in plan.units if u.rel == "tests/fuzz/fuzzer.cpp"]
        self.assertEqual([u.db_dir for u in fuzz], ["FUZZ"])


# ---------------------------------------------------------------------------------------------
# Findings, header filter and summary
# ---------------------------------------------------------------------------------------------


class FindingsTest(TempDirTestCase):
    """parse_findings and summary_line."""

    def test_parse(self):
        root = self.tmp / "repo"
        # On Windows the header also appears with backslashes (clang joins include paths natively).
        b_hpp = str(root).replace("/", "\\") + "\\src\\b.hpp" if WINDOWS else f"{root}/src/b.hpp"
        text = "\n".join([
            f"{root}/src/a.cpp:3:5: error: use nullptr [modernize-use-nullptr,-warnings-as-errors]",
            f"{root}/src/a.cpp:3:5: note: this is a note [modernize-use-nullptr]",
            f"{b_hpp}:10:1: warning: single-argument constructors must be marked explicit "
            "[google-explicit-constructor]\r",
            f"{root}/src/b.hpp:10:1: warning: duplicate from another TU [google-explicit-constructor]",
            f"{root}/src/c.cpp:1:2: error: two checks [bugprone-x,performance-y,-warnings-as-errors]",
            "/elsewhere/z.hpp:7:7: warning: outside [misc-z]",
            "12 warnings generated.",
            "Suppressed 3 warnings (3 in non-user code).",
            "error: unable to handle compilation [clang-diagnostic-error]",
        ])
        findings = vs.parse_findings(text, root)
        self.assertEqual([(f.path, f.line, f.col, f.severity, f.check) for f in findings], [
            ("src/a.cpp", 3, 5, "error", "modernize-use-nullptr"),
            ("src/b.hpp", 10, 1, "warning", "google-explicit-constructor"),
            ("src/c.cpp", 1, 2, "error", "bugprone-x"),
            ("src/c.cpp", 1, 2, "error", "performance-y"),
            ("/elsewhere/z.hpp", 7, 7, "warning", "misc-z"),
        ])
        self.assertEqual(findings[0].message, "use nullptr")

    def _result(self, findings=(), units=(), skipped=(), all_files=False) -> vs.TidyResult:
        return vs.TidyResult(list(units), list(findings), list(skipped), 0.0, all_files)

    def test_summary_forms(self):
        unit = vs.TidyUnit("a.cpp", "db", "a.cpp")
        ok = vs.UnitResult(unit, 0, "", [], 0.1)
        finding = vs.Finding("a.cpp", 1, 1, "error", "x", "m")
        found = vs.UnitResult(unit, 1, "", [finding], 0.1)
        timeout = vs.UnitResult(unit, None, "", [], 0.1)
        crash = vs.UnitResult(unit, 3, "boom", [], 0.1)
        cases = [
            (self._result(units=[ok, ok]), None, "tidy ok (2 TUs)"),
            (self._result(units=[ok], skipped=[("b.cpp", "why")]), None, "tidy ok (1 TUs; 1 skipped)"),
            (self._result(), None, "tidy ok (0 TUs: no C/C++ changes)"),
            (self._result(skipped=[("b.hpp", "no TU includes it")]), None, "tidy ok (0 TUs; 1 skipped)"),
            (self._result(units=[ok], all_files=True), "no changed-file list: git failed, see (x)",
             "tidy ok (1 TUs; all files: no changed-file list: git failed; see [x])"),
            (self._result(units=[ok], all_files=True), None, "tidy ok (1 TUs)"),
            (self._result(units=[found], findings=[finding, vs.Finding("b.hpp", 2, 2, "warning", "y", "m")]), None,
             "tidy FAILED (2 findings in 2 files)"),
            (self._result(units=[timeout, crash]), None, "tidy FAILED (timeout in 1 TUs; tool error in 1 TUs)"),
        ]
        for result, reason, expected in cases:
            with self.subTest(expected=expected):
                line = vs.summary_line(result, reason)
                self.assertEqual(line, expected)
                self.assertNotIn(", ", line)
                self.assertEqual(line.count("("), line.count(")"))
        self.assertFalse(found.failed)
        self.assertTrue(timeout.failed and timeout.timed_out)
        self.assertTrue(crash.failed and not crash.timed_out)


class HeaderFilterTest(unittest.TestCase):
    """header_filter_regex, checked with Python's re (the escaped constructs mean the same in POSIX ERE)."""

    def test_metacharacters_are_escaped_and_the_root_anchored(self):
        root = "/home/u/a+b (c)[d].e"
        regex = vs.header_filter_regex(root, windows=False)
        for path, hit in ((f"{root}/src/x.hpp", True), (f"{root}/tests/y/z.h", True), (f"{root}\\src\\x.hpp", True),
                          (f"{root}/build/linux-clang/src/x.hpp", False), (f"{root}/tools/x.hpp", False),
                          (f"/home/u/aab (c)[d].e/src/x.hpp", False), (f"/home/u/a+b (c)d.e/src/x.hpp", False),
                          (f"/home/u/a+b (c)[d]xe/src/x.hpp", False), (f"/x{root}/src/x.hpp", False),
                          (f"{root}/srcx/x.hpp", False)):
            with self.subTest(path=path):
                self.assertEqual(bool(re.search(regex, path)), hit)

    def test_windows_drive_letter_and_separators(self):
        regex = vs.header_filter_regex("E:\\dev\\a+b (c)[d].e\\", windows=True)
        self.assertTrue(regex.startswith("^[Ee]:"))
        for path, hit in (("E:\\dev\\a+b (c)[d].e\\src\\x.hpp", True), ("e:/dev/a+b (c)[d].e/tests/x.h", True),
                          ("E:/dev\\a+b (c)[d].e/src\\x.hpp", True), ("F:/dev/a+b (c)[d].e/src/x.hpp", False)):
            with self.subTest(path=path):
                self.assertEqual(bool(re.search(regex, path)), hit)


# ---------------------------------------------------------------------------------------------
# Runner (fake clang-tidy)
# ---------------------------------------------------------------------------------------------

# A clang-tidy stand-in. argv: control-file log-directory, then what run_tidy passes. The control file maps
# a TU's base name to {"out": text, "exit": n, "sleep": s}; each call writes its argv and times to a file of
# its own in the log directory (concurrent appends to one file are not atomic on Windows).
_FAKE_TIDY = textwrap.dedent("""\
    import json, os, sys, time
    control, log, *args = sys.argv[1:]
    start = time.time()
    case = json.load(open(control, encoding="utf-8")).get(os.path.basename(args[-1]), {})
    time.sleep(case.get("sleep", 0))
    sys.stdout.write(case.get("out", ""))
    sys.stdout.flush()
    with open(os.path.join(log, f"{os.getpid()}.json"), "w", encoding="utf-8") as handle:
        handle.write(json.dumps({"args": args, "start": start, "end": time.time()}))
    sys.exit(case.get("exit", 0))
""")


class RunnerTest(TempDirTestCase):
    """run_tidy with the fake: command line, parallelism, timeouts, tool errors and merging."""

    def setUp(self) -> None:
        super().setUp()
        self.fake = self.tmp / "fake_tidy.py"
        self.fake.write_text(_FAKE_TIDY, encoding="utf-8")
        self.control = self.tmp / "control.json"
        self.log = self.tmp / "calls"
        self.log.mkdir()

    def run_plan(self, cases: dict[str, dict], jobs: int = 2, timeout: float = 60.0) -> vs.TidyResult:
        self.control.write_text(json.dumps(cases), encoding="utf-8")
        units = [vs.TidyUnit(str(self.tmp / "repo" / "src" / name), str(self.tmp / "db"), f"src/{name}")
                 for name in cases]
        plan = vs.TidyPlan(units, {}, [("src/skip.hpp", "no TU includes it")])
        command = [sys.executable, "-B", "-S", str(self.fake), str(self.control), str(self.log)]
        return vs.run_tidy(plan, command, "^HF$", jobs, timeout, self.tmp / "repo")

    def calls(self) -> list[dict]:
        return [json.loads(path.read_text(encoding="utf-8")) for path in sorted(self.log.iterdir())]

    def test_command_line_order_and_merging(self):
        root = self.tmp / "repo"
        header = f"{root}/src/h.hpp:4:5: error: explicit [google-explicit-constructor,-warnings-as-errors]\n"
        result = self.run_plan({
            "a.cpp": {"out": header + f"{root}/src/a.cpp:1:1: error: nullptr [modernize-use-nullptr]\n", "exit": 1,
                      "sleep": 0.3},
            "b.cpp": {"out": header, "exit": 1},
            "c.cpp": {},
        })
        self.assertEqual([u.unit.rel for u in result.units], ["src/a.cpp", "src/b.cpp", "src/c.cpp"])
        self.assertEqual([(f.path, f.check) for f in result.findings],
                         [("src/a.cpp", "modernize-use-nullptr"), ("src/h.hpp", "google-explicit-constructor")])
        self.assertEqual(result.failed, [])
        self.assertEqual(result.skipped, [("src/skip.hpp", "no TU includes it")])
        self.assertEqual(vs.summary_line(result), "tidy FAILED (2 findings in 2 files)")
        args = sorted(c["args"] for c in self.calls())
        self.assertEqual(args[0], ["-p", str(self.tmp / "db"), "--quiet", "--use-color=false", "--header-filter=^HF$",
                                   str(root / "src" / "a.cpp")])

    def test_never_more_than_jobs_at_once(self):
        result = self.run_plan({f"{i}.cpp": {"sleep": 0.2} for i in range(6)}, jobs=2)
        self.assertEqual(vs.summary_line(result), "tidy ok (6 TUs; 1 skipped)")
        events = sorted([(c["start"], 1) for c in self.calls()] + [(c["end"], -1) for c in self.calls()])
        running = peak = 0
        for _, delta in events:
            running += delta
            peak = max(peak, running)
        self.assertLessEqual(peak, 2)

    def test_timeouts_and_tool_errors_fail(self):
        result = self.run_plan({"slow.cpp": {"sleep": 30}, "crash.cpp": {"out": "Error while processing\n", "exit": 1},
                                "fine.cpp": {}}, jobs=3, timeout=1.0)
        self.assertEqual([(u.unit.rel, u.timed_out, u.failed) for u in result.units],
                         [("src/slow.cpp", True, True), ("src/crash.cpp", False, True), ("src/fine.cpp", False, False)])
        self.assertEqual(vs.summary_line(result), "tidy FAILED (timeout in 1 TUs; tool error in 1 TUs)")


class CliErrorTest(TempDirTestCase):
    """The tidy command's usage and environment errors (exit 2, the reason last on stderr)."""

    def test_errors(self):
        repo = self.tmp / "repo"
        build = repo / "build"
        build.mkdir(parents=True)
        exe = os.path.abspath(sys.executable)
        listing = self.tmp / "changed.txt"
        listing.write_text("src/a.cpp\n", encoding="utf-8")
        base = ["tidy", "--repo", str(repo), "--build-dir", str(build), "--clang-tidy", exe, "--ninja", exe]
        cases = [
            (base + ["--all"], "no .clang-tidy"),
            (None, None),
            (["tidy", "--repo", str(repo), "--build-dir", str(build), "--clang-tidy", "clang-tidy", "--ninja", exe,
              "--all"], "clang-tidy not found"),
            (base + ["--all"], "no compile database"),
            (base + ["--changed", str(listing), "--extra-build-dir", str(build)], "--extra-build-dir needs --all"),
            (base + ["--changed", str(listing), "--reason", "x"], "--reason needs --all"),
        ]
        for args, message in cases:
            if args is None:
                (repo / ".clang-tidy").write_text("Checks: '-*'\n", encoding="utf-8")
                continue
            with self.subTest(message=message):
                result = run_tool("verify_support.py", *args)
                self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
                self.assertIn(message, result.stderr.strip().splitlines()[-1])


# ---------------------------------------------------------------------------------------------
# The pinned clang-tidy and the checked-in configuration
# ---------------------------------------------------------------------------------------------

# The root .clang-tidy's checks (ADR 0027 decision 1), as clang-tidy reports them for a file in src/core, and
# what src/app/.clang-tidy and tests/app/.clang-tidy remove for the frozen start-up code.
_ROOT_CHECKS = """
-*, bugprone-*, -bugprone-easily-swappable-parameters, -bugprone-signed-bitwise,
-bugprone-unchecked-optional-access, -bugprone-exception-escape,
-bugprone-implicit-widening-of-multiplication-result, -bugprone-misplaced-widening-cast,
-bugprone-unhandled-exception-at-new, -bugprone-random-generator-seed, -bugprone-casting-through-void,
-bugprone-multi-level-implicit-pointer-conversion, performance-*, -performance-enum-size,
-performance-no-int-to-ptr, -performance-inefficient-string-concatenation, -performance-inefficient-vector-operation,
modernize-use-nullptr, modernize-use-override, modernize-use-equals-default, modernize-use-equals-delete,
modernize-redundant-void-arg, modernize-use-emplace, modernize-loop-convert, modernize-make-unique,
modernize-make-shared, modernize-use-using, modernize-deprecated-headers, readability-container-size-empty,
readability-redundant-string-cstr, readability-misleading-indentation, readability-duplicate-include,
readability-redundant-control-flow, readability-redundant-smartptr-get, readability-static-definition-in-anonymous-namespace,
readability-delete-null-pointer, readability-redundant-declaration, misc-redundant-expression, misc-unused-using-decls,
misc-unused-alias-decls, misc-throw-by-value-catch-by-reference, misc-uniqueptr-reset-release,
misc-definitions-in-headers, misc-static-assert, misc-misleading-identifier, misc-misleading-bidirectional,
misc-confusable-identifiers, cppcoreguidelines-slicing, cppcoreguidelines-virtual-class-destructor,
cppcoreguidelines-missing-std-forward, cppcoreguidelines-prefer-member-initializer, google-explicit-constructor,
concurrency-thread-canceltype-asynchronous
"""
_SCOPED_REMOVALS = ["-bugprone-suspicious-stringview-data-usage", "-modernize-deprecated-headers"]
_SCOPED_DIRS = ["src/app", "tests/app"]


def _globs(text: str) -> list[str]:
    return [g.strip() for g in text.replace("\n", " ").split(",") if g.strip()]


class RealClangTidyTest(TempDirTestCase):
    """The checked-in .clang-tidy files under the pinned clang-tidy, and a runtime fixture."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.tidy = _require(_clang_tidy(), "clang-tidy")

    def run_tidy(self, *args: str, cwd: Path = REPO) -> subprocess.CompletedProcess:
        return subprocess.run([self.tidy, *args], cwd=cwd, capture_output=True, text=True, encoding="utf-8",
                              errors="replace", stdin=subprocess.DEVNULL, check=False)

    def dumped_checks(self, rel_dir: str) -> list[str]:
        result = self.run_tidy("--dump-config", str(REPO / rel_dir / "probe.cpp"), "--")
        self.assertEqual(result.returncode, 0, result.stderr)
        match = re.search(r"^Checks:\s*'?([^'\n]*)'?\s*$", result.stdout, re.M)
        self.assertIsNotNone(match, result.stdout)
        globs = _globs(match.group(1))
        # clang-tidy puts its own base, clang-diagnostic-*, in front of the configured list.
        return globs[1:] if globs[:1] == ["clang-diagnostic-*"] else globs

    def listed(self, rel_dir: str, *extra: str) -> set[str]:
        result = self.run_tidy(*extra, "--list-checks", str(REPO / rel_dir / "probe.cpp"), "--")
        self.assertEqual(result.returncode, 0, result.stderr)
        return {line.strip() for line in result.stdout.splitlines()[1:] if line.strip()}

    def test_configs_verify(self):
        for rel in ["", *_SCOPED_DIRS]:
            with self.subTest(dir=rel or "."):
                result = self.run_tidy("--verify-config", cwd=REPO / rel if rel else REPO)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn("No config errors detected", result.stdout + result.stderr)

    def test_effective_checks(self):
        self.assertEqual(self.dumped_checks("src/core"), _globs(_ROOT_CHECKS))
        for rel in _SCOPED_DIRS:
            with self.subTest(dir=rel):
                self.assertEqual(self.dumped_checks(rel), _globs(_ROOT_CHECKS) + _SCOPED_REMOVALS)

    def test_every_named_check_exists_and_every_exclusion_has_a_reason(self):
        everything = self.listed("src/core", "--checks=*")
        enabled = self.listed("src/core")
        root_text = (REPO / ".clang-tidy").read_text(encoding="utf-8")
        reasons = self._reasons(root_text)
        globs = _globs(_ROOT_CHECKS)
        for glob in globs:
            name = glob.lstrip("-")
            if "*" in name:
                continue
            with self.subTest(check=glob):
                self.assertIn(name, everything, "a check name that does not exist (typo?)")
                if glob.startswith("-"):
                    self.assertNotIn(name, enabled)
                    self.assertIn(name, reasons["excluded"], "an excluded check needs a reason line")
                else:
                    self.assertIn(name, enabled)
        self.assertEqual(set(reasons["excluded"]), {g[1:] for g in globs if g.startswith("-") and g != "-*"})
        for name in reasons["not_enabled"]:
            with self.subTest(not_enabled=name):
                pattern = re.compile("^" + re.escape(name).replace(r"\*", ".*") + "$")
                self.assertFalse(any(pattern.match(c) for c in enabled), f"{name} is enabled after all")
        for rel in _SCOPED_DIRS:
            scoped = self._reasons((REPO / rel / ".clang-tidy").read_text(encoding="utf-8"))
            with self.subTest(scoped=rel):
                self.assertEqual(set(scoped["excluded"]), {g[1:] for g in _SCOPED_REMOVALS})
                self.assertTrue(set(scoped["excluded"]) <= everything)

    @staticmethod
    def _reasons(text: str) -> dict[str, dict[str, str]]:
        """The reason lines of a .clang-tidy header: '#   -<check>  W/L: why' and '#   (not enabled) <check>  ...'."""
        out: dict[str, dict[str, str]] = {"excluded": {}, "not_enabled": {}}
        for line in text.splitlines():
            match = re.match(r"^#\s+-(\S+)\s+(\S+/\S+): (.+)$", line)
            if match:
                out["excluded"][match.group(1)] = match.group(3)
                continue
            match = re.match(r"^#\s+\(not enabled\) (\S+)\s+(\S+/\S+): (.+)$", line)
            if match:
                out["not_enabled"][match.group(1)] = match.group(3)
        return out

    def test_runtime_fixture(self):
        # A root whose name holds ERE metacharacters and spaces; headers under build/ are generated code.
        root = self.tmp / "a+b (c)[d].e"
        (root / "src" / "x").mkdir(parents=True)
        shutil.copyfile(REPO / ".clang-tidy", root / ".clang-tidy")
        files = {
            "src/x/x.hpp": "#pragma once\nstruct Wrapper {\n    Wrapper(int value) : v(value) {}\n    int v;\n};\n",
            "build/gen/gen.hpp": "#pragma once\nstruct Gen {\n    Gen(int value) : v(value) {}\n    int v;\n};\n",
            "src/x/x.cpp": '#include "x.hpp"\n#include "gen.hpp"\nint* make() { return 0; }\n'
                           "int use() { return Wrapper(1).v + Gen(2).v; }\n",
            "src/x/clean.cpp": "int clean() { return 1; }\n",
            "src/x/nolint.cpp": "int* none() {\n    return 0;  // NOLINT(modernize-use-nullptr): the test checks a suppression\n}\n",
        }
        for rel, text in files.items():
            (root / rel).parent.mkdir(parents=True, exist_ok=True)
            (root / rel).write_text(text, encoding="utf-8")
        ninja = shutil.which("ninja")
        build = _real_build(root, {"src/x/x.cpp": ["src/x/x.hpp"], "src/x/clean.cpp": [], "src/x/nolint.cpp": []},
                            ninja)
        db = json.loads((build / "compile_commands.json").read_text(encoding="utf-8"))
        for entry in db:
            entry["command"] = entry["command"].replace("-std=c++23", f'-std=c++23 "-I{root / "build" / "gen"}"')
        (build / "compile_commands.json").write_text(json.dumps(db), encoding="utf-8")
        common = ["tidy", "--repo", str(root), "--build-dir", str(build), "--config", "Debug", "--clang-tidy",
                  self.tidy, "--ninja", os.path.abspath(ninja or sys.executable), "--jobs", "2"]

        result = run_tool("verify_support.py", *common, "--all")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        lines = result.stdout.splitlines()
        self.assertEqual(sorted(line for line in lines if re.match(r"^src/x/\S+:\d+:\d+: ", line)), [
            "src/x/x.cpp:3:22: modernize-use-nullptr: use nullptr",
            "src/x/x.hpp:3:5: google-explicit-constructor: single-argument constructors must be marked explicit to "
            "avoid unintentional implicit conversions",
        ])
        self.assertNotIn("gen.hpp", result.stdout)
        self.assertEqual(lines[-1], "tidy FAILED (2 findings in 2 files)")
        self.assertTrue((build / "frost-tidy" / "compile_commands.json").is_file())

        changed = self.tmp / "changed.txt"
        changed.write_text("src/x/clean.cpp\nsrc/x/nolint.cpp\n", encoding="utf-8")
        result = run_tool("verify_support.py", *common, "--changed", str(changed))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(result.stdout.splitlines()[-1], "tidy ok (2 TUs)")

        if ninja:
            changed.write_text("src/x/x.hpp\nsrc/x/other.hpp\n", encoding="utf-8")
            result = run_tool("verify_support.py", *common, "--changed", str(changed))
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            self.assertIn("tidy: src/x/x.hpp through src/x/x.cpp", result.stdout)
            self.assertIn("tidy: skipped src/x/other.hpp: no TU includes it", result.stdout)
            self.assertEqual(result.stdout.splitlines()[-1], "tidy FAILED (2 findings in 2 files)")


class BuildTreeLabelsTest(unittest.TestCase):
    """
    The build tree's ctest properties, read with `ctest --show-only=json-v1`. The TSan quick leg selects
    tests by their mod.* label, so every test must carry exactly one, naming its directory under tests/;
    and parallel ctest relies on the kind labels' scheduling defaults (frost_add_test). ctest passes the
    tree, its configuration and ctest's path (tests/tools/CMakeLists.txt); run directly, both are skipped.
    """

    @classmethod
    def setUpClass(cls) -> None:
        cls.tests = None
        tree, config, ctest = (os.environ.get(name) for name in ("FROST_TEST_TREE", "FROST_TEST_CONFIG",
                                                                 "FROST_CTEST"))
        if tree and config and ctest:
            result = subprocess.run([ctest, "--test-dir", tree, "-C", config, "-N", "--show-only=json-v1"],
                                    capture_output=True, text=True, encoding="utf-8", timeout=300, check=False)
            cls.error = result.stderr if result.returncode != 0 else ""
            cls.tests = json.loads(result.stdout)["tests"] if result.returncode == 0 else []

    def setUp(self) -> None:
        if self.tests is None:
            self.skipTest("FROST_TEST_TREE, FROST_TEST_CONFIG and FROST_CTEST are set only under ctest")
        self.assertEqual(self.error, "")
        self.assertGreater(len(self.tests), 100)

    @staticmethod
    def _properties(test: dict) -> dict:
        return {p["name"]: p["value"] for p in test.get("properties", [])}

    def test_every_test_has_exactly_one_module_label(self):
        directories = {p.name for p in (REPO / "tests").iterdir() if (p / "CMakeLists.txt").is_file()}
        self.assertIn("tools.verify_support", {test["name"] for test in self.tests})
        wrong = []
        for test in self.tests:
            labels = self._properties(test).get("LABELS", [])
            modules = [label for label in labels if label.startswith("mod.")]
            if len(modules) != 1 or modules[0][len("mod."):] not in directories:
                wrong.append(f"{test['name']}: {labels}")
        self.assertEqual(wrong, [], f"{len(wrong)} of {len(self.tests)} tests")

    def test_integration_and_perf_tests_are_kept_apart(self):
        # ADR 0027 decision 3: an integration test holds a resource lock (frost_desktop by default), a
        # perf test runs serially; FrostProcess.* hold frost_desktop through the integration default.
        wrong, desktop = [], []
        for test in self.tests:
            properties = self._properties(test)
            labels = properties.get("LABELS", [])
            if "integration" in labels and not (properties.get("RESOURCE_LOCK") or properties.get("RUN_SERIAL")):
                wrong.append(f"{test['name']}: integration without a lock")
            if "perf" in labels and not properties.get("RUN_SERIAL"):
                wrong.append(f"{test['name']}: perf without RUN_SERIAL")
            if "frost_desktop" in properties.get("RESOURCE_LOCK", []):
                desktop.append(test["name"])
        self.assertEqual(wrong, [])
        if any(test["name"].startswith("FrostProcess.") for test in self.tests):  # Trees with the UI.
            self.assertTrue(any(name.startswith("FrostProcess.") for name in desktop), desktop[:5])

    def test_one_kind_label_and_tags_after_it(self):
        # frost_add_test (cmake/FrostLayers.cmake): LABELS starts with the kind, and the labels after it
        # are tags that ctest -L selects by and that keep the kind's scheduling default. Every test has
        # exactly one kind label; the tagged suites are build_msbuild_tests (Windows only),
        # lang_clangd_tests, lang_msbuild_tests (Windows only; both tags), build_cmake_tests (tag
        # "cmake"), build_msvc_env_tests (Windows only; tag "msvc"; ADR 0046) and ui_screenshot_tests
        # (trees with the UI).
        kinds = {"unit", "integration", "stress", "perf"}
        tagged = {"msbuild": ("integration", ("MsBuildDiscovery.", "MsBuildBuild.", "MsBuildRun.",
                                              "MsBuildEvaluationE2E.")),
                  "clangd": ("integration", ("ClangdE2E.", "MsBuildEvaluationE2E.")),
                  "cmake": ("integration", ("CMakeRealBuild.",)),
                  "msvc": ("integration", ("MsvcEnvironmentCapture.",)),
                  "screenshot": ("unit", ("ThemeScreenshot.", "ThemeScreenshot/", "ThemeContrast.",
                                          "GoldenComparator."))}
        wrong, seen = [], dict.fromkeys(tagged, 0)
        for test in self.tests:
            properties = self._properties(test)
            labels = properties.get("LABELS", [])
            if len([label for label in labels if label in kinds]) != 1:
                wrong.append(f"{test['name']}: {labels}")
            for tag, (kind, prefixes) in tagged.items():
                in_suite = test["name"].startswith(prefixes)
                if (tag in labels) != in_suite:
                    wrong.append(f"{test['name']}: {labels} (tag {tag})")
                elif in_suite:
                    seen[tag] += 1
                    locked = "frost_desktop" in properties.get("RESOURCE_LOCK", [])
                    if kind not in labels or (kind == "integration" and not locked):
                        wrong.append(f"{test['name']}: {labels}, lock {properties.get('RESOURCE_LOCK')}")
        self.assertEqual(wrong, [])
        self.assertGreater(seen["clangd"], 0)  # frost::lang builds in every tree with tests.
        if os.name == "nt":
            self.assertGreater(seen["msbuild"], 0)


if __name__ == "__main__":
    unittest.main()
