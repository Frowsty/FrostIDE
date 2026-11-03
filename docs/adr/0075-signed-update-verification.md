<!-- SPDX-License-Identifier: Apache-2.0 -->

# ADR 0075: Signed update verification boundary

Status: proposed, 2026-11-03.

FrostIDE needs authenticated updates (PLAN.md section 2.2). Release metadata and
transport are not trust roots. Exact manifest bytes are signed with Ed25519 by
an offline release key; the distributed application must embed its public key.
GitHub Releases for `Frowsty/FrostIDE` is the chosen discovery host. A signed
manifest must bind channel, platform, architecture, version/build, payload URL,
size and SHA-256. Neither this primitive nor the existence of a release makes an
installer eligible to run. In particular, verification alone does not prevent
replacement after verification: an installer handoff must preserve an immutable
payload until the installer has opened it.

Use libsodium 1.0.22 from the pinned vcpkg baseline for Ed25519 verification and
SHA-256. Its ISC licence is on the repository SPDX allowlist. The library's
signature primitive verifies the exact bytes before the application interprets
manifest contents. Tests generate ephemeral fixture keys only. This introduces
no production signing key. The application discovery and download UI remains
an unaccepted implementation until the full updater gates pass.

Qt Network (LGPL-3.0-only option, already in the pinned Qt distribution) is allowed
in the application for HTTPS update requests. The endpoint is limited to GitHub
release discovery and its HTTPS asset redirects; authenticated content still needs
signature verification. Update checks require interactive readiness and consent.
Windows deployment includes Qt's dynamically loaded Schannel TLS backend; staged
checks must exercise HTTPS without relying on the SDK plugin directory.

The pinned host build helper `vcpkg-make 2026-07-09#1` declares a null licence,
reported by its SBOM as `LicenseRef-vcpkg-null`. Its installed copyright identifies
MIT vcpkg CMake scripts and GPL-2.0-or-later Automake 1.17 shell wrappers. These run
only as locally installed build tools under PLAN section 2's separate-process
policy; no helper is linked into FrostIDE or included in its application payload.
The licence checker permits only this exact port, version/revision and declaration,
requires an installed manifest confined to `share/vcpkg-make`, and retains the
upstream SBOM declaration with a build-only attribution. Missing metadata, a changed
declaration/version or library/runtime artifact fails. This does not add GPL or
null declarations to the linked-code allowlist. Packaging these helpers would need
a separate distribution decision.

The complete update feature still requires consent and availability gates,
private bounded staging, persistent downgrade/replay protection,
and a close-safe installer helper with a continuous payload ownership guarantee.
It must also correctly report installer results. A user-provided per-user MSI
must define its upgrade, rollback and restart contract; a verified bundled
AppImage updater and Linux replacement policy are separately needed. Until
these exist, the cryptographic primitive does not enable update installation.

Interactive admission uses live state: the window is visible, workbench attachment
and startup project/session/editor restoration completed, and the current project
has published its initial scan without another scan pending. The welcome window
with no project needs no project scan. A project transition, pending scan, failed
scan, or window teardown closes the gate. This is a functional live-readiness
gate; it does not claim the unimplemented FSEG cache path or B2/B3 budgets.

The Windows clang-cl triplet uses a minimal overlay of the pinned vcpkg port:
it removes the unused MSBuild helper dependency and rejects cl.exe, while retaining
the port's normal clang make-based build and its source hash. This avoids building
an unrelated host pkgconf through Meson when the MSBuild path cannot be used.
The make configure passes the pinned Windows SDK/toolset through a compiler
response file because Autoconf word splitting loses spaces in linker options.
Debug CRT recursive SIGABRT detection is conservatively disabled: the observed
probe waited for a runtime dialog. That macro controls the third-party misuse
test only and is not used by the cryptographic library implementation. Debug misuse
signal-chain coverage is therefore absent; this is not an upstream test-suite pass.
Archive creation and convenience-archive extraction use the pinned LLVM `llvm-ar`:
`llvm-lib` does not implement the extraction command required by the make wrapper.
The configure override also supplies GNU archive flags (`cr`), excluding the
triplet's MSVC-only `-machine` and `-nologo` flags.

Accepted release state is a separate bounded, strict JSON tuple under the local
per-user update directory, shared across channels. Cooperative instances serialize
read, signature/policy revalidation and replacement using QLockFile with time-based
stale removal disabled. QSaveFile uses no direct-write fallback. Invalid or
unreadable state blocks acceptance; only a missing file starts without a floor.
The state is persisted before offering a download, then checked again after the
modal answer and at the eventual installer handoff. Exact identity retries remain
allowed. This is atomic replacement/process-crash scope, not hostile same-user or
power-loss rollback resistance. The earlier draft settings tuple was never
persisted by a shipped updater and is removed instead of being silently migrated.

Discovery enumerates up to four pages of 100 releases and 32 matching unique asset
pairs, with a two-minute overall deadline. Metadata order is not release order:
each candidate is authenticated, then tested against the installed/accepted floor.
Among eligible candidates, signed build order must agree with version order and
the same build must have the same payload identity. The selected envelope is
revalidated with a fresh clock before offering it. Expired or invalid candidates
cannot authorize an update; if no eligible candidate remains, any validation or
retrieval failure produces an error rather than an up-to-date claim. Exhausted
page/candidate/time budgets report an incomplete search. This does not prove
freshness of an adversarially withheld release listing. Download verification
drains the final reply buffer as well as ready-read events, requiring exactly the
signed size and digest with no ignored trailing data.
