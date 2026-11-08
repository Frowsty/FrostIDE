// SPDX-License-Identifier: Apache-2.0
//
// Find in Files (PLAN.md row 86; ADR 0045): the Qt-free search engine behind the Find in Files
// popup and the Find tool window. It searches a scope's files in parallel on sched workers and
// streams the matches back in file order, in batches; it is cancellable through a std::stop_token.
/*
 * Matching is frost::text's SearchPattern (ADR 0025), so the popup and the in-file find agree:
 * plain or RE2 regex, case-sensitive or not, whole words. Matches are per line unless
 * FindQuery::multiline is on (Rider's default): a match that spans a line break is dropped. Empty
 * regex matches are never reported.
 *
 * Text. A file open in an editor is searched in its committed buffer text (unsaved edits included),
 * captured on the owner thread through IOpenDocuments before the search starts. Every other file is
 * read through document::LoadService, so the large-file guard applies (files at or above its
 * threshold are skipped and counted), and decoded with m1/encodings' detection (UTF-8 and UTF-16
 * with or without a BOM, UTF-32, the legacy code pages). A file whose sample looks binary (a NUL
 * byte outside UTF-16/32) is skipped and counted.
 *
 * Order and batches. The files run on `limits.parallelism` tasks at once and are joined in list
 * order, so the sink sees FileMatches in exactly the order of the file list (Directory: sorted by
 * path). A batch goes out after `limits.batchFiles` files with matches, after 100 ms with at least
 * one, and at the end. The sink runs on the engine's lane (a scheduler thread), one call at a time.
 *
 * Limits. After limits.maxMatches matches (10,000 by default) the search stops and the summary says
 * limitReached ("too many results"); the file that crossed the limit is cut. Previews are cut to
 * limits.maxPreviewBytes around the match, at character boundaries.
 *
 * Lifetime: the loader, its file system and codecs, and the scheduler outlive the returned task.
 * Snapshots in OpenDocumentText are immutable and safe on any thread.
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/document/load_service.hpp"
#include "frost/sched/lane.hpp"
#include "frost/sched/scheduler.hpp"
#include "frost/sched/task.hpp"
#include "frost/search/search_scope.hpp"
#include "frost/text/editor/search.hpp"
#include "frost/text/snapshot.hpp"
#include "frost/text/text_codec.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace frost::document {
class SaveManager;
} // namespace frost::document

namespace frost::search {

// Rider's cap on Find in Files results.
inline constexpr std::size_t kMaxMatches = 10'000;

// What to look for.
struct FindQuery {
    std::string pattern;
    text::SearchOptions options;
    // Off (the default): a match never spans a line break.
    bool multiline = false;
    // FileMask syntax (file_mask.hpp); empty: every file.
    std::string fileMask;
};

struct FindLimits {
    std::size_t maxMatches = kMaxMatches;
    // Lines of context kept before and after each match, for the preview pane.
    std::uint32_t contextLines = 2;
    // A preview line (and each context line) is cut to about this many bytes.
    std::size_t maxPreviewBytes = 320;
    // Files searched at once; at least 1.
    unsigned parallelism = 4;
    // Files with matches per batch; at least 1.
    std::size_t batchFiles = 8;
    sched::Lane lane = sched::Lane::UserVisible;
};

// One open document's text, captured on its owner thread.
struct OpenDocumentText {
    std::string path;
    text::Snapshot text;
};

/*
 * The documents open in editors, whose unsaved text is searched instead of the disk's. Called on the
 * documents' owner thread (the UI thread) before a search starts; the engine only keeps the
 * snapshots. Never deleted through this interface.
 */
class IOpenDocuments {
public:
    // Every open document that has a file (untitled buffers are left out), in tab or opening order.
    [[nodiscard]] virtual std::vector<OpenDocumentText> openDocuments() const = 0;

protected:
    IOpenDocuments() = default;
    IOpenDocuments(const IOpenDocuments&) = default;
    IOpenDocuments& operator=(const IOpenDocuments&) = default;
    ~IOpenDocuments() = default;
};

// IOpenDocuments over a SaveManager: its documents' committed text. Owner thread only; the manager
// outlives this object.
class SaveManagerDocuments final : public IOpenDocuments {
public:
    explicit SaveManagerDocuments(const document::SaveManager& manager) noexcept : m_manager(&manager) {}
    [[nodiscard]] std::vector<OpenDocumentText> openDocuments() const override;

private:
    const document::SaveManager* m_manager;
};

// One match. Lines and columns are 0-based; columns and lengths count UTF-8 bytes.
struct FindMatch {
    std::uint32_t line = 0;
    std::uint32_t column = 0; // In the line.
    std::uint32_t length = 0; // Of the match (it may run past the line with multiline).
    std::uint64_t offset = 0; // Of the match in the decoded text (Replace in Files uses it).
    // The line without its break, cut around the match to about maxPreviewBytes.
    std::string lineText;
    std::uint32_t previewStart = 0;  // The line column where lineText starts (0 unless cut).
    std::uint32_t previewColumn = 0; // The match in lineText.
    std::uint32_t previewLength = 0; // The part of the match inside lineText.
    std::vector<std::string> before; // Up to contextLines lines before, oldest first.
    std::vector<std::string> after;  // Up to contextLines lines after.
};

// The matches of one file, in text order.
struct FileMatches {
    std::string path;
    bool openDocument = false; // Searched in an editor's buffer.
    // XXH3-64 of the decoded text that was searched: Replace in Files refuses a file whose text
    // changed since (replace_in_files.hpp).
    std::uint64_t textHash = 0;
    std::vector<FindMatch> matches;
};

struct FindBatch {
    std::vector<FileMatches> files;
};

// Why a file was not searched.
enum class SkipKind : std::uint8_t { Binary, TooLarge, Unreadable };

struct SkippedFile {
    std::string path;
    SkipKind kind = SkipKind::Unreadable;
    Error error{ErrorCode::Unknown};
};

struct FindSummary {
    std::size_t filesInScope = 0;  // After the file mask.
    std::size_t filesSearched = 0; // Whose text was searched.
    std::size_t filesMatched = 0;
    std::size_t matches = 0;
    std::size_t skippedBinary = 0;
    std::size_t skippedLarge = 0;
    std::size_t skippedUnreadable = 0;
    // The first kMaxReportedSkips skipped files, in file order.
    std::vector<SkippedFile> skipped;
    // Directory targets: the folders the walk listed, the root included (TreeScan::directoriesListed).
    std::size_t directoriesListed = 0;
    bool limitReached = false; // Stopped at limits.maxMatches: "too many results".
    bool cancelled = false;    // Stopped by the token; the batches so far are valid.
};

inline constexpr std::size_t kMaxReportedSkips = 50;

// Receives the batches (header comment). Must not throw.
using FindSink = std::function<void(FindBatch batch)>;

struct FindRequest {
    FindQuery query;
    ScopeTarget target;
    std::vector<OpenDocumentText> openDocuments;
    FindLimits limits;
};

/*
 * Runs `request` (header comment). Resumes on request.limits.lane. Errors: InvalidArgument (the
 * pattern does not compile, the file mask does not parse, parallelism or batchFiles 0), and for a
 * Directory target the walk's own error for the root (NotFound, InvalidArgument, PermissionDenied,
 * IoError). A stop on `token` is not an error: the summary says cancelled.
 */
[[nodiscard]] sched::Task<Result<FindSummary>> findInFiles(sched::IScheduler& scheduler,
                                                           const document::LoadService& loader, FindRequest request,
                                                           FindSink sink, std::stop_token token = {},
                                                           text::ITextCodecProvider& codecs = text::platformCodecs());

// The matches of `pattern` in `text`, at most `cap`, with previews (header comment). Thread-safe.
// O(n + matches x preview).
[[nodiscard]] std::vector<FindMatch> findInText(const text::SearchPattern& pattern, const text::Snapshot& text,
                                                bool multiline, const FindLimits& limits, std::size_t cap);

// XXH3-64 of `text`'s bytes (FileMatches::textHash). O(n).
[[nodiscard]] std::uint64_t hashText(const text::Snapshot& text) noexcept;

} // namespace frost::search
