# SPDX-License-Identifier: Apache-2.0
#
# Tests for tools/validate_sarif.py (ADR 0086): full OASIS SARIF schema acceptance, invalid
# report rejection, isolated-interpreter invocation and fail-closed schema/vendor pins.

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from tools_test_support import REPO, clean_env

import validate_sarif


SCRIPT = REPO / "tools" / "validate_sarif.py"
SCHEMA = REPO / "tests" / "tools" / "schemas" / "sarif-schema-2.1.0.json"


def _minimal_sarif() -> dict:
    return {"version": "2.1.0", "$schema": "https://json.schemastore.org/sarif-2.1.0.json", "runs": []}


class ValidateSarifTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(prefix="frost-sarif-")
        self.tmp = Path(self._tmp.name)

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def run_validator(self, document: object) -> subprocess.CompletedProcess[str]:
        report = self.tmp / "report.sarif"
        report.write_text(json.dumps(document), encoding="utf-8")
        return subprocess.run(
            [sys.executable, "-I", "-B", str(SCRIPT), str(report)],
            cwd=self.tmp,
            env=clean_env(),
            capture_output=True,
            text=True,
            encoding="utf-8",
            check=False,
        )

    def test_official_schema_accepts_minimal_sarif_in_isolated_mode(self) -> None:
        result = self.run_validator(_minimal_sarif())
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Valid SARIF 2.1.0", result.stdout)

    def test_official_schema_rejects_invalid_documents(self) -> None:
        for document in ({"runs": []}, {"version": "2.0.0", "runs": []},
                         {"version": "2.1.0", "runs": [None]}):
            with self.subTest(document=document):
                result = self.run_validator(document)
                self.assertEqual(result.returncode, 1)
                self.assertIn("SARIF validation failed", result.stderr)

    def test_pin_manifest_records_schema_and_vendored_file_digests(self) -> None:
        lock = validate_sarif._load_pins()
        self.assertEqual(lock["schema"]["path"], SCHEMA.name)
        self.assertTrue(lock["files"])
        self.assertIn("LICENSE", lock["files"])
        self.assertIn("AUTHORS", lock["files"])

    def test_schema_pin_mismatch_fails_closed(self) -> None:
        tampered = self.tmp / SCHEMA.name
        tampered.write_text("{}", encoding="utf-8")
        with mock.patch.object(validate_sarif, "SCHEMA_PATH", tampered):
            with self.assertRaisesRegex(validate_sarif.IntegrityError, "schema SHA-256 mismatch"):
                validate_sarif._load_pins()

    def test_recorded_validator_version_mismatch_fails_closed(self) -> None:
        lock = json.loads(validate_sarif.LOCK_PATH.read_text(encoding="utf-8"))
        lock["validator"]["version"] = "9.9.9"
        tampered = self.tmp / validate_sarif.LOCK_PATH.name
        tampered.write_text(json.dumps(lock), encoding="utf-8")
        with mock.patch.object(validate_sarif, "LOCK_PATH", tampered):
            with self.assertRaisesRegex(validate_sarif.IntegrityError, "validator provenance"):
                validate_sarif._load_pins()

    def test_external_schema_references_are_blocked(self) -> None:
        schema = {"$schema": "http://json-schema.org/draft-04/schema#", "$ref": "https://example.invalid/schema"}
        with self.assertRaisesRegex(validate_sarif.IntegrityError, "non-local reference"):
            validate_sarif.validate_document({}, schema)


if __name__ == "__main__":
    unittest.main()
