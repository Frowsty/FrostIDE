// SPDX-License-Identifier: Apache-2.0
//
// SaveManager (save_manager.hpp, ADR 0028): opening, saving, Save All, policies, local history and
// the stale temp-file sweep. External changes are in external_change.cpp, hot exit in hot_exit.cpp.

#include "frost/document/save_manager.hpp"

#include "frost/core/assert.hpp"
#include "frost/storage/fingerprint.hpp"
#include "frost/text/text_buffer.hpp"

#include <format>
#include <string>
#include <utility>

namespace frost::document {

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
    if (!saveAs && !document.dirty() && document.m_diskState == DiskState::InSync) {
        return outcome; // Clean and in sync: nothing to write.
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
        if (!document.dirty() && document.m_diskState != DiskState::Deleted) {
            continue;
        }
        if (!document.dirty() && reason == SaveReason::Autosave) {
            continue; // A clean buffer of a deleted file is not recreated by an autosave.
        }
        if (const auto skip = skipReason(document, reason)) {
            result.skipped.push_back(SkippedDocument{&document, *skip});
            continue;
        }
        SaveOptions options;
        options.reason = reason;
        if (auto saved = save(document, options); saved) {
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
