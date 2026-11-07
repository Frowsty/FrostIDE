// SPDX-License-Identifier: Apache-2.0
//
// SaveManager (save_manager.hpp, ADR 0028): opening, saving, Save All, policies, local history and
// the stale temp-file sweep. External changes are in external_change.cpp, hot exit in hot_exit.cpp.

#include "frost/document/save_manager.hpp"

#include "frost/core/assert.hpp"
#include "frost/diff/line_diff.hpp"
#include "frost/diff/line_sequence.hpp"
#include "frost/storage/fingerprint.hpp"
#include "frost/text/line_separator.hpp"
#include "frost/text/text_buffer.hpp"

#include <algorithm>
#include <format>
#include <string>
#include <utility>

namespace frost::document {

// --- The pre-save transform ---

std::vector<SaveEdit> saveTransformEdits(const text::Snapshot& current, const text::Snapshot& base,
                                         const SaveTransform& transform, std::string_view newLine) {
    std::vector<SaveEdit> edits;
    if (transform.trim == TrimTrailingWhitespace::None && !transform.insertFinalNewline)
        return edits;
    const std::string text = current.text();
    // Modified: the lines of `current` inside the line diff's hunks. Above the diff's limits no line
    // counts as modified; rewriting a whole huge file the user only touched is worse.
    std::vector<bool> modified;
    if (transform.trim == TrimTrailingWhitespace::Modified) {
        auto before = diff::LineSequence::fromSnapshot(base);
        auto after = diff::LineSequence::fromSnapshot(current);
        if (before && after) {
            if (auto lineDiff = diff::diffLines(*before, *after)) {
                modified.assign(after->size(), false);
                for (const diff::Hunk& hunk : lineDiff->hunks)
                    for (std::uint32_t line = hunk.b.begin.value(); line < hunk.b.end.value(); ++line)
                        modified[line] = true;
            }
        }
    }
    const auto keep = [&](std::uint32_t line) {
        return std::ranges::find(transform.keepLines, LineIndex{line}) != transform.keepLines.end();
    };
    const auto blank = [](char c) { return c == ' ' || c == '\t' || c == '\f' || c == '\v'; };
    std::size_t start = 0;
    for (std::uint32_t line = 0;; ++line) {
        std::size_t end = start;
        while (end < text.size() && text[end] != '\n' && text[end] != '\r')
            ++end;
        const bool trim =
            transform.trim == TrimTrailingWhitespace::All ||
            (transform.trim == TrimTrailingWhitespace::Modified && line < modified.size() && modified[line]);
        if (trim && !keep(line)) {
            std::size_t cut = end;
            while (cut > start && blank(text[cut - 1]))
                --cut;
            if (cut < end)
                edits.push_back(SaveEdit{text::ByteRange{ByteOffset{cut}, ByteOffset{end}}, {}});
        }
        if (end >= text.size())
            break;
        start = end + (text[end] == '\r' && end + 1 < text.size() && text[end + 1] == '\n' ? 2 : 1);
    }
    if (transform.insertFinalNewline && !text.empty() && text.back() != '\n' && text.back() != '\r') {
        const ByteOffset at{text.size()};
        // A trim that ends at the end of the text is followed directly by the line break.
        if (!edits.empty() && edits.back().range.end == at)
            edits.back().text = std::string(newLine);
        else
            edits.push_back(SaveEdit{text::ByteRange{at, at}, std::string(newLine)});
    }
    return edits;
}

// --- FileDocument ---

FileDocument::FileDocument(std::string path, std::string key, text::Document document) noexcept
    : m_path(std::move(path)), m_key(std::move(key)), m_document(std::move(document)) {}

bool FileDocument::dirty() const noexcept { return m_formatDirty || m_document.undoState().stateId != m_savedStateId; }

void FileDocument::convertTo(text::Encoding encoding, bool bom) noexcept {
    const text::DocumentEncoding converted = m_encoding.convertedTo(encoding, bom);
    if (converted.format != m_encoding.format) {
        m_formatDirty = true;
        ++m_formatVersion;
    }
    m_encoding = converted;
}

void FileDocument::setLineSeparator(text::LineSeparator separator) noexcept {
    if (m_encoding.format.lineSeparator != separator) {
        m_encoding.format.lineSeparator = separator;
        m_formatDirty = true;
        ++m_formatVersion;
    }
}

// --- SaveManager ---

SaveManager::SaveManager(SaveManagerServices services, SaveManagerOptions options, SavePolicies policies)
    : m_services(services), m_options(std::move(options)), m_policies(policies),
      m_loader(*services.fs, m_options.load, services.codecs != nullptr ? *services.codecs : text::platformCodecs()) {
    FROST_PRE(services.fs != nullptr && services.clock != nullptr);
}

SaveManager::~SaveManager() = default;

Result<std::string> SaveManager::normalize(std::string_view path) const {
    return io::normalizePath(path, m_options.style);
}

FileDocument& SaveManager::adopt(std::string path, std::string key, LoadedDocument loaded) {
    text::Document document(std::move(loaded.buffer), m_options.document, *m_services.clock);
    auto owned = std::unique_ptr<FileDocument>(new FileDocument(std::move(path), std::move(key), std::move(document)));
    FileDocument& file = *owned;
    file.m_encoding = loaded.encoding;
    file.m_readOnly = loaded.readOnly;
    file.m_disk = loaded.disk;
    file.m_baseText = file.m_document.committedText();
    file.m_savedStateId = file.m_document.undoState().stateId;
    file.m_diskState = file.m_path.empty() ? DiskState::Unsaved : DiskState::InSync;
    m_documents.push_back(std::move(owned));
    return file;
}

Result<FileDocument*> SaveManager::open(std::string_view path, std::optional<text::Encoding> encoding) {
    auto normalized = normalize(path);
    if (!normalized) {
        return std::unexpected(std::move(normalized.error()));
    }
    if (FileDocument* existing = find(*normalized)) {
        return existing;
    }
    auto loaded = m_loader.load(*normalized, encoding);
    if (!loaded) {
        return std::unexpected(std::move(loaded.error()));
    }
    std::vector<Error> ignored; // A sweep failure never fails an open.
    sweepDirectory(io::parentPath(*normalized, m_options.style), ignored);
    std::string key = *normalized;
    return &adopt(std::move(*normalized), std::move(key), std::move(*loaded));
}

FileDocument& SaveManager::createUntitled(text::TextFileFormat format) {
    FROST_PRE(format.isValid());
    auto buffer = text::TextBuffer::fromUtf8(std::string(), m_options.load.text.buffer);
    FROST_ASSERT(buffer.has_value()); // The empty text is always valid.
    LoadedDocument loaded{std::move(*buffer), {}, {}, false, {}, std::nullopt, ReadOnlyReason::None};
    loaded.encoding.format = format;
    loaded.encoding.detection = text::EncodingGuess{format.encoding, format.bom, 100, text::DetectionSource::Empty};
    return adopt(std::string(), std::format("untitled:{}", ++m_nextUntitled), std::move(loaded));
}

FileDocument* SaveManager::find(std::string_view path) const {
    auto normalized = normalize(path);
    if (!normalized) {
        return nullptr;
    }
    for (const auto& document : m_documents) {
        if (!document->untitled() && document->path() == *normalized) {
            return document.get();
        }
    }
    return nullptr;
}

Result<void> SaveManager::close(FileDocument& document) {
    Result<void> result;
    if (m_services.journal != nullptr && document.m_journaledGeneration != 0) {
        if (auto forgotten = m_services.journal->forget(document.key()); !forgotten) {
            result = std::unexpected(std::move(forgotten.error()));
        }
    }
    std::erase_if(m_documents, [&](const std::unique_ptr<FileDocument>& owned) { return owned.get() == &document; });
    return result;
}

bool SaveManager::anyDirty() const noexcept {
    for (const auto& document : m_documents) {
        if (document->dirty()) {
            return true;
        }
    }
    return false;
}

void SaveManager::applySettings(const settings::SettingsSnapshot& snapshot) {
    setPolicies(SavePolicies::fromSettings(snapshot));
}

// --- Saving ---

Result<SaveOutcome> SaveManager::save(FileDocument& document, const SaveOptions& options) {
    if (document.untitled()) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("{} has no file yet; save it with Save As", document.key()));
    }
    const std::string path = document.path();
    return saveTo(document, path, options, false);
}

Result<SaveOutcome> SaveManager::saveAs(FileDocument& document, std::string_view path, const SaveOptions& options) {
    auto normalized = normalize(path);
    if (!normalized) {
        return std::unexpected(std::move(normalized.error()));
    }
    if (FileDocument* other = find(*normalized); other != nullptr && other != &document) {
        return fail(ErrorCode::AlreadyExists, std::format("'{}' is already open in another editor", *normalized));
    }
    return saveTo(document, *normalized, options, *normalized != document.path());
}

Result<void> SaveManager::renameDocument(FileDocument& document, std::string_view newPath) {
    auto normalized = normalize(newPath);
    if (!normalized) {
        return std::unexpected(std::move(normalized.error()));
    }
    if (FileDocument* other = find(*normalized); other != nullptr && other != &document) {
        return fail(ErrorCode::AlreadyExists, std::format("'{}' is already open in another editor", *normalized));
    }
    const std::string oldKey = document.m_key;
    document.m_path = *normalized;
    document.m_key = *normalized;
    if (document.m_disk.has_value()) {
        if (auto status = m_services.fs->status(*normalized)) {
            document.m_disk->mtime = status->mtime;
        }
    }
    if (m_services.journal != nullptr && document.m_journaledGeneration != 0) {
        (void)m_services.journal->forget(oldKey);
        document.m_journaledGeneration = 0;
        if (document.dirty()) {
            std::vector<Error> ignored;
            putJournal(document, ignored);
        }
    }
    return {};
}

Result<SaveOutcome> SaveManager::saveTo(FileDocument& document, const std::string& path, const SaveOptions& options,
                                        bool saveAs) {
    const auto context = [&](Error error) {
        return std::unexpected(std::move(error).withContext("while saving " + path));
    };
    if (m_writeGuard) {
        if (auto allowed = m_writeGuard(path); !allowed)
            return context(std::move(allowed.error()));
    }
    SaveOutcome outcome;
    if (!saveAs && !options.overwriteExternalChanges) {
        if (document.m_diskState == DiskState::Changed) {
            return context(
                Error(ErrorCode::Busy, std::format("'{}' changed on disk; resolve the external change first, "
                                                   "or save over it",
                                                   path)));
        }
        if (document.m_diskState == DiskState::OutOfDate) {
            return context(
                Error(ErrorCode::Busy, std::format("'{}' changed on disk and the change was not loaded; save over it "
                                                   "explicitly",
                                                   path)));
        }
        // A change the watcher has not reported yet must not be overwritten silently.
        if (document.m_disk && document.m_diskState == DiskState::InSync) {
            const ExternalChange change = checkDisk(document);
            if (change.kind == ExternalChangeKind::MergePending || change.kind == ExternalChangeKind::OutOfDate) {
                if (m_listener) {
                    m_listener(change);
                }
                return context(Error(ErrorCode::Busy, std::format("'{}' changed on disk since it was loaded", path)));
            }
        }
    }
    const bool autosave = options.reason == SaveReason::Autosave;
    const bool clean = !saveAs && !document.dirty() && document.m_diskState == DiskState::InSync;
    // Clean and in sync: nothing to write, unless an autosave since the last explicit save left
    // save actions for this one (FileDocument::m_transformBase).
    if (clean && (autosave || !document.m_transformBase)) {
        return outcome;
    }
    /*
     * The pre-save transform (setSaveTransform): one undoable edit the editor shows, then the save.
     * Autosaves run it too, sparing the caret lines like every save, so with autosave on (the
     * default) the file on disk still gets the save actions. Modified lines are measured from the
     * last explicit save, not from the last autosave.
     */
    std::vector<SaveEdit> owned;
    const bool composing = document.m_document.compositionRange().has_value();
    if (m_saveTransform && !composing) {
        const SaveTransform transform = m_saveTransform(document, options.reason);
        owned = saveTransformEdits(document.m_document.committedText(),
                                   document.m_transformBase ? *document.m_transformBase : document.m_baseText,
                                   transform, text::separatorBytes(document.m_encoding.format.lineSeparator));
    }
    if (clean && owned.empty()) {
        // This explicit save is the new base: the file is already right. During an IME composition
        // the transform did not run, so what is pending stays for the next save.
        if (m_saveTransform && !composing)
            document.m_transformBase.reset();
        return outcome;
    }
    if (!owned.empty()) {
        std::vector<text::Edit> edits;
        edits.reserve(owned.size());
        for (const SaveEdit& edit : owned)
            edits.push_back(text::Edit{edit.range, edit.text});
        // Its own undo step: neither merged into the typing before it nor into the next. If the write
        // below fails, the edit stays in the (still dirty) buffer and can be undone.
        document.m_document.breakCoalescing();
        if (auto applied = document.m_document.edit(edits, text::EditContext{}); !applied) {
            applied.error().withContext("while applying the save actions");
            outcome.warnings.push_back(std::move(applied.error()));
        }
        document.m_document.breakCoalescing();
    }

    // Captured on the owner thread; everything up to the write only reads these immutable values.
    const text::Snapshot snapshot = document.m_document.committedText();
    const std::uint64_t stateId = document.m_document.undoState().stateId;
    const std::int64_t generation = generationOf(document);
    const text::DocumentEncoding encoding = document.m_encoding;

    auto bytes = text::encodeForSave(snapshot, encoding, options.consent,
                                     m_services.codecs != nullptr ? *m_services.codecs : text::platformCodecs());
    if (!bytes) {
        return context(std::move(bytes.error()));
    }
    auto written = m_services.fs->safeWrite(path, *bytes, io::SafeWriteOptions{options.overwriteReadOnly});
    if (!written) {
        return context(std::move(written.error()));
    }
    outcome.written = true;
    outcome.target = std::move(written->target);
    outcome.method = written->method;
    if (written->warning)
        outcome.warnings.push_back(std::move(*written->warning));
    outcome.bytes = bytes->size();

    storage::DiskBase base;
    base.size = bytes->size();
    base.fingerprint = storage::xxh3Hash128(*bytes);
    if (auto status = m_services.fs->status(path)) {
        base.mtime = status->mtime;
    } else {
        outcome.warnings.push_back(std::move(status.error()));
    }

    // The file now holds exactly the encoded text.
    const std::string oldKey = document.m_key;
    if (saveAs) {
        document.m_path = path;
        document.m_key = path;
    }
    document.m_encoding = encoding.afterSave();
    document.m_savedStateId = stateId;
    document.m_formatDirty = false;
    // An autosave keeps the Modified base at the last explicit save; any other save moves it here.
    if (!autosave)
        document.m_transformBase.reset();
    else if (!document.m_transformBase)
        document.m_transformBase = document.m_baseText;
    document.m_baseText = snapshot;
    document.m_disk = base;
    document.m_diskState = DiskState::InSync;
    document.m_pendingMerge.reset();
    document.m_outOfDate.reset();
    document.m_restored = false;

    if (m_services.journal != nullptr && document.m_journaledGeneration != 0) {
        auto cleaned = saveAs ? m_services.journal->forget(oldKey).transform([](bool) { return true; })
                              : m_services.journal->markClean(oldKey, generation);
        if (!cleaned) {
            outcome.warnings.push_back(std::move(cleaned.error()));
        } else if (*cleaned) {
            document.m_journaledGeneration = 0;
        }
    }
    recordHistory(document, storage::RevisionKind::Save, *bytes, outcome.warnings);
    if (m_services.settings != nullptr) {
        outcome.settingsCheck = m_services.settings->checkSavedFile(path, *bytes);
    }
    sweepDirectory(io::parentPath(outcome.target, m_options.style), outcome.warnings);
    return outcome;
}

std::optional<SkipReason> SaveManager::skipReason(const FileDocument& document, SaveReason reason) const noexcept {
    if (document.untitled()) {
        return SkipReason::Untitled;
    }
    switch (document.m_diskState) {
    case DiskState::OutOfDate: return SkipReason::OutOfDate;
    case DiskState::Changed: return SkipReason::MergePending;
    case DiskState::Deleted:
        if (reason == SaveReason::Autosave) {
            return SkipReason::Deleted;
        }
        break;
    case DiskState::InSync:
    case DiskState::Unsaved: break;
    }
    if (!document.m_encoding.fidelity.lossless()) {
        return SkipReason::NeedsConsent;
    }
    return std::nullopt;
}

SaveAllResult SaveManager::saveAll(SaveReason reason) {
    SaveAllResult result;
    // Saving never adds or removes documents, so the list is stable.
    for (const auto& owned : m_documents) {
        FileDocument& document = *owned;
        // A clean document is saved only for a deleted file, or for the save actions an autosave left
        // to an explicit Save All (saveTo writes it only when they change the text).
        const bool pendingTransform =
            reason != SaveReason::Autosave && document.m_transformBase && document.m_diskState == DiskState::InSync;
        if (!document.dirty() && document.m_diskState != DiskState::Deleted && !pendingTransform) {
            continue;
        }
        if (!document.dirty() && reason == SaveReason::Autosave) {
            continue; // A clean buffer of a deleted file is not recreated by an autosave.
        }
        if (const auto skip = skipReason(document, reason)) {
            if (pendingTransform && !document.dirty())
                continue; // Nothing unsaved to report: only the save actions could not run.
            result.skipped.push_back(SkippedDocument{&document, *skip});
            continue;
        }
        SaveOptions options;
        options.reason = reason;
        if (auto saved = save(document, options); saved) {
            if (!saved->written && !document.dirty())
                continue; // A pending-transform check that found nothing to change.
            result.saved.push_back(&document);
            for (auto& warning : saved->warnings)
                result.warnings.push_back(FailedSave{&document, std::move(warning)});
        } else {
            result.failed.push_back(FailedSave{&document, std::move(saved.error())});
        }
    }
    return result;
}

// --- Local history and housekeeping ---

void SaveManager::recordHistory(const FileDocument& document, storage::RevisionKind kind, std::string_view bytes,
                                std::vector<Error>& warnings) {
    if (m_services.history == nullptr || !m_policies.localHistory || document.untitled()) {
        return;
    }
    const std::string_view pieces[] = {bytes};
    if (auto recorded = m_services.history->record(document.key(), document.path(), kind, "", pieces); !recorded) {
        warnings.push_back(std::move(recorded.error()));
        return;
    }
    const IClock::SteadyTime now = m_services.clock->steadyNow();
    if (!m_lastPrune || now - *m_lastPrune >= m_options.pruneInterval) {
        if (auto pruned = pruneHistory(); !pruned) {
            warnings.push_back(std::move(pruned.error()));
        }
    }
}

Result<storage::PruneStats> SaveManager::pruneHistory() {
    if (m_services.history == nullptr) {
        return fail(ErrorCode::NotFound, "local history is not available");
    }
    m_lastPrune = m_services.clock->steadyNow();
    return m_services.history->prune(m_policies.retention);
}

void SaveManager::sweepDirectory(std::string_view directory, std::vector<Error>& warnings) {
    if (directory.empty() || m_sweptDirectories.contains(directory)) {
        return;
    }
    m_sweptDirectories.emplace(directory);
    const IClock::SystemTime olderThan = m_services.clock->systemNow() - m_options.staleTempAge;
    if (auto swept = io::removeStaleTempFiles(*m_services.fs, directory, olderThan, m_options.style); !swept) {
        warnings.push_back(std::move(swept.error()));
    }
}

} // namespace frost::document
