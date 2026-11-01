# SPDX-License-Identifier: Apache-2.0
#
# frost::text (PLAN.md section 3.2; ADR 0013): the text buffer. A persistent, reference-counted
# piece tree (SumTree<PieceTraits, 8>) over append-only UTF-8 storage, O(1) snapshots readable from
# any thread, TextBuffer edits with normalised TextChanges, and line/UTF-16 position conversions.
# Anchors, selections and Document's transactional undo tree (ADR 0017) sit on top of TextBuffer.
# Qt-free. Depends on core and on io (validUtf8Prefix; SaveManager will need io later anyway).
# TODO(M1): encodings (m1/encodings) and large-file mode with mapped originals (m1/large-file)
# join here.

frost_add_library(text LAYER text
    SOURCES
        src/invariants.cpp
        src/piece_tree.cpp
        src/snapshot.cpp
        src/storage.cpp
        src/text_buffer.cpp
        src/text_summary.cpp
        src/anchor.cpp
        src/anchor_set.cpp
        src/selection.cpp
        src/document.cpp
        src/undo_tree.cpp
    DEPS frost::core frost::io)
