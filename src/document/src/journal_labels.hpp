// SPDX-License-Identifier: Apache-2.0
//
// The hot-exit journal's encoding labels (ADR 0028 decision 6; ADR 0018 D18): how a document's
// whole text::DocumentEncoding travels through storage::DocumentState's opaque encoding, hasBom and
// lineEnding fields, so a lossy document is still guarded after a crash. Private to frost::document.
/*
 * DocumentState::encoding holds nine '|'-separated fields:
 *   <encoding name>|<loss kind>|<replacements>|<first loss file offset>|<first loss text offset>|
 *   <detected encoding name>|<detected bom 0/1>|<confidence>|<detection source>
 * with empty offsets for nullopt, names from encodingName, lossKindName and detectionSourceName.
 * hasBom is the format's BOM, lineEnding is separatorName of the format's separator.
 * decodeJournalLabels validates every field; anything it cannot read gives the conservative
 * result ADR 0018 asks for: UTF-8 (the journal content is the buffer's UTF-8), marked lossy
 * (LossKind::Replaced), so saving it needs the user's consent.
 */
#pragma once

#include "frost/storage/hot_exit_journal.hpp"
#include "frost/text/text_file.hpp"

#include <string>

namespace frost::document::detail {

// Writes `encoding` into the three label fields of `state`. O(1).
void encodeJournalLabels(const text::DocumentEncoding& encoding, storage::DocumentState& state);

// The encoding the labels of `state` describe, or the conservative fallback (see above). O(1).
[[nodiscard]] text::DocumentEncoding decodeJournalLabels(const storage::DocumentState& state);

} // namespace frost::document::detail
