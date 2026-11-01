# SPDX-License-Identifier: Apache-2.0
#
# frost::text (PLAN.md section 3.2; ADR 0013, ADR 0017, ADR 0018, ADR 0026): the text buffer, its
# file encodings and its display map. A persistent, reference-counted piece tree
# (SumTree<PieceTraits, 8>) over append-only UTF-8 storage, O(1) snapshots readable from any thread,
# TextBuffer edits with normalised TextChanges, and line/UTF-16 position conversions. Anchors,
# selections and Document's transactional undo tree (ADR 0017) sit on top of TextBuffer. The load
# and save codec pipeline (ADR 0018): BOMs, UTF-8/16/32, legacy code pages behind the ITextCodec
# seam (MultiByteToWideChar on Windows, glibc iconv on Linux, both already linked), detection, EOL
# statistics and the save guard. The layered display map (ADR 0026) maps the text to display rows
# through its ITextShaper seam.
# Qt-free. Depends on core and on io (validUtf8Prefix, IFileSystem, OS error mapping).
# TODO(M15): large-file mode with mapped originals joins here (moved from M1 by ADR 0029).

set(frost_text_sources
    src/anchor.cpp
    src/anchor_set.cpp
    src/display_block.cpp
    src/display_fold.cpp
    src/display_map.cpp
    src/display_wrap.cpp
    src/document.cpp
    src/encoding.cpp
    src/encoding_detection.cpp
    src/invariants.cpp
    src/legacy_codec.cpp
    src/line_separator.cpp
    src/piece_tree.cpp
    src/selection.cpp
    src/snapshot.cpp
    src/storage.cpp
    src/text_buffer.cpp
    src/text_file.cpp
    src/text_shaper.cpp
    src/text_summary.cpp
    src/undo_tree.cpp
    src/unicode_codec.cpp)
if(WIN32)
    list(APPEND frost_text_sources src/codec_win.cpp)
else()
    list(APPEND frost_text_sources src/codec_posix.cpp)
endif()

frost_add_library(text LAYER text
    SOURCES ${frost_text_sources}
    DEPS frost::core frost::io)
