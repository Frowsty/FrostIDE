<!-- SPDX-License-Identifier: Apache-2.0 -->

# ADR 0065: Find and Replace in Files in the workbench

- Status: accepted (feature-first phase, ADR 0031)
- Date: 2026-11-02
- Plan reference: M2 item 86 and U1 Find/Replace in Files; ADR 0045

## Context

The Find component and search engine existed independently of the application composition. The
workbench must search the current solution, folder and unsaved editors, and replacements must keep
editor state and language services coherent without discarding unsaved work.

## Decision

The workbench owns FindFeature, provides the current solution, folder, selection and document services,
and opens result locations using 1-based UTF-8 byte columns. Open-document replacements follow their
ChangeSet in the editor and use its normal text-changed path, including language-service updates and
undo. Closed documents retain the existing replace engine's encoding, stale-content and read-only
checks. Replacements use the current replacement field; changing search criteria invalidates old
results until the next search starts. Searches are cancelled and joined before project documents or
the scheduler are torn down. Shutdown destroys the feature so queued actions cannot restart searches.

Directory searches use the filesystem scanner's .gitignore and .ignore rules, including ignore files
between the current project root and a selected subdirectory. The existing directory-name exclusions are lower-precedence defaults that ignore files may
re-include; VCS metadata directories are always skipped. A directory outside the
current project starts its ignore walk at the selected directory; repository-global excludes and
ignore files above that root are not loaded. No project-selected external process is run.

Automatic directory and scope choices follow project changes; explicit user choices are retained
when applicable. Moving results into the Find tool window transfers ownership of their session.

## Validation

Workbench tests exercise unsaved and closed files, byte locations, undo and encoded writes, solution
scope, stale replacements, scope changes and transferred-session shutdown. Search tests cover nested
ignore files and project-ancestor rules. These tests require the platform build and test gates; source
inspection alone is not a passing result.
