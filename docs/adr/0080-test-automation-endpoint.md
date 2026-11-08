<!-- SPDX-License-Identifier: Apache-2.0 -->

# ADR 0080: A test-build-only automation endpoint

- Status: accepted; implemented by FND-7a (R1, DD-1a).
- Date: 2026-11-08
- Plan reference: FND-7a (PLAN.md Appendix C, R1), ADR 0078 (definition of done v2).

## Context

The R-ladder journeys (j1_cmake first) must drive the real FrostIDE against
the pinned corpora with real tools. Before this change a journey could only
start `frost` and read its log and spawn log: it could not open a folder,
pick a preset, build or read Problems. Clicking through the GUI from the
outside (UI Automation, xdotool) is fragile, needs a visible desktop, and
breaks the "no windows on the user's desktop" rule. A driver inside the
process is the usual answer, but a control channel into an IDE that runs
the user's build scripts is also a way to run code as the user, so it must
not exist in the product.

## Decision

1. `src/app/src/automation_endpoint.{hpp,cpp}` is a small line-delimited JSON
   protocol (`frost-automation-endpoint/1`) over `QLocalServer`: a named pipe
   on Windows, a Unix socket on Linux. Its methods call the workbench's own
   services: the CMake service, the build controller's trust, the Problems
   model, clangd's session and the editor. Every answer is read from that
   state. A part that is not available says so and nothing is invented
   (truth rule, ADR 0078).
2. **Debug test builds only.** CMake option `FROST_ENABLE_AUTOMATION_ENDPOINT`
   (default `FROST_BUILD_TESTS`) defines `FROST_AUTOMATION_ENDPOINT` for the
   Debug configuration only. Every other configuration compiles a stub whose
   `start()` returns `Unsupported`. The stub holds no protocol string, no
   socket code and no reference to the environment variable.
   `tests/tools/test_automation_release_probe.py` runs in every configuration.
   It checks that the Release (and RelWithDebInfo) image of `frost_app.dll`
   or `frost` holds none of the endpoint's strings, and that the Debug image
   holds all of them, which proves that the probe can see them.
3. **Off unless asked for.** Even in Debug, nothing listens unless the
   launcher sets `FROST_AUTOMATION_CONNECTION` to an absolute path.
4. **Same user only.** The server listens with
   `QLocalServer::UserAccessOption`, so only the user's own processes can
   connect. The server name is random:
   `frost-automation-<pid>-<64 random bits>`.
5. **Per-session token.** At start the endpoint takes 32 bytes from the
   system CSPRNG (`QRandomGenerator::system()`). It writes the protocol,
   server name, token and pid to the connection file atomically. The first
   request on a connection must be `hello` with that token, compared in
   constant time. Any other first request, or a wrong token, gets
   `PermissionDenied` and the connection is closed. Only a process that can
   read the file can drive the IDE, and the file lives in the test's own
   temporary profile.
6. **No bypass of trust.** The endpoint has no privileged paths. `trust` is
   the trust dialog's own grant (`BuildController::trust`). An untrusted
   folder opened through the endpoint starts nothing, as one opened by hand
   does (`AutomationEndpoint.DrivesTrustPresetsConfigureBuildProblemsAndCleanOfARealCMakeProject`
   checks this with a recording launcher).

## Consequences

- Journeys (`tests/journeys`) can drive a real `frost` offscreen on an
  isolated profile and record their evidence. They need a Debug build of
  FrostIDE.
- The endpoint grows with the journeys. Each new method must read existing
  workbench state or call an existing user action, never add a shortcut.
- A Release build that accidentally defines the macro fails the probe test
  in the `-Full` release leg.
- Threat accepted: any process of the same user that can read the test
  profile can drive a Debug test build while a journey runs. The same
  process could already run that user's build scripts directly.
