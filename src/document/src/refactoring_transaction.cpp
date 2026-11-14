// SPDX-License-Identifier: Apache-2.0
// REF-1 frozen native participants, durable decisions and owner-thread global document history.
#include "frost/document/refactoring_transaction.hpp"

#include "frost/io/file_transaction_replace.hpp"
#include "frost/io/spawn_log.hpp"
#include "frost/storage/fingerprint.hpp"
#include "frost/text/line_separator.hpp"
#include "frost/text/text_file.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <format>
#include <iterator>
#include <limits>
#include <system_error>
#include <utility>
namespace frost::document {
namespace {
TransactionId newIdentity() {
    static std::atomic<std::uint64_t> sequence{1};
    const auto stamp = static_cast<std::uint64_t>(systemClock().systemNow().time_since_epoch().count());
    const auto number = sequence.fetch_add(1, std::memory_order_relaxed);
    std::array<std::byte, 16> bytes{};
    for (unsigned i = 0; i < 8; ++i) {
        bytes[i] = static_cast<std::byte>((stamp >> (i * 8)) & 255);
        bytes[i + 8] = static_cast<std::byte>((number >> (i * 8)) & 255);
    }
    return TransactionId(bytes); // Exclusive directory creation, not this ID, prevents overwrite.
}
std::string identityText(TransactionId id) {
    std::string out;
    constexpr char hex[] = "0123456789abcdef";
    for (auto byte : id.bytes()) {
        const auto n = std::to_integer<unsigned>(byte);
        out += hex[n >> 4];
        out += hex[n & 15];
    }
    return out;
}
std::string pathKey(std::string_view path) {
    std::string key(path);
#ifdef _WIN32
    for (auto& c : key) {
        if (c == '\\')
            c = '/';
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
#endif
    return key;
}
struct Writer {
    std::string data = "FROST-REF1-1\n";
    void number(std::uint64_t value) {
        for (unsigned i = 0; i < 8; ++i)
            data += static_cast<char>((value >> (i * 8)) & 255);
    }
    void text(std::string_view value) {
        number(value.size());
        data.append(value);
    }
    std::string sealed() const { return data + io::sha256Hex(data); }
};
struct Reader {
    std::string_view data;
    std::size_t position = 0;
    bool good = true;
    explicit Reader(std::string_view value) {
        constexpr std::string_view magic = "FROST-REF1-1\n";
        if (value.size() < magic.size() + 64 || !value.starts_with(magic)) {
            good = false;
            return;
        }
        data = value.substr(0, value.size() - 64);
        if (io::sha256Hex(data) != value.substr(value.size() - 64)) {
            good = false;
            return;
        }
        position = magic.size();
    }
    std::uint64_t number() {
        if (!good || data.size() - position < 8) {
            good = false;
            return 0;
        }
        std::uint64_t value = 0;
        for (unsigned i = 0; i < 8; ++i)
            value |= static_cast<std::uint64_t>(static_cast<unsigned char>(data[position++])) << (8 * i);
        return value;
    }
    std::size_t count(std::size_t limit) {
        auto value = number();
        if (value > limit) {
            good = false;
            return 0;
        }
        return static_cast<std::size_t>(value);
    }
    bool flag() {
        auto value = number();
        if (value > 1)
            good = false;
        return value == 1;
    }
    std::string text(std::size_t limit) {
        auto length = count(limit);
        if (!good || length > data.size() - position) {
            good = false;
            return {};
        }
        std::string value(data.substr(position, length));
        position += length;
        return value;
    }
    bool done() const { return good && position == data.size(); }
};
std::optional<TransactionId> parseIdentity(std::string_view value) {
    if (value.size() != 32)
        return std::nullopt;
    std::array<std::byte, 16> bytes{};
    const auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        return -1;
    };
    for (std::size_t i = 0; i < 16; ++i) {
        const auto a = nibble(value[i * 2]), b = nibble(value[i * 2 + 1]);
        if (a < 0 || b < 0)
            return std::nullopt;
        bytes[i] = static_cast<std::byte>((a << 4) | b);
    }
    TransactionId id(bytes);
    return id.assigned() ? std::optional(id) : std::nullopt;
}
io::FileImageObservation readObservation(Reader& in) {
    io::FileImageObservation value;
    value.volume = in.text(65536);
    value.object = in.text(65536);
    value.metadata = in.text(65536);
    value.size = in.number();
    return value;
}
io::FileReplacementReceipt readReceipt(Reader& in) {
    io::FileReplacementReceipt r;
    r.images.targetPath = in.text(65536);
    r.images.stagePath = in.text(65536);
    r.images.beforeSha256 = in.text(64);
    r.images.afterSha256 = in.text(64);
    r.images.metadataObservationProfile = in.text(65536);
    r.images.target = readObservation(in);
    r.images.stage = readObservation(in);
    r.images.targetParent = readObservation(in);
    r.images.store = readObservation(in);
    r.replacementPath = in.text(65536);
    r.retainedBeforePath = in.text(65536);
    r.metadataProfile = in.text(65536);
    r.beforeMetadataSha256 = in.text(64);
    r.privateAfterMetadataSha256 = in.text(64);
    r.installedAfterMetadataSha256 = in.text(64);
    r.moveDestination = in.text(65536);
    r.moveParent = readObservation(in);
    const auto phase = in.number();
    if (phase > static_cast<unsigned>(io::FileReplacementPhase::MoveRestored))
        in.good = false;
    r.phase = static_cast<io::FileReplacementPhase>(phase);
    return r;
}
void observation(Writer& out, const io::FileImageObservation& value) {
    out.text(value.volume);
    out.text(value.object);
    out.text(value.metadata);
    out.number(value.size);
}
void receipt(Writer& out, const io::FileReplacementReceipt& value) {
    out.text(value.images.targetPath);
    out.text(value.images.stagePath);
    out.text(value.images.beforeSha256);
    out.text(value.images.afterSha256);
    out.text(value.images.metadataObservationProfile);
    observation(out, value.images.target);
    observation(out, value.images.stage);
    observation(out, value.images.targetParent);
    observation(out, value.images.store);
    out.text(value.replacementPath);
    out.text(value.retainedBeforePath);
    out.text(value.metadataProfile);
    out.text(value.beforeMetadataSha256);
    out.text(value.privateAfterMetadataSha256);
    out.text(value.installedAfterMetadataSha256);
    out.text(value.moveDestination);
    observation(out, value.moveParent);
    out.number(static_cast<unsigned>(value.phase));
}
} // namespace
struct RefactoringCoordinator::State {
    enum class Action { Apply, Undo, Redo };
    struct Bookkeeping {
        text::DocumentEncoding encoding;
        std::optional<storage::DiskBase> disk;
        text::Snapshot base;
        std::optional<text::Snapshot> transformBase;
        std::uint64_t saved = 0;
        bool formatDirty = false;
        std::uint64_t formatVersion = 0;
        bool restored = false;
    };
    struct Buffer {
        std::string originalPath, movedPath, beforeText, afterText, beforeDisk, notificationOldPath, recoveryPath;
        std::uint64_t instance = 0, beforeState = 0, afterState = 0;
        bool persist = false, dirtyBefore = false, completed = false, journaled = false;
        std::string previousJournalKey;
        Bookkeeping before, after;
        std::optional<text::ChangeSet> changes;
    };
    struct Participant {
        std::string path, destination, before, after;
        std::optional<io::PreparedFileReplacement> native;
    };
    struct ReadBuffer {
        std::string path, text;
        std::uint64_t instance = 0;
    };
    struct Record {
        TransactionId id;
        RefactoringPlan plan;
        std::string directory, manifestDigest;
        std::vector<Participant> files;
        std::vector<Buffer> buffers;
        std::vector<ReadBuffer> reads;
        Action action = Action::Apply;
        std::uint64_t decision = 0;
        bool pending = false, recovering = false;
        std::optional<Error> recoveryError;
    };
    SaveManager& manager;
    io::IFileSystem& fs;
    RefactoringOptions options;
    std::optional<io::TransactionStoreLease> storeLease;
    std::vector<std::unique_ptr<Record>> history;
    std::size_t cursor = 0;
    bool executing = false, recovered = false;
    State(SaveManager& docs, io::IFileSystem& filesystem, RefactoringOptions opts)
        : manager(docs), fs(filesystem), options(std::move(opts)) {}
    static Bookkeeping capture(const FileDocument& f) {
        return {f.m_encoding,     f.m_disk,        f.m_baseText,      f.m_transformBase,
                f.m_savedStateId, f.m_formatDirty, f.m_formatVersion, f.m_restored};
    }
    static void restoreBookkeeping(FileDocument& f, const Bookkeeping& b) {
        f.m_encoding = b.encoding;
        f.m_disk = b.disk;
        f.m_baseText = b.base;
        f.m_transformBase = b.transformBase;
        f.m_savedStateId = b.saved;
        f.m_formatDirty = b.formatDirty;
        f.m_formatVersion = b.formatVersion;
        f.m_restored = b.restored;
        f.m_pendingMerge.reset();
        f.m_outOfDate.reset();
        f.m_diskState = DiskState::InSync;
        f.m_generationStateId = FileDocument::kNoSavedState;
    }
    static bool sameBookkeeping(const FileDocument& f, const Bookkeeping& b) {
        const bool disk = (!f.m_disk && !b.disk) ||
                          (f.m_disk && b.disk && f.m_disk->size == b.disk->size() && f.m_disk->mtime == b.disk->mtime &&
                           f.m_disk->fingerprint == b.disk->fingerprint);
        const bool transform =
            (!f.m_transformBase && !b.transformBase) ||
            (f.m_transformBase && b.transformBase && f.m_transformBase->text() == b.transformBase->text());
        return disk && transform && f.m_encoding == b.encoding && f.m_savedStateId == b.saved &&
               f.m_formatDirty == b.formatDirty && f.m_formatVersion == b.formatVersion && f.m_restored == b.restored &&
               f.m_baseText.text() == b.base.text() && !f.m_pendingMerge && !f.m_outOfDate &&
               f.m_diskState == DiskState::InSync;
    }
    FileDocument* openDocument(std::string_view path) const {
        if (auto* exact = manager.find(path))
            return exact;
        auto physical = fs.physicalPath(path);
        if (!physical)
            return nullptr;
        for (const auto& doc : manager.documents()) {
            if (doc->untitled())
                continue;
            auto other = fs.physicalPath(doc->path());
            if (other && pathKey(*other) == pathKey(*physical))
                return doc.get();
        }
        return nullptr;
    }
    FileDocument* current(const Buffer& b, bool after) const {
        auto* doc = manager.find(after && !b.movedPath.empty() ? b.movedPath : b.originalPath);
        return doc && doc->instanceId() == b.instance ? doc : nullptr;
    }
    void unlock(TransactionId id) {
        for (const auto& doc : manager.documents())
            doc->document().releaseExternalWrite(id);
    }
    Result<void> dependencies(const Record& r, bool applied) const {
        for (const auto& d : r.plan.dependencies) {
            const auto written = std::find_if(r.files.begin(), r.files.end(), [&](const Participant& p) {
                return pathKey(p.path) == pathKey(d.path) ||
                       (!p.destination.empty() && pathKey(p.destination) == pathKey(d.path));
            });
            if (written != r.files.end())
                continue; // Native receipts validate Before/After/absence for mutable paths.
            if (d.directory) {
                if (d.expectedBytes)
                    return fail(ErrorCode::InvalidArgument, "directory dependency cannot contain file bytes");
                auto actual = fs.status(d.path);
                if (!actual)
                    return std::unexpected(actual.error());
                if (actual->type != io::FileType::Directory)
                    return fail(ErrorCode::Busy, "directory dependency changed kind: " + d.path);
                continue;
            }
            auto exists = fs.exists(d.path);
            if (!exists)
                return std::unexpected(exists.error());
            if (!d.expectedBytes) {
                if (*exists)
                    return fail(ErrorCode::Busy, "absent refactoring dependency appeared: " + d.path);
            } else {
                auto bytes = fs.readFile(d.path);
                if (!bytes)
                    return std::unexpected(bytes.error());
                if (*bytes != *d.expectedBytes)
                    return fail(ErrorCode::Busy, "refactoring dependency changed: " + d.path);
            }
        }
        for (const auto& read : r.reads) {
            if (std::any_of(r.buffers.begin(), r.buffers.end(),
                            [&](const Buffer& b) { return b.instance == read.instance; }))
                continue;
            auto* doc = manager.find(read.path);
            if (!doc || doc->instanceId() != read.instance || doc->document().committedText().text() != read.text)
                return fail(ErrorCode::Busy, "open refactoring dependency changed or closed: " + read.path);
        }
        static_cast<void>(applied);
        return {};
    }
    Result<void> write(Record& r, std::string_view name, std::string_view data) {
        auto path = io::joinPath(r.directory, name);
        if (!path)
            return std::unexpected(path.error());
        if (data.size() > options.maxTotalBytes)
            return fail(ErrorCode::OutOfRange, "transaction recovery record exceeds total limit");
        return fs.writeFileAtomic(*path, data);
    }
    std::string manifest(const Record& r) const {
        Writer out;
        out.text(identityText(r.id));
        out.text(r.plan.label);
        out.number(r.files.size());
        for (const auto& p : r.files) {
            out.text(p.path);
            out.text(p.destination);
            out.text(p.before);
            out.text(p.after);
            receipt(out, *p.native->receipt());
        }
        out.number(r.buffers.size());
        for (const auto& b : r.buffers) {
            out.text(b.originalPath);
            out.text(b.movedPath);
            out.text(b.beforeText);
            out.text(b.afterText);
            out.number(b.persist);
            out.number(b.dirtyBefore);
            out.text(text::encodingName(b.before.encoding.format.encoding));
            out.number(b.before.encoding.format.bom);
            out.text(text::separatorName(b.before.encoding.format.lineSeparator));
            out.text(b.beforeDisk);
            out.text(b.before.base.text());
            out.number(b.before.transformBase.has_value());
            if (b.before.transformBase)
                out.text(b.before.transformBase->text());
            out.number(b.before.formatDirty);
            out.number(b.before.formatVersion);
            out.number(b.before.restored);
            out.number(b.before.disk.has_value());
            if (b.before.disk)
                out.number(static_cast<std::uint64_t>(b.before.disk->mtime.time_since_epoch().count()));
            out.text(text::encodingName(b.before.encoding.detection.encoding));
            out.number(b.before.encoding.detection.bom);
            out.number(b.before.encoding.detection.confidence);
            out.text(text::detectionSourceName(b.before.encoding.detection.source));
        }
        out.number(r.plan.dependencies.size());
        for (const auto& d : r.plan.dependencies) {
            out.text(d.path);
            out.number(d.directory);
            out.number(d.expectedBytes.has_value());
            if (d.expectedBytes)
                out.text(*d.expectedBytes);
        }
        out.number(r.reads.size());
        for (const auto& read : r.reads) {
            out.text(read.path);
            out.text(read.text);
        }
        return out.sealed();
    }
    Result<void> decision(Record& r, Action action) {
        if (options.admit) {
            auto allowed = options.admit();
            if (!allowed)
                return std::unexpected(allowed.error());
        }
        if (action == Action::Apply && r.plan.revalidateContext) {
            auto fresh = r.plan.revalidateContext();
            if (!fresh)
                return std::unexpected(fresh.error());
        }
        if (auto checked = dependencies(r, action == Action::Undo); !checked)
            return checked;
        for (const auto& p : r.files) {
            auto valid = p.native->validateCurrent(action == Action::Undo);
            if (!valid)
                return valid;
        }
        for (const auto& b : r.buffers) {
            auto* doc = current(b, action == Action::Undo);
            const auto& saved = action == Action::Undo ? b.after : b.before;
            const auto& text = action == Action::Undo ? b.afterText : b.beforeText;
            if (!doc || !sameBookkeeping(*doc, saved) || doc->document().committedText().text() != text)
                return fail(ErrorCode::Busy, "refactoring state changed at final decision admission");
        }
        Writer out;
        out.text(identityText(r.id));
        out.number(r.decision + 1);
        out.number(static_cast<unsigned>(action));
        out.text(r.manifestDigest);
        auto saved = write(r, "decision-" + std::to_string(r.decision + 1), out.sealed());
        if (!saved)
            return saved;
        ++r.decision;
        r.action = action;
        r.pending = true;
        r.plan.revalidateContext = {};
        manager.m_refactoringPending = r.id;
        for (auto& b : r.buffers) {
            b.completed = false;
            b.journaled = false;
            b.previousJournalKey.clear();
            b.changes.reset();
        }
        return {};
    }
    Result<void> progress(Record& r) {
        Writer out;
        out.text(identityText(r.id));
        out.number(r.decision);
        for (const auto& p : r.files)
            receipt(out, *p.native->receipt());
        return write(r, "progress-" + std::to_string(r.decision), out.sealed());
    }
    Result<void> addBuffer(Record& r, const BufferReplacement& edit) {
        auto* doc = openDocument(edit.path);
        if (!doc)
            return fail(ErrorCode::Busy, "refactoring buffer closed: " + edit.path);
        if (doc->readOnly() != ReadOnlyReason::None || doc->pendingMerge() || doc->diskState() != DiskState::InSync)
            return fail(ErrorCode::Busy, "refactoring buffer is read-only or has unresolved disk changes");
        if (!doc->encoding().fidelity.lossless())
            return fail(ErrorCode::Unsupported, "refactoring requires lossless buffer encoding");
        if (doc->document().committedText().text() != edit.expectedText)
            return fail(ErrorCode::Busy, "refactoring buffer text changed");
        if (std::any_of(r.buffers.begin(), r.buffers.end(),
                        [&](const Buffer& b) { return b.instance == doc->instanceId(); }))
            return fail(ErrorCode::InvalidArgument, "duplicate refactoring buffer artifact");
        auto validated = text::TextBuffer::fromUtf8(edit.afterText);
        if (!validated)
            return std::unexpected(validated.error());
        auto owned = doc->document().acquireExternalWrite(r.id);
        if (!owned)
            return owned;
        auto capturedDisk = fs.readFile(doc->path());
        if (!capturedDisk)
            return std::unexpected(capturedDisk.error());
        if (capturedDisk->size() > options.maxFileBytes)
            return fail(ErrorCode::OutOfRange, "buffer disk baseline exceeds limit");
        if (doc->disk() && (doc->disk()->size != capturedDisk->size() ||
                            doc->disk()->fingerprint != storage::xxh3Hash128(*capturedDisk)))
            return fail(ErrorCode::Busy, "buffer disk baseline changed before refactoring");
        if (std::none_of(r.plan.dependencies.begin(), r.plan.dependencies.end(),
                         [&](const ReadDependency& d) { return pathKey(d.path) == pathKey(doc->path()); }))
            r.plan.dependencies.push_back({doc->path(), *capturedDisk, false});
        Buffer b;
        b.beforeDisk = *capturedDisk;
        b.originalPath = doc->path();
        b.instance = doc->instanceId();
        b.beforeText = edit.expectedText;
        b.afterText = edit.afterText;
        b.beforeState = doc->document().undoState().stateId;
        b.before = capture(*doc);
        b.dirtyBefore = doc->dirty();
        b.persist = edit.persistence == BufferPersistence::Persist ||
                    (edit.persistence == BufferPersistence::Automatic && !doc->dirty());
        if (!manager.m_services.journal && (b.dirtyBefore || (!b.persist && b.beforeText != b.afterText)))
            return fail(ErrorCode::Unsupported,
                        "dirty-buffer refactoring requires an available durable hot-exit journal");
        if (b.persist) {
            auto bytes =
                text::encodeForSave(validated->snapshot(), doc->encoding(), text::SaveConsent::None,
                                    manager.m_services.codecs ? *manager.m_services.codecs : text::platformCodecs());
            if (!bytes)
                return std::unexpected(bytes.error());
            auto disk = fs.readFile(doc->path());
            if (!disk)
                return std::unexpected(disk.error());
            if (doc->disk() &&
                (doc->disk()->size != disk->size() || doc->disk()->fingerprint != storage::xxh3Hash128(*disk)))
                return fail(ErrorCode::Busy, "disk changed under refactoring buffer");
            auto found = std::find_if(r.plan.disk.begin(), r.plan.disk.end(), [&](const DiskReplacement& d) {
                return pathKey(d.path) == pathKey(doc->path());
            });
            if (found == r.plan.disk.end())
                r.plan.disk.push_back({doc->path(), std::move(*disk), std::move(*bytes)});
            else if (found->expectedBytes != *disk || found->afterBytes != *bytes)
                return fail(ErrorCode::Busy, "buffer and disk effects disagree");
        } else if (std::any_of(r.plan.disk.begin(), r.plan.disk.end(),
                               [&](const DiskReplacement& d) { return pathKey(d.path) == pathKey(doc->path()); }))
            return fail(ErrorCode::Busy, "buffer-only edit cannot overwrite disk independently");
        r.buffers.push_back(std::move(b));
        return {};
    }

    Result<std::unique_ptr<Record>> prepare(const RefactoringPlan& proposed) {
        if (proposed.disk.size() + proposed.buffers.size() + proposed.renames.size() > options.maxParticipants)
            return fail(ErrorCode::OutOfRange, "too many refactoring participants");
        if (proposed.dependencies.size() + proposed.bufferDependencies.size() > options.maxReadDependencies)
            return fail(ErrorCode::OutOfRange, "too many read-only refactoring dependencies");
        std::size_t total = 0;
        const auto bounded = [&](std::string_view bytes) {
            if (bytes.size() > options.maxFileBytes || bytes.size() > options.maxTotalBytes - total)
                return false;
            total += bytes.size();
            return true;
        };
        for (const auto& d : proposed.disk)
            if (!bounded(d.expectedBytes) || !bounded(d.afterBytes))
                return fail(ErrorCode::OutOfRange, "disk image limit");
        for (const auto& b : proposed.buffers)
            if (!bounded(b.expectedText) || !bounded(b.afterText))
                return fail(ErrorCode::OutOfRange, "buffer image limit");
        for (const auto& m : proposed.renames)
            if (!bounded(m.expectedBytes))
                return fail(ErrorCode::OutOfRange, "rename image limit");
        for (const auto& d : proposed.dependencies)
            if (d.expectedBytes && !bounded(*d.expectedBytes))
                return fail(ErrorCode::OutOfRange, "dependency limit");
        for (const auto& d : proposed.bufferDependencies)
            if (!bounded(d.expectedText))
                return fail(ErrorCode::OutOfRange, "buffer dependency limit");
        auto r = std::make_unique<Record>();
        r->id = newIdentity();
        r->plan = proposed;
        const auto reject = [&](Error error) -> Result<std::unique_ptr<Record>> {
            unlock(r->id);
            return std::unexpected(std::move(error));
        };
        if (!storeLease) {
            auto owner = io::acquireTransactionStoreLease(options.recoveryDirectory);
            if (!owner)
                return reject(owner.error());
            storeLease.emplace(std::move(*owner));
        }
        for (auto& d : r->plan.disk) {
            auto normalized = io::normalizePath(d.path);
            if (!normalized || !io::isAbsolutePath(normalized ? *normalized : std::string{}))
                return reject(Error(ErrorCode::InvalidArgument, "absolute native refactoring paths required"));
            d.path = std::move(*normalized);
        }
        for (auto& move : r->plan.renames) {
            auto from = io::normalizePath(move.from);
            auto to = io::normalizePath(move.to);
            if (!from || !to || !io::isAbsolutePath(from ? *from : std::string{}) ||
                !io::isAbsolutePath(to ? *to : std::string{}))
                return reject(Error(ErrorCode::InvalidArgument, "absolute movement paths required"));
            move.from = std::move(*from);
            move.to = std::move(*to);
        }
        for (const auto& b : proposed.buffers)
            if (auto added = addBuffer(*r, b); !added)
                return reject(added.error());
        const auto copies = r->plan.disk;
        for (const auto& d : copies) {
            auto* doc = openDocument(d.path);
            if (!doc || std::any_of(r->buffers.begin(), r->buffers.end(),
                                    [&](const Buffer& b) { return b.instance == doc->instanceId(); }))
                continue;
            if (doc->dirty())
                return reject(Error(ErrorCode::Busy, "raw disk edit meets an unsaved open buffer: " + d.path));
            text::LoadOptions load;
            load.encoding = doc->encoding().format.encoding;
            auto decoded = text::decodeBytes(
                d.afterBytes, load, manager.m_services.codecs ? *manager.m_services.codecs : text::platformCodecs());
            if (!decoded || !decoded->encoding.fidelity.lossless())
                return reject(Error(ErrorCode::Corrupt, "disk output is not reversible open-document text"));
            if (auto added = addBuffer(*r, {doc->path(), doc->document().committedText().text(), decoded->text,
                                            BufferPersistence::Persist});
                !added)
                return reject(added.error());
        }
        for (const auto& move : r->plan.renames) {
            auto* doc = openDocument(move.from);
            if (auto* occupied = manager.find(move.to); occupied && occupied != doc)
                return reject(Error(ErrorCode::AlreadyExists, "rename destination is already open"));
            if (!doc)
                continue;
            auto found = std::find_if(r->buffers.begin(), r->buffers.end(),
                                      [&](const Buffer& b) { return b.instance == doc->instanceId(); });
            if (found == r->buffers.end()) {
                if (auto added = addBuffer(*r, {doc->path(), doc->document().committedText().text(),
                                                doc->document().committedText().text(), BufferPersistence::BufferOnly});
                    !added)
                    return reject(added.error());
                found = std::prev(r->buffers.end());
            }
            found->movedPath = move.to;
        }
        for (const auto& d : proposed.bufferDependencies) {
            auto* doc = openDocument(d.path);
            if (!doc || doc->document().committedText().text() != d.expectedText)
                return reject(Error(ErrorCode::Busy, "open read dependency changed/closed"));
            const bool leased = std::any_of(r->buffers.begin(), r->buffers.end(),
                                            [&](const Buffer& b) { return b.instance == doc->instanceId(); }) ||
                                std::any_of(r->reads.begin(), r->reads.end(),
                                            [&](const ReadBuffer& b) { return b.instance == doc->instanceId(); });
            if (!leased) {
                auto owned = doc->document().acquireExternalWrite(r->id);
                if (!owned)
                    return reject(owned.error());
            }
            r->reads.push_back({doc->path(), d.expectedText, doc->instanceId()});
        }
        const auto addNative = [&](std::string path, std::string before, std::string after,
                                   std::string destination) -> Result<void> {
            if (!options.privateStoreForPath)
                return fail(ErrorCode::InvalidArgument, "same-volume private store provider required");
            for (const auto& old : r->files)
                if (pathKey(path) == pathKey(old.path) ||
                    (!destination.empty() &&
                     (pathKey(destination) == pathKey(old.path) || pathKey(destination) == pathKey(old.destination))) ||
                    (!old.destination.empty() && pathKey(path) == pathKey(old.destination)))
                    return fail(ErrorCode::InvalidArgument,
                                "overlapping file effects require a combined producer plan");
            auto actual = fs.readFile(path);
            if (!actual)
                return std::unexpected(actual.error());
            if (*actual != before)
                return fail(ErrorCode::Busy, "refactoring input changed: " + path);
            auto store = options.privateStoreForPath(path);
            if (!store)
                return std::unexpected(store.error());
            auto native = io::prepareFileReplacement(path, *store, after, options.maxFileBytes, options.maxFileBytes);
            if (!native)
                return std::unexpected(native.error());
            if (!destination.empty()) {
                auto admitted = native->prepareMoveDestination(destination);
                if (!admitted)
                    return std::unexpected(admitted.error());
            }
            if (native->receipt()->images.beforeSha256 != io::sha256Hex(before))
                return fail(ErrorCode::Busy, "native capture differs from preview");
            for (const auto& old : r->files)
                if (old.native->receipt()->images.target.volume == native->receipt()->images.target.volume &&
                    old.native->receipt()->images.target.object == native->receipt()->images.target.object)
                    return fail(ErrorCode::InvalidArgument, "physical aliases cannot own separate transaction effects");
            r->files.push_back(
                {std::move(path), std::move(destination), std::move(before), std::move(after), std::move(*native)});
            return {};
        };
        for (const auto& d : r->plan.disk)
            if (d.expectedBytes != d.afterBytes) {
                auto added = addNative(d.path, d.expectedBytes, d.afterBytes, {});
                if (!added)
                    return reject(added.error());
            }
        for (const auto& m : r->plan.renames) {
            auto from = io::normalizePath(m.from);
            auto to = io::normalizePath(m.to);
            if (!from || !to || !io::isAbsolutePath(from ? *from : std::string{}) ||
                !io::isAbsolutePath(to ? *to : std::string{}) || *from == *to)
                return reject(Error(ErrorCode::InvalidArgument, "move requires distinct absolute endpoints"));
            auto added = addNative(*from, m.expectedBytes, m.expectedBytes, *to);
            if (!added)
                return reject(added.error());
        }
        if (r->files.empty() && r->buffers.empty())
            return reject(Error(ErrorCode::InvalidArgument, "refactoring has no effects"));
        if (auto checked = dependencies(*r, false); !checked)
            return reject(checked.error());
        for (const auto& file : r->files)
            if (auto checked = file.native->validateCurrent(false); !checked)
                return reject(checked.error());
        for (const auto& b : r->buffers) {
            auto* doc = current(b, false);
            if (!doc || doc->document().undoState().stateId != b.beforeState ||
                doc->document().committedText().text() != b.beforeText || !sameBookkeeping(*doc, b.before))
                return reject(Error(ErrorCode::Busy, "buffer changed during native preparation"));
        }
        auto directory = io::claimTransactionDirectory(options.recoveryDirectory, identityText(r->id));
        if (!directory)
            return reject(directory.error());
        r->directory = std::move(*directory);
        const auto bytes = manifest(*r);
        r->manifestDigest = io::sha256Hex(bytes);
        if (auto saved = write(*r, "manifest", bytes); !saved)
            return reject(saved.error());
        return r;
    }
    Result<std::string> readRecord(std::string_view directory, std::string_view name) {
        auto path = io::joinPath(directory, name);
        if (!path)
            return std::unexpected(path.error());
        auto info = fs.status(*path);
        if (!info)
            return std::unexpected(info.error());
        if (info->type != io::FileType::Regular || info->size > options.maxTotalBytes)
            return fail(ErrorCode::Corrupt, "invalid/oversized transaction record");
        auto value = fs.readFile(*path);
        if (!value)
            return value;
        if (value->size() > options.maxTotalBytes)
            return fail(ErrorCode::Corrupt, "transaction record grew beyond bound");
        return value;
    }
    Result<std::unique_ptr<Record>> loadPending(std::string directory, TransactionId id) {
        auto entries = fs.listDirectory(directory);
        if (!entries)
            return std::unexpected(entries.error());
        auto bytes = readRecord(directory, "manifest");
        if (!bytes)
            return std::unexpected(bytes.error());
        Reader in(*bytes);
        auto r = std::make_unique<Record>();
        r->id = id;
        r->directory = std::move(directory);
        if (in.text(32) != identityText(id))
            return fail(ErrorCode::Corrupt, "transaction identity mismatch");
        r->plan.label = in.text(65536);
        r->manifestDigest = io::sha256Hex(*bytes);
        const auto files = in.count(options.maxParticipants);
        std::vector<io::FileReplacementReceipt> receipts;
        for (std::size_t i = 0; i < files && in.good; ++i) {
            Participant p;
            p.path = in.text(65536);
            p.destination = in.text(65536);
            p.before = in.text(options.maxFileBytes);
            p.after = in.text(options.maxFileBytes);
            auto captured = readReceipt(in);
            if (captured.images.targetPath != p.path || captured.moveDestination != p.destination)
                in.good = false;
            receipts.push_back(std::move(captured));
            r->files.push_back(std::move(p));
        }
        const auto buffers = in.count(options.maxParticipants);
        for (std::size_t i = 0; i < buffers && in.good; ++i) {
            Buffer b;
            b.originalPath = in.text(65536);
            b.movedPath = in.text(65536);
            b.beforeText = in.text(options.maxFileBytes);
            b.afterText = in.text(options.maxFileBytes);
            b.persist = in.flag();
            b.dirtyBefore = in.flag();
            auto encoding = text::findEncoding(in.text(128));
            const auto bom = in.flag();
            auto separator = text::findSeparator(in.text(16));
            if (!encoding || !separator || (bom && !encoding->isUnicode()))
                in.good = false;
            if (encoding)
                b.before.encoding = b.before.encoding.convertedTo(*encoding, bom);
            if (separator)
                b.before.encoding.format.lineSeparator = *separator;
            b.beforeDisk = in.text(options.maxFileBytes);
            auto base = text::TextBuffer::fromUtf8(in.text(options.maxFileBytes));
            if (!base)
                return std::unexpected(base.error());
            b.before.base = base->snapshot();
            if (in.flag()) {
                auto transform = text::TextBuffer::fromUtf8(in.text(options.maxFileBytes));
                if (!transform)
                    return std::unexpected(transform.error());
                b.before.transformBase = transform->snapshot();
            }
            b.before.formatDirty = in.flag();
            b.before.formatVersion = in.number();
            b.before.restored = in.flag();
            if (in.flag()) {
                const auto ticks = static_cast<std::int64_t>(in.number());
                b.before.disk = storage::DiskBase{b.beforeDisk.size(), IClock::SystemTime(IClock::Duration(ticks)),
                                                  storage::xxh3Hash128(b.beforeDisk)};
            }
            auto detected = text::findEncoding(in.text(128));
            const auto detectedBom = in.flag();
            const auto confidence = in.number();
            auto source = text::findDetectionSource(in.text(128));
            if (!detected || !source || confidence > 100)
                in.good = false;
            else
                b.before.encoding.detection = {*detected, detectedBom, static_cast<std::uint8_t>(confidence), *source};
            r->buffers.push_back(std::move(b));
        }
        const auto deps = in.count(options.maxReadDependencies);
        for (std::size_t i = 0; i < deps && in.good; ++i) {
            ReadDependency d;
            d.path = in.text(65536);
            d.directory = in.flag();
            if (in.flag())
                d.expectedBytes = in.text(options.maxFileBytes);
            if (d.directory && d.expectedBytes)
                in.good = false;
            r->plan.dependencies.push_back(std::move(d));
        }
        const auto reads = in.count(options.maxReadDependencies);
        for (std::size_t i = 0; i < reads && in.good; ++i) {
            ReadBuffer read;
            read.path = in.text(65536);
            read.text = in.text(options.maxFileBytes);
            r->reads.push_back(std::move(read));
        }
        if (!in.done())
            return fail(ErrorCode::Corrupt, "invalid transaction manifest/checksum/limits");
        std::uint64_t latest = 0;
        for (const auto& entry : *entries)
            if (entry.name.starts_with("decision-")) {
                std::uint64_t sequence = 0;
                const auto digits = std::string_view(entry.name).substr(9);
                const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), sequence);
                if (entry.type != io::FileType::Regular || parsed.ec != std::errc{} ||
                    parsed.ptr != digits.data() + digits.size() || sequence == 0)
                    return fail(ErrorCode::Corrupt, "unknown transaction decision record");
                latest = std::max(latest, sequence);
            }
        if (latest == 0) {
            auto aborted = readRecord(r->directory, "abort");
            if (aborted) {
                Reader proof(*aborted);
                const auto token = proof.text(32), digest = proof.text(64), word = proof.text(16);
                if (proof.done() && token == identityText(id) && digest == r->manifestDigest && word == "ABORT")
                    return std::unique_ptr<Record>{};
            }
            return fail(ErrorCode::Corrupt, "unresolved preparation has no verified decision; artifacts retained");
        }
        auto decisionBytes = readRecord(r->directory, "decision-" + std::to_string(latest));
        if (!decisionBytes)
            return std::unexpected(decisionBytes.error());
        Reader decisionIn(*decisionBytes);
        const auto token = decisionIn.text(32);
        const auto sequence = decisionIn.number();
        const auto action = decisionIn.number();
        const auto digest = decisionIn.text(64);
        if (!decisionIn.done() || token != identityText(id) || sequence != latest || action > 2 ||
            digest != r->manifestDigest)
            return fail(ErrorCode::Corrupt, "transaction decision does not bind manifest");
        r->decision = latest;
        r->action = static_cast<Action>(action);
        auto settled = readRecord(r->directory, "settled-" + std::to_string(latest));
        if (settled) {
            Reader proof(*settled);
            const auto owner = proof.text(32);
            const auto step = proof.number();
            const auto word = proof.text(16);
            if (proof.done() && owner == identityText(id) && step == latest && word == "SETTLED")
                return std::unique_ptr<Record>{};
            return fail(ErrorCode::Corrupt, "corrupt settled transaction marker");
        }
        if (settled.error().code() != ErrorCode::NotFound)
            return std::unexpected(settled.error());
        r->pending = true;
        r->recovering = true;
        for (std::size_t i = 0; i < r->files.size(); ++i) {
            auto& p = r->files[i];
            auto native = io::recoverFileReplacement(receipts[i], p.before, p.after, r->action == Action::Undo,
                                                     options.maxFileBytes);
            if (!native) {
                r->recoveryError = native.error();
                continue;
            }
            p.native.emplace(std::move(*native));
        }
        return r;
    }
    Result<void> journalBuffer(Record& r, Buffer& b, bool before) {
        auto* doc = r.recovering ? manager.find(b.recoveryPath) : current(b, !before);
        if (!doc || doc->instanceId() != b.instance)
            return fail(ErrorCode::Busy, "refactoring journal document closed/replaced");
        auto* journal = manager.m_services.journal;
        if (doc->dirty() || doc->diskState() == DiskState::OutOfDate) {
            if (!journal)
                return fail(ErrorCode::Busy, "dirty refactoring buffer has no durable journal; keep recovery pending");
            std::vector<Error> errors;
            manager.putJournal(*doc, errors);
            if (!errors.empty())
                return std::unexpected(errors.front());
            if (doc->m_journaledGeneration != manager.generationOf(*doc))
                return fail(ErrorCode::Busy, "dirty refactoring generation was not durably journaled");
            auto rows = journal->entries();
            if (!rows)
                return std::unexpected(rows.error());
            const auto text = doc->document().committedText().text();
            const auto row = std::find_if(rows->begin(), rows->end(), [&](const storage::JournalEntry& entry) {
                return entry.state.documentKey == doc->key() && entry.state.generation == doc->m_journaledGeneration &&
                       entry.contentSize == text.size() && entry.contentFingerprint == storage::xxh3Hash128(text);
            });
            if (row == rows->end())
                return fail(ErrorCode::Busy, "dirty journal did not retain the exact transaction generation/content");
        } else if (journal && doc->m_journaledGeneration != 0) {
            auto cleaned = journal->markClean(doc->key(), manager.generationOf(*doc));
            if (!cleaned)
                return std::unexpected(cleaned.error());
            if (!*cleaned)
                return fail(ErrorCode::Busy, "a newer dirty journal generation must be preserved");
            doc->m_journaledGeneration = 0;
        }
        // Publish new-key dirty data (or prove clean persisted state) before discarding old key.
        if (!b.previousJournalKey.empty()) {
            if (!journal)
                return fail(ErrorCode::Busy, "rename cleanup requires its durable journal");
            auto forgotten = journal->forget(b.previousJournalKey);
            if (!forgotten)
                return std::unexpected(forgotten.error());
            b.previousJournalKey.clear(); // Never reset the already durable NEW-key generation.
        }
        b.journaled = true;
        return {};
    }
    Result<void> leaseRecoveredBuffers(Record& r) {
        for (auto& b : r.buffers) {
            if (!b.movedPath.empty()) {
                auto* from = manager.find(b.originalPath);
                auto* to = manager.find(b.movedPath);
                if (from && to && from != to)
                    return fail(ErrorCode::Busy,
                                "two recovered buffers claim rename endpoints; preserve both before resolving");
            }
            auto* doc = b.recoveryPath.empty() ? manager.find(b.originalPath) : manager.find(b.recoveryPath);
            if (!doc && !b.movedPath.empty())
                doc = manager.find(b.movedPath);
            if (!doc)
                continue;
            if (b.instance && doc->instanceId() != b.instance)
                return fail(ErrorCode::Busy, "recovery document was replaced");
            if (!doc->document().ownsExternalWrite(r.id)) {
                auto owned = doc->document().acquireExternalWrite(r.id);
                if (!owned)
                    return owned;
            }
            b.instance = doc->instanceId();
            b.recoveryPath = doc->path();
            const auto actual = doc->document().committedText().text();
            if ((actual != b.beforeText && actual != b.afterText) || doc->encoding().format != b.before.encoding.format)
                return fail(ErrorCode::Busy, "recovery retains foreign buffer text/format without overwriting it");
        }
        return {};
    }
    Result<void> publishRecoveredBuffer(Record& r, Buffer& b, bool before) {
        const auto finalPath = before || b.movedPath.empty() ? b.originalPath : b.movedPath;
        FileDocument* doc = b.recoveryPath.empty() ? nullptr : manager.find(b.recoveryPath);
        if (!doc) {
            if (auto* existing = manager.find(finalPath))
                doc = existing;
            else {
                auto loaded = manager.m_loader.load(finalPath, b.before.encoding.format.encoding);
                if (!loaded)
                    return std::unexpected(loaded.error());
                doc = &manager.adopt(finalPath, finalPath, std::move(*loaded));
            }
            b.instance = doc->instanceId();
            b.recoveryPath = doc->path();
        }
        if (doc->instanceId() != b.instance)
            return fail(ErrorCode::Busy, "recovery buffer instance changed");
        if (!doc->document().ownsExternalWrite(r.id)) {
            auto owned = doc->document().acquireExternalWrite(r.id);
            if (!owned)
                return owned;
        }
        const auto& desired = before ? b.beforeText : b.afterText;
        b.notificationOldPath = doc->path();
        if (!b.movedPath.empty())
            b.previousJournalKey = before ? b.movedPath : b.originalPath;
        if (doc->key() != finalPath && doc->m_journaledGeneration != 0)
            b.previousJournalKey = doc->key();
        if (doc->document().committedText().text() != desired) {
            auto changed = doc->document().replaceExternal(
                r.id, {ByteOffset{0}, ByteOffset{doc->document().text().size()}}, desired, text::EditContext{});
            if (!changed)
                return std::unexpected(changed.error());
            b.changes = std::move(*changed);
            auto marked = doc->document().markExternalTransaction(r.id);
            if (!marked)
                return marked;
        } else {
            auto boundary = doc->document().recordExternalBarrier(r.id);
            if (!boundary)
                return boundary;
            text::ChangeSet unchanged;
            unchanged.baseVersion = doc->document().version();
            unchanged.version = unchanged.baseVersion;
            b.changes = std::move(unchanged);
        }
        doc->m_path = finalPath;
        doc->m_key = finalPath;
        b.recoveryPath = finalPath;
        restoreBookkeeping(*doc, b.before);
        if (!before && b.persist) {
            auto disk = fs.readFile(finalPath);
            if (!disk)
                return std::unexpected(disk.error());
            auto status = fs.status(finalPath);
            if (!status)
                return std::unexpected(status.error());
            doc->m_disk = storage::DiskBase{disk->size(), status->mtime, storage::xxh3Hash128(*disk)};
            doc->m_baseText = doc->document().committedText();
            doc->m_transformBase.reset();
            doc->m_encoding = doc->m_encoding.afterSave();
            doc->m_formatDirty = false;
        }
        const bool dirty = before ? b.dirtyBefore : !b.persist && (b.dirtyBefore || b.beforeText != b.afterText);
        doc->m_savedStateId = dirty ? FileDocument::kNoSavedState : doc->document().undoState().stateId;
        b.completed = true;
        return {};
    }
    Result<void> preflightHistory(Record& r, bool after) {
        if (auto checked = dependencies(r, after); !checked)
            return checked;
        for (const auto& p : r.files)
            if (auto checked = p.native->validateCurrent(after); !checked)
                return checked;
        for (const auto& b : r.buffers) {
            auto* doc = current(b, after);
            if (!doc || !sameBookkeeping(*doc, after ? b.after : b.before) ||
                doc->document().committedText().text() != (after ? b.afterText : b.beforeText) ||
                (after ? doc->document().undoState().externalUndo : doc->document().undoState().externalRedo) != r.id)
                return fail(ErrorCode::Busy, "global history is stale, closed or not at its document boundary");
        }
        for (const auto& b : r.buffers) {
            auto owned = current(b, after)->document().acquireExternalWrite(r.id);
            if (!owned) {
                unlock(r.id);
                return owned;
            }
        }
        for (const auto& read : r.reads) {
            if (std::any_of(r.buffers.begin(), r.buffers.end(),
                            [&](const Buffer& b) { return b.instance == read.instance; }))
                continue;
            auto* doc = manager.find(read.path);
            if (!doc || doc->instanceId() != read.instance) {
                unlock(r.id);
                return fail(ErrorCode::Busy, "read dependency closed");
            }
            auto owned = doc->document().acquireExternalWrite(r.id);
            if (!owned) {
                unlock(r.id);
                return owned;
            }
        }
        return {};
    }

    RefactoringOutcome settle(Record& r) {
        RefactoringOutcome result{r.id, RefactoringStatus::PendingRecovery, {}};
        const bool before = r.action == Action::Undo;
        if (r.recovering) {
            auto leased = leaseRecoveredBuffers(r);
            if (!leased) {
                result.warnings.push_back(leased.error());
                return result;
            }
        }
        if (r.recoveryError) {
            result.warnings.push_back(*r.recoveryError);
            return result;
        }
        const auto incomplete = [&](Error error) {
            result.warnings.push_back(std::move(error));
            auto saved = progress(r);
            if (!saved)
                result.warnings.push_back(saved.error());
            return result;
        };
        for (std::size_t step = 0; step < r.files.size(); ++step) {
            auto& p = r.files[before ? r.files.size() - 1 - step : step];
            const auto phase = p.native->receipt()->phase;
            io::FileReplacementOutcome effect;
            if (!p.destination.empty()) {
                if (before) {
                    if (phase == io::FileReplacementPhase::MoveRestored)
                        continue;
                    effect = p.native->restoreMove();
                } else {
                    if (phase == io::FileReplacementPhase::OriginalMoved) {
                        auto valid = p.native->validateCurrent(true);
                        if (!valid)
                            return incomplete(valid.error());
                        continue;
                    }
                    effect = phase == io::FileReplacementPhase::MoveRestored ? p.native->reapplyMove()
                                                                             : p.native->moveOriginalTo(p.destination);
                }
            } else if (before) {
                if (phase == io::FileReplacementPhase::BeforeRestored)
                    continue;
                effect =
                    phase == io::FileReplacementPhase::AfterInstalled ? p.native->restoreBefore() : p.native->settle();
                if (effect.complete() && p.native->receipt()->phase == io::FileReplacementPhase::AfterInstalled)
                    effect = p.native->restoreBefore();
            } else {
                if (phase == io::FileReplacementPhase::AfterInstalled) {
                    auto valid = p.native->validateCurrent(true);
                    if (!valid)
                        return incomplete(valid.error());
                    continue;
                }
                effect =
                    phase == io::FileReplacementPhase::BeforeRestored ? p.native->reapplyAfter() : p.native->settle();
            }
            auto saved = progress(r);
            if (!saved)
                return incomplete(saved.error());
            if (!effect.complete())
                return incomplete(*effect.error);
        }
        // Finish all fallible disk observations before touching the first buffer.
        for (const auto& p : r.files) {
            auto valid = p.native->validateCurrent(!before);
            if (!valid)
                return incomplete(valid.error());
        }
        std::vector<std::optional<storage::DiskBase>> diskBases(r.buffers.size());
        for (std::size_t i = 0; i < r.buffers.size(); ++i) {
            const auto& b = r.buffers[i];
            if (!b.persist || before)
                continue;
            const auto& path = b.movedPath.empty() ? b.originalPath : b.movedPath;
            auto bytes = fs.readFile(path);
            if (!bytes)
                return incomplete(bytes.error());
            auto status = fs.status(path);
            if (!status)
                return incomplete(status.error());
            diskBases[i] = storage::DiskBase{bytes->size(), status->mtime, storage::xxh3Hash128(*bytes)};
        }
        for (std::size_t i = 0; i < r.buffers.size(); ++i) {
            auto& b = r.buffers[i];
            if (b.completed) {
                if (!b.journaled) {
                    auto durable = journalBuffer(r, b, before);
                    if (!durable)
                        return incomplete(durable.error());
                }
                continue;
            }
            if (r.recovering) {
                auto published = publishRecoveredBuffer(r, b, before);
                if (!published)
                    return incomplete(published.error());
                if (auto* doc = manager.find(b.recoveryPath))
                    manager.m_deferredDiskChecks.insert(doc->instanceId());
                auto durable = journalBuffer(r, b, before);
                if (!durable)
                    return incomplete(durable.error());
                continue;
            }
            auto* doc = current(b, before);
            if (!doc || !doc->document().undoState().externallyLocked)
                return incomplete(Error(ErrorCode::Busy, "leased document closed/replaced during settlement"));
            Result<text::ChangeSet> changes = fail(ErrorCode::Internal, "uninitialized replay");
            if (r.action == Action::Apply) {
                if (b.beforeText == b.afterText) {
                    auto edge = doc->document().recordExternalBarrier(r.id);
                    if (!edge)
                        return incomplete(edge.error());
                    text::ChangeSet empty;
                    empty.baseVersion = doc->document().version();
                    empty.version = empty.baseVersion;
                    changes = std::move(empty);
                } else {
                    changes = doc->document().replaceExternal(
                        r.id, {ByteOffset{0}, ByteOffset{doc->document().text().size()}}, b.afterText,
                        text::EditContext{});
                    if (changes) {
                        auto marked = doc->document().markExternalTransaction(r.id);
                        if (!marked)
                            return incomplete(marked.error());
                    }
                }
            } else
                changes = before ? doc->document().undoExternal(r.id) : doc->document().redoExternal(r.id);
            if (!changes)
                return incomplete(changes.error());
            b.changes = std::move(*changes);
            const auto oldKey = doc->m_key;
            b.notificationOldPath = doc->path();
            doc->m_path = before || b.movedPath.empty() ? b.originalPath : b.movedPath;
            doc->m_key = doc->m_path;
            if (before)
                restoreBookkeeping(*doc, b.before);
            else if (r.action == Action::Redo)
                restoreBookkeeping(*doc, b.after);
            else {
                if (b.persist) {
                    doc->m_savedStateId = doc->document().undoState().stateId;
                    doc->m_encoding = doc->m_encoding.afterSave();
                    doc->m_formatDirty = false;
                    doc->m_baseText = doc->document().committedText();
                    doc->m_transformBase.reset();
                    doc->m_disk = diskBases[i];
                } else if (!b.dirtyBefore && b.beforeText == b.afterText)
                    doc->m_savedStateId = doc->document().undoState().stateId;
                b.afterState = doc->document().undoState().stateId;
                b.after = capture(*doc);
            }
            if (oldKey != doc->m_key && doc->m_journaledGeneration != 0)
                b.previousJournalKey = oldKey;
            doc->m_generationStateId = FileDocument::kNoSavedState;
            b.completed = true;
            auto durable = journalBuffer(r, b, before);
            if (!durable)
                return incomplete(durable.error());
        }
        Writer complete;
        complete.text(identityText(r.id));
        complete.number(r.decision);
        complete.text("SETTLED");
        if (auto saved = write(r, "settled-" + std::to_string(r.decision), complete.sealed()); !saved)
            return incomplete(saved.error());
        r.pending = false;
        unlock(r.id);
        manager.m_refactoringPending = {};
        result.status = before                     ? RefactoringStatus::Undone
                        : r.action == Action::Redo ? RefactoringStatus::Redone
                                                   : RefactoringStatus::Applied;
        for (auto& b : r.buffers) {
            auto* doc = r.recovering ? manager.find(b.recoveryPath) : current(b, !before);
            if (!doc)
                continue;
            if (!b.notificationOldPath.empty() && b.notificationOldPath != doc->path() && options.renamed)
                options.renamed(*doc, b.notificationOldPath);
            if (b.changes && options.changed)
                options.changed(*doc, *b.changes);
        }
        for (const auto& doc : manager.documents()) {
            if (manager.m_deferredDiskChecks.erase(doc->instanceId()) == 0)
                continue;
            auto change = manager.checkDisk(*doc);
            if (change.kind != ExternalChangeKind::None && manager.m_listener)
                manager.m_listener(change);
        }
        return result;
    }
};
RefactoringCoordinator::RefactoringCoordinator(SaveManager& documents, io::IFileSystem& fs, RefactoringOptions options)
    : m_state(std::make_unique<State>(documents, fs, std::move(options))) {}
RefactoringCoordinator::~RefactoringCoordinator() = default;
Result<RefactoringOutcome> RefactoringCoordinator::apply(const RefactoringPlan& plan) {
    auto& s = *m_state;
    if (!s.recovered) {
        auto loaded = recoverPending();
        if (!loaded)
            return std::unexpected(loaded.error());
    }
    if (pending())
        return RefactoringOutcome{pendingToken(),
                                  RefactoringStatus::PendingRecovery,
                                  {Error(ErrorCode::Busy, "a previous committed refactoring requires recovery")}};
    if (s.executing || s.manager.m_refactoringPending.assigned())
        return fail(ErrorCode::Busy, "another refactoring requires settlement");
    s.executing = true;
    struct Reset {
        bool& flag;
        ~Reset() { flag = false; }
    } reset{s.executing};
    auto prepared = s.prepare(plan);
    if (!prepared)
        return std::unexpected(prepared.error());
    auto& r = **prepared;
    if (auto decided = s.decision(r, State::Action::Apply); !decided) {
        Writer abort;
        abort.text(identityText(r.id));
        abort.text(r.manifestDigest);
        abort.text("ABORT");
        auto saved = s.write(r, "abort", abort.sealed());
        s.unlock(r.id);
        if (!saved)
            return std::unexpected(
                decided.error().withContext("abort proof could not be recorded; recovery material retained"));
        return std::unexpected(decided.error());
    }
    s.history.erase(s.history.begin() + static_cast<std::ptrdiff_t>(s.cursor), s.history.end());
    s.history.push_back(std::move(*prepared));
    auto result = s.settle(*s.history.back());
    if (result.status != RefactoringStatus::PendingRecovery)
        s.cursor = s.history.size();
    return result;
}
Result<RefactoringOutcome> RefactoringCoordinator::undo(TransactionId token) {
    auto& s = *m_state;
    if (s.executing || pending() || s.cursor == 0 || s.history[s.cursor - 1]->id != token)
        return fail(ErrorCode::Busy, "Undo is not the latest global transaction");
    s.executing = true;
    struct Reset {
        bool& flag;
        ~Reset() { flag = false; }
    } reset{s.executing};
    auto& r = *s.history[s.cursor - 1];
    if (auto ready = s.preflightHistory(r, true); !ready)
        return std::unexpected(ready.error());
    if (auto decided = s.decision(r, State::Action::Undo); !decided) {
        s.unlock(token);
        return std::unexpected(decided.error());
    }
    auto result = s.settle(r);
    if (result.status == RefactoringStatus::Undone)
        --s.cursor;
    return result;
}
Result<RefactoringOutcome> RefactoringCoordinator::redo(TransactionId token) {
    auto& s = *m_state;
    if (s.executing || pending() || s.cursor >= s.history.size() || s.history[s.cursor]->id != token)
        return fail(ErrorCode::Busy, "Redo is not the next global transaction");
    s.executing = true;
    struct Reset {
        bool& flag;
        ~Reset() { flag = false; }
    } reset{s.executing};
    auto& r = *s.history[s.cursor];
    if (auto ready = s.preflightHistory(r, false); !ready)
        return std::unexpected(ready.error());
    if (auto decided = s.decision(r, State::Action::Redo); !decided) {
        s.unlock(token);
        return std::unexpected(decided.error());
    }
    auto result = s.settle(r);
    if (result.status == RefactoringStatus::Redone)
        ++s.cursor;
    return result;
}
Result<RefactoringOutcome> RefactoringCoordinator::resume(TransactionId token) {
    auto& s = *m_state;
    if (s.executing)
        return fail(ErrorCode::Busy, "refactoring coordinator is reentrant");
    auto found = std::find_if(s.history.begin(), s.history.end(), [&](const auto& r) { return r->id == token; });
    if (found == s.history.end() || !(*found)->pending)
        return fail(ErrorCode::NotFound, "no pending refactoring decision");
    s.executing = true;
    struct Reset {
        bool& flag;
        ~Reset() { flag = false; }
    } reset{s.executing};
    if ((*found)->recovering && (*found)->recoveryError) {
        auto fresh = s.loadPending((*found)->directory, token);
        if (fresh && *fresh)
            *found = std::move(*fresh);
        else
            return RefactoringOutcome{
                token,
                RefactoringStatus::PendingRecovery,
                {fresh ? Error(ErrorCode::Corrupt, "pending record unexpectedly lost its decision") : fresh.error()}};
    }
    auto result = s.settle(**found);
    if ((*found)->recovering && result.status != RefactoringStatus::PendingRecovery) {
        s.history.erase(found);
        s.cursor = s.history.size();
    } else if (result.status == RefactoringStatus::Applied)
        s.cursor = static_cast<std::size_t>(found - s.history.begin()) + 1;
    else if (result.status == RefactoringStatus::Undone)
        --s.cursor;
    else if (result.status == RefactoringStatus::Redone)
        ++s.cursor;
    return result;
}
TransactionId RefactoringCoordinator::latestUndo() const noexcept {
    return !pending() && m_state->cursor ? m_state->history[m_state->cursor - 1]->id : TransactionId{};
}
TransactionId RefactoringCoordinator::latestRedo() const noexcept {
    return !pending() && m_state->cursor < m_state->history.size() ? m_state->history[m_state->cursor]->id
                                                                   : TransactionId{};
}
bool RefactoringCoordinator::pending() const noexcept {
    return std::any_of(m_state->history.begin(), m_state->history.end(), [](const auto& r) { return r->pending; });
}
TransactionId RefactoringCoordinator::pendingToken() const noexcept {
    const auto found =
        std::find_if(m_state->history.begin(), m_state->history.end(), [](const auto& r) { return r->pending; });
    return found == m_state->history.end() ? TransactionId{} : (*found)->id;
}
Result<std::vector<RefactoringOutcome>> RefactoringCoordinator::recoverPending() {
    auto& s = *m_state;
    if (s.executing)
        return fail(ErrorCode::Busy, "refactoring coordinator is reentrant");
    if (s.recovered)
        return std::vector<RefactoringOutcome>{};
    s.executing = true;
    struct Reset {
        bool& flag;
        ~Reset() { flag = false; }
    } reset{s.executing};
    if (!s.storeLease) {
        auto owner = io::acquireTransactionStoreLease(s.options.recoveryDirectory);
        if (!owner)
            return std::unexpected(owner.error());
        s.storeLease.emplace(std::move(*owner));
    }
    auto entries = s.fs.listDirectory(s.options.recoveryDirectory);
    if (!entries)
        return std::unexpected(entries.error());
    if (entries->size() > s.options.maxReadDependencies)
        return fail(ErrorCode::OutOfRange, "too many retained recovery-root entries");
    std::vector<RefactoringOutcome> outcomes;
    for (const auto& item : *entries) {
        auto id = parseIdentity(item.name);
        if (!id)
            continue; // Only our exclusively claimed opaque transaction leaves.
        auto directory = io::joinPath(s.options.recoveryDirectory, item.name);
        if (!directory)
            return std::unexpected(directory.error()); // Valid hex leaf; cannot fail for later entries.
        Result<std::unique_ptr<State::Record>> loaded =
            item.type == io::FileType::Directory
                ? s.loadPending(*directory, *id)
                : Result<std::unique_ptr<State::Record>>(
                      fail(ErrorCode::Corrupt, "transaction directory became a non-directory"));
        if (loaded && !*loaded)
            continue; // Verified settled or explicit ABORT. Never sweep its retained objects.
        if (!loaded) {
            auto blocked = std::make_unique<State::Record>();
            blocked->id = *id;
            blocked->directory = *directory;
            blocked->pending = true;
            blocked->recovering = true;
            blocked->recoveryError = loaded.error();
            loaded = std::move(blocked);
        }
        auto& record = **loaded;
        s.manager.m_refactoringPending = record.id;
        s.history.push_back(std::move(*loaded));
        auto outcome = s.settle(*s.history.back());
        outcomes.push_back(std::move(outcome));
        if (!s.history.back()->pending) {
            // Session-local Undo is deliberately not invented from old editor node IDs. The restored
            // buffer boundary is a tombstone; original recovery material stays available on disk.
            s.history.pop_back();
        }
        if (s.manager.m_refactoringPending.assigned())
            break; // Do not settle a second transaction past a conflict.
    }
    s.cursor = s.history.size();
    s.recovered = true;
    return outcomes; // Earlier settled callbacks/outcomes survive a later pending/corrupt record.
}
} // namespace frost::document
