# SPDX-License-Identifier: Apache-2.0
#
# frost::search (PLAN.md section 3.2 "search, fuzzy matcher, trigram index"). Qt-free, layer
# "search" (rank 7): it links document and project (rank 6) and the lower layers. Two parts:
#   - the fuzzy matcher behind Go to File and Search Everywhere's file section (path segments,
#     CamelHump and word starts) and the "file:line" suffix parser (ADR 0042);
#   - Find and Replace in Files (ADR 0045): the engine searches a scope's files in parallel on sched
#     workers with frost::text's SearchPattern, decodes them through frost::document's load service
#     (m1/encodings' detection, the large-file guard), searches open documents in their buffers,
#     and applies Replace in Files through the SaveManager.
# TODO(M13): the trigram index PLAN.md assigns here joins this module.

frost_add_library(search LAYER search
    SOURCES
        src/file_mask.cpp
        src/find_in_files.cpp
        src/fuzzy_path.cpp
        src/replace_in_files.cpp
        src/search_scope.cpp
    DEPS frost::document frost::project frost::text_editor frost::text frost::storage frost::io frost::sched
         frost::core)
