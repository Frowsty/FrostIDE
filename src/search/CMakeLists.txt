# SPDX-License-Identifier: Apache-2.0
#
# frost::search (PLAN.md section 3.2 "search, fuzzy matcher, trigram index"; ADR 0042): the fuzzy
# matcher behind Go to File and Search Everywhere's file section (path segments, CamelHump and word
# starts) and the "file:line" suffix parser. Qt-free, layer search (rank 7); the trigram index and
# text search arrive in M2.
frost_add_library(search LAYER search
    SOURCES
        src/fuzzy_path.cpp
    DEPS frost::core)
