<!-- SPDX-License-Identifier: Apache-2.0 -->

# ADR 0085: IHttpClient, network policy with consent, and safe updater with rollback (FND-16, FND-11)

- Status: accepted
- Date: 2026-11-10
- Plan reference: PLAN.md section 2.2 ("Updates and network policy"), section 7 "Roadmap v2" (R5: FND-16, FND-11),
  Appendix C (`FND-16`, `FND-11`); builds on ADR 0075

## Context
FrostIDE's authenticated update pipeline (ADR 0075) requires two Qt-free foundation seams in `frost::update`:
1. **`FND-16` (`IHttpClient`, network policy and consent):** A Qt-free HTTP/HTTPS client seam implemented with native
   WinHTTP on Windows and libcurl on Linux, governed by an offline-by-default `NetworkPolicy` and an explicit user
   consent gate that blocks any outbound request before a socket or transport call is attempted.
2. **`FND-11` (`SafeUpdater` with rollback):** A transactional installation and rollback coordinator using
   `frost::io::IFileSystem` and `frost::io::IProcessLauncher` that verifies the update payload against its signed
   SHA-256 digest and size, backs up the existing installation, installs the candidate, runs a bounded `--selftest`
   health check via `IProcessLauncher`, and on any health-check failure automatically restores the previous installation
   and verifies that the previous version starts.

## Decision

### 1. `IHttpClient` transport and dependency licences (`FND-16`)
- `frost::update::IHttpClient` (`src/update/include/frost/update/http_client.hpp`) defines a Qt-free synchronous/streaming
  interface for bounded `GET` and `HEAD` requests (`request`) and streaming downloads (`download`, `downloadToFile`) with:
  - Explicit timeout (`std::chrono::milliseconds`), maximum response byte budget (`maxResponseBytes`), redirect hop
    limit (`maxRedirects`), custom `User-Agent`, optional request headers, redirect URL validation callback, and
    cancellation callback (`isCancelled`).
  - Manual redirect inspection at every hop so redirect targets are validated against scheme/host policy and redirect
    limits before following, and redirect body bytes never enter a download stream sink.
  - HTTPS-only by default; plain HTTP is permitted only when `allowHttpLoopbackForTests` is explicitly enabled and the
    target host is `127.0.0.1` or `localhost`.
- **Windows backend (`src/update/src/http_client_win.cpp`):** Uses the Windows system library WinHTTP (`<winhttp.h>`,
  linked against `winhttp`). WinHTTP is an OS component provided by the Windows SDK and requires no third-party
  redistribution. All WinHTTP calls use wide (`wchar_t`) strings converted via `frost::io::toWide` / `fromWide`.
- **Linux backend (`src/update/src/http_client_curl.cpp`):** Uses `CURL::libcurl` from the pinned vcpkg manifest
  (`curl` with `default-features: false` on Linux). `libcurl` is licensed under the `curl` (MIT/X-derivative) licence,
  which is on the repository SPDX `ALLOWLIST` in `tools/licensecheck.py`.

### 2. Offline-by-default `NetworkPolicy` and `PolicyGatedHttpClient` (`FND-16`)
- `frost::update::NetworkPolicy` (`src/update/include/frost/update/network_policy.hpp`) defaults to offline
  (`isOffline() == true`).
- Even when online mode is enabled (`setOffline(false)` or live settings reader returning `false`), outbound requests
  require explicit session consent obtained through `ConsentPromptCallback`.
- `PolicyGatedHttpClient` wraps an `IHttpClient` and consults `NetworkPolicy::checkAndRequestConsent` before forwarding
  any request or download. When offline (`ErrorCode::PermissionDenied`) or when the user declines consent
  (`ErrorCode::Cancelled`), the request fails immediately with zero calls to the underlying transport. Switching back to
  offline mode revokes session consent and aborts in-flight requests via the cancellation callback.

### 3. `SafeUpdater` with health check and automatic rollback (`FND-11`)
- `frost::update::SafeUpdater` (`src/update/include/frost/update/safe_updater.hpp`) coordinates installation using only
  `frost::io::IFileSystem` and `frost::io::IProcessLauncher` (never `QProcess`, never blocking the GUI thread):
  1. Verifies the candidate payload against the authenticated release size and SHA-256 digest (`verifyPayload`).
  2. Stages the candidate payload (from a `.zip` archive, staged directory, or single-binary payload) into an isolated
     staging directory before mutating `installDir`.
  3. Moves the existing `installDir` into `backupDir` and places the staged candidate into `installDir`.
  4. Runs `<installDir>/<executable> --selftest` (with `QT_QPA_PLATFORM=offscreen` and a bounded timeout) through
     `IProcessLauncher`.
  5. If the health check exits `0`, commits the update, writes the installed release record, and optionally launches the
     new version detached.
  6. If the health check fails (non-zero exit code, crash signal, timeout, or missing executable), removes the failed
     installation, restores `backupDir` to `installDir`, launches the restored previous version through
     `IProcessLauncher` to verify that it starts cleanly (`previousVersionStarted = true`), and returns a structured
     `UpdateOutcome` report (`rolledBack = true`, `failureReason`).

## Consequences
- Outbound update discovery and downloads in `Workbench` are gated by `NetworkPolicy` and `PolicyGatedHttpClient`, and
  update installation runs asynchronously off the GUI thread through `SafeUpdater`.
- `update_unit_tests` verifies both the no-network/consent gates against a local `127.0.0.1` loopback server and full
  healthy/rollback update cycles with real child processes launched via `IProcessLauncher`.
