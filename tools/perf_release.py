# SPDX-License-Identifier: Apache-2.0
#
# Validate independently anchored release performance receipts without weakening the live budgets.

from collections import defaultdict
from datetime import datetime
import hashlib
import json
import math
from pathlib import Path, PureWindowsPath
import re


_HEX = re.compile(r"[0-9a-f]{64}")
_COMMIT = re.compile(r"[0-9a-f]{40}")
_COMPONENTS = {
    "B1": "Only warm empty-workspace screen capture exists. R3, cold and ETW correlation are missing.",
    "B7": "Only the Go to File viewport paint component exists. Full R3 and R4-XL receipts are missing.",
    "B28": "The standalone build runner exists. Full-run receipt admission and baseline matching are missing.",
}


def read_json(path):
    def object_pairs(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError(f"duplicate JSON key {key}")
            result[key] = value
        return result

    raw = path.read_bytes()
    data = json.loads(raw, object_pairs_hook=object_pairs)
    if not isinstance(data, dict):
        raise ValueError("expected a JSON object")
    return hashlib.sha256(raw).hexdigest(), data


def bound_json(root, entry):
    relative = Path(entry["file"])
    if relative.is_absolute() or relative.drive:
        raise ValueError("evidence path must be relative")
    path = (root / relative).resolve(strict=True)
    if not path.is_relative_to(root):
        raise ValueError("evidence path escapes its manifest directory")
    if not _HEX.fullmatch(entry["sha256"]):
        raise ValueError("evidence requires SHA-256")
    digest, data = read_json(path)
    if digest != entry["sha256"]:
        raise ValueError("evidence SHA-256 mismatch")
    return data


def timestamp(value):
    parsed = datetime.fromisoformat(value)
    if parsed.tzinfo is None:
        raise ValueError("timestamps need a UTC offset")
    return parsed


def conditions(data, environment):
    if data["environmentSha256"] != environment:
        raise ValueError("measurement environment differs from the reviewed REF-W1 environment")
    if any(data.get(key) is not True for key in ("precheckStart", "precheckEnd", "inputUnchanged", "quiet")):
        raise ValueError("precheck, input or quiet-machine evidence is absent or invalid")
    if data.get("concurrentBuilds") is not False:
        raise ValueError("measurement was contended or build contention was not checked")
    if timestamp(data["endUtc"]) <= timestamp(data["startUtc"]):
        raise ValueError("invalid measurement time range")


def validate_b14(payload, pin, identity, command):
    if (payload.get("budget"), payload.get("corpus"), payload.get("corpusCommit"), payload.get("scope")) != (
        "B14", "R3", pin, "absolute-budget-only"
    ) or payload.get("gitCommand") != list(command):
        raise ValueError("B14 requires the native R3 fsmonitor workload")
    if payload.get("budgetManifestSha256") != identity["budgetsSha256"]:
        raise ValueError("B14 native budget manifest differs")
    if payload.get("corpusManifestSha256") != identity["corporaSha256"]:
        raise ValueError("B14 native corpus manifest differs")
    if payload.get("state") not in ("within", "over") or payload.get("inputUnchanged") is not True:
        raise ValueError("B14 did not complete without input")
    work = PureWindowsPath(payload["workTree"])
    if not work.is_absolute() or work.drive.upper() != "E:" or ".." in work.parts or len(work.parts) < 2:
        raise ValueError("B14 fixture must be on the REF-W1 E: volume")
    for key in ("gitSha256", "gitRuntimeSha256", "sessionProbeSha256", "runnerSha256", "indexSha256", "statusSha256"):
        if not _HEX.fullmatch(payload[key]):
            raise ValueError(f"B14 lacks {key}")
    expected = f"# branch.oid {pin}\0# branch.head (detached)\0".encode()
    if payload["statusSha256"] != hashlib.sha256(expected).hexdigest():
        raise ValueError("B14 status digest differs from the pristine pinned corpus")
    for key in ("sessionStart", "sessionEnd"):
        report = payload[key]
        if report["session"].get("valid") is not True or (
            report.get("inputDesktop"), report.get("lockState"), report.get("displayPower")
        ) != ("Default", "unlocked", "on"):
            raise ValueError("B14 did not run on a valid interactive Windows console")
        session_id = report["processSession"]
        if type(session_id) is not int or not 0 < session_id < 0xffffffff or session_id != report["activeConsoleSession"]:
            raise ValueError("B14 did not run on a valid interactive Windows console")
        if not any(o.get("attached") is True and (o.get("width"), o.get("height"), o.get("refreshHz")) ==
                   (5120, 1440, 144) for o in report["outputs"]):
            raise ValueError("B14 reference display mode differs")
    for key in ("fsmonitorStart", "fsmonitorEnd"):
        witness = payload[key]
        if not _HEX.fullmatch(witness["traceSha256"]) or type(witness["responseBytes"]) is not int or (
            witness["responseBytes"] < 1 or type(witness["appliedChanges"]) is not int or witness["appliedChanges"] < 0
        ):
            raise ValueError("B14 lacks a valid incremental fsmonitor witness")
    samples = payload["samplesMs"]
    if not isinstance(samples, list) or len(samples) != 20 or any(
        type(x) not in (float, int) or not math.isfinite(x) or x <= 0 for x in samples
    ):
        raise ValueError("B14 requires exactly 20 positive finite measured samples")
    return samples


def mann_whitney(left, right):
    """Two-sided exact permutation p-value of U, with midranks for ties."""
    n = len(left)
    pooled = sorted((value, group) for group, values in enumerate((left, right)) for value in values)
    distribution = [defaultdict(int) for _ in range(n + 1)]
    distribution[0][0] = 1
    observed = 0
    i = 0
    while i < len(pooled):
        j = i + 1
        while j < len(pooled) and pooled[i][0] == pooled[j][0]:
            j += 1
        # Ranks are doubled to make midranks integral for ties.
        rank = i + 1 + j
        observed += rank * sum(group == 0 for _, group in pooled[i:j])
        updated = [defaultdict(int) for _ in range(n + 1)]
        for count in range(min(i, n) + 1):
            for taken in range(min(j - i, n - count) + 1):
                choices = math.comb(j - i, taken)
                for score, ways in distribution[count].items():
                    updated[count + taken][score + taken * rank] += ways * choices
        distribution = updated
        i = j
    center = n * (len(pooled) + 1)
    distance = abs(observed - center)
    extreme = sum(ways for score, ways in distribution[n].items() if abs(score - center) >= distance)
    return (observed - n * (n + 1)) / 2, extreme / math.comb(len(pooled), n)


def check_b14(root, entry, identity, environment, pin, budget, command, percentile, verify_revision):
    if not isinstance(entry, dict) or not {"current", "baseline"} <= entry.keys():
        raise ValueError("B14 requires both a candidate receipt and an independently reviewed comparative baseline")
    candidate = bound_json(root, entry["current"])
    baseline = bound_json(root, entry["baseline"])
    for envelope in (candidate, baseline):
        if envelope.get("schemaVersion") != 1 or envelope.get("kind") != "B14-native":
            raise ValueError("expected a reviewed B14-native receipt")
        if envelope.get("identity") != {**identity, "commit": envelope.get("identity", {}).get("commit")}:
            raise ValueError("B14 receipt lock, corpus or budget manifest differs")
        conditions(envelope, environment)
    if candidate["identity"] != identity or not _COMMIT.fullmatch(baseline["identity"]["commit"]):
        raise ValueError("B14 candidate source or baseline revision differs")
    if baseline["identity"]["commit"] == identity["commit"]:
        raise ValueError("B14 baseline must identify a prior source revision")
    for envelope in (candidate, baseline):
        verify_revision(envelope["identity"]["commit"], envelope["native"]["runnerSha256"])
    if timestamp(baseline["endUtc"]) > timestamp(candidate["startUtc"]):
        raise ValueError("B14 baseline must end before candidate measurement")
    current = validate_b14(candidate["native"], pin, identity, command)
    previous = validate_b14(baseline["native"], pin, identity, command)
    if any(candidate["native"][key] != baseline["native"][key] for key in ("gitSha256", "gitRuntimeSha256")):
        raise ValueError("B14 baseline used different Git binaries")
    stats = {key: percentile(current, key) for key in ("p50", "p99")}
    base_p50 = percentile(previous, "p50")
    u, probability = mann_whitney(current, previous)
    relative = (stats["p50"] - base_p50) / base_p50
    absolute_over = any(stats[target["percentile"]] > target["max"] for target in budget["targets"])
    comparative_over = relative > 0.05 and probability < 0.01
    return {"state": "over" if absolute_over or comparative_over else "within",
            "currentMs": stats, "baselineP50Ms": base_p50, "u": u, "pValue": probability,
            "medianChange": relative, "absoluteExceeded": absolute_over, "comparativeExceeded": comparative_over}


def run_full(rows, identity, pin, manifest_path, manifest_sha, command, percentile, measure_b27, verify_revision):
    if sorted(row["id"] for row in rows) != sorted(f"B{i}" for i in range(1, 29)):
        raise ValueError("full performance run requires exactly the 28 registered budgets")
    outcomes = {row["id"]: {"state": "blocked", "metric": row["metric"], "requiredMethod": row["method"],
                            "reason": _COMPONENTS.get(row["id"], "No release-qualified instrument is wired.")}
                for row in rows}
    for key in ("B14", "B27"):
        outcomes[key]["reason"] = "Independently reviewed measurement inputs are required."
    if manifest_path is None:
        return outcomes
    root = manifest_path.resolve(strict=True).parent
    digest, manifest = read_json(manifest_path)
    if not _HEX.fullmatch(manifest_sha) or digest != manifest_sha:
        raise ValueError("full manifest differs from its independent SHA-256 anchor")
    if manifest.get("schemaVersion") != 1 or manifest.get("identity") != identity:
        raise ValueError("full manifest source, toolchain lock, budgets or corpus lock differs")
    environment_entry = manifest["environment"]
    environment = bound_json(root, environment_entry)
    if (environment["profile"], environment["os"], environment["logicalThreads"], environment["memoryGiB"],
        environment["gpu"], environment["display"]) != (
        "REF-W1", "windows", 24, 32, "NVIDIA GeForce RTX 4070 SUPER", [5120, 1440, 144]
    ):
        raise ValueError("reviewed environment does not match the REF-W1 hardware profile")
    if not isinstance(manifest["reviewer"], str) or not manifest["reviewer"].strip():
        raise ValueError("full manifest requires an independent reviewer")
    inputs = manifest["budgets"]
    if not isinstance(inputs, dict) or not inputs.keys() <= outcomes.keys():
        raise ValueError("full manifest contains an unknown budget")
    by_id = {row["id"]: row for row in rows}
    for key in ("B14", "B27"):
        if key not in inputs:
            continue
        try:
            if key == "B14":
                item = check_b14(root, inputs[key], identity, environment_entry["sha256"], pin,
                                 by_id[key], command, percentile, verify_revision)
            else:
                item = measure_b27(root, inputs[key], identity)
            outcomes[key].pop("reason")
            outcomes[key].update(item)
        except (OSError, ValueError, TypeError, KeyError, AttributeError, OverflowError) as error:
            outcomes[key].update(state="invalid", reason=str(error))
    return outcomes
