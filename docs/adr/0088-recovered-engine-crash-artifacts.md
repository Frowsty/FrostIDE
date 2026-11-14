<!-- SPDX-License-Identifier: Apache-2.0 -->

# ADR 0088: Recover local engine crash artifacts from authenticated source facts

- Status: accepted design; source authored, runtime acceptance outstanding
- Date: 2026-11-11
- Plan reference: R7, FER-16; ADR 0081, ADR 0087

## Context

FER-16 asks for a `crash.dmp` notification and an offer to open the dump. FrostEngine's existing
CrashHandler writes `crash.json` and, on Windows, a real MiniDumpNormal `crash.dmp`, then the
process terminates. A dead process cannot truthfully send an authenticated live-session push.
The fatal handlers must retain their fixed-buffer, platform-specific behavior: no new networking,
JSON allocation, application callbacks or authentication work runs there.

## Decision

A live mutually authenticated v2 `server.info` may include `crashArtifactSource`. The descriptor
contains protocol version 1, a random source ID, physically resolved crash root, actual OS PID,
CrashHandler installation start time and process role, and the exact live engine instance,
authentication generation and project path. It is omitted when no real bound source exists.
There are no credentials in this descriptor and the four signed R6 capabilities stay unchanged.

The host installs or adopts the actual process CrashHandler during normal initialization. A
normal-thread registration ID distinguishes even same-millisecond reinstallation. Exact-ID retain
leases keep an adopted handler alive through configured Telemetry shutdown and actual engine
teardown. An owner's Uninstall becomes pending while retained; the last exact release performs
native removal. A source it installed requests conditional exact-ID uninstall; an adopted source
only releases its lease. The OS fatal-handler implementations are unchanged.

The IDE captures a directory baseline only while that exact authenticated session is still alive.
After unexpected loss or an actual owned-process exit, a worker may briefly observe new reports
from that captured root. Explicit disconnect, project change, source replacement and trust
revocation discard the observation. The app rechecks the captured physical-project trust before
capture, polling, publication and Open. There is no new engine push after process death.

Candidates must have a new report directory, exact PID, installation start time and process role,
and a crash timestamp after capture. Physical path components, final regular files and canonical
containment are checked; links and reparse traversal are refused. Parsing is bounded to 256 report
entries, 64 KiB per report, 256 MiB per dump and 256 MiB of dump input per poll. The minidump header,
stream directories, process ID and referenced thread/context/module/memory ranges must be valid.
Missing, busy, truncated or changing files remain unavailable and may be retried within the app's
bounded observation window; they never generate an offer prematurely. POSIX metadata without an
actual Windows minidump does not produce a dump offer.

The result owns immutable inspected bytes and their SHA-256. Opening first writes an atomic,
byte-verified copy into an existing IDE-owned cache directory after checking its physical path.
Only that copy is passed to the separate read-only DbgEng dump adapter. No executable is guessed,
launched or inferred from a report. Existing local cache retention and explicit Open apply.

## Provenance and limits

The notification describes a **locally recovered dump from a previously authenticated process**.
The descriptor authenticates expected source facts, not mutable file contents. This is correlation,
not a cryptographic signature on the dump. Existing fatal reports record PID, start and role;
they do not attest the engine's project or authentication generation at the instant of the crash.
The original authenticated identity remains notification context, not a crash-time identity claim.
A missing prior live capture, incomplete baseline, stale report, changed trust or changed project
cannot be repaired by guessing an identity after the crash.

File validation uses an explicit bounded snapshot operation in the file-system seam, with no
unbounded read fallback. It opens a single-link regular file without following a final link,
checks its size before allocation, reads at most the reserved byte limit, and verifies stable
handle metadata. Windows refuses concurrent write/delete sharing during the copy. Parent physical
paths are checked before and after the read; directory enumeration processing limits do not make
the underlying general directory-list allocation bounded. Immutable snapshots prevent later source-file edits from changing the offered
bytes. They do not turn a same-user mutable filesystem into a cryptographic provenance boundary.

## Verification

Unit sources cover descriptor binding, malformed/truncated minidumps, stale and distractor reports,
partial-write retry, link/reparse rejection, changed reads, immutable cache copies and limits.
A test-only, non-product/non-Dist helper uses the actual AutomationServer and CrashHandler. Its
parent authenticates, captures the real source, requests an actual contained abort, waits for the
real exit and inspects the resulting dump. That is crash-handler/protocol harness evidence, not a
full FrostEditor window-crash journey. The DbgEng real dump-open acceptance remains required.
No builds, configured tests or runtime acceptance were run while the predecessor Full gate was active.
