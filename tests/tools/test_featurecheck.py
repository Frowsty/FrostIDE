# SPDX-License-Identifier: Apache-2.0
#
# Tests for tools/featurecheck.py against the mini plan in tests/tools/fixtures/features:
# parsing Appendix A rows and markers, the run order from the milestone ladder,
# generate/check round trips, preservation of the hand-maintained fields, and each
# failure the check reports.

from __future__ import annotations

import json
import shutil
import unittest
from pathlib import Path

from tools_test_support import FIXTURES, TempDirTestCase, run_tool

import featurecheck


class FeatureCheckTest(TempDirTestCase):
    def setUp(self) -> None:
        super().setUp()
        shutil.copy(FIXTURES / "features" / "PLAN.md", self.tmp / "PLAN.md")
        self.matrix = self.tmp / "tests" / "features" / "matrix.json"

    def _generate(self) -> None:
        result = run_tool("featurecheck.py", "--repo", str(self.tmp), "generate")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def _check(self) -> list[str]:
        return [str(p) for p in featurecheck.check(self.tmp)]

    def _edit_plan(self, old: str, new: str) -> None:
        plan = self.tmp / "PLAN.md"
        text = plan.read_text(encoding="utf-8")
        self.assertIn(old, text)
        plan.write_text(text.replace(old, new), encoding="utf-8", newline="\n")

    def _rows(self) -> dict[str, dict]:
        return {r["id"]: r for r in json.loads(self.matrix.read_text(encoding="utf-8"))["rows"]}

    def test_generate_parses_rows_and_markers(self) -> None:
        self._generate()
        rows = self._rows()
        self.assertEqual(sorted(rows), sorted(["3", "20", "28", "57", "250", "I7", "I14", "I15", "I18"]))
        self.assertEqual(rows["57"]["completes"], "M6")
        self.assertEqual(rows["57"]["feature"], "gutter")
        self.assertTrue(rows["250"]["new"])
        self.assertEqual(rows["250"]["feature"], "soft wrap")
        self.assertTrue(rows["3"]["p2_pulled_in"])
        self.assertEqual(rows["3"]["priority"], "P1")
        self.assertEqual(rows["20"]["feature"], "toolchain detection, v1 slice (WSL/Docker/remote: 1.x)")
        self.assertEqual((rows["I14"]["milestone"], rows["I14"]["completes"], rows["I14"]["note"]),
                         ("M5", "M6", "for Linux"))
        self.assertEqual((rows["I18"]["priority"], rows["I18"]["milestone"]), ("P1", "M7"))
        self.assertEqual(rows["I7"]["kind"], "infra")
        document = json.loads(self.matrix.read_text(encoding="utf-8"))
        self.assertEqual(document["totals"], {"P0": 3, "P1": 2, "infra": 4})
        self.assertEqual(document["inspection_counts"]["targets"]["M13"],
                         {"frost_checks_min": 60, "cfg_checks_min": 5, "resharper_cpp_coverage_percent_min": 75})
        self.assertEqual(self._check(), [])
        self.assertEqual(run_tool("featurecheck.py", "--repo", str(self.tmp)).returncode, 0)

    def test_generate_preserves_hand_maintained_fields(self) -> None:
        self._generate()
        document = json.loads(self.matrix.read_text(encoding="utf-8"))
        for row in document["rows"]:
            if row["id"] == "28":
                row["status"], row["acceptance_test"] = "green", "ui.file_system_view"
        document["inspection_counts"]["s9_baseline"]["clang_tidy_checks"] = 406
        self.matrix.write_text(featurecheck.render(document), encoding="utf-8", newline="\n")
        self.assertEqual(self._check(), [])
        self._edit_plan("28 File System view", "28 File System view and filters")
        self._generate()
        rows = self._rows()
        self.assertEqual((rows["28"]["status"], rows["28"]["acceptance_test"]), ("green", "ui.file_system_view"))
        self.assertEqual(rows["28"]["feature"], "File System view and filters")
        self.assertEqual(json.loads(self.matrix.read_text(encoding="utf-8"))
                         ["inspection_counts"]["s9_baseline"]["clang_tidy_checks"], 406)

    def test_generate_refuses_to_overwrite_an_unparsable_matrix(self) -> None:
        self._generate()
        document = json.loads(self.matrix.read_text(encoding="utf-8"))
        for row in document["rows"]:
            if row["id"] == "28":
                row["status"], row["acceptance_test"] = "green", "ui.file_system_view"
        # A merge conflict leaves the hand-maintained fields in a file that no longer parses.
        broken = "<<<<<<< HEAD\n" + featurecheck.render(document)
        self.matrix.write_text(broken, encoding="utf-8", newline="\n")
        result = run_tool("featurecheck.py", "--repo", str(self.tmp), "generate")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("tests/features/matrix.json:1: not valid JSON", result.stdout)
        self.assertEqual(self.matrix.read_text(encoding="utf-8"), broken)

    def test_out_of_date_matrix_fails(self) -> None:
        self._generate()
        self._edit_plan("57 gutter (+M6)", "57 gutter (+M7)")
        problems = self._check()
        self.assertEqual(len(problems), 1)
        self.assertIn("tests/features/matrix.json:1: out of date", problems[0])
        self.assertIn("rows 57", problems[0])
        result = run_tool("featurecheck.py", "--repo", str(self.tmp), "check")
        self.assertEqual(result.returncode, 1)

    def test_missing_matrix_fails(self) -> None:
        problems = self._check()
        self.assertEqual(len(problems), 1)
        self.assertIn("missing", problems[0])

    def test_declared_tier_count_must_match(self) -> None:
        self._edit_plan("| M1 | P0 (3) |", "| M1 | P0 (4) |")
        problems = self._check()
        self.assertTrue(any("M1 P0 declares 4 rows but lists 3" in p for p in problems), problems)

    def test_totals_must_match(self) -> None:
        self._edit_plan("P1: 2.", "P1: 3.")
        problems = self._check()
        self.assertTrue(any("totals say P0 3, P1 3" in p for p in problems), problems)

    def test_rows_need_an_owning_milestone(self) -> None:
        self._edit_plan("I18 Power Save Mode (M7)", "I18 Power Save Mode")
        problems = self._check()
        self.assertTrue(any("P1 infra row I18 has no owning milestone" in p for p in problems), problems)
        result = run_tool("featurecheck.py", "--repo", str(self.tmp), "generate")
        self.assertEqual(result.returncode, 1)

    def test_milestones_run_from_m0_to_m17(self) -> None:
        # ADR 0029 added M15-M17 (the deferred halves of M1-M4); M18 is not a milestone.
        self._edit_plan("| M2 | P1 (2) |", "| M17 | P1 (2) |")
        self._edit_plan("57 gutter (+M6)", "57 gutter (+M15)")
        self._generate()
        rows = self._rows()
        self.assertEqual((rows["3"]["milestone"], rows["20"]["milestone"]), ("M17", "M17"))
        self.assertEqual(rows["57"]["completes"], "M15")
        self.assertEqual(self._check(), [])
        self._edit_plan("| M17 | P1 (2) |", "| M18 | P1 (2) |")
        problems = self._check()
        self.assertTrue(any("P1 feature row 3 has no owning milestone" in p for p in problems), problems)

    def test_completion_must_name_a_milestone(self) -> None:
        self._edit_plan("57 gutter (+M6)", "57 gutter (+M18)")
        problems = self._check()
        self.assertTrue(any("row 57 completes in M18, which is not a milestone" in p for p in problems), problems)
        result = run_tool("featurecheck.py", "--repo", str(self.tmp), "generate")
        self.assertEqual(result.returncode, 1)

    def test_completion_must_run_after_the_owner_in_section_7_order(self) -> None:
        # The ladder's order, not the number, decides: M17 runs before M7 (ADR 0029).
        self._edit_plan("| M2 | P1 (2) |", "| M17 | P1 (2) |")
        self._edit_plan("(WSL/Docker/remote: 1.x)", "(WSL/Docker/remote: 1.x) (+M7)")
        self._generate()
        self.assertEqual(self._rows()["20"]["completes"], "M7")
        self.assertEqual(self._check(), [])
        # M15 runs before M17, and an infrastructure row cannot complete before its owner either.
        self._edit_plan("1.x) (+M7)", "1.x) (+M15)")
        self._edit_plan("(+M6 for Linux)", "(+M1 for Linux)")
        problems = self._check()
        self.assertTrue(any("row 20 completes in M15, which does not run after its owner M17" in p
                            for p in problems), problems)
        self.assertTrue(any("row I14 completes in M1, which does not run after its owner M5" in p
                            for p in problems), problems)

    def test_ladder_must_list_every_milestone_once(self) -> None:
        self._edit_plan("| M16 | ProjectHost | 9–11 | 15–18 |\n", "")
        self._edit_plan("| M8 | Claude C2 | 3–4 | 23–28 |",
                        "| M8 | Claude C2 | 3–4 | 23–28 |\n| M3 | again | 1 | 1 |")
        problems = self._check()
        self.assertTrue(any("the milestone ladder does not list M16" in p for p in problems), problems)
        self.assertTrue(any("the milestone ladder lists M3 twice" in p for p in problems), problems)

    def test_matrix_rows_without_a_milestone_fail(self) -> None:
        self._generate()
        text = self.matrix.read_text(encoding="utf-8").replace(
            '"id": "28", "kind": "feature", "feature": "File System view", "priority": "P0", "milestone": "M1"',
            '"id": "28", "kind": "feature", "feature": "File System view", "priority": "P0", "milestone": null')
        self.matrix.write_text(text, encoding="utf-8", newline="\n")
        problems = self._check()
        self.assertTrue(any("P0 feature row 28 has no owning milestone" in p for p in problems), problems)

    def test_invalid_hand_maintained_fields_fail(self) -> None:
        self._generate()
        document = json.loads(self.matrix.read_text(encoding="utf-8"))
        for row in document["rows"]:
            if row["id"] == "28":
                row["status"] = "done"
            if row["id"] == "57":
                row["status"] = "green"
        self.matrix.write_text(featurecheck.render(document), encoding="utf-8", newline="\n")
        problems = self._check()
        self.assertTrue(any("row 28: status must be one of" in p for p in problems), problems)
        self.assertTrue(any("row 57 is green without an acceptance_test" in p for p in problems), problems)

    def test_duplicate_rows_fail(self) -> None:
        self._edit_plan("28 File System view; 57 gutter (+M6); 250 soft wrap*",
                        "28 File System view; 28 gutter (+M6); 250 soft wrap*")
        problems = self._check()
        self.assertTrue(any("row 28 is listed twice" in p for p in problems), problems)


class SplitTest(unittest.TestCase):
    def test_separators_inside_parentheses_and_backticks_are_kept(self) -> None:
        self.assertEqual(featurecheck._split_items("1 a (x; y); 2 `b; c`; 3 d"), ["1 a (x; y)", "2 `b; c`", "3 d"])


if __name__ == "__main__":
    unittest.main()
