# SPDX-License-Identifier: Apache-2.0
#
# frost::text (PLAN.md section 3.2; ADR 0013, ADR 0018): the text buffer and its file encodings. A
# persistent, reference-counted piece tree (SumTree<PieceTraits, 8>) over append-only UTF-8 storage,
# O(1) snapshots readable from any thread, TextBuffer edits with normalised TextChanges, and
# line/UTF-16 position conversions; plus the load and save codec pipeline (ADR 0018): BOMs,
# UTF-8/16/32, legacy code pages behind the ITextCodec seam (MultiByteToWideChar on Windows, glibc
# iconv on Linux, both already linked), detection, EOL statistics and the save guard.
# Qt-free. Depends on core and on io (validUtf8Prefix, IFileSystem, OS error mapping).
# TODO(M1): anchors and the undo tree (m1/undo-anchors) and large-file mode with mapped originals
# (m1/large-file) join here.

set(frost_text_sources
    src/encoding.cpp
    src/encoding_detection.cpp
    src/invariants.cpp
    src/legacy_codec.cpp
    src/line_separator.cpp
    src/piece_tree.cpp
    src/snapshot.cpp
    src/storage.cpp
    src/text_buffer.cpp
    src/text_file.cpp
    src/text_summary.cpp
    src/unicode_codec.cpp)
if(WIN32)
    list(APPEND frost_text_sources src/codec_win.cpp)
else()
    list(APPEND frost_text_sources src/codec_posix.cpp)
endif()

frost_add_library(text LAYER text
    SOURCES ${frost_text_sources}
    DEPS frost::core frost::io)
