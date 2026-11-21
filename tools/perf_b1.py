# SPDX-License-Identifier: Apache-2.0
#
# Windows B1 component runner for an empty-workspace warm launch on screen.

"""Collect warm first-frame measurements from the native Desktop Duplication probe."""

import argparse
import ctypes
from ctypes import wintypes
import hashlib
import json
import math
import os
from pathlib import Path
from statistics import median
import subprocess
import sys
import time

BLOCK_RUNS = 10
BLOCK_COUNT = 2


def file_digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def last_input_tick():
    class LastInputInfo(ctypes.Structure):
        _fields_ = [("cbSize", wintypes.UINT), ("dwTime", wintypes.DWORD)]

    info = LastInputInfo(ctypes.sizeof(LastInputInfo), 0)
    if not ctypes.windll.user32.GetLastInputInfo(ctypes.byref(info)):
        raise RuntimeError("Windows could not read the session's last input time")
    return info.dwTime


def check_input(initial_tick):
    if last_input_tick() != initial_tick:
        raise RuntimeError("Windows reported input during the capture")


def read_samples(report):
    if (
        report.get("tool") != "s1_first_frame"
        or report.get("target", "").lower() != "frost.exe"
        or report.get("arguments") != ""
        or report.get("mode") != "warm"
        or report.get("status") != "valid"
        or report.get("sessionStart", {}).get("valid") is not True
        or report.get("sessionEnd", {}).get("valid") is not True
        or report.get("accessLost") != 0
        or report.get("captureFailure") != ""
    ):
        raise ValueError("native capture or session validation failed")
    launches = report.get("launches")
    if not isinstance(launches, list) or len(launches) != BLOCK_RUNS + 1:
        raise ValueError("native report is missing expected launches")
    samples = []
    for idx, launch in enumerate(launches):
        elapsed = launch.get("firstFrameMs")
        if (
            launch.get("priming") is not (idx == 0)
            or launch.get("valid") is not True
            or type(elapsed) not in (int, float)
            or not math.isfinite(elapsed)
            or elapsed <= 0
        ):
            raise ValueError("native report contains an invalid launch")
        if idx:
            samples.append(elapsed)
    return samples


def capture_block(probe, frost, output, env, initial_tick):
    check_input(initial_tick)
    command = [str(probe), "--exe", str(frost), "--mode", "warm", "--runs", str(BLOCK_RUNS),
               "--out", str(output)]
    with output.with_suffix(".log").open("wb") as log, subprocess.Popen(
        command, env=env, stdout=log, stderr=subprocess.STDOUT
    ) as process:
        try:
            deadline = time.monotonic() + 150
            while process.poll() is None:
                check_input(initial_tick)
                if time.monotonic() >= deadline:
                    raise RuntimeError("native capture exceeded its time limit")
                time.sleep(0.05)
            check_input(initial_tick)
        finally:
            if process.poll() is None:
                # The native probe owns the launched IDE in a kill-on-close job.
                process.kill()
    if process.returncode != 0:
        raise RuntimeError(f"native capture exited with code {process.returncode}; see {output.with_suffix('.log')}")
    return read_samples(json.loads(output.read_text(encoding="utf-8")))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", type=Path, required=True, help="built s1_first_frame.exe")
    parser.add_argument("--frost", type=Path, required=True, help="built frost.exe")
    parser.add_argument("--out-dir", type=Path, required=True, help="new directory for raw and summary reports")
    args = parser.parse_args()
    if sys.platform != "win32":
        parser.error("an interactive Windows console is required")
    try:
        probe = args.probe.resolve(strict=True)
        frost = args.frost.resolve(strict=True)
        if probe.name.lower() != "s1_first_frame.exe" or frost.name.lower() != "frost.exe":
            parser.error("provide s1_first_frame.exe and frost.exe")
        images = {"probe": probe, "frost": frost, "frost_app": frost.with_name("frost_app.dll")}
        digests = {name: file_digest(path) for name, path in images.items()}
        out = args.out_dir.resolve()
        out.mkdir()
        profile = out / "profile"
        profile.mkdir()
    except OSError as exc:
        parser.error(str(exc))

    summary = {
        "schemaVersion": 1,
        "budget": "B1",
        "corpus": "empty",
        "variant": "warm",
        "measurement": "process start to pixel threshold via Desktop Duplication",
        "scope": "component-only",
        "paths": {name: str(path) for name, path in images.items()},
        "sha256": digests,
    }
    samples = []
    try:
        initial_tick = last_input_tick()
        env = dict(os.environ, APPDATA=str(profile), LOCALAPPDATA=str(profile))
        for idx in range(BLOCK_COUNT):
            samples.extend(capture_block(probe, frost, out / f"block-{idx + 1}.json", env, initial_tick))
        if digests != {name: file_digest(path) for name, path in images.items()}:
            raise RuntimeError("a measured image changed during capture")
        check_input(initial_tick)
        summary.update(status="measured", samplesMs=samples, medianMs=median(samples), maxMs=max(samples))
        exit_code = 0
    except (AttributeError, OSError, RuntimeError, ValueError) as exc:
        summary.update(status="invalid", reason=str(exc))
        exit_code = 3
    (out / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(f"{summary['status']}: {out / 'summary.json'}")
    return exit_code


if __name__ == "__main__":
    sys.exit(main())
