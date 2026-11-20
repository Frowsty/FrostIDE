# ADR 0106: Per-user archive delivery and local acceptance evidence

- Status: Accepted implementation choice for R16; runtime acceptance pending
- Date: 2026-11-20

## Context

FND-17 needs local install, association and uninstall behavior without a new installer dependency.
D41 permits a zip plus self-registration. The implementation-first instruction defers all execution.

## Decision

Keep tools/stage.ps1 and tools/stage.sh as payload producers. Windows tools/install_user.py copies a
fresh stage into a fresh per-user directory. The optional HKCU registration offers FrostIDE in Open
with for `.frostproj`, `.sln`, `.slnx` and `.lua` without replacing the user's default. Explorer
offers an additional verb on `.txt` files whose `System.FileName` equals `CMakeLists.txt`. The verb
passes the selected file to FrostIDE's ordinary project open entry point. Other text files retain
their existing associations. The CLI notifies the Windows Shell of association changes.
The install manifest records the registry values and keys it added. A conflicting registration
fails without replacement, partial writes roll back, and uninstall checks the saved ownership and
current command before removing unchanged values. Legacy manifests retain `.frostproj` cleanup.
Uninstall removes manifest-listed unchanged files and their empty ancestor directories. Modified
and foreign files stay.
On Linux, extract the archive to its final location and run `python3 install_desktop.py register`
from the extracted directory. It offers the packaged launcher for project files, Visual Studio
solutions, CMake and Lua through the user's desktop and MIME databases. It leaves the user's
`mimeapps.list` and default choices intact. Before creating registration files, the helper
requires `update-mime-database` and `update-desktop-database` (`shared-mime-info` and
`desktop-file-utils` on Ubuntu). Both caches are refreshed before registration succeeds. Run
`python3 install_desktop.py unregister` before moving or removing the archive. It removes the
registration files whose contents still match the receipt and preserves modified files.
The Python helper is a local administrative delivery tool, requiring an installed Python interpreter;
it is not an MSI, signed installer, automatic updater, or standalone bootstrap executable.

The stage must include the source-pinned Lua interpreter, matching shared API library and MIT notice.
A separately configured Lua debugging extension remains user-installed (ADR 0094); it is not silently
redistributed. Linux stages copy pinned Qt shared libraries, ICU, plugins, translations and SDK notices, with a
frost wrapper that supplies package-relative loader/plugin paths and executes frost.bin. Release
staging requires the pinned Qt source archive; system desktop/XCB libraries remain host dependencies.

CLI diff uses the shared read-only editor viewer. Merge uses the shared editable three-way editor and
atomically replaces output after checking source and output snapshots. It admits bounded UTF-8 text;
other encodings and binary data produce an error. Checks detect changes observed before apply; no
exclusive cross-process lock is promised. Cancellation leaves output unchanged and returns status 1.
CLI wait uses current-user project IPC and actual editor destruction/accepted application shutdown.
Save or close cancellation never completes a wait. Abandoned client tickets expire by polling lease.

UPG-1 tools/delivery_gate.py validates an offline candidate lock and emits an upgrade plan identifying
both lock digests and an ADR. It neither changes pins nor downloads/builds tools. The release-gate mode
requires named local acceptance records, exact source/lock identity, evidence hashes and reviewers.
It validates recorded evidence, not the truth of human assertions or successful feature execution.
REL-1 still requires all actual gates and explicit user sign-off; no release is asserted by this slice.

## Consequences

No new dependency or license is introduced. Install/uninstall, CLI ownership/close behavior, complete
runtime staging and real upgraded-tool suites need later Windows/Linux acceptance before any release.
Python regression fixtures are authored but have not been run during the implementation-first phase.
