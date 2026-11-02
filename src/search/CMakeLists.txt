# SPDX-License-Identifier: Apache-2.0
#
# frost::search (PLAN.md section 3.2, src/search; ADR 0045): Find and Replace in Files. The Qt-free
# engine searches a scope's files in parallel on sched workers with frost::text's SearchPattern,
# decodes them through frost::document's load service (m1/encodings' detection, the large-file
# guard), searches open documents in their buffers, and applies Replace in Files through the
# SaveManager. Layer "search" (rank 7): it links document and project (rank 6).
# TODO(M13): the fuzzy matcher and the trigram index PLAN.md assigns here join this module.

frost_add_library(search LAYER search
    SOURCES
        src/file_mask.cpp
        src/find_in_files.cpp
        src/replace_in_files.cpp
        src/search_scope.cpp
    DEPS frost::document frost::project frost::text_editor frost::text frost::storage frost::io frost::sched)
