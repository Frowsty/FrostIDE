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
}


def unique_pairs(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate JSON key {key}")
        result[key] = value
    return result


def json_object(raw):
    data = json.loads(raw, object_pairs_hook=unique_pairs)
    if not isinstance(data, dict):
        raise ValueError("expected a JSON object")
    return data


def read_json(path):
    raw = path.read_bytes()
    return hashlib.sha256(raw).hexdigest(), json_object(raw)


def bound_bytes(root, entry):
    relative = Path(entry["file"])
    if relative.is_absolute() or relative.drive:
        raise ValueError("evidence path must be relative")
    path = (root / relative).resolve(strict=True)
    if not path.is_relative_to(root):
        raise ValueError("evidence path escapes its manifest directory")
    if not _HEX.fullmatch(entry["sha256"]):
        raise ValueError("evidence requires SHA-256")
    raw = path.read_bytes()
    if hashlib.sha256(raw).hexdigest() != entry["sha256"]:
        raise ValueError("evidence SHA-256 mismatch")
    return raw


def bound_json(root, entry):
    return json_object(bound_bytes(root, entry))


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


def validate_session(report):
    if report["session"].get("valid") is not True or (
        report.get("inputDesktop"), report.get("lockState"), report.get("displayPower")
    ) != ("Default", "unlocked", "on"):
        raise ValueError("measurement did not run on a valid interactive Windows console")
    session_id = report["processSession"]
    if type(session_id) is not int or not 0 < session_id < 0xffffffff or session_id != report["activeConsoleSession"]:
        raise ValueError("measurement did not run on a valid interactive Windows console")
    if not any(o.get("attached") is True and (o.get("width"), o.get("height"), o.get("refreshHz")) ==
               (5120, 1440, 144) for o in report["outputs"]):
        raise ValueError("reference display mode differs")


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
        validate_session(payload[key])
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


def reviewed_pair(root, entry, identity, environment, kind):
    if not isinstance(entry, dict) or not {"current", "baseline"} <= entry.keys():
        raise ValueError(f"{kind} requires a candidate receipt and an independently reviewed comparative baseline")
    candidate = bound_json(root, entry["current"])
    baseline = bound_json(root, entry["baseline"])
    for envelope in (candidate, baseline):
        if envelope.get("schemaVersion") != 1 or envelope.get("kind") != kind:
            raise ValueError(f"expected a reviewed {kind} receipt")
        if envelope.get("identity") != {**identity, "commit": envelope.get("identity", {}).get("commit")}:
            raise ValueError(f"{kind} receipt lock, corpus or budget manifest differs")
        conditions(envelope, environment)
    if candidate["identity"] != identity or not _COMMIT.fullmatch(baseline["identity"]["commit"]):
        raise ValueError(f"{kind} candidate source or baseline revision differs")
    if baseline["identity"]["commit"] == identity["commit"]:
        raise ValueError(f"{kind} baseline must identify a prior source revision")
    if timestamp(baseline["endUtc"]) > timestamp(candidate["startUtc"]):
        raise ValueError(f"{kind} baseline must end before candidate measurement")
    return candidate, baseline


def comparative(current, previous, limit, percentile):
    p50 = percentile(current, "p50")
    baseline = percentile(previous, "p50")
    u, probability = mann_whitney(current, previous)
    relative = (p50 - baseline) / baseline
    absolute_over = p50 > limit
    comparative_over = relative > 0.05 and probability < 0.01
    return {"state": "over" if absolute_over or comparative_over else "within",
            "currentP50": p50, "baselineP50": baseline, "u": u, "pValue": probability,
            "medianChange": relative, "absoluteExceeded": absolute_over, "comparativeExceeded": comparative_over}


def check_b14(root, entry, identity, environment, pin, budget, command, percentile, verify_revision):
    candidate, baseline = reviewed_pair(root, entry, identity, environment, "B14-native")
    for envelope in (candidate, baseline):
        verify_revision(envelope["identity"]["commit"], envelope["native"]["runnerSha256"])
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


_B28_INPUTS = {"core": "src/core/src/error.cpp", "main": "src/app/src/main.cpp", "sched": "src/sched/src/lane.cpp"}
_B28_OBJECTS = {
    "core": "src/core/CMakeFiles/frost_core.dir/Debug/src/error.cpp.obj",
    "main": "src/app/CMakeFiles/frost_app_shared.dir/Debug/src/main.cpp.obj",
    "sched": "src/sched/CMakeFiles/frost_sched.dir/Debug/src/lane.cpp.obj",
}
_B28_REBUILD = (("core", "frost"), ("core", "all"), ("main", "frost"), ("main", "all"), ("sched", "all"))
_B28_TOOLS = {"cmake", "ninja", "sccache", "clang_cl", "powershell", "wsl"}


def validate_b28(root, envelope, revision, runner_revision, verify_revision):
    raw = envelope["native"]
    run = bound_json(root, raw["conditions"])
    summary = bound_json(root, raw["summary"])
    log = [json.loads(line, object_pairs_hook=unique_pairs)
           for line in bound_bytes(root, raw["runs"]).splitlines()]
    proof = run["release"]
    if proof["kind"] != "B28-release" or proof.get("inputUnchanged") is not True:
        raise ValueError("B28 requires the release collection mode and uninterrupted input")
    if (timestamp(envelope["startUtc"]) > timestamp(proof["startUtc"]) or
        timestamp(envelope["endUtc"]) < timestamp(proof["endUtc"]) or
        timestamp(proof["startUtc"]) >= timestamp(proof["endUtc"])):
        raise ValueError("B28 native timing falls outside the reviewed interval")
    for key in ("sessionStart", "sessionEnd"):
        validate_session(proof[key])
    for key in ("initialCpuPct", "finalCpuPct"):
        value = proof[key]
        if type(value) not in (float, int) or not 0 <= value <= 5:
            raise ValueError("B28 native idle checkpoint did not pass")
    fingerprints = proof["toolsSha256"]
    tool_paths = {key: PureWindowsPath(value) for key, value in proof["toolsPath"].items()}
    if set(fingerprints) != _B28_TOOLS or set(tool_paths) != _B28_TOOLS or (
        any(not _HEX.fullmatch(item) for item in fingerprints.values()) or
        any(not path.is_absolute() or not re.fullmatch(r"[A-Za-z]:", path.drive) for path in tool_paths.values())
    ):
        raise ValueError("B28 native executable provenance is incomplete")
    if not _HEX.fullmatch(proof["sessionProbeSha256"]):
        raise ValueError("B28 native session probe provenance is incomplete")
    verify_revision(revision, proof["runnerSha256"], runner_revision)

    work = PureWindowsPath(run["repo"])
    build = work / "build" / "win-clangcl"
    if not work.is_absolute() or work.drive.upper() != "E:" or ".." in work.parts or (
        build != PureWindowsPath(run["build_dir"])
    ):
        raise ValueError("B28 requires the REF-W1 E: checkout and its default build tree")
    if (run["schemaVersion"], run["budget"], run["runs"], run["warmup"], run["variants"], run["files"]) != (
        1, "B28", 20, 1, ["rebuild", "verify"], _B28_INPUTS
    ) or run["git"]["sha"] != revision or run["git"]["status_porcelain"] or run["verify_base_head"] is not True:
        raise ValueError("B28 native source or method differs from the release workload")
    if summary["schemaVersion"] != 1 or summary["budget"] != "B28" or summary["valid"] is not True or (
        summary["invalid_reason"] is not None or set(summary["variants"]) != {"rebuild", "verify"}
    ):
        raise ValueError("B28 native run is invalid or incomplete")
    commands = run["commands"]
    for key in ("settle", "rebuild_all", "rebuild_frost"):
        command = commands[key]
        if PureWindowsPath(command[0]) != tool_paths["cmake"] or (
            len(command) != (7 if key == "rebuild_frost" else 5)
        ) or command[1] != "--build" or PureWindowsPath(command[2]) != build or (
            command[3:] != ["--config", "Debug"] + (["--target", "frost"] if key == "rebuild_frost" else [])
        ):
            raise ValueError("B28 collect command is not the normal default Debug build")
    verify = commands["verify"]
    if len(verify) != 11 or PureWindowsPath(verify[0]) != tool_paths["powershell"] or (
        verify[1:4] != ["-NoProfile", "-ExecutionPolicy", "Bypass"] or verify[4] != "-File"
    ) or PureWindowsPath(verify[5]) != work / "tools" / "verify.ps1" or verify[6:] != [
        "-Quick", "-Jobs", "4", "-Base", "HEAD"
    ]:
        raise ValueError("B28 verify command differs from the fixed Quick workload")

    # Fixed chronological sequence guards against cherry-picking, mixed probe groups and reused records.
    expected = [(r, key, target, "rebuild") for r in range(21) for key, target in _B28_REBUILD]
    expected.extend((r, ("core", "main")[r % 2], "verify", "verify") for r in range(21))
    if len(log) != len(expected):
        raise ValueError("B28 receipt does not have every warmup and counted sample")
    samples = defaultdict(list)
    probes = set()
    for item, (index, key, target, variant) in zip(log, expected, strict=True):
        verify_sample = variant == "verify"
        native = item["b28"]
        position = item["run"] if verify_sample else item["round"]
        elapsed = item["value"]
        if (item["schemaVersion"], item["budget"], item["variant"], item["file"], position, item["unit"]) != (
            1, "B28", variant, key, index, "s"
        ) or item["counted"] is not (index > 0) or item["valid"] is not True or item["exit"] != 0 or (
            type(elapsed) not in (int, float) or not math.isfinite(elapsed) or elapsed <= 0 or item["wall_s"] != elapsed
        ):
            raise ValueError("B28 has a mismatched, failed or invalid timed sample")
        probe = native["probe"]
        if type(probe) is not int or probe <= 0 or probe in probes or native["settle"]["exit"] != 0:
            raise ValueError("B28 has a repeated probe or failed settle")
        probes.add(probe)
        cache = native["sccache"]
        if type(cache["misses"]) is not int or cache["misses"] <= 0 or cache["fails"] != 0 or cache["errors"] != 0:
            raise ValueError("B28 does not prove a clean sccache miss")
        outputs = [PureWindowsPath(edge["output"]) for edge in native["ninja_log_edges"]]
        required = build / _B28_OBJECTS[key]
        objects = {build / output for output in outputs if output.suffix.lower() == ".obj"}
        if required not in objects or not any(output.suffix.lower() in {".exe", ".dll", ".lib"} for output in outputs):
            raise ValueError("B28 did not record the expected native compilation and linking")
        if verify_sample:
            if native["command"] != verify or not (item["summary_line"] or "").startswith("Verified:"):
                raise ValueError("B28 verify sample lacks its exact successful Quick command")
        elif native["target"] != target or objects != {required}:
            raise ValueError("B28 rebuild did not measure only the selected translation unit")
        if index and target != "frost":
            samples["verify" if verify_sample else f"{key}/all"].append(elapsed)
    return samples, fingerprints


def check_b28(root, entry, identity, environment, budget, percentile, verify_revision):
    candidate, baseline = reviewed_pair(root, entry, identity, environment, "B28-native")
    current, tools = validate_b28(root, candidate, candidate["identity"]["commit"], identity["commit"], verify_revision)
    previous, predecessor_tools = validate_b28(root, baseline, baseline["identity"]["commit"], identity["commit"],
                                               verify_revision)
    if tools != predecessor_tools:
        raise ValueError("B28 baseline used different executable tools")
    targets = {target["variant"]: target for target in budget["targets"]}
    result = {}
    for family in ("core/all", "main/all", "sched/all", "verify"):
        target = targets["verify -Quick" if family == "verify" else "one-.cpp rebuild"]
        if target["percentile"] != "p50" or target["unit"] not in ("s", "min"):
            raise ValueError("B28 budget targets or units differ")
        limit = target["max"] * (60 if target["unit"] == "min" else 1)
        result[family] = comparative(current[family], previous[family], limit, percentile)
    return {"state": "over" if any(value["state"] == "over" for value in result.values()) else "within",
            "series": result}


def run_full(rows, identity, pin, manifest_path, manifest_sha, command, percentile, measure_b27, verify_revision):
    if sorted(row["id"] for row in rows) != sorted(f"B{i}" for i in range(1, 29)):
        raise ValueError("full performance run requires exactly the 28 registered budgets")
    outcomes = {row["id"]: {"state": "blocked", "metric": row["metric"], "requiredMethod": row["method"],
                            "reason": _COMPONENTS.get(row["id"], "No release-qualified instrument is wired.")}
                for row in rows}
    for key in ("B14", "B27", "B28"):
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
    for key in ("B14", "B27", "B28"):
        if key not in inputs:
            continue
        try:
            if key == "B14":
                item = check_b14(root, inputs[key], identity, environment_entry["sha256"], pin,
                                 by_id[key], command, percentile, verify_revision)
            elif key == "B28":
                item = check_b28(root, inputs[key], identity, environment_entry["sha256"], by_id[key],
                                 percentile, verify_revision)
            else:
                item = measure_b27(root, inputs[key], identity)
            outcomes[key].pop("reason")
            outcomes[key].update(item)
        except (OSError, ValueError, TypeError, KeyError, AttributeError, OverflowError) as error:
            outcomes[key].update(state="invalid", reason=str(error))
    return outcomes
