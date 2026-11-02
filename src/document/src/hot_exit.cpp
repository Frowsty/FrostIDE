// SPDX-License-Identifier: Apache-2.0
//
// SaveManager's hot exit (save_manager.hpp, ADR 0028): journal generations, journaling unsaved
// buffers into storage::HotExitJournal, the clean shutdown, and restoring the entries of journals an
// exited or crashed process left, through the load service and its large-file guard.

#include "frost/document/save_manager.hpp"
#include "frost/text/line_separator.hpp"

#include "journal_labels.hpp"

#include <format>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace frost::document {

std::int64_t SaveManager::generationOf(FileDocument& document) {
    const std::uint64_t stateId = document.m_document.undoState().stateId;
    if (document.m_generation == 0 || document.m_generationStateId != stateId ||
        document.m_generationFormat != document.m_formatVersion) {
        document.m_generation = ++m_nextGeneration;
        document.m_generationStateId = stateId;
        document.m_generationFormat = document.m_formatVersion;
    }
    return document.m_generation;
}

bool SaveManager::needsJournal(const FileDocument& document) const noexcept {
    // An out-of-date buffer may be the only copy of its text: the disk now holds something else.
    return document.dirty() || document.m_diskState == DiskState::OutOfDate;
}

void SaveManager::putJournal(FileDocument& document, std::vector<Error>& errors) {
    storage::DocumentState state;
    state.documentKey = document.key();
    state.path = document.path();
    state.generation = generationOf(document);
    detail::encodeJournalLabels(document.m_encoding, state);
    state.base = document.m_disk;
    // The committed text (never an IME preedit), straight from the immutable snapshot's chunks.
    const text::Snapshot snapshot = document.m_document.committedText();
    std::vector<std::string_view> pieces;
    for (const std::string_view chunk : snapshot.chunks(text::ByteRange{ByteOffset{0}, snapshot.size()})) {
        pieces.push_back(chunk);
    }
    auto put = m_services.journal->put(state, pieces);
    if (!put) {
        errors.push_back(std::move(put.error()).withContext(std::format("while journaling {}", document.key())));
        return;
    }
    document.m_journaledGeneration = state.generation;
}

JournalReport SaveManager::journalDirty() {
    JournalReport report;
    if (m_services.journal == nullptr || !m_policies.hotExit) {
        return report;
    }
    for (const auto& owned : m_documents) {
        FileDocument& document = *owned;
        if (needsJournal(document)) {
            if (document.m_journaledGeneration != generationOf(document)) {
                putJournal(document, report.errors);
                report.written += document.m_journaledGeneration == document.m_generation ? 1 : 0;
            }
        } else if (document.m_journaledGeneration != 0) {
            // Undo back to the saved state: the entry is no longer needed.
            auto cleaned = m_services.journal->markClean(document.key(), generationOf(document));
            if (!cleaned) {
                report.errors.push_back(std::move(cleaned.error()));
            } else if (*cleaned) {
                document.m_journaledGeneration = 0;
                ++report.cleared;
            }
        }
    }
    return report;
}

Result<void> SaveManager::closeJournal() {
    if (m_services.journal == nullptr) {
        return {};
    }
    JournalReport report = journalDirty();
    storage::HotExitJournal* journal = std::exchange(m_services.journal, nullptr);
    auto closed = journal->closeCleanly();
    if (!report.errors.empty()) {
        return std::unexpected(std::move(report.errors.front()));
    }
    return closed;
}

bool SaveManager::carryOver(const storage::JournalEntry& entry, std::string_view content, Error reason,
                            RestoreReport& report) {
    bool carried = false;
    if (m_services.journal != nullptr) {
        storage::DocumentState state = entry.state;
        state.generation = ++m_nextGeneration;
        const std::string_view pieces[] = {content};
        if (auto put = m_services.journal->put(state, pieces); put) {
            carried = true;
        } else {
            report.errors.push_back(std::move(put.error()));
        }
    }
    report.deferred.push_back(DeferredEntry{entry.state.documentKey, entry.state.path, std::move(reason)});
    return carried;
}

bool SaveManager::restoreEntry(const storage::JournalEntry& entry, std::string content, RestoreReport& report) {
    const storage::DocumentState& state = entry.state;
    const std::string label = state.path.empty() ? state.documentKey : state.path;
    if (!state.path.empty() && find(state.path) != nullptr) {
        return !carryOver(entry, content, Error(ErrorCode::AlreadyExists, std::format("'{}' is already open", label)),
                          report);
    }
    if (auto guard = m_loader.checkSize(content.size(), label); !guard) {
        return !carryOver(entry, content, std::move(guard.error()), report);
    }
    auto loaded = m_loader.fromUtf8(content, label);
    if (!loaded) {
        return !carryOver(entry, content, std::move(loaded.error()), report);
    }
    loaded->encoding = detail::decodeJournalLabels(state);
    std::string path = state.path;
    std::string key = state.documentKey;
    if (state.path.empty() && key.starts_with("untitled:")) {
        key = std::format("untitled:{}", ++m_nextUntitled); // Fresh numbering in this process.
    }
    FileDocument& document = adopt(std::move(path), std::move(key), std::move(*loaded));
    document.m_restored = true;
    document.m_savedStateId = FileDocument::kNoSavedState;
    document.m_disk.reset();
    document.m_baseText = text::Snapshot();
    document.m_diskState = DiskState::Unsaved;
    if (!document.untitled()) {
        // The disk file becomes the base: a later merge or save compares against it.
        std::optional<text::Encoding> encoding;
        if (document.m_encoding.detection.source == text::DetectionSource::UserOverride) {
            encoding = document.m_encoding.format.encoding;
        }
        auto disk = m_loader.load(document.path(), encoding);
        if (disk) {
            const bool changed = !state.base || state.base->size != disk->disk->size ||
                                 state.base->fingerprint != disk->disk->fingerprint;
            document.m_baseText = disk->buffer.snapshot();
            document.m_disk = disk->disk;
            document.m_diskState = DiskState::InSync;
            if (changed) {
                report.diskChanged.push_back(&document);
            }
            const text::Snapshot restored = document.m_document.committedText();
            if (restored.size() == document.m_baseText.size() && restored.text() == document.m_baseText.text() &&
                document.m_encoding.format == disk->encoding.format) {
                document.m_savedStateId = document.m_document.undoState().stateId; // Nothing unsaved after all.
            }
        } else if (disk.error().code() == ErrorCode::NotFound) {
            document.m_diskState = DiskState::Deleted;
        } else {
            document.m_diskState = DiskState::OutOfDate;
            document.m_outOfDate = std::move(disk.error());
        }
    }
    report.restored.push_back(&document);
    if (m_services.journal != nullptr && m_policies.hotExit && needsJournal(document)) {
        const std::size_t errors = report.errors.size();
        putJournal(document, report.errors);
        return report.errors.size() != errors; // Not in this journal: keep the old one.
    }
    return false;
}

RestoreReport SaveManager::restore(storage::RecoveryScan& scan) {
    RestoreReport report;
    struct Candidate {
        storage::RecoveredJournal* journal = nullptr;
        storage::JournalEntry entry;
    };
    // The newest entry of each document key across every recovered journal.
    std::map<std::string, Candidate, std::less<>> newest;
    std::vector<bool> keep(scan.journals.size(), false);
    for (std::size_t i = 0; i < scan.journals.size(); ++i) {
        auto entries = scan.journals[i].entries();
        if (!entries) {
            report.errors.push_back(std::move(entries.error()));
            keep[i] = true;
            continue;
        }
        for (storage::JournalEntry& entry : *entries) {
            auto [slot, inserted] = newest.try_emplace(entry.state.documentKey);
            if (inserted || entry.updatedAt > slot->second.entry.updatedAt) {
                slot->second = Candidate{&scan.journals[i], std::move(entry)};
            }
        }
    }
    for (auto& [key, candidate] : newest) {
        const auto index = static_cast<std::size_t>(candidate.journal - scan.journals.data());
        auto content = candidate.journal->readContent(candidate.entry.id);
        if (!content) {
            // A Corrupt entry stays unreadable; anything else may succeed at the next start.
            keep[index] = keep[index] || content.error().code() != ErrorCode::Corrupt;
            report.errors.push_back(std::move(content.error()).withContext(std::format("while restoring {}", key)));
            continue;
        }
        if (restoreEntry(candidate.entry, std::move(*content), report)) {
            keep[index] = true;
        }
    }
    for (std::size_t i = 0; i < scan.journals.size(); ++i) {
        if (keep[i]) {
            continue;
        }
        if (auto discarded = std::move(scan.journals[i]).discard(); discarded) {
            ++report.journalsDiscarded;
        } else {
            report.errors.push_back(std::move(discarded.error()));
        }
    }
    return report;
}

} // namespace frost::document
