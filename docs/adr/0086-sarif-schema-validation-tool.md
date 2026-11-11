<!-- SPDX-License-Identifier: Apache-2.0 -->

# ADR 0086: Validate inspection output with the official SARIF schema

- Status: accepted
- Date: 2026-11-10
- Plan reference: PLAN.md section 7, R5 (CPP-10); ADR 0078 (real-tool acceptance)

## Context

CPP-10 exports inspection results as SARIF. A structural check implemented by FrostIDE is not an independent
schema conformance check and can accept invalid output. The acceptance test needs to validate the emitted document
against the current published SARIF schema, reproducibly and without network access during verification.

## Decision

1. Use a test-only Python command, `tools/validate_sarif.py`, as the normative SARIF schema validator. It uses the
   pure-Python `fastjsonschema` 2.22.3 package, vendored under `tools/_vendor/fastjsonschema` from the PyPI wheel
   `https://files.pythonhosted.org/packages/d7/0d/6caab33488dc9d26440c24221fa610c7c8b83c30715ab45d31f0e004308e/fastjsonschema-2.22.3-py3-none-any.whl`
   (SHA-256 `f670b443830b75aca3f00b75949efa20f4b2a80302ec7a21505c28dd54cffd43`). The package is licensed BSD-3-Clause.
   Preserve its upstream LICENSE and AUTHORS files with the vendored source. `tests/tools/schemas/sarif-validator.lock.json`
   pins the validator identity, wheel provenance and digest, every vendored file digest, and the schema digest;
   `tools/licensecheck.py` checks those values, the exact vendored file set, the BSD-3-Clause notice, and this ADR.
   This remains the ordinary allowlist check (BSD-3-Clause); no exception or allowlist relaxation is introduced.
   The validator is a development/test tool; it is not linked, bundled, or imported by the FrostIDE application.
2. Validate against the byte-exact OASIS SARIF 2.1.0 Errata 01 schema at
   `https://docs.oasis-open.org/sarif/sarif/v2.1.0/errata01/os/schemas/sarif-schema-2.1.0.json`, stored as
   `tests/tools/schemas/sarif-schema-2.1.0.json` (112,768 bytes; SHA-256
   `c3b4bb2d6093897483348925aaa73af03b3e3f4bd4ca38cef26dcb4212a2682e`). The validator's pin manifest records
   this schema digest and validator package provenance; startup fails closed if either the recorded version or
   schema digest differs.
   The official schema declares JSON Schema Draft 4. `fastjsonschema` supports Draft 4, 6 and 7; validation follows
   the schema's own `$schema` declaration.
3. Retain the OASIS copyright and permission notice in `tests/tools/schemas/NOTICE.txt`. Mark the schema and notice
   in REUSE as `LicenseRef-OASIS-SARIF-Errata-01`, defined in
   `LICENSES/LicenseRef-OASIS-SARIF-Errata-01.txt`, not Apache-2.0 or BSD. This LicenseRef reflects the actual OASIS
   terms: the notice permits copying and distribution when the notice is retained, and prohibits modifying the
   standard itself. The validator license does not apply to the schema. Any schema update must come from the official
   OASIS URL, remain byte-exact, and update the pin and this ADR after review.
4. The CPP-10 test writes its generated SARIF and invokes this validator. The C++ inspection helper may perform
   inexpensive structural checks, but it is not evidence of schema validity and is not the normative validator.

## Consequences

- Schema acceptance runs offline against a fixed official artifact and a pinned validator.
- The added Python package affects development and test tooling only; no application build or runtime dependency is
  added.
- Upgrading SARIF schema or validator requires updating the pinned provenance and reviewing the generated report
  against the new version.

## Alternatives considered

- **Validate only with FrostIDE's own C++ checks.** Rejected because that duplicates the producer's assumptions and
  does not independently establish conformance to the published schema.
- **Download the schema during a test.** Rejected because verification must be deterministic and offline-capable.
