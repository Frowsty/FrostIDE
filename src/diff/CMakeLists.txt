# SPDX-License-Identifier: Apache-2.0
#
# frost::diff (ADR 0019): line diff with the histogram algorithm (JGit's, run the way git runs it)
# and a Myers fallback, git's classic slider compaction, intra-line refinement, a unified-diff
# renderer, and the three-way merge cores (diff3 chunking, the resolution model, conflict markers).
# Qt-free. Layer choice (ADR 0019, D1): a separate target in the text layer (rank 4), so that
# frost::text-layer targets and every higher layer (the save manager, the merge view, M2's CLI, M5's
# vcs) can link it, while frost_text itself does not grow. Depends on core and text (Snapshot input
# and text::Edit output).
frost_add_library(diff LAYER text
    SOURCES
        src/compaction.cpp
        src/escape.cpp
        src/histogram.cpp
        src/interner.cpp
        src/line_diff.cpp
        src/line_sequence.cpp
        src/merge.cpp
        src/merge_model.cpp
        src/myers.cpp
        src/refine.cpp
        src/unified.cpp
    DEPS frost::core frost::text)
