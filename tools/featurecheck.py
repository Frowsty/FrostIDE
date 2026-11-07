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
# infrastructure row lacks an owning milestone, when a row's completion marker "(+Mx)"
# names no milestone or one that does not run after the row's owner, when the milestone
# ladder (PLAN.md "Plan at a glance", rows in run order) does not list every milestone
# exactly once, when a hand-maintained field is invalid, or when matrix.json is out of
# date against PLAN.md.
#
# Roadmap v2 (ADR 0077, FND-15): it also reads the R-ladder (section 7 "Roadmap v2"), the epic
# catalogue (Appendix C) and Appendix A's "Epic mapping" table, and fails when an epic id is
# listed twice, has an unknown priority or depends on an unknown epic, when a merged epic names
# no live owner, when Appendix C's R groups or a "#### Rn" section's epic list disagree with the
# ladder, when the mapping names an unknown or merged epic or an unknown row, when an Appendix A
# row is neither mapped to an epic nor listed as out of epic scope (an orphan), and when a
# matrix.json row references an unknown or merged epic. `generate` writes each row's `epics`
# and a `roadmap` section: the ladder, every epic with its rows and their M-ladder milestones
# (m_ids), and the out-of-scope rows.
#
# Output: one "path:line: message" line per violation on stdout; exit status 1 on any
# violation, 0 otherwise, 2 on usage errors. Python 3.11+, standard library only.
#
# TODO(M1): fail a milestone exit when a row it owns has no green acceptance test
# (needs the test runner's results; PLAN.md 6.1).
# TODO(M15): diff the matrix against the checklist of IntelliJ editor settings pages (PLAN.md 6.1),
# with the editor settings that M15 completes (ADR 0029).
# TODO(M7): gate milestone exits on the generated inspection counts against
# inspection_counts.targets (registry vs LLVM's tables, frost and CFG check counts,
# ReSharper C++ coverage), once the S9 baseline and the generated registry exist.

from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

PLAN_FILE = "PLAN.md"
MATRIX_FILE = "tests/features/matrix.json"
# Milestone IDs M0..M17 (PLAN.md section 7). IDs are identities, not positions: ADR 0029
# added M15-M17 for the deferred halves of M1-M4, which run between M5 and M6. Keep in
# step with frost_lint.py's range.
FIRST_MILESTONE, LAST_MILESTONE = 0, 17
# Allowed values of the hand-maintained `status` field.
STATUSES = ("planned", "in-progress", "green", "deferred")

_TIER_ROW = re.compile(r"^\|\s*(M\d+)\s*\|\s*(P[0-2])\s*\((\d+)\)\s*\|\s*(.*?)\s*\|\s*$")
_FEATURE_ITEM = re.compile(r"^(\d+)\s+(.+)$")
_INFRA_LINE = re.compile(r"^-\s+\*\*(M\d+|P1):\*\*\s*(.*)$")
_INFRA_ITEM = re.compile(r"^(I\d+)\s+(.+)$")
_COMPLETES = re.compile(r"\s*\(\+(M\d+)(?:\s+([^)]*))?\)$")
_P2_IN = re.compile(r"\s*\(P2 in\)$")
_OWNER = re.compile(r"\s*\((M\d+)\)$")
# A row of the milestone ladder: | Mn | name | units | ends around month |.
_LADDER_ROW = re.compile(r"^\|\s*(M\d+)\s*\|[^|]*\|[^|]*\|[^|]*\|\s*$")
_TOTALS = re.compile(r"\*\*Totals\.\*\*\s*P0:\s*(\d+).*?P1:\s*(\d+)")
_TARGET = re.compile(r"(M\d+)\s+≥?(\d+)(?:\s*frost)?,\s*≥?(\d+)(?:\s*CFG)?,\s*≥?(\d+)%")

# Roadmap v2 (ADR 0077, FND-15). Epic ids such as FND-1, FND-6b or LUA-11a; R-milestones R0..Rn
# from the ladder table under "### Roadmap v2"; the epics from Appendix C, one bullet each:
#   - **FND-5 Anti-fabrication lint and evidence matrix** (P0, 1.5u; after FND-1). Summary.
# A merged epic reads "(merged, 0u; ...)" and names the epics that absorbed it in its summary.
EPIC_PRIORITIES = ("P0", "P1", "P2", "P3", "merged")
_EPIC_ID = r"[A-Z]+-\d+[a-z]?"
_EPIC_ID_RE = re.compile(r"\b" + _EPIC_ID + r"\b")
_EPIC_LINE = re.compile(r"^- \*\*(" + _EPIC_ID + r") (.+?)\*\* \(([^,;]+),\s*([\d.]+)u;\s*after ([^)]*)\)\.?\s*(.*)$")
_R_HEADING = re.compile(r"^### (R\d+)\s*$")
_R_LADDER_ROW = re.compile(r"^\|\s*(R\d+)\s*\|[^|]*\|\s*[\d.]+\s*\|\s*[\d.]+\s*\|\s*$")
_R_SECTION = re.compile(r"^#### (R\d+):")
_EPICS_LINE = "**Epics** (Appendix C):"
# Appendix A's epic mapping: "| CPP-5 | 36, 37, 50 |" rows after "**Epic mapping**", and the rows
# no epic owns after "**Out of epic scope**...:".
_MAPPING_START = "**Epic mapping**"
_MAPPING_ROW = re.compile(r"^\|\s*(" + _EPIC_ID + r")\s*\|\s*(.*?)\s*\|\s*$")
_OUT_OF_SCOPE = re.compile(r"^\*\*Out of epic scope\*\*[^:]*:\s*(.*)$")
_ROW_REF = re.compile(r"\bI?\d+\b")


@dataclass(frozen=True)
class Problem:
    path: str
    line: int
    message: str

    def __str__(self) -> str:
        return f"{self.path}:{self.line}: {self.message}"


@dataclass
class Epic:
    """One Roadmap v2 epic from PLAN.md Appendix C."""

    id: str
    title: str
    milestone: str  # R0..R19
    priority: str  # P0..P3 or "merged"
    units: float
    after: list[str]  # epic ids it depends on
    owners: list[str]  # for a merged epic: the epics that absorbed it
    line: int


@dataclass
class Roadmap:
    """Roadmap v2: the R-ladder, the Appendix C epics and Appendix A's epic mapping."""

    ladder: list[str] = field(default_factory=list)  # R ids in run order
    epics: dict[str, Epic] = field(default_factory=dict)  # in Appendix C order
    mapping: dict[str, list[str]] = field(default_factory=dict)  # epic id -> Appendix A row ids
    out_of_scope: list[str] = field(default_factory=list)  # Appendix A row ids no epic owns


@dataclass
class Parsed:
    rows: list[dict]
    targets: dict[str, dict]
    problems: list[Problem]
    roadmap: Roadmap = field(default_factory=Roadmap)


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


def parse_run_order(lines: list[str], problems: list[Problem]) -> list[str]:
    """The milestone IDs in the order they run, from the ladder table that follows the
    '**Milestone ladder.**' paragraph (its rows are in run order, PLAN.md section 7). IDs
    are identities, not positions: M15-M17 run between M5 and M6 (ADR 0029). Appends a
    problem when the table is missing or does not list each of M0..M17 exactly once."""
    start = next((i for i, l in enumerate(lines) if l.startswith("**Milestone ladder.**")), None)
    if start is None:
        problems.append(Problem(PLAN_FILE, 1, "no '**Milestone ladder.**' table (the run order)"))
        return []
    order: list[str] = []
    for index in range(start + 1, len(lines)):
        line = lines[index]
        if line.startswith("#") or line.strip() == "---":
            break
        m = _LADDER_ROW.match(line)
        if not m:
            continue
        if not _is_milestone(m.group(1)):
            problems.append(Problem(PLAN_FILE, index + 1, f"the milestone ladder lists {m.group(1)}, "
                                    f"which is not a milestone (M{FIRST_MILESTONE}..M{LAST_MILESTONE})"))
        elif m.group(1) in order:
            problems.append(Problem(PLAN_FILE, index + 1, f"the milestone ladder lists {m.group(1)} twice"))
        else:
            order.append(m.group(1))
    missing = [f"M{n}" for n in range(FIRST_MILESTONE, LAST_MILESTONE + 1) if f"M{n}" not in order]
    if missing:
        problems.append(Problem(PLAN_FILE, start + 1, f"the milestone ladder does not list {', '.join(missing)}"))
    return order


def _section(lines: list[str], prefix: str) -> tuple[int, int] | None:
    """(start, end) line indexes of the '## '-level section whose heading starts with `prefix`."""
    start = next((i for i, l in enumerate(lines) if l.startswith(prefix)), None)
    if start is None:
        return None
    end = next((i for i in range(start + 1, len(lines)) if lines[i].startswith("## ")), len(lines))
    return start, end


def parse_r_ladder(lines: list[str], problems: list[Problem]) -> list[str]:
    """The R-milestones in run order from the table under '### Roadmap v2' (PLAN.md section 7)."""
    start = next((i for i, l in enumerate(lines) if l.startswith("### Roadmap v2")), None)
    if start is None:
        problems.append(Problem(PLAN_FILE, 1, "no '### Roadmap v2' section (the R-ladder)"))
        return []
    ladder: list[str] = []
    for index in range(start + 1, len(lines)):
        line = lines[index]
        if line.startswith(("#### ", "### ", "## ")):
            break
        m = _R_LADDER_ROW.match(line)
        if not m:
            continue
        if m.group(1) in ladder:
            problems.append(Problem(PLAN_FILE, index + 1, f"the R-ladder lists {m.group(1)} twice"))
        else:
            ladder.append(m.group(1))
    expected = [f"R{n}" for n in range(len(ladder))]
    if not ladder:
        problems.append(Problem(PLAN_FILE, start + 1, "the Roadmap v2 section has no R-ladder table"))
    elif ladder != expected:
        problems.append(Problem(PLAN_FILE, start + 1, f"the R-ladder must run R0..R{len(ladder) - 1} in order, "
                                f"not {', '.join(ladder)}"))
    return ladder


def parse_epics(lines: list[str], ladder: list[str], problems: list[Problem]) -> dict[str, Epic]:
    """The Appendix C epics (see _EPIC_LINE), checked: unique ids, known priorities, known
    dependencies, merged epics owned by live ones, and '### Rn' groups that match the R-ladder."""
    bounds = _section(lines, "## Appendix C")
    if bounds is None:
        problems.append(Problem(PLAN_FILE, 1, "no '## Appendix C' section (the Roadmap v2 epic catalogue)"))
        return {}
    start, end = bounds
    epics: dict[str, Epic] = {}
    headings: list[str] = []
    milestone = None
    for index in range(start + 1, end):
        line, number = lines[index], index + 1
        if line.startswith("### "):
            m = _R_HEADING.match(line)
            milestone = m.group(1) if m else None
            if m:
                headings.append(milestone)
            continue
        if milestone is None or not line.startswith("- **"):
            continue
        m = _EPIC_LINE.match(line)
        if not m:
            problems.append(Problem(PLAN_FILE, number, f"cannot parse Appendix C epic '{line[:60]}'"))
            continue
        epic_id, title, priority, units, after, summary = m.groups()
        if epic_id in epics:
            problems.append(Problem(PLAN_FILE, number, f"epic {epic_id} is listed twice (first on line "
                                    f"{epics[epic_id].line})"))
            continue
        if priority not in EPIC_PRIORITIES:
            problems.append(Problem(PLAN_FILE, number, f"epic {epic_id}: priority {priority!r} is not one of "
                                    f"{', '.join(EPIC_PRIORITIES)}"))
        depends = [] if after.strip() == "none" else [d.strip() for d in after.split(",")]
        owners = [i for i in dict.fromkeys(_EPIC_ID_RE.findall(summary)) if i != epic_id] \
            if priority == "merged" else []
        value = float(units)
        epics[epic_id] = Epic(epic_id, title.strip(), milestone, priority, int(value) if value.is_integer() else value,
                              depends, owners, number)
    for epic in epics.values():
        for dependency in epic.after:
            if dependency not in epics:
                problems.append(Problem(PLAN_FILE, epic.line, f"epic {epic.id} depends on unknown epic {dependency}"))
        if epic.priority == "merged":
            if not epic.owners:
                problems.append(Problem(PLAN_FILE, epic.line, f"merged epic {epic.id} names no epic that absorbed it"))
            for owner in epic.owners:
                if owner not in epics:
                    problems.append(Problem(PLAN_FILE, epic.line, f"merged epic {epic.id} names unknown epic {owner}"))
                elif epics[owner].priority == "merged":
                    problems.append(Problem(PLAN_FILE, epic.line,
                                            f"merged epic {epic.id} names {owner}, which is merged itself"))
    if ladder and headings != ladder:
        problems.append(Problem(PLAN_FILE, start + 1, f"Appendix C groups epics under {', '.join(headings) or 'nothing'}"
                                f", not the R-ladder's {', '.join(ladder)}"))
    _check_milestone_epic_lists(lines, epics, problems)
    return epics


def _check_milestone_epic_lists(lines: list[str], epics: dict[str, Epic], problems: list[Problem]) -> None:
    """Each '#### Rn:' section's '**Epics** (Appendix C):' list names exactly Rn's live epics."""
    milestone = None
    for index, line in enumerate(lines):
        m = _R_SECTION.match(line)
        if m:
            milestone = m.group(1)
            continue
        if milestone is None or not line.startswith(_EPICS_LINE):
            continue
        listed = [_EPIC_ID_RE.match(item.strip()) for item in line[len(_EPICS_LINE):].split(";")]
        ids = [found.group(0) for found in listed if found]
        for epic_id in ids:
            epic = epics.get(epic_id)
            if epic is None:
                problems.append(Problem(PLAN_FILE, index + 1, f"{milestone} lists unknown epic {epic_id}"))
            elif epic.milestone != milestone:
                problems.append(Problem(PLAN_FILE, index + 1,
                                        f"{milestone} lists {epic_id}, which Appendix C puts in {epic.milestone}"))
            elif epic.priority == "merged":
                problems.append(Problem(PLAN_FILE, index + 1, f"{milestone} lists merged epic {epic_id}"))
        missing = [e.id for e in epics.values() if e.milestone == milestone and e.priority != "merged" and
                   e.id not in ids]
        if missing:
            problems.append(Problem(PLAN_FILE, index + 1, f"{milestone}'s epic list lacks {', '.join(missing)}"))
        milestone = None


def parse_epic_mapping(lines: list[str], rows: list[dict], epics: dict[str, Epic],
                       problems: list[Problem]) -> tuple[dict[str, list[str]], list[str]]:
    """Appendix A's '**Epic mapping**' table and '**Out of epic scope**' list, checked: known and
    live epics, known rows, every Appendix A row mapped or out of scope (never both)."""
    bounds = _section(lines, "## Appendix A")
    if bounds is None:
        return {}, []
    start, end = bounds
    begin = next((i for i in range(start, end) if lines[i].startswith(_MAPPING_START)), None)
    if begin is None:
        problems.append(Problem(PLAN_FILE, start + 1, "Appendix A has no '**Epic mapping**' table (FND-15)"))
        return {}, []
    row_ids = {row["id"] for row in rows}
    mapping: dict[str, list[str]] = {}
    out_of_scope: list[str] = []
    out_line = None
    for index in range(begin + 1, end):
        line, number = lines[index], index + 1
        m = _OUT_OF_SCOPE.match(line)
        if m:
            out_line = number
            for ref in _ROW_REF.findall(m.group(1)):
                if ref not in row_ids:
                    problems.append(Problem(PLAN_FILE, number, f"out of epic scope: {ref} is not an Appendix A row"))
                elif ref in out_of_scope:
                    problems.append(Problem(PLAN_FILE, number, f"out of epic scope lists row {ref} twice"))
                else:
                    out_of_scope.append(ref)
            continue
        m = _MAPPING_ROW.match(line)
        if not m:
            continue
        epic_id, refs = m.group(1), _ROW_REF.findall(m.group(2))
        epic = epics.get(epic_id)
        if epic is None:
            problems.append(Problem(PLAN_FILE, number, f"epic mapping: unknown epic {epic_id} (not in Appendix C)"))
            continue
        if epic.priority == "merged":
            problems.append(Problem(PLAN_FILE, number, f"epic mapping: {epic_id} is merged into "
                                    f"{', '.join(epic.owners)}; map its rows there"))
            continue
        if epic_id in mapping:
            problems.append(Problem(PLAN_FILE, number, f"epic mapping lists {epic_id} twice"))
            continue
        if not refs:
            problems.append(Problem(PLAN_FILE, number, f"epic mapping: {epic_id} maps no rows"))
        for ref in refs:
            if ref not in row_ids:
                problems.append(Problem(PLAN_FILE, number, f"epic mapping: {epic_id} maps {ref}, which is not an "
                                        f"Appendix A row"))
        mapping[epic_id] = [ref for ref in refs if ref in row_ids]
    mapped = {ref for refs in mapping.values() for ref in refs}
    for row in rows:
        if row["id"] in mapped and row["id"] in out_of_scope:
            problems.append(Problem(PLAN_FILE, out_line or begin + 1,
                                    f"row {row['id']} is both mapped to an epic and out of epic scope"))
        elif row["id"] not in mapped and row["id"] not in out_of_scope:
            problems.append(Problem(PLAN_FILE, row["_line"], f"orphan row {row['id']} ({row['feature']}): map it to "
                                    f"an epic or list it as out of epic scope (Appendix A 'Epic mapping')"))
    return mapping, out_of_scope


def _check_completions(rows: list[dict], order: list[str], problems: list[Problem]) -> None:
    """A row's "(+Mx)" must name a milestone that runs after the row's owner."""
    for row in rows:
        completes, owner = row["completes"], row["milestone"]
        if completes is None:
            continue
        if not _is_milestone(completes):
            problems.append(Problem(PLAN_FILE, row["_line"], f"row {row['id']} completes in {completes}, which is "
                                    f"not a milestone (M{FIRST_MILESTONE}..M{LAST_MILESTONE})"))
        elif owner in order and completes in order and order.index(completes) <= order.index(owner):
            problems.append(Problem(PLAN_FILE, row["_line"],
                                    f"row {row['id']} completes in {completes}, which does not run after its "
                                    f"owner {owner} (PLAN.md section 7 order)"))


def parse_plan(text: str) -> Parsed:
    """Parses Appendix A (rows, totals), the run order from the milestone ladder and the
    section 6.1 inspection-count targets."""
    lines = text.splitlines()
    problems: list[Problem] = []
    rows: list[dict] = []
    order = parse_run_order(lines, problems)
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
    _check_completions(rows, order, problems)
    ladder = parse_r_ladder(lines, problems)
    epics = parse_epics(lines, ladder, problems)
    mapping, out_of_scope = parse_epic_mapping(lines, rows, epics, problems)
    rows.sort(key=lambda r: (r["kind"] != "feature", int(r["id"].lstrip("I"))))
    return Parsed(rows, _parse_targets(lines, problems), problems, Roadmap(ladder, epics, mapping, out_of_scope))


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
    roadmap = parsed.roadmap
    epics_of: dict[str, list[str]] = {}
    for epic_id in roadmap.epics:
        for ref in roadmap.mapping.get(epic_id, []):
            epics_of.setdefault(ref, []).append(epic_id)
    rows = []
    for row in parsed.rows:
        out = {k: v for k, v in row.items() if not k.startswith("_")}
        out["epics"] = epics_of.get(row["id"], [])
        old = old_rows.get((row["kind"], row["id"]), {})
        out["acceptance_test"] = old.get("acceptance_test")
        out["status"] = old.get("status", "planned")
        rows.append(out)
    totals = {
        "P0": sum(1 for r in rows if r["kind"] == "feature" and r["priority"] == "P0"),
        "P1": sum(1 for r in rows if r["kind"] == "feature" and r["priority"] == "P1"),
        "infra": sum(1 for r in rows if r["kind"] == "infra"),
    }
    owner_of = {row["id"]: row["milestone"] for row in rows}
    epics = []
    for epic in roadmap.epics.values():
        refs = roadmap.mapping.get(epic.id, [])
        m_ids = sorted({owner_of[ref] for ref in refs if owner_of.get(ref)}, key=lambda m: int(m[1:]))
        epics.append({"id": epic.id, "title": epic.title, "milestone": epic.milestone, "priority": epic.priority,
                      "units": epic.units, "after": epic.after, "merged_into": epic.owners, "rows": refs,
                      "m_ids": m_ids})
    return {
        "schemaVersion": 2,
        "description": ("FrostIDE feature matrix, generated from PLAN.md Appendix A and Appendix C by "
                        "`python tools/featurecheck.py generate`; `featurecheck.py check` (run by the verify "
                        "scripts) fails when it is out of date. Hand-maintained fields: acceptance_test (the "
                        "ctest name or scenario that proves the row), status (planned, in-progress, green, "
                        "deferred) and inspection_counts.s9_baseline; generate preserves them. Feature row IDs "
                        "are the Appendix A row numbers; infrastructure rows use the I prefix. 'milestone' "
                        "owns the row; 'completes' is the milestone where its last sub-part lands (+Mx); "
                        "'epics' are the Roadmap v2 epics that deliver it (Appendix A 'Epic mapping')."),
        "source": "PLAN.md Appendix A; inspection-count targets from PLAN.md section 6.1",
        "totals": totals,
        "rows": rows,
        "roadmap": {
            "description": ("Roadmap v2 (ADR 0077): the R-ladder from PLAN.md section 7, the epics of Appendix C "
                            "with the Appendix A rows each delivers and those rows' M-ladder milestones (m_ids; "
                            "empty for an epic with no Appendix A row, such as a process or engine epic), and the "
                            "rows no epic owns (out_of_epic_scope: foundation already built)."),
            "milestones": roadmap.ladder,
            "out_of_epic_scope": roadmap.out_of_scope,
            "epics": epics,
        },
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
        elif key == "roadmap":
            # One epic per line as well; the other roadmap fields on one line each.
            parts.append('  "roadmap": {')
            sub = list(value)
            for sub_index, sub_key in enumerate(sub):
                sub_comma = "," if sub_index < len(sub) - 1 else ""
                if sub_key == "epics":
                    parts.append('    "epics": [')
                    for epic_index, epic in enumerate(value[sub_key]):
                        epic_comma = "," if epic_index < len(value[sub_key]) - 1 else ""
                        parts.append("      " + json.dumps(epic, ensure_ascii=False) + epic_comma)
                    parts.append("    ]" + sub_comma)
                else:
                    parts.append(f"    {json.dumps(sub_key)}: {json.dumps(value[sub_key], ensure_ascii=False)}{sub_comma}")
            parts.append("  }" + comma)
        else:
            body = json.dumps(value, ensure_ascii=False, indent=2).replace("\n", "\n  ")
            parts.append(f"  {json.dumps(key)}: {body}{comma}")
    parts.append("}")
    return "\n".join(parts) + "\n"


def _row_line(matrix_text: str, row_id: str) -> int:
    position = matrix_text.find(f'"id": "{row_id}"')
    return 1 if position < 0 else matrix_text.count("\n", 0, position) + 1


def _check_row_epics(row: dict, row_id: str, line: int, roadmap: Roadmap) -> list[Problem]:
    """A matrix row's 'epics' name known, live Appendix C epics; a row without one is out of epic scope."""
    epics = row.get("epics")
    if not isinstance(epics, list) or not all(isinstance(e, str) for e in epics):
        return [Problem(MATRIX_FILE, line, f"row {row_id}: epics must be a list of Roadmap v2 epic ids")]
    out = []
    for epic_id in epics:
        epic = roadmap.epics.get(epic_id)
        if epic is None:
            out.append(Problem(MATRIX_FILE, line, f"row {row_id} references unknown epic {epic_id} "
                               f"(not in PLAN.md Appendix C)"))
        elif epic.priority == "merged":
            out.append(Problem(MATRIX_FILE, line, f"row {row_id} references merged epic {epic_id}; its owner is "
                               f"{', '.join(epic.owners)}"))
    if not epics and row_id not in roadmap.out_of_scope:
        out.append(Problem(MATRIX_FILE, line, f"orphan row {row_id}: no epic delivers it and Appendix A does not "
                           f"list it as out of epic scope"))
    return out


def check(repo: Path) -> list[Problem]:
    """The gate: Appendix A consistency, owning milestones, Roadmap v2 epics and their mapping,
    valid fields, an up-to-date matrix."""
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
        problems.extend(_check_row_epics(row, row_id, line, parsed.roadmap))
    expected = render(build_matrix(parsed, matrix))
    if expected != matrix_text.replace("\r\n", "\n"):
        old = {(r.get("kind"), str(r.get("id"))): r for r in matrix.get("rows", []) if isinstance(r, dict)}
        new = {(r["kind"], r["id"]): r for r in json.loads(expected)["rows"]}
        changed = sorted(k[1] for k in old.keys() | new.keys() if old.get(k) != new.get(k))
        detail = f" (rows {', '.join(changed[:10])}{', ...' if len(changed) > 10 else ''})" if changed else ""
        problems.append(Problem(MATRIX_FILE, 1,
                                f"out of date against PLAN.md Appendix A and C{detail}; "
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
