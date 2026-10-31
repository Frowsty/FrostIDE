# SPDX-License-Identifier: Apache-2.0
#
# Tests that tests/perf/budgets.json captures every row of the PLAN.md section 4 budgets
# table verbatim (plan_* fields) and that its structured targets are well formed, so a
# budget cannot change in one place without the other (changing one needs an ADR).

from __future__ import annotations

import json
import re
import unittest

from tools_test_support import REPO

_ROW = re.compile(r"^\| (B\d+) \| (.*?) \| (.*?) \| (.*?) \| (.*?) \|$")
_UNITS = {"ms", "s", "min", "MB", "GB", "percent", "count"}
_PERCENTILES = {"p50", "p95", "p99", "max", None}


def plan_rows() -> dict[str, tuple[str, str, str, str]]:
    """The budgets table of PLAN.md section 4: id -> (metric, target, corpus, gate)."""
    text = (REPO / "PLAN.md").read_text(encoding="utf-8")
    section = text[text.index("## 4. Performance budgets"):text.index("## 5. ")]
    return {m.group(1): m.group(2, 3, 4, 5) for m in map(_ROW.match, section.splitlines()) if m}


class BudgetsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.doc = json.loads((REPO / "tests" / "perf" / "budgets.json").read_text(encoding="utf-8"))
        cls.budgets = {b["id"]: b for b in cls.doc["budgets"]}

    def test_every_plan_row_is_present_verbatim(self) -> None:
        rows = plan_rows()
        self.assertEqual(len(rows), 28)
        self.assertEqual(sorted(self.budgets, key=lambda i: int(i[1:])), sorted(rows, key=lambda i: int(i[1:])))
        self.assertEqual(len(self.doc["budgets"]), len(self.budgets), "duplicate budget IDs")
        for budget_id, (metric, target, corpus, gate) in rows.items():
            budget = self.budgets[budget_id]
            self.assertEqual(budget["plan_metric"], metric, budget_id)
            self.assertEqual(budget["plan_target"], target, budget_id)
            self.assertEqual(budget["plan_corpus"], corpus, budget_id)
            self.assertEqual(budget["plan_gate"], gate, budget_id)

    def test_structured_fields_are_well_formed(self) -> None:
        for budget_id, budget in self.budgets.items():
            for key in ("metric", "targets", "corpus", "preconditions", "method", "gate", "percentiles"):
                self.assertIn(key, budget, budget_id)
            self.assertTrue(budget["targets"], budget_id)
            for target in budget["targets"]:
                self.assertIn(target["unit"], _UNITS, budget_id)
                self.assertIn(target.get("percentile"), _PERCENTILES, budget_id)
                self.assertTrue(target.get("max") is not None or target.get("relative"), budget_id)
            gates = [g["milestone"] for g in budget["gate"]]
            self.assertTrue(gates, budget_id)
            for milestone in gates:
                self.assertRegex(milestone, r"^M(\d|1[0-4])$", budget_id)
                self.assertIn(milestone, budget["plan_gate"], budget_id)


if __name__ == "__main__":
    unittest.main()
