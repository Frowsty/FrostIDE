# SPDX-License-Identifier: Apache-2.0
#
# Tests for tools/frost_perf.py, the performance-budget runner behind tools/perf.ps1 (ADR 0027):
# the probe edits, the statistics against hand-computed values, .ninja_log parsing, the phase
# split, the budgets.json binding of the B28 harness, the WSL path conversion, and end-to-end runs
# in a temporary git repository with fake cmake, sccache, verify and wsl commands (Python scripts
# injected through frost_perf.Tools), including the failure, interrupt, kill and external-edit
# paths. The two ten-round gated series swap the fake cmake process for an in-process stand-in
# (Hooks.run_build), which keeps this suite from being the longest Windows test; the other runs keep
# the subprocess path. Nothing runs a real build, and every result goes to a temporary results root.

from __future__ import annotations

import contextlib
import io
import json
import random
import subprocess
import sys
import textwrap
import unittest
from pathlib import Path
from unittest import mock

from tools_test_support import FIXTURES, REPO, TOOLS, TempDirTestCase, clean_env, git

import frost_perf


class ProbeTest(unittest.TestCase):
    def test_inserts_before_the_first_namespace_line(self) -> None:
        data = b"// SPDX\n#include <x>\n\nnamespace frost {\nint f();\n}\nnamespace other {}\n"
        out = frost_perf.insert_probe(data, 42)
        self.assertEqual(out, b"// SPDX\n#include <x>\n\n[[maybe_unused]] constexpr int kB28Probe = 42;\n\n"
                              b"namespace frost {\nint f();\n}\nnamespace other {}\n")

    def test_replaces_an_existing_probe(self) -> None:
        data = frost_perf.insert_probe(b"namespace {\n}\n", 1)
        out = frost_perf.insert_probe(data, 2)
        self.assertEqual(out, b"[[maybe_unused]] constexpr int kB28Probe = 2;\n\nnamespace {\n}\n")
        self.assertEqual(len(frost_perf.PROBE_RE.findall(out)), 1)

    def test_a_file_without_namespace_is_refused(self) -> None:
        with self.assertRaises(ValueError):
            frost_perf.insert_probe(b"int main() { return 0; }\n  namespace indented {}\n", 1)

    def test_crlf_bytes_are_kept(self) -> None:
        data = b"#include <x>\r\n\r\nnamespace a {\r\n}\r\n"
        out = frost_perf.insert_probe(data, 7)
        self.assertEqual(out, b"#include <x>\r\n\r\n[[maybe_unused]] constexpr int kB28Probe = 7;\r\n\r\nnamespace a {\r\n}\r\n")
        self.assertEqual(frost_perf.remove_probe(out), data)

    def test_property_insert_then_remove_is_the_identity(self) -> None:
        rng = random.Random(28)
        pieces = [b"int x;", b"// namespace in a comment", b"#define N 1", b"", b"  namespace nested {}",
                  b"\xc3\xa4\xe6\xb5\x8b", b"\t}", b"namespace q {", b"namespace{"]
        for _ in range(300):
            eol = rng.choice([b"\n", b"\r\n"])
            lines = [rng.choice(pieces) for _ in range(rng.randint(0, 12))]
            lines.insert(rng.randint(0, len(lines)), rng.choice([b"namespace frost {", b"namespace {"]))
            data = eol.join(lines) + rng.choice([b"", eol])
            once = frost_perf.insert_probe(data, rng.randint(0, 2_000_000_000))
            self.assertEqual(frost_perf.remove_probe(once), data)
            twice = frost_perf.insert_probe(once, rng.randint(0, 2_000_000_000))
            self.assertEqual(len(frost_perf.PROBE_RE.findall(twice)), 1)
            self.assertEqual(frost_perf.remove_probe(twice), data)

    def test_probe_values_never_repeat(self) -> None:
        values = frost_perf.ProbeValues(wall=lambda: 1_700_000_000.0)
        seen = [values.next() for _ in range(5)]
        self.assertEqual(seen, sorted(set(seen)))
        self.assertTrue(all(v < 2_000_000_000 for v in seen))


class StatsTest(unittest.TestCase):
    def test_median_matches_hand_computed_values(self) -> None:
        self.assertEqual(frost_perf.median([1, 2, 3, 4]), 2.5)
        # ADR 0010's six verify -Quick walls: sorted 80.4 81.2 82.2 82.3 82.6 85.5, so p50 = (82.2 + 82.3) / 2.
        self.assertAlmostEqual(frost_perf.median([85.5, 82.2, 80.4, 82.3, 81.2, 82.6]), 82.25)

    def test_nearest_rank_table(self) -> None:
        five = [15, 20, 35, 40, 50]
        ten = list(range(1, 11))
        table = [(five, 5, 15), (five, 30, 20), (five, 40, 20), (five, 50, 35), (five, 100, 50),
                 (ten, 1, 1), (ten, 10, 1), (ten, 70, 7), (ten, 90, 9), (ten, 99, 10), (ten, 99.9, 10)]
        for samples, q, expected in table:
            self.assertEqual(frost_perf.percentile(samples, q), expected, (samples, q))

    def test_property_bounds_monotonicity_and_domain(self) -> None:
        rng = random.Random(6)
        for _ in range(200):
            samples = [rng.uniform(0, 100) for _ in range(rng.randint(1, 30))]
            qs = sorted(rng.uniform(0.01, 100) for _ in range(5))
            values = [frost_perf.percentile(samples, q) for q in qs]
            self.assertTrue(all(min(samples) <= v <= max(samples) for v in values))
            self.assertEqual(values, sorted(values))
        for q in (0, -1, 100.5, 101):
            with self.assertRaises(ValueError):
                frost_perf.percentile([1.0], q)
        with self.assertRaises(ValueError):
            frost_perf.percentile([], 50)
        with self.assertRaises(ValueError):
            frost_perf.median([])

    def test_unit_conversion(self) -> None:
        self.assertEqual(frost_perf.to_seconds(5, "min"), 300)
        self.assertEqual(frost_perf.to_seconds(20, "s"), 20)
        self.assertEqual(frost_perf.to_seconds(150, "ms"), 0.15)
        with self.assertRaises(ValueError):
            frost_perf.to_seconds(1, "MB")


class NinjaLogTest(TempDirTestCase):
    def test_v5_v6_and_v7_parse_alike_and_skip_malformed_lines(self) -> None:
        for version in (5, 6, 7):
            edges, end = frost_perf.parse_ninja_log(FIXTURES / "perf" / f"ninja_log_v{version}.txt", 0)
            outputs = [e["output"] for e in edges]
            self.assertIn("Debug/frost.exe", outputs, version)
            self.assertTrue(all(e["end_ms"] >= e["start_ms"] for e in edges), version)
            self.assertEqual(end, (FIXTURES / "perf" / f"ninja_log_v{version}.txt").stat().st_size, version)
        v7, _ = frost_perf.parse_ninja_log(FIXTURES / "perf" / "ninja_log_v7.txt", 0)
        self.assertEqual([e["output"] for e in v7],
                         ["CMakeFiles/frost_runtime_dlls_tests_io-Debug",
                          "src/core/CMakeFiles/frost_core.dir/Debug/src/error.cpp.obj",
                          "src/core/Debug/frost_core.lib", "Debug/frost.exe"])
        self.assertEqual(v7[1], {"start_ms": 60, "end_ms": 1290,
                                 "output": "src/core/CMakeFiles/frost_core.dir/Debug/src/error.cpp.obj"})

    def test_offsets_return_only_new_lines(self) -> None:
        log = self.tmp / ".ninja_log"
        log.write_bytes(b"# ninja log v7\n1\t2\t3\ta.obj\tff\n")
        _, offset = frost_perf.parse_ninja_log(log, 0)
        with log.open("ab") as handle:
            handle.write(b"5\t9\t3\tb.obj\tff\n")
        edges, end = frost_perf.parse_ninja_log(log, offset)
        self.assertEqual([e["output"] for e in edges], ["b.obj"])
        self.assertEqual(end, log.stat().st_size)
        # A recompacted (shorter) log is read from its start.
        log.write_bytes(b"# ninja log v7\n5\t9\t3\tb.obj\tff\n")
        edges, _ = frost_perf.parse_ninja_log(log, 10_000)
        self.assertEqual([e["output"] for e in edges], ["b.obj"])
        self.assertEqual(frost_perf.parse_ninja_log(self.tmp / "missing", 0), ([], 0))

    def test_other_versions_are_rejected(self) -> None:
        log = self.tmp / ".ninja_log"
        log.write_bytes(b"# ninja log v4\n1\t2\t3\ta.obj\n")
        with self.assertRaises(ValueError):
            frost_perf.parse_ninja_log(log, 0)


class PhasesTest(unittest.TestCase):
    # A consistency check (labelled as such, PLAN.md 6.1): the phases partition the wall time.
    def test_phase_durations_add_up_to_the_wall_time(self) -> None:
        rng = random.Random(3)
        for _ in range(100):
            wall = rng.uniform(1, 300)
            markers = sorted((rng.uniform(0, wall), rng.choice(["debug build", "format", "tidy", "wsl"]))
                             for _ in range(rng.randint(0, 8)))
            phases = frost_perf.phase_split(markers, wall)
            self.assertAlmostEqual(sum(phases.values()), wall, delta=0.005 * (len(markers) + 1))
            self.assertIn("startup", phases)
            self.assertEqual(len(phases), len(markers) + 1)

    def test_names_and_repeats(self) -> None:
        phases = frost_perf.phase_split([(1.0, "a"), (3.0, "b"), (4.0, "a")], 10.0)
        self.assertEqual(phases, {"startup": 1.0, "a": 2.0, "b": 1.0, "a #2": 6.0})


class BudgetTest(unittest.TestCase):
    def test_b28_loads_from_the_real_budgets_json(self) -> None:
        budget = frost_perf.load_budget(REPO / "tests" / "perf" / "budgets.json", "B28")
        self.assertEqual(budget.id, "B28")
        self.assertEqual({(t.variant, t.percentile, t.max, t.unit) for t in budget.targets},
                         {("one-.cpp rebuild", "p50", 20.0, "s"), ("verify -Quick", "p50", 5.0, "min")})

    def test_the_method_text_matches_the_harness_constants(self) -> None:
        # A drift oracle: budgets.json's method text names what B28Harness hard-codes.
        method = frost_perf.load_budget(REPO / "tests" / "perf" / "budgets.json", "B28").method
        for path in frost_perf.B28_PROBE_FILES.values():
            self.assertIn(path, method)
        self.assertIn(f"-Jobs {frost_perf.B28_VERIFY_JOBS}", method)
        self.assertIn(r"cmake --build build\win-clangcl --config Debug", method)
        self.assertIn("ninja's default parallelism", method)

    def test_gated_variants_name_exact_budget_variants(self) -> None:
        for budget_id, harness in frost_perf.HARNESSES.items():
            budget = frost_perf.load_budget(REPO / "tests" / "perf" / "budgets.json", budget_id)
            variants = {t.variant for t in budget.targets}
            gated = [v for v in harness.variants if v.target_variant is not None]
            self.assertTrue(gated, budget_id)
            for variant in gated:
                self.assertIn(variant.target_variant, variants, variant.name)
                self.assertGreater(variant.min_counted, 0, variant.name)

    def test_unknown_budgets_and_budgets_without_a_harness_exit_2(self) -> None:
        for argv, expected in ((["-Budget", "B99"], "unknown budget"), (["-Budget", "B5"], "harnessed budgets: B28")):
            err = io.StringIO()
            with contextlib.redirect_stderr(err), contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(frost_perf.main(argv), 2)
            self.assertIn(expected, err.getvalue())

    def test_list_names_every_budget(self) -> None:
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.assertEqual(frost_perf.main(["-List"]), 0)
        lines = out.getvalue().splitlines()
        self.assertEqual(len(lines), 28)
        self.assertTrue(any(line.startswith("B28") and " harness " in line for line in lines), lines)

    def test_powershell_spellings_and_comma_lists(self) -> None:
        args = frost_perf.parse_args(["-budget", "b28", "-Variant", "rebuild,noop", "-Files", "core", "-PerfRoot",
                                      "X:\\r", "--warmup", "0"])
        self.assertEqual((args.budget, args.perf_root, args.warmup), ("b28", "X:\\r", 0))
        self.assertEqual(frost_perf._split(args.variant), ["rebuild", "noop"])


class WslPathTest(unittest.TestCase):
    def test_conversion_table(self) -> None:
        # The same table tests verify.ps1's ConvertTo-WslPath (test_verify_scripts.py).
        table = json.loads((FIXTURES / "verify" / "wsl_paths.json").read_text(encoding="utf-8"))
        for row in table:
            self.assertEqual(frost_perf.to_wsl_path(row["windows"]), row["wsl"], row)
        with self.assertRaises(ValueError):
            frost_perf.to_wsl_path("/home/x")


_FAKE_CMAKE = r'''
import json, os, sys
from pathlib import Path
here = Path(__file__).resolve().parent
args = sys.argv[1:]
if args[:1] == ["--version"]:
    print("cmake version 4.4.5")
    sys.exit(0)
counter = here / "cmake.count"
calls = int(counter.read_text()) + 1 if counter.exists() else 1
counter.write_text(str(calls))
control = json.loads((here / "control.json").read_text()) if (here / "control.json").exists() else {}
build = Path(args[args.index("--build") + 1])
build.mkdir(parents=True, exist_ok=True)
log = build / ".ninja_log"
with log.open("a", newline="\n") as handle:
    if log.stat().st_size == 0:
        handle.write("# ninja log v7\n")
    handle.write(f"0\t{calls}\t1\tobj/call{calls}.obj\tff\n")
print(f"[1/1] Building CXX object obj/call{calls}.obj")
sys.exit(1 if calls == control.get("fail_on_call") else 0)
'''

_FAKE_SCCACHE = r'''
import sys
from pathlib import Path
here = Path(__file__).resolve().parent
counter = here / "sccache.count"
calls = int(counter.read_text()) + 1 if counter.exists() else 1
counter.write_text(str(calls))
if sys.argv[1:2] == ["--version"]:
    print("sccache 0.19.0")
else:
    print('{"stats": {"compile_requests": %d, "requests_executed": %d, "cache_hits": {"counts": {}}, '
          '"cache_misses": {"counts": {"C/C++": %d}}, "cache_errors": {"counts": {}}, "compile_fails": 0}}'
          % (calls, calls, calls))
'''

_FAKE_VERIFY = r'''
import json, shutil, subprocess, sys
from pathlib import Path
here = Path(__file__).resolve().parent
(here / "verify.args").write_text(json.dumps(sys.argv[1:]))
# What verify's changed-file list would hold with -Base HEAD (the run's cwd is the repository).
diff = subprocess.run([shutil.which("git"), "diff", "--name-only", "HEAD"], capture_output=True, text=True).stdout
with (here / "verify.changed").open("a", encoding="utf-8") as handle:
    handle.write(json.dumps(diff.split()) + "\n")
for marker in ("=== changed files", "=== debug configure", "=== debug build", "=== debug test", "=== tidy",
               "=== wsl /mnt/x/tools/verify.sh"):
    print(marker, flush=True)
print("Verified: win debug 1/1, tidy ok (1 TUs)")
'''

_FAKE_WSL = r'''
import sys
args = sys.argv[1:]
if "--list" in args:
    sys.exit(0)
if args[-1:] == ["true"]:
    sys.exit(0)
print("verify.sh: dropped 0 Windows PATH entries (ADR 0027)")
print("verify.sh: source /mnt/x, mirror /home/x/m, jobs 4")
for label in ("debug: configure (linux-clang)", "debug: build (linux-clang-debug)", "debug: test (linux-clang-debug)",
              "tsan-quick: configure (linux-clang-tsan)", "tidy"):
    print("=== " + label, flush=True)
print("wsl: debug 1/1, tsan-quick 1/1, tidy ok (1 TUs), licensecheck ok")
'''

# Runs frost_perf.main with the fakes and a hook that ends the process with os._exit after the
# first probe write, as a timeout wrapper's kill would: no finally block runs.
_KILLED_CHILD = r'''
import json, os, sys
config = json.loads(open(sys.argv[1], encoding="utf-8").read())
sys.path.insert(0, config["tools"])
import frost_perf
tools = frost_perf.Tools(**config["fakes"])
hooks = frost_perf.Hooks(cpu_sample_s=0, timestamp=lambda: config["timestamp"],
                         after_probe_write=lambda key, path, n: os._exit(9))
sys.exit(frost_perf.main(config["argv"], tools=tools, hooks=hooks))
'''

_STAND_IN = b"// SPDX-License-Identifier: Apache-2.0\n// A stand-in probe file.\n\nnamespace frost {\nint f%d();\n}\n"


class EndToEndTest(TempDirTestCase):
    """B28 runs against a temporary repository; the fakes stand in for every external tool."""

    def setUp(self) -> None:
        super().setUp()
        self.repo = self.tmp / "repo"
        for index, rel in enumerate(frost_perf.B28_PROBE_FILES.values()):
            path = self.repo / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(_STAND_IN % index)
        budgets = self.repo / "tests" / "perf" / "budgets.json"
        budgets.parent.mkdir(parents=True)
        budgets.write_bytes((REPO / "tests" / "perf" / "budgets.json").read_bytes())
        (self.repo / ".gitignore").write_text("/build/\n", encoding="utf-8")
        git("init", "-q", cwd=self.repo)
        git("add", "-A", cwd=self.repo)
        git("commit", "-q", "-m", "base", cwd=self.repo)
        git("checkout", "-q", "-b", "m1/perf-test", cwd=self.repo)
        self.sha7 = git("rev-parse", "HEAD", cwd=self.repo).strip()[:7]
        self.originals = {key: (self.repo / rel).read_bytes() for key, rel in frost_perf.B28_PROBE_FILES.items()}
        self.fakes = self.tmp / "fakes"
        self.fakes.mkdir()
        for name, text in (("cmake", _FAKE_CMAKE), ("sccache", _FAKE_SCCACHE), ("verify", _FAKE_VERIFY),
                           ("wsl", _FAKE_WSL)):
            (self.fakes / f"fake_{name}.py").write_text(textwrap.dedent(text), encoding="utf-8")
        self.root = self.tmp / "perf"
        self._stamp = 0
        self._builds = 0

    def fake_tools(self, verify_base: bool = False, sccache: bool = True) -> dict:
        """The Tools fields for the fakes, as JSON-compatible lists (sccache=False: no sccache, as on Linux)."""
        # The base interpreter, not a venv's python.exe: on Windows that is a launcher that starts the
        # base interpreter as a second process, which made every fake call about 70 % dearer.
        python = getattr(sys, "_base_executable", None) or sys.executable
        prefix = lambda name: [python, "-B", "-S", str(self.fakes / f"fake_{name}.py")]  # noqa: E731
        return {"cmake": prefix("cmake"), "ninja": None, "sccache": prefix("sccache") if sccache else None,
                "powershell": None, "wsl": prefix("wsl"), "verify_command": prefix("verify"),
                "verify_base": verify_base}

    def in_process_build(self, cmd: list[str], timeout: float) -> tuple[int, bytes]:
        """
        Hooks.run_build stand-in for the long gated series: what fake_cmake.py does (one .ninja_log
        edge per call), without starting a process. The subprocess path keeps its own tests.
        """
        self._builds += 1
        build = Path(cmd[cmd.index("--build") + 1])
        build.mkdir(parents=True, exist_ok=True)
        log = build / ".ninja_log"
        with log.open("a", encoding="utf-8", newline="\n") as handle:
            if log.stat().st_size == 0:
                handle.write("# ninja log v7\n")
            handle.write(f"0\t{self._builds}\t1\tobj/call{self._builds}.obj\tff\n")
        return 0, f"[1/1] Building CXX object obj/call{self._builds}.obj\n".encode()

    def timestamp(self) -> str:
        self._stamp += 1
        return f"20261101T{self._stamp:06d}"

    def run_perf(self, *argv: str, clock=None, after_probe_write=None, verify_base: bool = False,
                 timestamp=None, sccache: bool = True, in_process: bool = False) -> tuple[int, str, str]:
        """Runs frost_perf.main in-process; returns (exit status, stdout, stderr). in_process: no build processes."""
        hooks = frost_perf.Hooks(cpu_sample_s=0, timestamp=timestamp or self.timestamp,
                                 after_probe_write=after_probe_write, distro_wait_s=0)
        if clock is not None:
            hooks.clock = clock
        if in_process:
            hooks.run_build = self.in_process_build
        out, err = io.StringIO(), io.StringIO()
        full = ["-Repo", str(self.repo), "-PerfRoot", str(self.root), *argv]
        tools = frost_perf.Tools(**self.fake_tools(verify_base, sccache))
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = frost_perf.main(full, tools=tools, hooks=hooks)
        return code, out.getvalue(), err.getvalue()

    def run_dirs(self) -> list[Path]:
        base = self.root / "m1" / "b28"
        return sorted(base.iterdir()) if base.is_dir() else []

    def assert_restored(self) -> None:
        for key, rel in frost_perf.B28_PROBE_FILES.items():
            self.assertEqual((self.repo / rel).read_bytes(), self.originals[key], rel)
        self.assertEqual(git("status", "--porcelain", cwd=self.repo), "")
        git_dir = Path(git("rev-parse", "--absolute-git-dir", cwd=self.repo).strip())
        self.assertFalse((git_dir / frost_perf.POINTER_NAME).exists())

    def load(self, run_dir: Path) -> tuple[list[dict], dict]:
        runs = [json.loads(line) for line in (run_dir / "runs.jsonl").read_text(encoding="utf-8").splitlines()]
        return runs, json.loads((run_dir / "summary.json").read_text(encoding="utf-8"))

    def test_two_rebuild_rounds_record_and_restore(self) -> None:
        code, out, err = self.run_perf("-Variant", "rebuild", "-Runs", "2", "-Warmup", "0")
        self.assertEqual(code, 0, out + err)
        [run_dir] = self.run_dirs()
        self.assertEqual(run_dir.name, f"20261101T000001-{self.sha7}")
        runs, summary = self.load(run_dir)
        self.assertEqual(len(runs), 10)
        self.assertEqual([(r["file"], r["b28"]["target"]) for r in runs[:5]],
                         [("core", "frost"), ("core", "all"), ("main", "frost"), ("main", "all"), ("sched", "all")])
        for r in runs:
            self.assertEqual((r["schemaVersion"], r["budget"], r["variant"], r["unit"]), (1, "B28", "rebuild", "s"))
            self.assertEqual(r["target_variant"], "one-.cpp rebuild")
            self.assertTrue(r["valid"] and r["counted"])
            self.assertEqual(r["b28"]["sccache"]["misses"], 1, r)  # the fake adds one per stats call
            self.assertEqual(len(r["b28"]["ninja_log_edges"]), 1)
            self.assertEqual(r["b28"]["settle"]["exit"], 0)
        variant = summary["variants"]["rebuild"]
        self.assertEqual((variant["verdict"], variant["verdict_reason"], variant["n"]), ("info", "n < 10", 2))
        self.assertEqual(variant["target"], {"percentile": "p50", "max": 20.0, "unit": "s"})
        self.assertEqual(sorted(variant["per_probe"]), ["core/all", "core/frost", "main/all", "main/frost", "sched/all"])
        conditions = json.loads((run_dir / "conditions.json").read_text(encoding="utf-8"))
        self.assertEqual(conditions["git"]["sha"][:7], self.sha7)
        self.assertEqual(conditions["versions"]["cmake"], "cmake version 4.4.5")
        self.assertTrue((run_dir / "logs" / "r00-core-frost.log").is_file())
        self.assertEqual((run_dir / "originals" / "core.orig").read_bytes(), self.originals["core"])
        self.assertIn("B28: one-.cpp rebuild p50", out.splitlines()[-1])
        self.assert_restored()
        # Everything the run wrote lies under the temporary root (or the ignored build tree).
        written = {p.relative_to(self.tmp).parts[0] for p in self.tmp.rglob("*") if p.is_file()}
        self.assertLessEqual(written, {"repo", "perf", "fakes"})

    def test_the_method_count_gives_a_gated_verdict(self) -> None:
        code, out, err = self.run_perf("-Variant", "rebuild", "-Runs", "10", "-Warmup", "0", sccache=False,
                                       in_process=True)
        self.assertEqual(code, 0, out + err)
        runs, summary = self.load(self.run_dirs()[0])
        # Ten rounds of five probes, each a settle and a timed build, each timed build one log edge.
        self.assertEqual((len(runs), self._builds), (50, 100))
        self.assertTrue(all(len(r["b28"]["ninja_log_edges"]) == 1 for r in runs))
        variant = summary["variants"]["rebuild"]
        self.assertEqual((variant["verdict"], variant["n"]), ("ok", 10))
        self.assertIn(variant["worst_probe"], ("core", "main", "sched"))
        self.assertRegex(out.splitlines()[-1], r"^B28: one-\.cpp rebuild p50 \d+\.\d+ s \(worst probe \w+; max 20 s\) ok$")
        self.assert_restored()

    def test_over_budget_exits_1(self) -> None:
        ticks = iter(range(0, 1_000_000, 25))
        code, out, err = self.run_perf("-Variant", "rebuild", "-Runs", "10", "-Warmup", "0",
                                       clock=lambda: float(next(ticks)), sccache=False, in_process=True)
        self.assertEqual(code, 1, out + err)
        _, summary = self.load(self.run_dirs()[0])
        self.assertEqual(summary["variants"]["rebuild"]["verdict"], "over")
        self.assertEqual(summary["variants"]["rebuild"]["percentiles"]["p50"], 25.0)
        self.assert_restored()

    def test_a_failed_build_makes_the_run_invalid(self) -> None:
        # Two cmake calls per step (settle, timed), five steps per round: call 12 is round 2's first timed build.
        (self.fakes / "control.json").write_text('{"fail_on_call": 12}', encoding="utf-8")
        code, out, err = self.run_perf("-Variant", "rebuild", "-Runs", "3", "-Warmup", "0", sccache=False)
        self.assertEqual(code, 3, out + err)
        runs, summary = self.load(self.run_dirs()[0])
        self.assertEqual(len(runs), 6)
        self.assertFalse(runs[-1]["valid"])
        self.assertEqual((runs[-1]["round"], runs[-1]["exit"]), (1, 1))
        self.assertFalse(summary["valid"])
        self.assertIn("core-frost failed", summary["invalid_reason"])
        self.assertIn("INVALID", out.splitlines()[-1])
        self.assert_restored()

    def test_an_interrupt_restores_and_invalidates(self) -> None:
        writes = []

        def interrupt(key: str, path: Path, n: int) -> None:
            writes.append(key)
            if len(writes) == 3:
                raise KeyboardInterrupt

        code, out, err = self.run_perf("-Variant", "rebuild", "-Runs", "2", "-Warmup", "0",
                                       after_probe_write=interrupt)
        self.assertEqual(code, 3, out + err)
        _, summary = self.load(self.run_dirs()[0])
        self.assertEqual(summary["invalid_reason"], "interrupted")
        self.assert_restored()

    def _killed_run(self, timestamp: str) -> Path:
        config = {"tools": str(TOOLS), "fakes": self.fake_tools(), "timestamp": timestamp,
                  "argv": ["-Repo", str(self.repo), "-PerfRoot", str(self.root), "-Variant", "rebuild", "-Runs", "1"]}
        (self.tmp / "child.json").write_text(json.dumps(config), encoding="utf-8")
        (self.tmp / "child.py").write_text(_KILLED_CHILD, encoding="utf-8")
        result = subprocess.run([sys.executable, "-B", str(self.tmp / "child.py"), str(self.tmp / "child.json")],
                                capture_output=True, text=True, env=clean_env(), check=False)
        self.assertEqual(result.returncode, 9, result.stdout + result.stderr)
        run_dir = self.root / "m1" / "b28" / f"{timestamp}-{self.sha7}"
        self.assertTrue((run_dir / "probes.json").is_file())
        self.assertNotEqual((self.repo / frost_perf.B28_PROBE_FILES["core"]).read_bytes(), self.originals["core"])
        return run_dir

    def test_a_killed_run_blocks_the_next_until_restored(self) -> None:
        run_dir = self._killed_run("20261101T100000")
        code, _, err = self.run_perf("-Variant", "rebuild", "-Runs", "1")
        self.assertEqual(code, 2)
        self.assertIn(f"-Restore {run_dir}", err)
        code, _, err = self.run_perf("-Restore", str(run_dir))
        self.assertEqual(code, 0, err)
        self.assert_restored()
        # Killed again; then the probed file is edited by someone else, so -Restore refuses.
        run_dir = self._killed_run("20261101T100001")
        edited = b"namespace frost { int edited(); }\n"
        (self.repo / frost_perf.B28_PROBE_FILES["core"]).write_bytes(edited)
        code, _, err = self.run_perf("-Restore", str(run_dir))
        self.assertEqual(code, 3)
        self.assertIn("changed outside the harness", err)
        self.assertEqual((self.repo / frost_perf.B28_PROBE_FILES["core"]).read_bytes(), edited)

    def test_a_kill_between_the_journal_and_the_probe_write_is_restorable(self) -> None:
        # The second probe write of core is cut off after the journal names it but before the file
        # changes, so the file still holds the first write: -Restore must accept that state too.
        path = self.repo / frost_perf.B28_PROBE_FILES["core"]
        run_dir = self.tmp / "run"
        journal = frost_perf.ProbeJournal(run_dir, self.repo, self.tmp / "gitdir", {"core": path})
        journal.begin()
        journal.write_probe("core", 1)
        first = path.read_bytes()
        real_write = frost_perf.write_atomic

        def killed(target: Path, data: bytes) -> None:
            if Path(target) == path:
                raise KeyboardInterrupt
            real_write(target, data)

        with mock.patch.object(frost_perf, "write_atomic", killed), self.assertRaises(KeyboardInterrupt):
            journal.write_probe("core", 2)
        self.assertEqual(path.read_bytes(), first)
        self.assertEqual(frost_perf.restore_journal(run_dir / "probes.json"), [])
        self.assertEqual(path.read_bytes(), self.originals["core"])
        self.assertFalse((self.tmp / "gitdir" / frost_perf.POINTER_NAME).exists())

    def test_restoring_a_probe_mid_run_checks_the_bytes(self) -> None:
        path = self.repo / frost_perf.B28_PROBE_FILES["core"]
        journal = frost_perf.ProbeJournal(self.tmp / "run", self.repo, self.tmp / "gitdir", {"core": path})
        journal.begin()
        self.assertFalse(journal.restore_original("core"))  # already original: nothing written
        journal.write_probe("core", 1)
        self.assertTrue(journal.restore_original("core"))
        self.assertEqual(path.read_bytes(), self.originals["core"])
        journal.write_probe("core", 2)  # the journal still accepts the file after the restore
        path.write_bytes(b"namespace frost { int edited(); }\n")
        with self.assertRaises(frost_perf.RunInvalid):
            journal.restore_original("core")
        self.assertEqual(path.read_bytes(), b"namespace frost { int edited(); }\n")

    def test_dirty_or_probed_files_exit_2_before_any_write(self) -> None:
        core = self.repo / frost_perf.B28_PROBE_FILES["core"]
        core.write_bytes(self.originals["core"] + b"// edit\n")
        code, _, err = self.run_perf("-Variant", "rebuild", "-Runs", "1")
        self.assertEqual(code, 2)
        self.assertIn("uncommitted changes", err)
        self.assertEqual(self.run_dirs(), [])
        core.write_bytes(frost_perf.insert_probe(self.originals["core"], 5))
        git("commit", "-q", "-am", "probed", cwd=self.repo)
        code, _, err = self.run_perf("-Variant", "rebuild", "-Runs", "1")
        self.assertEqual(code, 2)
        self.assertIn("already carries", err)
        self.assertEqual(self.run_dirs(), [])

    def test_a_dirty_worktree_stops_the_verify_variant(self) -> None:
        (self.repo / "untracked.txt").write_text("x", encoding="utf-8")
        code, _, err = self.run_perf("-Variant", "verify", "-Runs", "1")
        self.assertEqual(code, 2)
        self.assertIn("clean worktree", err)
        self.assertEqual(self.run_dirs(), [])

    def test_an_existing_output_directory_exits_2(self) -> None:
        (self.root / "m1" / "b28" / f"20261101T000000-{self.sha7}").mkdir(parents=True)
        code, _, err = self.run_perf("-Variant", "noop", "-Runs", "1", timestamp=lambda: "20261101T000000")
        self.assertEqual(code, 2)
        self.assertIn("already exists", err)

    def test_an_external_edit_mid_run_is_not_overwritten(self) -> None:
        edited = b"namespace frost { int someone_else(); }\n"
        core = self.repo / frost_perf.B28_PROBE_FILES["core"]

        def edit(key: str, path: Path, n: int) -> None:
            if key == "core":
                core.write_bytes(edited)

        code, out, err = self.run_perf("-Variant", "rebuild", "-Runs", "1", "-Warmup", "0", after_probe_write=edit)
        self.assertEqual(code, 3, out + err)
        self.assertEqual(core.read_bytes(), edited)
        _, summary = self.load(self.run_dirs()[0])
        self.assertIn("changed outside the harness", summary["invalid_reason"])
        self.assertIn("restoring the probe files failed", summary["invalid_reason"])

    def test_verify_runs_record_phases_and_pass_base_when_declared(self) -> None:
        code, out, err = self.run_perf("-Variant", "verify", "-Runs", "2", "-Warmup", "1", verify_base=True)
        self.assertEqual(code, 0, out + err)
        runs, summary = self.load(self.run_dirs()[0])
        self.assertEqual([(r["file"], r["counted"]) for r in runs], [("core", False), ("main", True), ("core", True)])
        for r in runs:
            self.assertEqual(r["summary_line"], "Verified: win debug 1/1, tidy ok (1 TUs)")
            self.assertAlmostEqual(sum(r["phases"].values()), r["wall_s"], delta=0.05)
            self.assertIn("tidy", r["phases"])
            self.assertIn("wsl", r["phases"])
        args = json.loads((self.fakes / "verify.args").read_text(encoding="utf-8"))
        self.assertEqual(args, ["-Quick", "-Jobs", "4", "-Base", "HEAD"])
        # Each timed run sees only its own probe file changed: the previous run's probe went back first.
        seen = [json.loads(line) for line in (self.fakes / "verify.changed").read_text(encoding="utf-8").splitlines()]
        files = frost_perf.B28_PROBE_FILES
        self.assertEqual(seen, [[files["core"]], [files["main"]], [files["core"]]])
        self.assertEqual([r["b28"]["restored"] for r in runs], [[], ["core"], ["main"]])
        variant = summary["variants"]["verify"]
        self.assertEqual((variant["verdict"], variant["verdict_reason"], variant["n"]), ("info", "n < 6", 2))
        self.assertIn("tidy", variant["phases_p50"])
        self.assert_restored()

    def test_verify_without_base(self) -> None:
        code, out, err = self.run_perf("-Variant", "verify", "-Runs", "1", "-Warmup", "0", verify_base=False)
        self.assertEqual(code, 0, out + err)
        self.assertEqual(json.loads((self.fakes / "verify.args").read_text(encoding="utf-8")), ["-Quick", "-Jobs", "4"])

    def test_wsl_leg_runs_split_phases(self) -> None:
        code, out, err = self.run_perf("-Variant", "wsl-leg", "-Runs", "2")
        self.assertEqual(code, 0, out + err)
        run_dir = self.run_dirs()[0]
        runs, summary = self.load(run_dir)
        self.assertEqual(len(runs), 2)
        for r in runs:
            self.assertIsNotNone(r["b28"]["distro_start_s"])
            self.assertIn("debug: configure", r["phases"])
            self.assertIn("tsan-quick: configure", r["phases"])
            self.assertTrue(r["summary_line"].startswith("wsl: "))
            self.assertIn("FROST_VERIFY_BASE=HEAD", r["b28"]["command"])
        self.assertEqual((run_dir / "wsl-changed-00.txt").read_text(encoding="utf-8"), "src/core/src/error.cpp\n")
        self.assertEqual(summary["variants"]["wsl-leg"]["verdict"], "info")
        self.assert_restored()


if __name__ == "__main__":
    unittest.main()
