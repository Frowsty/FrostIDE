// SPDX-License-Identifier: Apache-2.0
//
// The document load service (ADR 0028; ADR 0029 decision 7): reads through io::IFileSystem with
// the 128 MiB guard, fingerprints the bytes for external-change checks, decodes with frost::text's
// pipeline and marks texts with over-long lines (frost::text::kLongLineCapBytes) read-only.

#include "frost/document/load_service.hpp"

#include "frost/core/assert.hpp"
#include "frost/storage/fingerprint.hpp"

#include <format>
#include <string>
#include <utility>

namespace frost::document {

LoadService::LoadService(io::IFileSystem& fs, LoadServiceOptions options, text::ITextCodecProvider& codecs) noexcept
    : m_fs(&fs), m_options(std::move(options)), m_codecs(&codecs) {
    FROST_PRE(m_options.maxFileBytes >= 1);
    m_options.text.maxFileBytes = m_options.maxFileBytes - 1;
}

Result<void> LoadService::checkSize(std::uint64_t bytes, std::string_view path) const {
    if (bytes >= m_options.maxFileBytes) {
        return fail(ErrorCode::OutOfRange,
                    std::format("'{}' has {} bytes; files of {} bytes or more are not opened until large-file mode",
                                path, bytes, m_options.maxFileBytes));
    }
    return {};
}

ReadOnlyReason LoadService::readOnlyReason(const text::Snapshot& text) const noexcept {
    /*
     * The summary counts the longest line in characters, not bytes. Snapshot content is well-formed
     * UTF-8, so a line has at least as many bytes as characters and at most four times as many:
     * a text no larger than the cap, or whose longest line has at most a quarter of the cap in
     * characters, has no long line, and one whose longest line has more characters than the cap
     * has one. Only the texts in between are measured line by line.
     */
    const std::uint64_t cap = m_options.maxEditableLineBytes;
    const text::TextSummary& summary = text.summary();
    const std::uint64_t longestChars = summary.longestLineChars;
    if (summary.bytes <= cap || longestChars * 4 <= cap) {
        return ReadOnlyReason::None;
    }
    if (longestChars > cap) {
        return ReadOnlyReason::LongLine;
    }
    auto lines = text.linesFrom(LineIndex{0});
    if (!lines) {
        return ReadOnlyReason::None; // Unreachable: every text has line 0.
    }
    text::LineInfo line;
    while (lines->next(line)) {
        if (line.content.size() > cap) {
            return ReadOnlyReason::LongLine;
        }
    }
    return ReadOnlyReason::None;
}

Result<RawFile> LoadService::read(std::string_view path) const {
    auto status = m_fs->status(path);
    if (!status) {
        return std::unexpected(std::move(status.error()));
    }
    if (status->type != io::FileType::Regular) {
        return fail(ErrorCode::InvalidArgument, std::format("'{}' is not a regular file", path));
    }
    if (auto guard = checkSize(status->size, path); !guard) {
        return std::unexpected(std::move(guard.error()));
    }
    auto bytes = m_fs->readFile(path);
    if (!bytes) {
        return std::unexpected(std::move(bytes.error()));
    }
    // A file that grew between status and read is checked again on what was read.
    if (auto guard = checkSize(bytes->size(), path); !guard) {
        return std::unexpected(std::move(guard.error()));
    }
    RawFile file;
    file.disk.size = bytes->size();
    file.disk.mtime = status->mtime;
    file.disk.fingerprint = storage::xxh3Hash128(*bytes);
    file.bytes = std::move(*bytes);
    return file;
}

Result<LoadedDocument> LoadService::decode(RawFile file, std::string_view path,
                                           std::optional<text::Encoding> encoding) const {
    text::LoadOptions options = m_options.text;
    if (encoding) {
        options.encoding = encoding;
    }
    auto decoded = text::decodeTextFile(std::move(file.bytes), options, *m_codecs);
    if (!decoded) {
        return std::unexpected(std::move(decoded.error()).withContext(std::format("while loading {}", path)));
    }
    LoadedDocument loaded{std::move(decoded->buffer), decoded->encoding, decoded->eol,        decoded->binaryLikely,
                          std::move(decoded->tried),  file.disk,         ReadOnlyReason::None};
    loaded.readOnly = readOnlyReason(loaded.buffer.snapshot());
    return loaded;
}

Result<LoadedDocument> LoadService::load(std::string_view path, std::optional<text::Encoding> encoding) const {
    auto raw = read(path);
    if (!raw) {
        return std::unexpected(std::move(raw.error()).withContext(std::format("while loading {}", path)));
    }
    return decode(std::move(*raw), path, encoding);
}

Result<LoadedDocument> LoadService::fromUtf8(std::string text, std::string_view label) const {
    if (auto guard = checkSize(text.size(), label); !guard) {
        return std::unexpected(std::move(guard.error()));
    }
    auto buffer = text::TextBuffer::fromUtf8(std::move(text), m_options.text.buffer);
    if (!buffer) {
        return std::unexpected(std::move(buffer.error()).withContext(std::format("while restoring {}", label)));
    }
    LoadedDocument loaded{std::move(*buffer), {}, {}, false, {}, std::nullopt, ReadOnlyReason::None};
    loaded.encoding.format = text::TextFileFormat{text::Encoding::utf8(), false, text::platformDefaultSeparator()};
    loaded.eol = text::eolStats(loaded.buffer.snapshot());
    loaded.readOnly = readOnlyReason(loaded.buffer.snapshot());
    return loaded;
}

} // namespace frost::document
