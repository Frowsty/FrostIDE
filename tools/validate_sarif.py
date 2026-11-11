#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
#
# validate_sarif.py: validate a SARIF document against the byte-pinned official OASIS
# SARIF 2.1.0 Errata 01 JSON Schema (ADR 0086). This offline test tool has no runtime
# dependency outside the checked-in fastjsonschema source under tools/_vendor.
#
#   python -I tools/validate_sarif.py <report.sarif>
#
# Exit status: 0 valid, 1 invalid document, integrity error, or input error; 2 usage error.

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
LOCK_PATH = ROOT / "tests" / "tools" / "schemas" / "sarif-validator.lock.json"
SCHEMA_PATH = ROOT / "tests" / "tools" / "schemas" / "sarif-schema-2.1.0.json"
VENDOR_ROOT = ROOT / "tools" / "_vendor"
PINNED_SCHEMA_SHA256 = "c3b4bb2d6093897483348925aaa73af03b3e3f4bd4ca38cef26dcb4212a2682e"
PINNED_VERSION = "2.22.3"
PINNED_WHEEL_SHA256 = "f670b443830b75aca3f00b75949efa20f4b2a80302ec7a21505c28dd54cffd43"
PINNED_WHEEL_URL = (
    "https://files.pythonhosted.org/packages/d7/0d/6caab33488dc9d26440c24221fa610c7c8b83c30715ab45d31f0e004308e/"
    "fastjsonschema-2.22.3-py3-none-any.whl"
)


class IntegrityError(Exception):
    """A pinned schema, manifest, or vendored package does not match the expected bytes."""


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _load_pins() -> dict[str, Any]:
    try:
        lock = json.loads(LOCK_PATH.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise IntegrityError(f"cannot read validator pin manifest: {exc}") from exc

    schema_pin = lock.get("schema", {})
    validator_pin = lock.get("validator", {})
    if schema_pin.get("path") != SCHEMA_PATH.name or schema_pin.get("sha256") != PINNED_SCHEMA_SHA256:
        raise IntegrityError("pin manifest schema path or SHA-256 differs from the reviewed pin")
    if (validator_pin.get("name") != "fastjsonschema"
            or validator_pin.get("version") != PINNED_VERSION
            or validator_pin.get("wheel_url") != PINNED_WHEEL_URL
            or validator_pin.get("wheel_sha256") != PINNED_WHEEL_SHA256
            or validator_pin.get("license") != "BSD-3-Clause"):
        raise IntegrityError("pin manifest validator provenance differs from the reviewed pin")
    if _sha256(SCHEMA_PATH.read_bytes()) != PINNED_SCHEMA_SHA256:
        raise IntegrityError("official SARIF schema SHA-256 mismatch")

    file_pins = lock.get("files")
    if not isinstance(file_pins, dict) or not file_pins:
        raise IntegrityError("pin manifest has no vendored-file digest map")
    package_root = VENDOR_ROOT / "fastjsonschema"
    actual_paths = {
        path.relative_to(package_root).as_posix()
        for path in package_root.rglob("*")
        if path.is_file() and "__pycache__" not in path.parts and path.suffix != ".pyc"
    }
    if actual_paths != set(file_pins):
        raise IntegrityError("vendored fastjsonschema file set differs from the pin manifest")
    for relative, expected in file_pins.items():
        try:
            actual = _sha256((package_root / relative).read_bytes())
        except OSError as exc:
            raise IntegrityError(f"cannot read vendored file {relative}: {exc}") from exc
        if actual != expected:
            raise IntegrityError(f"vendored fastjsonschema digest mismatch: {relative}")
    return lock


def _validator():
    # Isolated mode intentionally omits site-packages and the script directory. Add only the
    # reviewed local vendor root, preventing ambient package imports or network resolution.
    sys.dont_write_bytecode = True
    sys.path.insert(0, str(VENDOR_ROOT))
    import fastjsonschema  # noqa: PLC0415

    if fastjsonschema.VERSION != PINNED_VERSION:
        raise IntegrityError(f"vendored fastjsonschema version mismatch: {fastjsonschema.VERSION}")
    return fastjsonschema


def validate_document(document: Any, schema: dict[str, Any]) -> None:
    fastjsonschema = _validator()

    def check_refs(value: Any) -> None:
        if isinstance(value, dict):
            ref = value.get("$ref")
            if ref is not None and (not isinstance(ref, str) or not ref.startswith("#")):
                raise IntegrityError(f"schema contains non-local reference: {ref}")
            for child in value.values():
                check_refs(child)
        elif isinstance(value, list):
            for child in value:
                check_refs(child)

    # The pinned official schema is self-contained. Reject every non-local reference before
    # compilation so no URI scheme can trigger a resolver or network request.
    check_refs(schema)

    def reject_remote_ref(uri: str) -> Any:
        raise IntegrityError(f"schema attempted external reference resolution: {uri}")

    validator = fastjsonschema.compile(
        schema,
        handlers={"http": reject_remote_ref, "https": reject_remote_ref, "file": reject_remote_ref},
        use_default=False,
    )
    validator(document)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Validate SARIF against the pinned official OASIS schema.")
    parser.add_argument("report", type=Path, help="SARIF JSON document to validate")
    args = parser.parse_args(argv)
    try:
        _load_pins()
        schema = json.loads(SCHEMA_PATH.read_text(encoding="utf-8"))
        document = json.loads(args.report.read_text(encoding="utf-8"))
        validate_document(document, schema)
    except (IntegrityError, OSError, UnicodeError, json.JSONDecodeError) as exc:
        print(f"SARIF validation failed: {exc}", file=sys.stderr)
        return 1
    except Exception as exc:
        # fastjsonschema documents validation failures with JsonSchemaException; keep schema
        # definition or unexpected validator failures visible as a clean nonzero CLI result.
        print(f"SARIF validation failed: {exc}", file=sys.stderr)
        return 1
    print(f"Valid SARIF 2.1.0: {args.report}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
