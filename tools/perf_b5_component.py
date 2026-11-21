# SPDX-License-Identifier: Apache-2.0
#
# Read the production B5 handler timing component from an anchored FrostIDE log.

import argparse
from collections import defaultdict
import hashlib
import json
import math
from pathlib import Path
import re
from statistics import median

_HEX = re.compile("[0-9a-f]{64}")
_RECORD = re.compile(
    r"\[info\] \[perf\.b5/\d+\] endpoint=(raster-paint-return|qrhi-display-list) "
    r"wrap=(true|false) elapsed_ns=(\d+) text_version=(\d+) "
    r"viewport=(\d+(?:\.\d+)?)x(\d+(?:\.\d+)?) file_id=([0-9a-f]{64})"
)


def component(log, expected_sha, file_id):
    raw = log.read_bytes()
    digest = hashlib.sha256(raw).hexdigest()
    if not _HEX.fullmatch(expected_sha) or digest != expected_sha:
        raise ValueError("log bytes differ from the supplied SHA-256")
    if not _HEX.fullmatch(file_id):
        raise ValueError("select the exact document file_id")
    groups = defaultdict(list)
    for line in raw.decode("utf-8").splitlines():
        if "[info] [perf.b5/" not in line:
            continue
        # The normal logger prefixes the message with UTC timestamp and thread id.
        match = _RECORD.fullmatch(line.split(" ", 1)[1]) if " " in line else None
        if not match:
            raise ValueError("malformed B5 timing record")
        endpoint, wrap, elapsed, version, width, height, observed = match.groups()
        if observed != file_id:
            continue
        if min(int(elapsed), int(version), float(width), float(height)) <= 0:
            raise ValueError("B5 sample lacks valid timing, text version or viewport")
        groups[endpoint, wrap, width, height].append(int(elapsed) / 1_000_000)
    if not groups:
        raise ValueError("no matching production B5 timing components")
    series = []
    for (endpoint, wrap, width, height), values in sorted(groups.items()):
        order = sorted(values)
        series.append({"endpoint": endpoint, "wrap": wrap == "true",
                       "viewportPixels": [float(width), float(height)], "samplesMs": values,
                       "medianMs": median(values), "p99Ms": order[math.ceil(len(order) * 0.99) - 1]})
    return {"schemaVersion": 1, "budget": "B5", "scope": "component-only",
            "fileId": file_id, "logSha256": digest, "series": series}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--log", required=True, type=Path)
    parser.add_argument("--log-sha256", required=True)
    parser.add_argument("--file-id", required=True)
    args = parser.parse_args()
    try:
        report = component(args.log, args.log_sha256, args.file_id)
    except (OSError, ValueError) as error:
        print(json.dumps({"budget": "B5", "scope": "component-only", "state": "invalid", "reason": str(error)}))
        return 3
    print(json.dumps(report, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
