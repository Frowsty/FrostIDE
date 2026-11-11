# SPDX-License-Identifier: Apache-2.0
# Supplemental Windows runner for the three existing bounded ProjectOpenProcess Unity opt-ins.
# Launch only after independent guard review and a serialized runtime grant. No activation/downloads.
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import sys
import time

from windows_fixture_guard import (Dpapi, GuardError, OwnedJob, PreferenceGuard, WindowsPreferences,
                                   durable_write, restrict_current_user, windows_only)

REPO = Path(__file__).resolve().parents[2]
EDITOR_VERSION = "6000.6.4f1"
INPUT_HASHES = {
    "Unity.exe": "eaba9cb7a7fcfa922c439bf85e60a4e5c3e58215a14cdb53e820d1e9c0ad638f",
    "Data/MonoBleedingEdge/bin/mono.exe": "6e5dcf9a3f8c8d8a32ac83578e0185fc74982479520428829b3ea060f234a06c",
    "Data/MonoBleedingEdge/lib/mono/4.5/csc.exe": "7228085c2028c2426f8ae4272c9ba7efdcaa76b08b8ea8ea9168c203bb0bcb15",
}
CASES = {
    "mono": "ProjectOpenProcess.UnityMonoClientAnnouncesThroughRealBroker",
    "bridge": "ProjectOpenProcess.UnityEditorBridgeReusesDirtyScriptAtLocation",
    "controls": "ProjectOpenProcess.UnityEditorDrivesProductionControls",
}
CHECKPOINTS = ("initial", "playing", "paused", "stepped", "resumed", "stopped", "revoked")


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_json(path: Path, data: dict) -> None:
    durable_write(path, (json.dumps(data, indent=2, ensure_ascii=True) + "\n").encode("ascii"))


def fixture_source() -> str:
    # The real consumer adds its unsaved /*é*/ prefix to this third line only.
    return ("public class FrostUnityProbe : UnityEngine.MonoBehaviour {\n"
            "    private void Update() {}\n"
            "    // \U0001f600 " + "x" * 96 + "\n}\n")


def validate_fixture_position(source: str) -> None:
    line = "/*é*/" + source.splitlines()[2]
    units = 0
    prefix = ""
    for character in line:
        if units == 39:  # Unity's one-based UTF-16 column40.
            break
        prefix += character
        units += len(character.encode("utf-16-le")) // 2
    if units != 39 or len(prefix.encode("utf-8")) != 42:
        raise GuardError("Scratch script does not satisfy the existing UTF-16 bridge oracle")


def prepare_project(project: Path, observer: bool) -> None:
    (project / "Assets/Probe").mkdir(parents=True)
    (project / "Assets/Editor").mkdir()
    (project / "ProjectSettings").mkdir()
    (project / "Packages").mkdir()
    source = fixture_source()
    validate_fixture_position(source)
    (project / "Assets/Probe/FrostUnityProbe.cs").write_text(source, encoding="utf-8", newline="")
    (project / "ProjectSettings/ProjectVersion.txt").write_text(
        f"m_EditorVersion: {EDITOR_VERSION}\n", encoding="utf-8")
    write_json(project / "Packages/manifest.json", {"dependencies": {}})
    package = REPO / "integrations/unity/com.frostide.editor"
    if any(path.is_symlink() for path in package.rglob("*")):
        raise GuardError("Fixture package contains an unexpected linked input")
    shutil.copytree(package, project / "Packages/com.frostide.editor")
    shutil.copyfile(REPO / "tests/unity/FrostBridgeScratchProbe.cs",
                    project / "Assets/Editor/FrostBridgeScratchProbe.cs")
    if observer:
        shutil.copyfile(REPO / "tests/unity/FrostControlObserver.cs",
                        project / "Assets/Editor/FrostControlObserver.cs")


def source_manifest(project: Path) -> dict[str, str]:
    result = {}
    for directory in ("Assets", "Packages"):
        for path in sorted((project / directory).rglob("*")):
            if path.is_file():
                if path.is_symlink():
                    raise GuardError("Scratch source identity changed to a link")
                result[path.relative_to(project).as_posix()] = digest(path)
    return result


def validate_editor(editor: Path) -> dict[str, str]:
    if editor.name != "Unity.exe" or editor.parent.parent.name != EDITOR_VERSION:
        raise GuardError("The fixture requires the reviewed Unity6000.6.4f1 installation")
    actual = {}
    for relative, expected in INPUT_HASHES.items():
        path = editor.parent / relative
        if not path.is_file() or digest(path) != expected:
            raise GuardError("Installed Unity fixture input differs from the recorded6000.6.4f1 input")
        actual[str(path)] = expected
    return actual


def base_environment(directory: Path) -> dict[str, str]:
    environment = os.environ.copy()
    # Preserve HOME/USERPROFILE for the existing entitlement. These do not isolate HKCU EditorPrefs.
    for name, relative in (("APPDATA", "profile/appdata"), ("LOCALAPPDATA", "profile/localappdata"),
                           ("DOTNET_CLI_HOME", "profile/dotnet"), ("NUGET_PACKAGES", "profile/nuget")):
        path = directory / relative
        path.mkdir(parents=True, exist_ok=True)
        environment[name] = str(path)
    for key in tuple(environment):
        if key.startswith("FROST_TEST_UNITY_") or key.startswith("FROST_UNITY_"):
            del environment[key]
    environment["QT_QPA_PLATFORM"] = "offscreen"
    environment["DOTNET_CLI_TELEMETRY_OPTOUT"] = "1"
    environment["DOTNET_SKIP_FIRST_TIME_EXPERIENCE"] = "1"
    return environment


def run_owned(guard, arguments: list[str], environment: dict[str, str], directory: Path,
              log: Path, timeout: float) -> int:
    job = OwnedJob(guard)
    job.start(arguments, environment, directory, log)
    code = job.wait(timeout)
    job.wait_empty(15)
    if code == 198:
        raise GuardError("Unity entitlement is unavailable in the inherited profile; no activation attempted")
    if code != 0:
        raise GuardError("Owned fixture command failed; inspect its private process log")
    return code


def check_test_log(path: Path, case: str) -> None:
    text = path.read_text(encoding="utf-8", errors="replace")
    if f"[       OK ] {case}" not in text or "[  SKIPPED ]" in text or "[  FAILED  ]" in text:
        raise GuardError("The exact Unity opt-in did not execute successfully")


def wait_file(path: Path, job, guard, timeout: float) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        guard.check_writers()
        if path.is_file():
            return
        if job.poll() is not None:
            raise GuardError("The fixture exited before its actual broker-ready marker")
        time.sleep(0.02)
    raise GuardError("The fixture did not publish its broker-ready marker")


def validate_observations(exchange: Path) -> dict:
    observations = {}
    for checkpoint in CHECKPOINTS:
        value = json.loads((exchange / checkpoint).read_text(encoding="utf-8"))
        if value.get("checkpoint") != checkpoint:
            raise GuardError("Actual Editor observer checkpoint identity differs")
        for name in ("playing", "paused"):
            if type(value.get(name)) is not bool:
                raise GuardError("Actual Editor observer lacks a typed state")
        for name in ("frame", "generation"):
            if type(value.get(name)) is not int or value[name] < 0:
                raise GuardError("Actual Editor observer lacks a typed frame/generation")
        observations[checkpoint] = value
    if (observations["initial"]["playing"] or not observations["playing"]["playing"]
            or observations["playing"]["generation"] <= observations["initial"]["generation"]
            or not observations["paused"]["paused"]
            or observations["stepped"]["frame"] <= observations["paused"]["frame"]
            or observations["resumed"]["paused"] or observations["stopped"]["playing"]
            or observations["revoked"]["playing"]):
        raise GuardError("Actual Editor observations contradict an existing control oracle")
    records = [json.loads(line)["checkpoint"] for line in
               (exchange / "workbench-assertions.jsonl").read_text(encoding="utf-8").splitlines()]
    if records != list(CHECKPOINTS):
        raise GuardError("The production Workbench did not record all seven exact checkpoints")
    return observations


def run_mono(args, guard, directory: Path, environment: dict[str, str]) -> dict:
    mono = args.unity_editor.parent / "Data/MonoBleedingEdge/bin/mono.exe"
    compiler = args.unity_editor.parent / "Data/MonoBleedingEdge/lib/mono/4.5/csc.exe"
    framework = args.unity_editor.parent / "Data/NetStandard/ref/2.1.0/netstandard.dll"
    client = directory / "UnityMonoBrokerClient.exe"
    sources = [REPO / "integrations/unity/com.frostide.editor/Editor" / name
               for name in ("SafeControlFile.cs", "UnityBrokerClient.cs")]
    sources.append(REPO / "tests/unity/UnityMonoBrokerClient.cs")
    compile_arguments = [str(mono), str(compiler), "/nologo", "/noconfig", "/nostdlib+", "/target:exe",
                         "/out:" + str(client), "/reference:" + str(framework), *(str(path) for path in sources)]
    run_owned(guard, compile_arguments, environment, directory, directory / "compiler.log", 60)
    if not client.is_file():
        raise GuardError("The actual Unity Mono compiler produced no client executable")
    current = environment.copy()
    current["FROST_TEST_UNITY_MONO"] = str(mono)
    current["FROST_TEST_UNITY_MONO_CLIENT"] = str(client)
    log = directory / "test.log"
    run_owned(guard, [str(args.test_executable), "--gtest_filter=" + CASES["mono"]],
              current, directory, log, 30)
    check_test_log(log, CASES["mono"])
    return {"case": CASES["mono"], "compilerArguments": compile_arguments,
            "frameworkSha256": digest(framework), "clientSha256": digest(client), "exitCode": 0}


def run_editor_case(args, guard, directory: Path, environment: dict[str, str], workflow: str) -> dict:
    project = directory / "Project space é"
    prepare_project(project, workflow == "controls")
    current = environment.copy()
    current["FROSTIDE_EXECUTABLE"] = str(args.frost_executable)
    if workflow == "controls":
        exchange = directory / "exchange"
        exchange.mkdir()
        current["FROST_UNITY_EXCHANGE"] = str(exchange)
        current["FROST_UNITY_FIXTURE_PROJECT"] = str(project)
    # Preparation is outside the original consumer deadlines and remains separately evidenced.
    run_owned(guard, [str(args.unity_editor), "-batchmode", "-nographics", "-quit", "-job-worker-count", "2",
                      "-projectPath", str(project), "-logFile", str(directory / "unity-import.log")],
              current, directory, directory / "import-process.log", args.import_timeout)
    for assembly in ("FrostIDE.UnityIntegration.Editor.dll", "Assembly-CSharp-Editor.dll"):
        path = project / "Library/ScriptAssemblies" / assembly
        if not path.is_file() or path.stat().st_size == 0:
            raise GuardError("Unity did not compile the actual package and fixture observer/probe")
    before = source_manifest(project)
    current["FROST_TEST_UNITY_EDITOR"] = str(args.unity_editor)
    current["FROST_TEST_UNITY_PROJECT"] = str(project)
    current["FROST_TEST_UNITY_PROFILE"] = str(directory / "profile")
    case = CASES[workflow]
    log = directory / "test.log"
    if workflow == "bridge":
        run_owned(guard, [str(args.test_executable), "--gtest_filter=" + case], current, directory, log, 125)
        observations = None
    else:
        current["FROST_TEST_UNITY_EXCHANGE"] = str(exchange)
        test = OwnedJob(guard)
        test.start([str(args.test_executable), "--gtest_filter=" + case], current, directory, log)
        wait_file(exchange / "broker-ready", test, guard, 30)
        editor = OwnedJob(guard)
        editor.start([str(args.unity_editor), "-job-worker-count", "2", "-projectPath", str(project),
                      "-logFile", str(directory / "unity-controls.log")],
                     current, directory, directory / "editor-process.log")
        try:
            # Outer containment exceeds the sum of unchanged consumer deadlines (480s).
            if test.wait(600) != 0:
                raise GuardError("Production Workbench controls failed; inspect the private test log")
            test.wait_empty(15)
            observations = validate_observations(exchange)
        finally:
            durable_write(exchange / "quit-request", b"quit owned fixture\n")
            if editor.wait(30) != 0:
                raise GuardError("Owned Unity Editor did not exit normally")
            editor.wait_empty(15)
    check_test_log(log, case)
    after = source_manifest(project)
    if after != before:
        raise GuardError("Scratch source or metadata changed during acceptance")
    return {"case": case, "exitCode": 0, "sourceAndMetaUnchanged": True,
            "sourceManifest": before, "observations": observations}


def execute(args) -> dict:
    windows_only()
    for path in (args.unity_editor, args.test_executable, args.frost_executable, args.scratch):
        if not path.is_absolute():
            raise GuardError("Every fixture input and scratch destination must be absolute")
    if not args.test_executable.is_file() or not args.frost_executable.is_file():
        raise GuardError("Matching built test and FrostIDE executables are required")
    if not math.isfinite(args.import_timeout) or args.import_timeout <= 0:
        raise GuardError("The scratch import timeout must be positive and finite")
    inputs = validate_editor(args.unity_editor)
    # Fresh destination, including a non-linked parent, so no existing project/profile is reused.
    if args.scratch.exists() or args.scratch.parent.resolve() != args.scratch.parent:
        raise GuardError("A fresh physical scratch destination is required")
    args.scratch.mkdir()
    restrict_current_user(args.scratch)
    evidence = {"schemaVersion": 1, "status": "failed", "cases": [], "inputs": inputs,
                "testExecutableSha256": digest(args.test_executable),
                "frostExecutableSha256": digest(args.frost_executable),
                "scope": "Three legacy Windows opt-ins; no broader Unity or Linux claim",
                "preferenceAttribution": "Polling cannot exclude short-lived or same-value external edits"}
    guard = PreferenceGuard(args.scratch, WindowsPreferences(), Dpapi())
    failure = None
    completed = False
    try:
        guard.begin()
        workflows = CASES if args.workflow == "all" else (args.workflow,)
        for workflow in workflows:
            directory = args.scratch / workflow
            directory.mkdir()
            environment = base_environment(directory)
            if workflow == "mono":
                result = run_mono(args, guard, directory, environment)
            else:
                result = run_editor_case(args, guard, directory, environment, workflow)
            evidence["cases"].append(result)
        completed = True
    except BaseException as error:
        failure = error
        # Exception payloads may contain registry values or credentials from provider internals.
        evidence["failureType"] = type(error).__name__
        evidence["interrupted"] = isinstance(error, (KeyboardInterrupt, SystemExit))
        if isinstance(error, GuardError):
            evidence["failureReason"] = str(error)
    finally:
        cleanup_errors = []
        for job in guard.jobs:
            try:
                job.terminate()
            except Exception as error:
                cleanup_errors.append(type(error).__name__)
        try:
            evidence["ownedJobsEmpty"] = all(job.empty() for job in guard.jobs)
            if guard.baseline_durable:
                evidence["preferences"] = guard.restore()
        except Exception as error:
            cleanup_errors.append(type(error).__name__)
        for job in guard.jobs:
            try:
                job.close()
            except Exception as error:
                cleanup_errors.append(type(error).__name__)
        if cleanup_errors:
            evidence["cleanupFailures"] = cleanup_errors
        expected_cases = len(CASES) if args.workflow == "all" else 1
        evidence["status"] = ("passed" if completed and len(evidence["cases"]) == expected_cases
                              and failure is None and not cleanup_errors else "failed")
        write_json(args.scratch / "result.json", evidence)
    if failure is not None and not isinstance(failure, Exception):
        raise failure
    return evidence


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--unity-editor", type=Path, required=True)
    parser.add_argument("--test-executable", type=Path, required=True)
    parser.add_argument("--frost-executable", type=Path, required=True)
    parser.add_argument("--scratch", type=Path, required=True)
    parser.add_argument("--workflow", choices=["all", *CASES], default="all")
    parser.add_argument("--import-timeout", type=float, default=600)
    args = parser.parse_args()
    try:
        result = execute(args)
    except Exception as error:
        reason = str(error) if isinstance(error, GuardError) else type(error).__name__
        print("Legacy Unity fixture refused (" + reason + ")", file=sys.stderr)
        return 1
    print(json.dumps({"status": result["status"], "completedCases": len(result["cases"]),
                      "ownedJobsEmpty": result.get("ownedJobsEmpty", False),
                      "preferenceDifferences": result.get("preferences", {}).get("finalDifferences")}))
    return 0 if result["status"] == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
