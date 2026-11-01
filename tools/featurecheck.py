#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
#
# featurecheck.py: frost-featurecheck v0, the feature-matrix gate (PLAN.md section 6.1
# "Feature matrix" and Appendix A).
#
#   python tools/featurecheck.py [--repo <path>] generate   # PLAN.md -> tests/features/matrix.json
#   python tools/featurecheck.py [--repo <path>] [check]    # the gate run by the verify scripts
#
# `generate` parses PLAN.md Appendix A (the milestone table and the infrastructure-row
# list) into tests/features/matrix.json, one row per feature or infrastructure row with
# its priority, owning milestone and completion milestone. The hand-maintained fields
# (acceptance_test, status, and inspection_counts.s9_baseline, which spike S9 records)
# are carried over from the existing file, so regenerating never loses them; if the
# existing file is not valid JSON (a merge conflict, say), `generate` refuses to write.
#
# `check` fails when Appendix A is inconsistent (a tier's declared row count or the
# totals line disagree with the listed rows, duplicate IDs), when a P0, P1 or
# infrastructure row lacks an owning milestone, when a hand-maintained field is invalid,
# or when matrix.json is out of date against PLAN.md.
#
# Output: one "path:line: message" line per violation on stdout; exit status 1 on any
# violation, 0 otherwise, 2 on usage errors. Python 3.11+, standard library only.
#
# TODO(M1): fail a milestone exit when a row it owns has no green acceptance test
# (needs the test runner's results; PLAN.md 6.1).
# TODO(M1): diff the matrix against the checklist of IntelliJ editor settings pages (PLAN.md 6.1).
# TODO(M7): gate milestone exits on the generated inspection counts against
# inspection_counts.targets (registry vs LLVM's tables, frost and CFG check counts,
# ReSharper C++ coverage), once the S9 baseline and the generated registry exist.

from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass
from pathlib import Path

PLAN_FILE = "PLAN.md"
MATRIX_FILE = "tests/features/matrix.json"
FIRST_MILESTONE, LAST_MILESTONE = 0, 14
# Allowed values of the hand-maintained `status` field.
STATUSES = ("planned", "in-progress", "green", "deferred")

_TIER_ROW = re.compile(r"^\|\s*(M\d+)\s*\|\s*(P[0-2])\s*\((\d+)\)\s*\|\s*(.*?)\s*\|\s*$")
_FEATURE_ITEM = re.compile(r"^(\d+)\s+(.+)$")
_INFRA_LINE = re.compile(r"^-\s+\*\*(M\d+|P1):\*\*\s*(.*)$")
_INFRA_ITEM = re.compile(r"^(I\d+)\s+(.+)$")
_COMPLETES = re.compile(r"\s*\(\+(M\d+)(?:\s+([^)]*))?\)$")
_P2_IN = re.compile(r"\s*\(P2 in\)$")
_OWNER = re.compile(r"\s*\((M\d+)\)$")
_TOTALS = re.compile(r"\*\*Totals\.\*\*\s*P0:\s*(\d+).*?P1:\s*(\d+)")
_TARGET = re.compile(r"(M\d+)\s+≥?(\d+)(?:\s*frost)?,\s*≥?(\d+)(?:\s*CFG)?,\s*≥?(\d+)%")


@dataclass(frozen=True)
class Problem:
    path: str
    line: int
    message: str

    def __str__(self) -> str:
        return f"{self.path}:{self.line}: {self.message}"


@dataclass
class Parsed:
    rows: list[dict]
    targets: dict[str, dict]
    problems: list[Problem]


def _split_items(text: str) -> list[str]:
    """Splits a ';'-separated list, ignoring separators inside parentheses or backticks."""
    items, depth, tick, current = [], 0, False, []
    for ch in text:
        if ch == "`":
            tick = not tick
        elif not tick and ch == "(":
            depth += 1
        elif not tick and ch == ")":
            depth = max(0, depth - 1)
        if ch == ";" and depth == 0 and not tick:
            items.append("".join(current).strip())
            current = []
        else:
            current.append(ch)
    items.append("".join(current).strip())
    return [item for item in items if item]


def _is_milestone(value: object) -> bool:
    if not isinstance(value, str):
        return False
    m = re.fullmatch(r"M(\d+)", value)
    return bool(m) and FIRST_MILESTONE <= int(m.group(1)) <= LAST_MILESTONE


def _row(row_id: str, kind: str, feature: str, priority: str, milestone: str | None) -> dict:
    """A matrix row with the PLAN-derived fields; markers are stripped from `feature`."""
    completes = note = None
    pulled_in = new = False
    while True:
        before = feature
        if feature.endswith("*"):
            new, feature = True, feature[:-1].rstrip()
        m = _COMPLETES.search(feature)
        if m:
            completes, note, feature = m.group(1), m.group(2), feature[: m.start()]
        m = _P2_IN.search(feature)
        if m:
            pulled_in, feature = True, feature[: m.start()]
        if feature == before:
            break
    return {
        "id": row_id, "kind": kind, "feature": feature.strip(), "priority": priority,
        "milestone": milestone, "completes": completes, "note": note,
        "p2_pulled_in": pulled_in, "new": new,
    }


def parse_plan(text: str) -> Parsed:
    """Parses Appendix A (rows, totals) and the section 6.1 inspection-count targets."""
    lines = text.splitlines()
    problems: list[Problem] = []
    rows: list[dict] = []
    start = next((i for i, l in enumerate(lines) if l.startswith("## Appendix A")), None)
    if start is None:
        return Parsed([], {}, [Problem(PLAN_FILE, 1, "no '## Appendix A' section")])
    end = next((i for i in range(start + 1, len(lines)) if lines[i].startswith("## ")), len(lines))
    totals = None
    in_infra = False
    tier_counts: dict[str, int] = {"P0": 0, "P1": 0}
    for index in range(start, end):
        line, number = lines[index], index + 1
        m = _TIER_ROW.match(line)
        if m:
            milestone, priority, declared, body = m.group(1), m.group(2), int(m.group(3)), m.group(4)
            items = _split_items(body)
            for item in items:
                fm = _FEATURE_ITEM.match(item)
                if not fm:
                    problems.append(Problem(PLAN_FILE, number, f"cannot parse feature row '{item}'"))
                    continue
                rows.append(_row(fm.group(1), "feature", fm.group(2), priority,
                                 milestone if _is_milestone(milestone) else None) | {"_line": number})
            if len(items) != declared:
                problems.append(Problem(PLAN_FILE, number,
                                        f"{milestone} {priority} declares {declared} rows but lists {len(items)}"))
            tier_counts[priority] = tier_counts.get(priority, 0) + len(items)
            continue
        m = _TOTALS.search(line)
        if m:
            totals = (number, int(m.group(1)), int(m.group(2)))
            continue
        if line.startswith("**Infrastructure rows**"):
            in_infra = True
            continue
        m = _INFRA_LINE.match(line) if in_infra else None
        if m:
            label, body = m.group(1), m.group(2).rstrip().rstrip(".")
            for item in _split_items(body):
                im = _INFRA_ITEM.match(item.rstrip("."))
                if not im:
                    problems.append(Problem(PLAN_FILE, number, f"cannot parse infrastructure row '{item}'"))
                    continue
                feature, priority, milestone = im.group(2), "P0", label
                if label == "P1":
                    priority = "P1"
                    om = _OWNER.search(feature)
                    milestone = om.group(1) if om else None
                    if om:
                        feature = feature[: om.start()]
                rows.append(_row(im.group(1), "infra", feature, priority,
                                 milestone if _is_milestone(milestone) else None) | {"_line": number})
    if totals is None:
        problems.append(Problem(PLAN_FILE, start + 1, "Appendix A has no '**Totals.** P0: n ... P1: n' line"))
    else:
        number, p0, p1 = totals
        if p0 != tier_counts.get("P0", 0) or p1 != tier_counts.get("P1", 0):
            problems.append(Problem(PLAN_FILE, number,
                                    f"totals say P0 {p0}, P1 {p1} but the table lists "
                                    f"P0 {tier_counts.get('P0', 0)}, P1 {tier_counts.get('P1', 0)}"))
    seen: dict[tuple[str, str], int] = {}
    for row in rows:
        key = (row["kind"], row["id"])
        if key in seen:
            problems.append(Problem(PLAN_FILE, row["_line"],
                                    f"row {row['id']} is listed twice (first on line {seen[key]})"))
        seen.setdefault(key, row["_line"])
        if row["milestone"] is None:
            problems.append(Problem(PLAN_FILE, row["_line"],
                                    f"{row['priority']} {row['kind']} row {row['id']} has no owning milestone"))
    rows.sort(key=lambda r: (r["kind"] != "feature", int(r["id"].lstrip("I"))))
    return Parsed(rows, _parse_targets(lines, problems), problems)


def _parse_targets(lines: list[str], problems: list[Problem]) -> dict[str, dict]:
    """Inspection-count targets per milestone from the section 6.1 'Feature matrix' bullet."""
    for index, line in enumerate(lines):
        if line.startswith("- **Feature matrix.**"):
            targets = {
                m.group(1): {"frost_checks_min": int(m.group(2)), "cfg_checks_min": int(m.group(3)),
                             "resharper_cpp_coverage_percent_min": int(m.group(4))}
                for m in _TARGET.finditer(line)
            }
            if not targets:
                problems.append(Problem(PLAN_FILE, index + 1, "no inspection-count targets found"))
            return targets
    problems.append(Problem(PLAN_FILE, 1, "no '- **Feature matrix.**' bullet in section 6.1"))
    return {}


S9_TEMPLATE = {
    "recorded": None,
    "llvm_version": None,
    "clang_warning_groups": None,
    "clang_tidy_checks": None,
    "csa_checkers": None,
    "method": ("Spike S9 (PLAN.md M0): configurable warning groups from LLVM's diagnostic tables "
               "(diagtool tree, or clang-tblgen -gen-clang-diag-groups over the 23.1.x sources), primary "
               "tidy checks from `clang-tidy -checks=* --list-checks` with aliases removed, CSA checkers "
               "from `clang -cc1 -analyzer-checker-help`. Recorded by hand; featurecheck generate keeps it."),
}


def build_matrix(parsed: Parsed, existing: dict | None) -> dict:
    """The matrix.json document: PLAN-derived fields plus the preserved hand-maintained ones."""
    old_rows = {}
    old_s9 = None
    if isinstance(existing, dict):
        for row in existing.get("rows", []):
            if isinstance(row, dict):
                old_rows[(row.get("kind"), row.get("id"))] = row
        old_s9 = (existing.get("inspection_counts") or {}).get("s9_baseline")
    rows = []
    for row in parsed.rows:
        out = {k: v for k, v in row.items() if not k.startswith("_")}
        old = old_rows.get((row["kind"], row["id"]), {})
        out["acceptance_test"] = old.get("acceptance_test")
        out["status"] = old.get("status", "planned")
        rows.append(out)
    totals = {
        "P0": sum(1 for r in rows if r["kind"] == "feature" and r["priority"] == "P0"),
        "P1": sum(1 for r in rows if r["kind"] == "feature" and r["priority"] == "P1"),
        "infra": sum(1 for r in rows if r["kind"] == "infra"),
    }
    return {
        "schemaVersion": 1,
        "description": ("FrostIDE feature matrix, generated from PLAN.md Appendix A by "
                        "`python tools/featurecheck.py generate`; `featurecheck.py check` (run by the verify "
                        "scripts) fails when it is out of date. Hand-maintained fields: acceptance_test (the "
                        "ctest name or scenario that proves the row), status (planned, in-progress, green, "
                        "deferred) and inspection_counts.s9_baseline; generate preserves them. Feature row IDs "
                        "are the Appendix A row numbers; infrastructure rows use the I prefix. 'milestone' "
                        "owns the row; 'completes' is the milestone where its last sub-part lands (+Mx)."),
        "source": "PLAN.md Appendix A; inspection-count targets from PLAN.md section 6.1",
        "totals": totals,
        "rows": rows,
        "inspection_counts": {
            "description": ("Spike S9 records the baseline counts of the pinned LLVM here; the M7, M9 and M13 "
                            "exits gate on the targets (PLAN.md section 6.1)."),
            "s9_baseline": old_s9 if isinstance(old_s9, dict) else dict(S9_TEMPLATE),
            "targets": parsed.targets,
        },
    }


def render(matrix: dict) -> str:
    """Serialises the matrix with one row per line, so diffs show row-level changes."""
    parts = ["{"]
    keys = list(matrix)
    for index, key in enumerate(keys):
        comma = "," if index < len(keys) - 1 else ""
        value = matrix[key]
        if key == "rows":
            parts.append('  "rows": [')
            for row_index, row in enumerate(value):
                row_comma = "," if row_index < len(value) - 1 else ""
                parts.append("    " + json.dumps(row, ensure_ascii=False) + row_comma)
            parts.append("  ]" + comma)
        else:
            body = json.dumps(value, ensure_ascii=False, indent=2).replace("\n", "\n  ")
            parts.append(f"  {json.dumps(key)}: {body}{comma}")
    parts.append("}")
    return "\n".join(parts) + "\n"


def _row_line(matrix_text: str, row_id: str) -> int:
    position = matrix_text.find(f'"id": "{row_id}"')
    return 1 if position < 0 else matrix_text.count("\n", 0, position) + 1


def check(repo: Path) -> list[Problem]:
    """The gate: Appendix A consistency, owning milestones, valid fields, an up-to-date matrix."""
    plan_path, matrix_path = repo / PLAN_FILE, repo / MATRIX_FILE
    try:
        parsed = parse_plan(plan_path.read_text(encoding="utf-8"))
    except OSError as error:
        return [Problem(PLAN_FILE, 1, f"cannot read: {error}")]
    problems = list(parsed.problems)
    try:
        matrix_text = matrix_path.read_text(encoding="utf-8")
        matrix = json.loads(matrix_text)
    except OSError:
        return problems + [Problem(MATRIX_FILE, 1, "missing; run python tools/featurecheck.py generate")]
    except ValueError as error:
        return problems + [Problem(MATRIX_FILE, 1, f"not valid JSON: {error}")]
    for row in matrix.get("rows", []):
        row_id = str(row.get("id"))
        line = _row_line(matrix_text, row_id)
        if row.get("priority") in ("P0", "P1") and not _is_milestone(row.get("milestone")):
            problems.append(Problem(MATRIX_FILE, line,
                                    f"{row.get('priority')} {row.get('kind')} row {row_id} has no owning milestone"))
        if row.get("status") not in STATUSES:
            problems.append(Problem(MATRIX_FILE, line,
                                    f"row {row_id}: status must be one of {', '.join(STATUSES)}"))
        test = row.get("acceptance_test")
        if test is not None and (not isinstance(test, str) or not test.strip()):
            problems.append(Problem(MATRIX_FILE, line, f"row {row_id}: acceptance_test must be null or a name"))
        if row.get("status") == "green" and not test:
            problems.append(Problem(MATRIX_FILE, line, f"row {row_id} is green without an acceptance_test"))
    expected = render(build_matrix(parsed, matrix))
    if expected != matrix_text.replace("\r\n", "\n"):
        old = {(r.get("kind"), str(r.get("id"))): r for r in matrix.get("rows", []) if isinstance(r, dict)}
        new = {(r["kind"], r["id"]): r for r in json.loads(expected)["rows"]}
        changed = sorted(k[1] for k in old.keys() | new.keys() if old.get(k) != new.get(k))
        detail = f" (rows {', '.join(changed[:10])}{', ...' if len(changed) > 10 else ''})" if changed else ""
        problems.append(Problem(MATRIX_FILE, 1,
                                f"out of date against PLAN.md Appendix A{detail}; "
                                f"run python tools/featurecheck.py generate"))
    return problems


def generate(repo: Path) -> int:
    """Writes matrix.json; fails without writing if Appendix A does not parse cleanly or
    an existing matrix.json is not valid JSON (its hand-maintained fields would be lost)."""
    parsed = parse_plan((repo / PLAN_FILE).read_text(encoding="utf-8"))
    if parsed.problems:
        for problem in parsed.problems:
            print(problem)
        return 1
    matrix_path = repo / MATRIX_FILE
    existing = None
    if matrix_path.is_file():
        # An unparsable matrix (e.g. a merge conflict) still holds the hand-maintained
        # fields; regenerating would reset them to defaults, so refuse instead.
        try:
            existing = json.loads(matrix_path.read_text(encoding="utf-8"))
        except ValueError as error:
            print(Problem(MATRIX_FILE, 1, f"not valid JSON ({error}); fix it before regenerating"))
            return 1
    matrix_path.parent.mkdir(parents=True, exist_ok=True)
    matrix_path.write_text(render(build_matrix(parsed, existing)), encoding="utf-8", newline="\n")
    totals = build_matrix(parsed, existing)["totals"]
    print(f"featurecheck: wrote {MATRIX_FILE} (P0 {totals['P0']}, P1 {totals['P1']}, infra {totals['infra']})",
          file=sys.stderr)
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="frost-featurecheck v0: the PLAN.md feature-matrix gate")
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parent.parent,
                        help="repository root (default: the checkout containing this script)")
    parser.add_argument("command", nargs="?", choices=("check", "generate"), default="check")
    args = parser.parse_args(argv)
    repo = args.repo.resolve()
    if args.command == "generate":
        return generate(repo)
    problems = check(repo)
    for problem in problems:
        print(problem)
    print(f"featurecheck: {len(problems)} problem(s)", file=sys.stderr)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
