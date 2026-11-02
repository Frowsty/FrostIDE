# SPDX-License-Identifier: Apache-2.0
#
# frost::vcs (PLAN.md section 3.2: git on src/diff's diff and merge; M5, ADR 0048): git over its
# CLI. Finding git, running it (English output, no prompts, time limits, cancellation of the process
# tree), repository discovery, the porcelain v2 status parser and the per-file status map the UI
# colours from, file versions from HEAD and the index, commit of selected files, rollback, branches,
# fetch, pull and push, the editor's line-change markers and the side-by-side diff model, and
# GitService, which keeps the status fresh from the file watcher and runs the operations on its own
# threads. Qt-free; depends on diff, settings, io, sched, text and core.
# TODO(M5): frost-askpass, commit signing, stash and shelve, partial commits, changelists, LFS.

frost_add_library(vcs LAYER vcs
    SOURCES
        src/environment.cpp
        src/git_locator.cpp
        src/git_repository.cpp
        src/git_runner.cpp
        src/git_service.cpp
        src/git_status.cpp
        src/line_changes.cpp
        src/serial_queue.cpp
        src/side_by_side.cpp
        src/vcs_status.cpp
    DEPS frost::diff frost::settings frost::io frost::sched frost::text frost::core)
