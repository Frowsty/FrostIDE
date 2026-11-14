# SPDX-License-Identifier: Apache-2.0
#
# frost::document (ADR 0028; PLAN.md section 2.2 "Data safety", I2): the SaveManager and the
# Qt-free document load service. Opens files through the load service with the large-file guard
# (ADR 0029 decision 7), saves byte-exactly through io::IFileSystem::safeWrite, autosaves from the
# save.* settings, journals unsaved buffers for hot exit (frost::storage), records local history,
# turns file-watcher changes into reloads or three-way merge models (frost::diff), and rewrites
# closed files for Replace in Files (ADR 0045). Layer "document" (rank 6): it links storage and
# diff (both rank 4) and settings (rank 5), ADR 0019 D1.
frost_add_library(document LAYER document
    SOURCES
        src/autosave.cpp
        src/external_change.cpp
        src/hot_exit.cpp
        src/journal_labels.cpp
        src/load_service.cpp
        src/policies.cpp
        src/rewrite.cpp
        src/refactoring_transaction.cpp
        src/save_manager.cpp
    DEPS frost::io frost::sched frost::text frost::diff frost::storage frost::settings)
