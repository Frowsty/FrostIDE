<!-- SPDX-License-Identifier: Apache-2.0 -->

# ADR 0076: Limited Windows beta save profile

- Status: accepted scope by user decision, 2026-11-03; implementation requires combined verification.
- Supersedes: ADR 0028 Windows single-link implementation for fixed local NTFS.

## Decision

The user requested the minimum work needed for a usable beta with the new GUI after
discussion of the interrupted-save defect and the option to refuse unsupported files.
Ship the tested copy-and-rename path and explicit limitations. Do not integrate the
experimental object-ID transfer protocol or unfinished project-edit/Unity features.

For existing single-link files on fixed local NTFS, copy to a private adjacent file,
preserve and check supported metadata, write and flush the new contents, then publish
with one handle rename. Short-name recovery uses the tested bounded journal. A committed
save with unfinished recovery reports a warning without marking the saved bytes unsaved.

Existing local NTFS files carrying object IDs or EFS encryption are refused before
mutation, with guidance to copy edits into a new unencrypted file. The original file
and dirty buffer remain available. Save As is not exposed in this beta's UI.
The earlier object-ID successful-save expectation is replaced by an unchanged-file
refusal regression for this beta; its historical failed evidence is retained.

Genuine hardlinks keep the existing in-place rewrite contract and its interrupted-write
limitation, subject to the same local NTFS unsupported-metadata checks. Other Windows
filesystems retain the legacy replacement path and its known interruption limitation.
The timestamped atomic-write API is unchanged. No power-loss guarantee is introduced.
Use local NTFS workspaces for the tested beta save profile. Copying edits from encrypted
files requires an unencrypted destination. These limitations must accompany the portable beta.

## Verification and remaining work

Retain the existing whole-file kill test, next-session save test, alias recovery cases,
metadata, locked-file and hardlink tests, plus warning and normal-close UI tests. Run
the combined local Windows and Linux checks and stage a private-desktop self-test.
This is a limited beta, not full Rider parity. Broader metadata/filesystem recovery,
interrupted in-place writes and unfinished feature integrations remain future work.
