// SPDX-License-Identifier: Apache-2.0
//
// LoadService: the Qt-free document load service (ADR 0028; ADR 0029 decision 7). Every way a
// document gets its text goes through it: opening a file, the reload after an external change, the
// external-change merge and hot-exit restore. It reads through io::IFileSystem, decodes with
// frost::text's load pipeline (ADR 0018) and applies the large-file guard.
/*
 * The large-file guard (until large-file mode, M15):
 *   - a file of kMaxOpenFileBytes (128 MiB) or more is refused with OutOfRange before it is read;
 *   - a text whose longest line has more than kMaxEditableLineChars characters loads, marked
 *     ReadOnlyReason::LongLine, so the editor opens it read-only;
 *   - a reload or merge that would cross the size threshold fails here, and the SaveManager keeps
 *     the buffer and its journal entry, marked out of date (DiskState::OutOfDate).
 * Thread safety: const members are safe from any thread when the file system and the codec
 * provider are (the real ones are). Every call that reads blocks on disk I/O.
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/io/file_system.hpp"
#include "frost/storage/hot_exit_journal.hpp"
#include "frost/text/snapshot.hpp"
#include "frost/text/text_buffer.hpp"
#include "frost/text/text_codec.hpp"
#include "frost/text/text_file.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace frost::document {

// Files of this many bytes or more are refused (ADR 0029 decision 7): 128 MiB.
inline constexpr std::uint64_t kMaxOpenFileBytes = std::uint64_t{128} << 20;

// The longest line, in characters, that the editor view edits; a longer line opens the document
// read-only. A named constant here; m1/editor-view sets the final value.
inline constexpr std::uint32_t kMaxEditableLineChars = std::uint32_t{1} << 20;

// Why a loaded document is read-only in the editor.
enum class ReadOnlyReason : std::uint8_t {
    None,
    LongLine, // A line is longer than LoadServiceOptions::maxEditableLineChars.
};

struct LoadServiceOptions {
    // Files of this size or more are refused (OutOfRange). FROST_PRE: >= 1.
    std::uint64_t maxFileBytes = kMaxOpenFileBytes;
    // Texts with a longer line load read-only.
    std::uint32_t maxEditableLineChars = kMaxEditableLineChars;
    // Detection and buffer options for frost::text's decoder; its maxFileBytes is replaced by
    // maxFileBytes - 1, and an encoding passed to load() or decode() replaces its override.
    text::LoadOptions text{};
};

// A file's raw bytes and the identity of what was read (size, modification time, XXH3-128).
struct RawFile {
    std::string bytes;
    storage::DiskBase disk;
};

// A decoded document, ready to become a text::Document.
struct LoadedDocument {
    text::TextBuffer buffer; // Version 0, the decoded text.
    text::DocumentEncoding encoding;
    text::EolStats eol;
    bool binaryLikely = false;
    std::vector<text::CandidateAttempt> tried;
    std::optional<storage::DiskBase> disk; // The file as read; nullopt for text that came from the journal.
    ReadOnlyReason readOnly = ReadOnlyReason::None;
};

class LoadService {
public:
    // Lifetime: `fs` and `codecs` outlive the service. Precondition: options.maxFileBytes >= 1.
    explicit LoadService(io::IFileSystem& fs, LoadServiceOptions options = {},
                         text::ITextCodecProvider& codecs = text::platformCodecs()) noexcept;

    /*
     * Reads and decodes the file at `path` (read() then decode()). `encoding` is the user's
     * override ("Reload in"), else detection decides. Errors: read()'s and decode()'s, each with the
     * context "while loading <path>". O(size).
     */
    [[nodiscard]] Result<LoadedDocument> load(std::string_view path,
                                              std::optional<text::Encoding> encoding = std::nullopt) const;

    /*
     * The file's bytes and identity, without decoding. The size guard runs on fs.status() before the
     * read and again on the bytes read (a file that grew meanwhile). Errors: OutOfRange (the guard:
     * "has N bytes; files of M bytes or more are not opened"), InvalidArgument (not a regular file),
     * and the file system's errors. O(size).
     */
    [[nodiscard]] Result<RawFile> read(std::string_view path) const;

    // Decodes bytes read by read() (the reload and merge paths read first to compare fingerprints).
    // Errors: text::decodeTextFile's, with the context "while loading <path>". O(size).
    [[nodiscard]] Result<LoadedDocument> decode(RawFile file, std::string_view path,
                                                std::optional<text::Encoding> encoding = std::nullopt) const;

    /*
     * Guards UTF-8 text that does not come from the file (the hot-exit journal's content): the size
     * rule on text.size(), then the buffer and the long-line rule. `label` names the source in errors.
     * The encoding is UTF-8 (the restore code replaces it with the journal's). Errors: OutOfRange (the
     * guard), InvalidArgument (not valid UTF-8). O(size).
     */
    [[nodiscard]] Result<LoadedDocument> fromUtf8(std::string text, std::string_view label) const;

    // The size rule alone: OutOfRange when `bytes` >= maxFileBytes, naming `path`. O(1).
    [[nodiscard]] Result<void> checkSize(std::uint64_t bytes, std::string_view path) const;

    // The long-line rule alone. O(1) (the snapshot's summary).
    [[nodiscard]] ReadOnlyReason readOnlyReason(const text::Snapshot& text) const noexcept;

    [[nodiscard]] const LoadServiceOptions& options() const noexcept { return m_options; }
    [[nodiscard]] io::IFileSystem& fileSystem() const noexcept { return *m_fs; }

private:
    io::IFileSystem* m_fs;
    LoadServiceOptions m_options;
    text::ITextCodecProvider* m_codecs;
};

} // namespace frost::document
