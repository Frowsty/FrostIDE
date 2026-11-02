// SPDX-License-Identifier: Apache-2.0
//
// The Find in Files engine (find_in_files.hpp; ADR 0045): a coordinator coroutine resolves the file
// list (walking a Directory target with io::scanTree), keeps up to `parallelism` per-file tasks in
// flight on the scheduler, joins them in list order and hands the matches to the sink in batches.

#include "frost/search/find_in_files.hpp"

#include "frost/document/save_manager.hpp"
#include "frost/io/path.hpp"
#include "frost/io/tree_scan.hpp"
#include "frost/search/file_mask.hpp"
#include "frost/storage/fingerprint.hpp"
#include "frost/text/encoding_detection.hpp"

#include <algorithm>
#include <chrono>
#include <deque>
#include <format>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace frost::search {
namespace {

using namespace std::chrono_literals;

// What every file task reads; it lives in the coordinator's frame until every task is joined.
struct SearchContext {
    const text::SearchPattern* pattern = nullptr;
    const FindLimits* limits = nullptr;
    bool multiline = false;
    const document::LoadService* loader = nullptr;
    text::ITextCodecProvider* codecs = nullptr;
    const std::unordered_map<std::string, const OpenDocumentText*>* open = nullptr;
};

struct FileOutcome {
    enum class Kind : std::uint8_t { Searched, Skipped, Cancelled };
    Kind kind = Kind::Searched;
    FileMatches matches;
    SkippedFile skipped;
};

// The last component of an absolute path in either syntax.
[[nodiscard]] std::string_view baseName(std::string_view path) noexcept {
    const io::PathStyle style =
        io::isAbsolutePath(path, io::PathStyle::Windows) ? io::PathStyle::Windows : io::PathStyle::Posix;
    return io::fileName(path, style);
}

// Default excludes have lower precedence than project ignore files, so !build/ can re-include.
[[nodiscard]] io::IgnoreRules directoryExcludes(const std::vector<std::string>& names) {
    std::string patterns;
    for (const std::string& name : names) {
        for (const char c : name) {
            if (std::string_view("\\!*?[]# ").find(c) != std::string_view::npos)
                patterns += '\\';
            patterns += c;
        }
        patterns += "/\n";
    }
    return io::IgnoreRules::parse(patterns, {}, "Find directory exclusions", io::IgnoreOptions{true});
}

// True for a path equal to or below a directory, respecting path component boundaries.
[[nodiscard]] bool withinDirectory(std::string_view path, std::string_view directory) {
    const std::string child = pathKey(path);
    std::string parent = pathKey(directory);
    if (child == parent)
        return true;
    if (parent.empty())
        return false;
    if (parent.back() != '/' && parent.back() != '\\') {
        parent += io::isAbsolutePath(directory, io::PathStyle::Windows) ? '\\' : '/';
    }
    return child.starts_with(parent);
}

FileOutcome skippedOutcome(std::string path, SkipKind kind, Error error) {
    FileOutcome outcome;
    outcome.kind = FileOutcome::Kind::Skipped;
    outcome.skipped = SkippedFile{std::move(path), kind, std::move(error)};
    return outcome;
}

FileOutcome searchOne(const SearchContext& context, const std::string& path, const std::stop_token& token) {
    FileOutcome outcome;
    if (token.stop_requested()) {
        outcome.kind = FileOutcome::Kind::Cancelled;
        return outcome;
    }
    outcome.matches.path = path;
    const std::size_t cap = context.limits->maxMatches + 1; // One more says "the limit was crossed".
    if (const auto open = context.open->find(pathKey(path)); open != context.open->end()) {
        const text::Snapshot& text = open->second->text;
        outcome.matches.openDocument = true;
        outcome.matches.matches = findInText(*context.pattern, text, context.multiline, *context.limits, cap);
        if (!outcome.matches.matches.empty()) {
            outcome.matches.textHash = hashText(text);
        }
        return outcome;
    }
    auto raw = context.loader->read(path);
    if (!raw) {
        const SkipKind kind = raw.error().code() == ErrorCode::OutOfRange ? SkipKind::TooLarge : SkipKind::Unreadable;
        return skippedOutcome(path, kind, std::move(raw.error()));
    }
    const text::EncodingDetection detection =
        text::detectEncoding(raw->bytes, context.loader->options().text.detect, *context.codecs);
    if (detection.binaryLikely) {
        return skippedOutcome(path, SkipKind::Binary, Error(ErrorCode::InvalidArgument, "looks like a binary file"));
    }
    if (token.stop_requested()) {
        outcome.kind = FileOutcome::Kind::Cancelled;
        return outcome;
    }
    auto loaded = context.loader->decode(std::move(*raw), path);
    if (!loaded) {
        const SkipKind kind =
            loaded.error().code() == ErrorCode::OutOfRange ? SkipKind::TooLarge : SkipKind::Unreadable;
        return skippedOutcome(path, kind, std::move(loaded.error()));
    }
    if (loaded->binaryLikely) {
        return skippedOutcome(path, SkipKind::Binary, Error(ErrorCode::InvalidArgument, "looks like a binary file"));
    }
    const text::Snapshot text = loaded->buffer.snapshot();
    outcome.matches.matches = findInText(*context.pattern, text, context.multiline, *context.limits, cap);
    if (!outcome.matches.matches.empty()) {
        outcome.matches.textHash = hashText(text);
    }
    return outcome;
}

sched::Task<FileOutcome> fileTask(const SearchContext& context, std::string path, std::stop_token token) {
    co_return searchOne(context, path, token);
}

// `range` (char boundaries) cut to at most `maxBytes`, ending on a char boundary.
[[nodiscard]] std::string cutText(const text::Snapshot& text, text::ByteRange range, std::size_t maxBytes) {
    if (range.size() > maxBytes) {
        range.end = text.clip(range.begin + maxBytes);
    }
    return text.text(range);
}

} // namespace

std::vector<OpenDocumentText> SaveManagerDocuments::openDocuments() const {
    std::vector<OpenDocumentText> out;
    for (const auto& document : m_manager->documents()) {
        if (!document->untitled()) {
            out.push_back(OpenDocumentText{document->path(), document->document().committedText()});
        }
    }
    return out;
}

std::uint64_t hashText(const text::Snapshot& text) noexcept {
    storage::Xxh3Stream64 stream;
    for (const std::string_view chunk : text.chunks(text::ByteRange{ByteOffset{0}, text.size()})) {
        stream.update(chunk);
    }
    return stream.digest();
}

std::vector<FindMatch> findInText(const text::SearchPattern& pattern, const text::Snapshot& text, bool multiline,
                                  const FindLimits& limits, std::size_t cap) {
    std::vector<FindMatch> out;
    if (cap == 0) {
        return out;
    }
    const std::size_t maxPreview = std::max<std::size_t>(limits.maxPreviewBytes, 16);
    std::optional<std::uint32_t> cachedLine;
    text::LineInfo info{};
    std::string lineText;
    const auto contextLine = [&](std::uint32_t line) {
        const text::LineInfo other = *text.lineInfo(LineIndex{line});
        return cutText(text, other.content, maxPreview);
    };
    for (const text::ByteRange& range : pattern.findAll(text)) {
        if (range.empty()) {
            continue;
        }
        const std::uint32_t line = text.lineAt(range.begin).value();
        if (cachedLine != line) {
            info = *text.lineInfo(LineIndex{line});
            lineText.clear();
            cachedLine = line;
        }
        if (!multiline && range.end > info.content.end) {
            continue; // It spans a line break.
        }
        FindMatch match;
        match.line = line;
        match.column = static_cast<std::uint32_t>(range.begin - info.content.begin);
        match.length = static_cast<std::uint32_t>(range.size());
        match.offset = range.begin.value();
        const ByteOffset matchEnd = std::min(range.end, info.content.end);
        if (info.content.size() <= maxPreview) {
            if (lineText.empty()) {
                lineText = text.text(info.content);
            }
            match.lineText = lineText;
            match.previewStart = 0;
            match.previewLength = static_cast<std::uint32_t>(matchEnd - range.begin);
        } else {
            // A window of maxPreview bytes with the match about a third of the way in.
            const std::uint64_t lead = std::min<std::uint64_t>(match.column, maxPreview / 3);
            const ByteOffset begin = text.clip(range.begin - lead);
            const ByteOffset end = text.clip(std::min(info.content.end, begin + maxPreview));
            match.lineText = text.text(text::ByteRange{begin, end});
            match.previewStart = static_cast<std::uint32_t>(begin - info.content.begin);
            match.previewLength = static_cast<std::uint32_t>(std::min(matchEnd, end) - range.begin);
        }
        match.previewColumn = match.column - match.previewStart;
        const std::uint32_t first = line >= limits.contextLines ? line - limits.contextLines : 0;
        for (std::uint32_t l = first; l < line; ++l) {
            match.before.push_back(contextLine(l));
        }
        const std::uint32_t lastLine = text.lineCount() - 1;
        for (std::uint32_t l = line + 1; l <= lastLine && l <= line + limits.contextLines; ++l) {
            match.after.push_back(contextLine(l));
        }
        out.push_back(std::move(match));
        if (out.size() >= cap) {
            break;
        }
    }
    return out;
}

sched::Task<Result<FindSummary>> findInFiles(sched::IScheduler& scheduler, const document::LoadService& loader,
                                             FindRequest request, FindSink sink, std::stop_token token,
                                             text::ITextCodecProvider& codecs) {
    const FindLimits limits = request.limits;
    if (limits.parallelism == 0 || limits.batchFiles == 0) {
        co_return fail(ErrorCode::InvalidArgument, "Find in Files: parallelism and batchFiles must be at least 1");
    }
    auto pattern = text::SearchPattern::compile(request.query.pattern, request.query.options);
    if (!pattern) {
        co_return std::unexpected(std::move(pattern.error()));
    }
    auto mask = FileMask::parse(request.query.fileMask);
    if (!mask) {
        co_return std::unexpected(std::move(mask.error()));
    }
    co_await scheduler.schedule(limits.lane);

    FindSummary summary;
    std::vector<std::string> files = std::move(request.target.files);
    if (request.target.directory) {
        io::ScanOptions scan;
        scan.readGitignore = true;
        scan.extraIgnoreFiles = {".ignore"};
        scan.skipNames = io::defaultProjectSkipNames();
        scan.ignore = scan.ignore.pushed(directoryExcludes(request.target.excludes));
        scan.lane = limits.lane;
        scan.parallelism = limits.parallelism;
        const std::string selected = *request.target.directory;
        const std::string root =
            !request.target.ignoreRoot.empty() && withinDirectory(selected, request.target.ignoreRoot)
                ? request.target.ignoreRoot
                : selected;
        // Walking an ancestor must not turn a missing or non-directory selection into zero matches.
        if (root != selected) {
            const auto status = loader.fileSystem().status(selected);
            if (!status)
                co_return std::unexpected(status.error());
            if (status->type != io::FileType::Directory)
                co_return fail(ErrorCode::InvalidArgument, std::format("'{}' is not a directory", selected));
        }
        scan.descend = [selected](std::string_view directory) {
            return withinDirectory(directory, selected) || withinDirectory(selected, directory);
        };
        auto tree = co_await io::scanTree(loader.fileSystem(), scheduler, root, std::move(scan), token);
        if (!tree) {
            if (tree.error().code() == ErrorCode::Cancelled) {
                summary.cancelled = true;
                co_return summary;
            }
            co_return std::unexpected(std::move(tree.error()));
        }
        for (io::ScanEntry& entry : tree->entries) {
            if (entry.type == io::FileType::Regular && withinDirectory(entry.path, selected)) {
                files.push_back(std::move(entry.path));
            }
        }
    }
    std::erase_if(files, [&](const std::string& path) { return !mask->matches(baseName(path)); });
    summary.filesInScope = files.size();

    std::unordered_map<std::string, const OpenDocumentText*> open;
    for (const OpenDocumentText& document : request.openDocuments) {
        open.emplace(pathKey(document.path), &document);
    }
    const SearchContext context{&*pattern, &limits, request.query.multiline, &loader, &codecs, &open};

    FindBatch batch;
    IClock::SteadyTime lastFlush = scheduler.clock().steadyNow();
    const auto flush = [&] {
        if (batch.files.empty()) {
            return;
        }
        if (sink) {
            sink(std::move(batch));
        }
        batch = FindBatch{};
        lastFlush = scheduler.clock().steadyNow();
    };

    std::deque<sched::JoinHandle<FileOutcome>> inflight;
    std::size_t next = 0;
    bool stopped = false;
    while (next < files.size() || !inflight.empty()) {
        while (!stopped && next < files.size() && inflight.size() < limits.parallelism) {
            inflight.push_back(scheduler.spawn(limits.lane, fileTask(context, files[next], token)));
            ++next;
        }
        if (inflight.empty()) {
            break;
        }
        // Every task in flight is joined, also after a stop: they reference `context`.
        FileOutcome outcome = co_await std::move(inflight.front());
        inflight.pop_front();
        if (stopped) {
            continue;
        }
        if (token.stop_requested() || outcome.kind == FileOutcome::Kind::Cancelled) {
            summary.cancelled = true;
            stopped = true;
            continue;
        }
        if (outcome.kind == FileOutcome::Kind::Skipped) {
            switch (outcome.skipped.kind) {
            case SkipKind::Binary: ++summary.skippedBinary; break;
            case SkipKind::TooLarge: ++summary.skippedLarge; break;
            case SkipKind::Unreadable: ++summary.skippedUnreadable; break;
            }
            if (summary.skipped.size() < kMaxReportedSkips) {
                summary.skipped.push_back(std::move(outcome.skipped));
            }
            continue;
        }
        ++summary.filesSearched;
        std::vector<FindMatch>& matches = outcome.matches.matches;
        const std::size_t remaining = limits.maxMatches - std::min(limits.maxMatches, summary.matches);
        if (matches.size() > remaining) {
            matches.resize(remaining);
            summary.limitReached = true;
            stopped = true;
        }
        if (!matches.empty()) {
            ++summary.filesMatched;
            summary.matches += matches.size();
            batch.files.push_back(std::move(outcome.matches));
        }
        if (batch.files.size() >= limits.batchFiles ||
            (!batch.files.empty() && scheduler.clock().steadyNow() - lastFlush >= 100ms)) {
            flush();
        }
    }
    flush();
    co_return summary;
}

} // namespace frost::search
