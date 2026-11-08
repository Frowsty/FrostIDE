# SPDX-License-Identifier: Apache-2.0
#
# The automation endpoint is absent from Release (FND-7a, ADR 0080): ctest runs this in every
# configuration with the built image that holds the endpoint (frost_app.dll on Windows, frost on
# Linux) and the configuration's name. A Debug image of a test build must contain the protocol
# marker and the connection variable's name (so the probe is known to see them); every other
# configuration's image must contain neither, in UTF-8 or UTF-16. Run by ctest only:
#   test_automation_release_probe.py <image> <config> <ON|OFF (FROST_ENABLE_AUTOMATION_ENDPOINT)>

from __future__ import annotations

import sys
import unittest
from pathlib import Path

# The strings only the endpoint's code holds (src/app/src/automation_endpoint.cpp and main.cpp).
MARKERS = ("frost-automation-endpoint/1", "FROST_AUTOMATION_CONNECTION", "frost-automation-%1-%2")


def found_markers(image: bytes) -> list[str]:
    """The markers present in `image`, as UTF-8 or UTF-16LE bytes."""
    return [marker for marker in MARKERS
            if marker.encode("utf-8") in image or marker.encode("utf-16-le") in image]


class ReleaseProbeTest(unittest.TestCase):
    image: Path
    config: str
    enabled: bool

    def test_the_endpoint_is_in_debug_test_builds_only(self) -> None:
        self.assertTrue(self.image.is_file(), f"no built image at {self.image}")
        found = found_markers(self.image.read_bytes())
        if self.enabled and self.config == "Debug":
            self.assertEqual(found, list(MARKERS), f"the Debug image {self.image} lacks the endpoint")
        else:
            self.assertEqual(found, [], f"the {self.config} image {self.image} holds the automation endpoint")

    def test_the_probe_finds_both_encodings(self) -> None:
        self.assertEqual(found_markers(b"\0" + MARKERS[0].encode() + b"\0"), [MARKERS[0]])
        self.assertEqual(found_markers(MARKERS[2].encode("utf-16-le")), [MARKERS[2]])
        self.assertEqual(found_markers(b"frost-automation-endpoint/2 FROST_AUTOMATION"), [])


if __name__ == "__main__":
    if len(sys.argv) < 4:
        print("usage: test_automation_release_probe.py <image> <config> <ON|OFF> [unittest args]", file=sys.stderr)
        sys.exit(2)
    ReleaseProbeTest.image = Path(sys.argv[1])
    ReleaseProbeTest.config = sys.argv[2]
    ReleaseProbeTest.enabled = sys.argv[3].upper() in ("ON", "1", "TRUE", "YES")
    unittest.main(argv=[sys.argv[0], *sys.argv[4:]])
