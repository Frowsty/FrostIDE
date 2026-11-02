// SPDX-License-Identifier: Apache-2.0
//
// SaveManager::rewriteClosedFile (save_manager.hpp; ADR 0045): edits a file that no editor has open,
// for Replace in Files, with the save path's data-safety rules (ADR 0018, ADR 0028): the file's own
// format, a lossy load refused, a round-trip check of the new bytes, a local-history snapshot of the
// old bytes, and safeWrite.

#include "frost/document/save_manager.hpp"

#include "frost/text/text_buffer.hpp"
#include "frost/text/text_file.hpp"

#include <format>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace frost::document {

Result<RewriteOutcome> SaveManager::rewriteClosedFile(std::string_view path, RewriteFunction edit,
                                                      std::string_view label) {
    auto normalized = normalize(path);
    if (!normalized) {
        return std::unexpected(std::move(normalized.error()));
    }
    const std::string& file = *normalized;
    const auto context = [&](Error error) {
        return std::unexpected(std::move(error).withContext("while rewriting " + file));
    };
    if (find(file) != nullptr) {
        return context(Error(ErrorCode::Busy, std::format("'{}' is open in an editor; edit its document instead", file)));
    }

    // 1. Read and decode, keeping the old bytes for local history and the change check.
    auto raw = m_loader.read(file);
    if (!raw) {
        return context(std::move(raw.error()));
    }
    const std::string original = raw->bytes;
    auto loaded = m_loader.decode(std::move(*raw), file);
    if (!loaded) {
        return context(std::move(loaded.error()));
    }

    // 2. Only a lossless text load can be written back without touching the other bytes.
    if (!loaded->encoding.fidelity.lossless()) {
        return context(Error(ErrorCode::Corrupt,
                             std::format("'{}' did not decode losslessly as {} ({} replaced characters), so writing "
                                         "it would change bytes outside the replacements",
                                         file, text::encodingName(loaded->encoding.format.encoding),
                                         loaded->encoding.fidelity.replacements)));
    }
    if (loaded->binaryLikely) {
        return context(Error(ErrorCode::InvalidArgument, std::format("'{}' looks like a binary file", file)));
    }

    // 3. The edits.
    text::TextBuffer buffer = std::move(loaded->buffer);
    auto edits = edit(buffer.current());
    if (!edits) {
        return context(std::move(edits.error()));
    }
    RewriteOutcome outcome;
    if (edits->empty()) {
        return outcome;
    }
    std::vector<text::Edit> batch;
    batch.reserve(edits->size());
    for (const RewriteEdit& one : *edits) {
        batch.push_back(text::Edit{one.range, one.text});
    }
    if (auto applied = buffer.applyEdits(batch); !applied) {
        return context(std::move(applied.error()));
    }
    const text::Snapshot edited = buffer.snapshot();

    // 4. Encode with the file's own format; the bytes must decode back to exactly the edited text.
    text::ITextCodecProvider& codecs = m_services.codecs != nullptr ? *m_services.codecs : text::platformCodecs();
    auto bytes = text::encodeForSave(edited, loaded->encoding, text::SaveConsent::None, codecs);
    if (!bytes) {
        return context(std::move(bytes.error()));
    }
    {
        text::LoadOptions check = m_options.load.text;
        check.encoding = loaded->encoding.format.encoding;
        check.maxFileBytes = UINT64_MAX;
        auto decoded = text::decodeBytes(*bytes, check, codecs);
        const bool same = decoded && decoded->encoding.fidelity.lossless() &&
                          (decoded->payloadIsText ? std::string_view(*bytes).substr(decoded->payloadOffset)
                                                  : std::string_view(decoded->text)) == edited.text();
        if (!same) {
            return context(Error(ErrorCode::Internal,
                                 std::format("the new bytes of '{}' do not decode back to the replaced text", file)));
        }
    }

    // 5. The old bytes go into local history before anything is written.
    const bool history = m_services.history != nullptr && m_policies.localHistory;
    if (history) {
        const std::string_view pieces[] = {original};
        if (auto recorded = m_services.history->record(file, file, storage::RevisionKind::Labelled,
                                                       std::format("Before {}", label), pieces);
            !recorded) {
            return context(std::move(recorded.error()));
        }
    }

    // 6. The file must still hold what was read; then the atomic, attribute-keeping write.
    auto current = m_services.fs->readFile(file);
    if (!current) {
        return context(std::move(current.error()));
    }
    if (*current != original) {
        return context(Error(ErrorCode::Busy, std::format("'{}' changed on disk while it was being rewritten", file)));
    }
    auto written = m_services.fs->safeWrite(file, *bytes, io::SafeWriteOptions{false});
    if (!written) {
        return context(std::move(written.error()));
    }
    outcome.written = true;
    outcome.target = std::move(written->target);
    outcome.bytes = bytes->size();
    outcome.edits = edits->size();
    if (history) {
        const std::string_view pieces[] = {*bytes};
        if (auto recorded = m_services.history->record(file, file, storage::RevisionKind::Save, label, pieces);
            !recorded) {
            outcome.warnings.push_back(std::move(recorded.error()));
        }
    }
    sweepDirectory(io::parentPath(outcome.target, m_options.style), outcome.warnings);
    return outcome;
}

} // namespace frost::document
