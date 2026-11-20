// SPDX-License-Identifier: Apache-2.0
//
// LanguageController (frost/ui/lang/language_controller.hpp): navigation, go to declaration (and
// metadata sources), implementation and related file, find and highlight usages, file structure
// and go to class or symbol (split from language_controller.cpp, FND-12a).

#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/lang/navigation.hpp"
#include "frost/lang/symbols.hpp"
#include "frost/lang/unity_assets.hpp"
#include "frost/lang/uri.hpp"
#include "frost/ui/lang/language_controller.hpp"

#include "language_controller_internal.hpp"
#include <QCryptographicHash>

#include <algorithm>
#include <cstdint>
#include <format>
#include <memory>
#include <utility>

namespace frost::ui::lang {

namespace fl = ::frost::lang;

namespace {

using detail::fileName;
using detail::lowerAscii;
using detail::qs;

bool isHeaderExt(std::string_view ext) {
    return ext == ".h" || ext == ".hpp" || ext == ".hh" || ext == ".hxx" || ext == ".inl" || ext == ".ipp";
}

bool isSourceExt(std::string_view ext) {
    return ext == ".c" || ext == ".cc" || ext == ".cpp" || ext == ".cxx" || ext == ".m" || ext == ".mm";
}

struct SplitPathParts {
    std::string dir;
    std::string stem;
    std::string ext;
    std::string prefixWithoutExt;
};

SplitPathParts splitPathParts(std::string_view path) {
    SplitPathParts out;
    const std::size_t slash = path.find_last_of("/\\");
    const std::string_view dirView = slash == std::string_view::npos ? std::string_view{} : path.substr(0, slash);
    const std::string_view baseView = slash == std::string_view::npos ? path : path.substr(slash + 1);
    out.dir = lowerAscii(dirView);
    std::ranges::replace(out.dir, '\\', '/');
    const std::size_t dot = baseView.rfind('.');
    if (dot == std::string_view::npos || dot == 0) {
        out.stem = lowerAscii(baseView);
        out.prefixWithoutExt = std::string(path);
    } else {
        out.stem = lowerAscii(baseView.substr(0, dot));
        out.ext = lowerAscii(baseView.substr(dot));
        const std::size_t baseOffset = slash == std::string_view::npos ? 0 : slash + 1;
        out.prefixWithoutExt = std::string(path.substr(0, baseOffset + dot));
    }
    return out;
}

std::optional<std::string> findRelatedProjectFile(std::string_view currentPath,
                                                  const std::function<std::vector<std::string>()>& projectFiles,
                                                  io::IFileSystem& fs) {
    const SplitPathParts current = splitPathParts(currentPath);
    const bool wantSource = isHeaderExt(current.ext);
    const bool wantHeader = isSourceExt(current.ext);
    if ((!wantSource && !wantHeader) || current.stem.empty()) {
        return std::nullopt;
    }

    if (projectFiles) {
        std::optional<std::string> best;
        int bestScore = -1;
        for (const std::string& candidateStr : projectFiles()) {
            if (fl::documentKey(candidateStr) == fl::documentKey(currentPath)) {
                continue;
            }
            const SplitPathParts candidate = splitPathParts(candidateStr);
            if (candidate.stem != current.stem) {
                continue;
            }
            if ((wantSource && !isSourceExt(candidate.ext)) || (wantHeader && !isHeaderExt(candidate.ext))) {
                continue;
            }
            int score = 1;
            if (candidate.dir == current.dir) {
                score = 100;
            } else {
                const std::size_t limit = std::min(candidate.dir.size(), current.dir.size());
                std::size_t prefix = 0;
                while (prefix < limit && candidate.dir[prefix] == current.dir[prefix]) {
                    ++prefix;
                }
                score = 10 + static_cast<int>(prefix);
            }
            if (score > bestScore) {
                bestScore = score;
                best = candidateStr;
            }
        }
        if (best) {
            return best;
        }
    }

    static constexpr std::string_view kSourceExts[] = {".cpp", ".cc", ".cxx", ".c", ".mm", ".m"};
    static constexpr std::string_view kHeaderExts[] = {".hpp", ".h", ".hh", ".hxx", ".inl", ".ipp"};
    const std::span<const std::string_view> exts =
        wantSource ? std::span<const std::string_view>(kSourceExts) : std::span<const std::string_view>(kHeaderExts);
    for (std::string_view targetExt : exts) {
        std::string siblingStr = current.prefixWithoutExt + std::string(targetExt);
        if (fs.exists(siblingStr) == true) {
            return siblingStr;
        }
    }
    return std::nullopt;
}

// The identifier at `position`, for messages ("Source not available for 'X'"). "" when none.
std::string identifierAtPosition(const std::optional<text::Snapshot>& snapshot, fl::LspPosition position,
                                 fl::PositionEncoding encoding) {
    if (!snapshot) {
        return {};
    }
    const auto offset = fl::fromLspPosition(*snapshot, position, encoding);
    if (!offset) {
        return {};
    }
    const std::string text = snapshot->text();
    std::size_t pos = std::min(offset->value(), text.size());
    if (pos > 0 && (pos == text.size() || !fl::isIdentifierByte(text[pos])) && fl::isIdentifierByte(text[pos - 1])) {
        --pos;
    }
    if (pos >= text.size() || !fl::isIdentifierByte(text[pos])) {
        return {};
    }
    std::size_t begin = pos;
    while (begin > 0 && fl::isIdentifierByte(text[begin - 1])) {
        --begin;
    }
    std::size_t end = pos;
    while (end < text.size() && fl::isIdentifierByte(text[end])) {
        ++end;
    }
    return text.substr(begin, end - begin);
}

// A metadata URI as C# language servers answer for symbols without source (csharp:/metadata/...).
bool isMetadataUri(std::string_view uri) {
    return uri.starts_with("csharp:") || uri.starts_with("roslyn:") || uri.starts_with("metadata:");
}

// A file name for metadata `uri` under the cache: its last segment reduced to [A-Za-z0-9._-],
// prefixed by a hash of the whole URI so two assemblies' "Console.cs" do not collide.
std::string metadataFileName(std::string_view uri) {
    const std::size_t slash = uri.find_last_of("/\\:");
    std::string_view last = slash == std::string_view::npos ? uri : uri.substr(slash + 1);
    std::string name;
    for (const char c : last) {
        const bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
                          c == '_' || c == '-';
        name.push_back(safe ? c : '_');
    }
    while (!name.empty() && name.front() == '.') {
        name.erase(name.begin());
    }
    if (name.empty()) {
        name = "metadata.cs";
    }
    // FNV-1a (public domain), 64-bit.
    std::uint64_t hash = 14695981039346656037ULL;
    for (const char c : uri) {
        hash = (hash ^ static_cast<unsigned char>(c)) * 1099511628211ULL;
    }
    return std::format("{:016x}-{}", hash, name);
}

// What metadataSourceTask found: the cached source's location, and whether the server answered
// textDocument/definition with a metadata URI at all (so "no cache" can be told from "no metadata").
struct MetadataSource {
    std::optional<fl::Location> location;
    bool offered = false;
};

// The owned host returns actual byte/assembly receipts. Cache names contain only validated
// digests; provider paths never become output paths. The host supplies the profile-owned root.
sched::Task<Result<MetadataSource>> librarySourceTask(fl::ILspRequester& server, io::IFileSystem& fs,
                                                      std::string cacheDir, std::string path, fl::LspPosition position,
                                                      std::stop_token token, std::string mode, bool allowSourceLink,
                                                      std::string workspaceRoot) {
    if (cacheDir.empty())
        co_return MetadataSource{};
    auto uri = fl::pathToUri(path);
    if (!uri)
        co_return std::unexpected(uri.error());
    auto result = co_await server.request(
        "frost/managed/librarySource",
        fl::Json::object()
            .set("textDocument", fl::Json::object().set("uri", *uri))
            .set("position", fl::Json::object()
                                 .set("line", static_cast<std::int64_t>(position.line))
                                 .set("character", static_cast<std::int64_t>(position.character)))
            .set("mode", mode)
            .set("allowSourceLink", allowSourceLink),
        token);
    if (!result)
        co_return std::unexpected(result.error());
    if (!result->isObject())
        co_return MetadataSource{};
    const auto source = result->stringAt("source");
    const auto identity = result->stringAt("identity");
    const auto digest = result->stringAt("sourceSha256");
    const auto extension = result->stringAt("extension");
    const auto validDigest = [](std::string_view value) {
        return value.size() == 64 &&
               std::ranges::all_of(value, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
    };
    if (source.empty() || source.size() > 4 * 1024 * 1024 || !validDigest(identity) || !validDigest(digest) ||
        (extension != ".cs" && extension != ".il"))
        co_return fail(ErrorCode::Corrupt, "invalid library source receipt");
    const auto line = result->integerAt("line").value_or(0);
    if (line < 0 || line > 1000000)
        co_return fail(ErrorCode::Corrupt, "library source location is out of range");
    const auto bytes = QByteArray(source.data(), static_cast<qsizetype>(source.size()));
    if (QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex().toStdString() != digest)
        co_return fail(ErrorCode::Corrupt, "library source checksum mismatch");
    if (token.stop_requested())
        co_return fail(ErrorCode::Cancelled, "library navigation cancelled");
    auto normalized = io::normalizePath(cacheDir);
    if (!normalized)
        co_return std::unexpected(normalized.error());
    if (!workspaceRoot.empty()) {
        auto project = io::normalizePath(workspaceRoot);
        if (!project)
            co_return std::unexpected(project.error());
        const auto root = fl::documentKey(*project);
        const auto cache = fl::documentKey(*normalized);
        if (cache == root ||
            (cache.size() > root.size() && cache.starts_with(root) &&
             (root.back() == '/' || root.back() == '\\' || cache[root.size()] == '/' || cache[root.size()] == '\\')))
            co_return fail(ErrorCode::PermissionDenied, "library cache must be outside the workspace");
    }
    std::string ancestor = *normalized;
    while (true) {
        auto exists = fs.exists(ancestor);
        if (!exists)
            co_return std::unexpected(exists.error());
        if (*exists)
            break;
        auto parent = std::string(io::parentPath(ancestor));
        if (parent.empty() || parent == ancestor)
            co_return fail(ErrorCode::PermissionDenied, "library cache has no existing owned ancestor");
        ancestor = std::move(parent);
    }
    auto physicalAncestor = fs.physicalPath(ancestor);
    if (!physicalAncestor || fl::documentKey(*physicalAncestor) != fl::documentKey(ancestor))
        co_return fail(ErrorCode::PermissionDenied, "library cache ancestor resolves through an alias");
    if (auto made = fs.createDirectories(*normalized); !made)
        co_return std::unexpected(made.error());
    auto physical = fs.physicalPath(*normalized);
    if (!physical || fl::documentKey(*physical) != fl::documentKey(*normalized))
        co_return fail(ErrorCode::PermissionDenied, "library cache must not resolve through filesystem aliases");
    if (!workspaceRoot.empty()) {
        auto project = fs.physicalPath(workspaceRoot);
        if (!project)
            co_return std::unexpected(project.error());
        const auto root = fl::documentKey(*project);
        const auto cache = fl::documentKey(*physical);
        if (cache == root ||
            (cache.size() > root.size() && cache.starts_with(root) &&
             (root.back() == '/' || root.back() == '\\' || cache[root.size()] == '/' || cache[root.size()] == '\\')))
            co_return fail(ErrorCode::PermissionDenied, "library cache must be outside the workspace");
    }
    auto entries = fs.listDirectory(*normalized);
    if (!entries)
        co_return std::unexpected(entries.error());
    if (entries->size() >= 4096)
        co_return fail(ErrorCode::OutOfRange, "library cache entry budget exceeded");
    std::uint64_t total = 0;
    for (const auto& entry : *entries) {
        if (entry.type != io::FileType::Regular)
            co_return fail(ErrorCode::PermissionDenied, "library cache contains a non-regular entry");
        auto item = io::joinPath(*normalized, entry.name);
        auto status = item ? fs.status(*item) : Result<io::FileStatus>(std::unexpected(item.error()));
        if (!status)
            co_return std::unexpected(status.error());
        total += status->size;
        if (total > 128 * 1024 * 1024)
            co_return fail(ErrorCode::OutOfRange, "library cache size budget exceeded");
    }
    auto file = io::joinPath(*normalized, std::string(identity) + std::string(extension));
    if (!file)
        co_return std::unexpected(file.error());
    auto exists = fs.exists(*file);
    if (!exists)
        co_return std::unexpected(exists.error());
    if (*exists) {
        auto existing = fs.readFile(*file);
        if (!existing || *existing != source)
            co_return fail(ErrorCode::Corrupt, "cached library source was modified");
    } else {
        if (total + source.size() > 128 * 1024 * 1024)
            co_return fail(ErrorCode::OutOfRange, "library cache size budget exceeded");
        auto stillPhysical = fs.physicalPath(*normalized);
        if (token.stop_requested() || !stillPhysical || *stillPhysical != *physical)
            co_return fail(ErrorCode::Cancelled, "library cache changed before publication");
        if (auto wrote = fs.writeFileAtomic(*file, source); !wrote)
            co_return std::unexpected(wrote.error());
    }
    co_return MetadataSource{
        .location = fl::Location{.path = *file,
                                 .uri = fl::pathToUri(*file).value_or(""),
                                 .range = {.start = {.line = static_cast<std::uint32_t>(line), .character = 0},
                                           .end = {.line = static_cast<std::uint32_t>(line), .character = 0}}},
        .offered = true};
}
/*
 * Real metadata-as-source for the symbol at `position` (LTR-1): textDocument/definition answered
 * with a metadata URI, and the server's csharp/metadata answered with its source. The source is
 * written to `cacheDir` (never the workspace) and the location points into that file at the range
 * the server gave. No location when the server has no metadata, or when no cache directory is
 * configured (then nothing more is asked or written, and `offered` tells whether there was some).
 */
sched::Task<Result<MetadataSource>> metadataSourceTask(fl::ILspRequester& server, io::IFileSystem& fs,
                                                       std::string cacheDir, std::string path, fl::LspPosition position,
                                                       std::stop_token token, std::string mode = "auto",
                                                       bool allowSourceLink = false, std::string workspaceRoot = {}) {
    if (const auto caps = server.serverCapabilities();
        caps && caps->raw.find("experimental") &&
        caps->raw.find("experimental")->integerAt("frostLibrarySources").value_or(0) == 1)
        co_return co_await librarySourceTask(server, fs, std::move(cacheDir), std::move(path), position, token,
                                             std::move(mode), allowSourceLink, std::move(workspaceRoot));
    if (mode != "auto" && mode != "decompiled")
        co_return fail(ErrorCode::Unsupported, "this language server has no library IL/SourceLink/metadata provider");
    const auto uri = fl::pathToUri(path);
    if (!uri) {
        co_return std::unexpected(uri.error());
    }
    fl::Json params = fl::Json::object()
                          .set("textDocument", fl::Json::object().set("uri", *uri))
                          .set("position", fl::Json::object()
                                               .set("line", static_cast<std::int64_t>(position.line))
                                               .set("character", static_cast<std::int64_t>(position.character)));
    auto definition = co_await server.request("textDocument/definition", std::move(params), token);
    if (!definition) {
        co_return std::unexpected(std::move(definition.error()));
    }
    std::string metadataUri;
    fl::LspRange range{};
    const auto consider = [&](const fl::Json& item) {
        for (std::string_view key : {"uri", "targetUri"}) {
            if (const std::string_view value = item.stringAt(key); metadataUri.empty() && isMetadataUri(value)) {
                metadataUri = std::string(value);
                for (std::string_view rangeKey : {"targetSelectionRange", "range", "targetRange"}) {
                    if (const fl::Json* r = item.find(rangeKey); r != nullptr) {
                        if (auto parsed = fl::parseRange(*r)) {
                            range = *parsed;
                            break;
                        }
                    }
                }
            }
        }
    };
    if (definition->isObject()) {
        consider(*definition);
    } else {
        for (const fl::Json& item : definition->items()) {
            if (item.isObject()) {
                consider(item);
            }
        }
    }
    MetadataSource found{.location = std::nullopt, .offered = !metadataUri.empty()};
    if (metadataUri.empty() || cacheDir.empty()) {
        co_return found;
    }
    auto metadata = co_await server.request(
        "csharp/metadata", fl::Json::object().set("textDocument", fl::Json::object().set("uri", metadataUri)), token);
    if (!metadata || !metadata->isObject() || metadata->stringAt("source").empty()) {
        co_return found;
    }
    auto file = io::joinPath(cacheDir, metadataFileName(metadataUri));
    if (!file) {
        co_return std::unexpected(std::move(file.error()));
    }
    if (auto made = fs.createDirectories(cacheDir); !made) {
        co_return std::unexpected(std::move(made.error()));
    }
    if (auto wrote = fs.writeFileAtomic(*file, metadata->stringAt("source")); !wrote) {
        co_return std::unexpected(std::move(wrote.error()));
    }
    const std::string fileUri = fl::pathToUri(*file).value_or("");
    found.location = fl::Location{.path = *file, .uri = fileUri, .range = range};
    co_return found;
}

// Go to Declaration or Usages from the server alone; when it found nothing for a C# document, the
// server's metadata source (metadataSourceTask) if it has one.
struct GotoOutcome {
    fl::GotoResult result;
    std::vector<std::string> diagnostics;
    bool metadata = false;        // result.locations holds one location in the metadata cache.
    bool metadataNoCache = false; // The server offered metadata, but no cache is configured (LTR-6).
};

sched::Task<Result<GotoOutcome>> gotoDeclarationTask(fl::ILspRequester& server, io::IFileSystem& fs,
                                                     std::string cacheDir, bool askMetadata, std::string path,
                                                     fl::LspPosition position, std::vector<std::string> unityFiles,
                                                     std::vector<fl::UnityAssetSnapshot> unityOverlays,
                                                     std::size_t offset, std::stop_token token,
                                                     std::string workspaceRoot) {
    if (!path.ends_with(".cs") && !unityFiles.empty()) {
        auto unity = fl::queryUnityAssets(fs, unityFiles, unityOverlays, path, offset, token);
        if (unity.recognized || !unity.diagnostics.empty()) {
            GotoOutcome outcome;
            outcome.result.locations = std::move(unity.declarations);
            outcome.diagnostics.insert(outcome.diagnostics.end(), unity.diagnostics.begin(), unity.diagnostics.end());
            co_return outcome;
        }
    }
    // The declarations' lines from disk, to pass over a forward type declaration (navigation.hpp).
    auto lineSource = std::make_shared<fl::FileLineSource>(fs, nullptr);
    auto result = co_await fl::gotoDeclarationOrUsages(
        server, path, position, token,
        [lineSource](std::string_view file, std::uint32_t line) { return lineSource->line(file, line); });
    if (!result) {
        co_return std::unexpected(std::move(result.error()));
    }
    GotoOutcome outcome{.result = std::move(*result), .diagnostics = {}};
    if (askMetadata && !outcome.result.showUsages && outcome.result.locations.empty()) {
        const bool noCache = cacheDir.empty();
        auto metadata = co_await metadataSourceTask(server, fs, std::move(cacheDir), path, position, token, "auto",
                                                    false, std::move(workspaceRoot));
        if (!metadata)
            co_return std::unexpected(std::move(metadata.error()));
        if (metadata->location) {
            outcome.result.locations.push_back(std::move(*metadata->location));
            outcome.metadata = true;
        } else if (metadata && metadata->offered && noCache) {
            outcome.metadataNoCache = true;
        }
    }
    co_return outcome;
}

struct UnityUsagesOutcome {
    std::vector<fl::UsageGroup> groups;
    std::vector<std::string> diagnostics;
};
sched::Task<Result<UnityUsagesOutcome>> unityUsagesTask(fl::ILspRequester& server, io::IFileSystem& fs,
                                                        std::vector<std::pair<std::string, text::Snapshot>> open,
                                                        std::vector<std::string> files, std::string path,
                                                        fl::LspPosition position, fl::PositionEncoding encoding,
                                                        std::size_t offset, std::stop_token token) {
    UnityUsagesOutcome outcome;
    std::vector<fl::UnityAssetSnapshot> overlays;
    for (const auto& [file, snapshot] : open)
        overlays.push_back({file, snapshot.text()});
    fl::UnityAssetQuery unity;
    if (path.ends_with(".cs") && !files.empty()) {
        auto params = fl::textDocumentPosition(path, position);
        if (params) {
            auto semantics = co_await server.request("frost/unity/semantics", std::move(*params), token);
            if (semantics && semantics->stringAt("contract") == "unity-source-semantics-v1") {
                const auto* member = semantics->find("selectedMember");
                if (member && member->isObject()) {
                    const auto source = member->stringAt("sourcePath"), name = member->stringAt("name"),
                               kind = member->stringAt("kind");
                    const auto* identity = member->find("identity");
                    const bool inventoried = std::ranges::any_of(
                        files, [&](const auto& file) { return fl::documentKey(file) == fl::documentKey(source); });
                    if (inventoried && fl::isUnityScriptAssociation(source, member->stringAt("declaringType")) &&
                        identity && !identity->stringAt("declarationId").empty() &&
                        !identity->stringAt("assembly").empty() &&
                        (kind == "field" || kind == "eventMethod" || kind == "asset"))
                        unity = fl::queryUnityAssets(fs, files, overlays, source, 0, token,
                                                     kind == "asset" ? std::string_view{} : name, kind);
                }
            } else if (!semantics)
                outcome.diagnostics.push_back(
                    "Unity source-symbol asset usages require the owned Roslyn semantic provider");
        }
    } else
        unity = fl::queryUnityAssets(fs, files, overlays, path, offset, token);
    outcome.diagnostics.insert(outcome.diagnostics.end(), unity.diagnostics.begin(), unity.diagnostics.end());
    if (path.ends_with(".cs") || !unity.recognized) {
        auto ordinary = co_await detail::runFindUsagesTask(server, fs, open, path, position, encoding, token);
        if (ordinary)
            outcome.groups = std::move(*ordinary);
        else if (!unity.recognized)
            co_return std::unexpected(ordinary.error());
        else
            outcome.diagnostics.push_back(
                "C# symbol usages unavailable; showing serialized asset GUID references only");
    }
    for (auto& group : unity.usages)
        outcome.groups.push_back(std::move(group));
    co_return outcome;
}
} // namespace

// --- Navigation ---

void LanguageController::gotoDeclaration() {
    if (hasDocument()) {
        gotoDeclarationAtPosition(caretPosition());
    }
}

void LanguageController::gotoDeclarationAt(ByteOffset offset) {
    if (hasDocument()) {
        gotoDeclarationAtPosition(positionAt(offset));
    }
}

void LanguageController::gotoDeclarationAtPosition(fl::LspPosition position) {
    const std::string path = m_services.host.documentPath();
    const std::string symbol = identifierAtPosition(m_services.host.snapshot(), position, m_services.server.encoding());
    // Only C# servers answer with metadata URIs; C++ declarations come from clangd alone.
    const bool askMetadata = capabilities().languageId == "csharp";
    auto unityFiles = m_services.unityAssetFiles ? m_services.unityAssetFiles() : std::vector<std::string>{};
    std::vector<fl::UnityAssetSnapshot> unityOverlays;
    for (const auto& [file, snapshot] : m_services.host.openDocuments())
        unityOverlays.push_back({file, snapshot.text()});
    std::size_t offset = 0;
    if (const auto snapshot = m_services.host.snapshot()) {
        if (const auto at = fl::fromLspPosition(*snapshot, position, m_services.server.encoding()))
            offset = at->value();
    }
    start<GotoOutcome>(
        m_navigationSlot,
        [&server = m_services.server, &fs = m_services.fs, cacheDir = m_services.metadataCacheDir,
         workspaceRoot = m_services.host.libraryWorkspaceRoot(), askMetadata, path, position,
         unityFiles = std::move(unityFiles), unityOverlays = std::move(unityOverlays), offset](std::stop_token token) {
            return gotoDeclarationTask(server, fs, cacheDir, askMetadata, path, position, unityFiles, unityOverlays,
                                       offset, std::move(token), workspaceRoot);
        },
        [this, path, position, symbol](Result<GotoOutcome> outcome) {
            if (!outcome) {
                report(outcome.error(), "Go to declaration");
                return;
            }
            if (!outcome->diagnostics.empty())
                emit statusMessage(qs(outcome->diagnostics.front()));
            if (outcome->result.showUsages) {
                findUsagesAt(path, position);
                return;
            }
            if (outcome->metadata) {
                openMetadataLocation(outcome->result.locations.front(), qs(symbol));
                return;
            }
            if (outcome->metadataNoCache) {
                // A real answer that cannot be shown here: say so instead of "returned no result".
                emit statusMessage(QStringLiteral("Source not available for '%1': %2 has metadata source for it, "
                                                  "but no metadata cache is configured (LTR-6)")
                                       .arg(qs(symbol), qs(capabilities().serverName)));
                return;
            }
            openTargets(std::move(outcome->result.locations), QStringLiteral("Choose Declaration"),
                        emptyResultMessage(QStringLiteral("Cannot find a declaration to go to")));
        });
}

void LanguageController::openMetadataLocation(const fl::Location& location, const QString& symbol) {
    emit statusMessage(QStringLiteral("Showing library source for '%1' from %2 (read-only cache)")
                           .arg(symbol, qs(capabilities().serverName)));
    m_services.host.openReadOnlyLocation(location.path, location.range.start);
}

void LanguageController::decompileExternalSymbol() { showLibrarySource("decompiled", false); }
void LanguageController::showExternalSourceLink() { showLibrarySource("sourceLink", true); }
void LanguageController::showExternalIl() { showLibrarySource("il", false); }
void LanguageController::showAssemblyMetadata() { showLibrarySource("assembly", false); }

void LanguageController::showLibrarySource(std::string mode, bool allowSourceLink) {
    if (!hasDocument()) {
        return;
    }
    const std::string path = m_services.host.documentPath();
    const fl::LspPosition position = caretPosition();
    const std::string symbol = identifierAtPosition(m_services.host.snapshot(), position, m_services.server.encoding());
    if (symbol.empty()) {
        emit statusMessage(QStringLiteral("Place the caret on a symbol to show its metadata source"));
        return;
    }
    const auto& caps = capabilities();
    if (caps.languageId != "csharp") {
        emit statusMessage(
            QStringLiteral("Source not available for '%1': %2 provides no metadata or decompiled "
                           "source (library sources and decompiler: LTR-6)")
                .arg(qs(symbol), caps.serverName.empty() ? QStringLiteral("no language server") : qs(caps.serverName)));
        return;
    }
    if (m_services.metadataCacheDir.empty()) {
        emit statusMessage(
            QStringLiteral("Source not available for '%1': no metadata cache is configured (LTR-6)").arg(qs(symbol)));
        return;
    }
    start<MetadataSource>(
        m_navigationSlot,
        [&server = m_services.server, &fs = m_services.fs, cacheDir = m_services.metadataCacheDir,
         workspaceRoot = m_services.host.libraryWorkspaceRoot(), path, position, mode = std::move(mode),
         allowSourceLink](std::stop_token token) {
            return metadataSourceTask(server, fs, cacheDir, path, position, std::move(token), mode, allowSourceLink,
                                      workspaceRoot);
        },
        [this, symbol](Result<MetadataSource> source) {
            if (!source) {
                report(source.error(), "Metadata source");
                return;
            }
            const std::optional<fl::Location>& location = source->location;
            if (!location) {
                emit statusMessage(
                    QStringLiteral("Source not available for '%1': %2 returned no metadata source (LTR-6)")
                        .arg(qs(symbol), qs(capabilities().serverName)));
                return;
            }
            openMetadataLocation(*location, qs(symbol));
        });
}

void LanguageController::gotoImplementation() {
    if (!hasDocument()) {
        return;
    }
    start<std::vector<fl::Location>>(
        m_navigationSlot,
        [&server = m_services.server, path = m_services.host.documentPath(), position = caretPosition()](
            std::stop_token token) { return fl::gotoImplementation(server, path, position, std::move(token)); },
        [this](Result<std::vector<fl::Location>> locations) {
            if (!locations) {
                report(locations.error(), "Go to implementation");
                return;
            }
            openTargets(std::move(*locations), QStringLiteral("Choose Implementation"),
                        emptyResultMessage(QStringLiteral("No implementations found")));
        });
}

void LanguageController::gotoRelatedFile() {
    if (!hasDocument()) {
        return;
    }
    const std::string currentPath = m_services.host.documentPath();
    start<std::optional<std::string>>(
        m_navigationSlot,
        [&server = m_services.server, path = currentPath](std::stop_token token) {
            return fl::requestSwitchSourceHeader(server, path, std::move(token));
        },
        [this, currentPath](Result<std::optional<std::string>> related) {
            if (related && *related && !(*related)->empty()) {
                m_services.host.openLocation(**related, fl::LspPosition{});
                return;
            }
            if (const auto fallback = findRelatedProjectFile(currentPath, m_services.projectFiles, m_services.fs)) {
                m_services.host.openLocation(*fallback, fl::LspPosition{});
                return;
            }
            if (!related) {
                report(related.error(), "Go to related files");
                return;
            }
            emit statusMessage(QStringLiteral("No related file found"));
        });
}

void LanguageController::findUsages() {
    if (m_services.combinedBindingUsages && m_services.combinedBindingUsages())
        return;
    if (hasDocument()) {
        findUsagesAt(m_services.host.documentPath(), caretPosition());
    }
}

void LanguageController::findUsagesAt(std::string path, fl::LspPosition position) {
    // The symbol's name for the panel title: the identifier around the caret.
    QString name;
    if (const std::optional<text::Snapshot> snapshot = m_services.host.snapshot()) {
        ByteOffset begin = snapshot->clip(m_services.host.caretOffset());
        ByteOffset end = begin;
        while (begin.value() > 0 && fl::isIdentifierByte(snapshot->byteAt(ByteOffset{begin.value() - 1}))) {
            begin = ByteOffset{begin.value() - 1};
        }
        while (end < snapshot->size() && fl::isIdentifierByte(snapshot->byteAt(end))) {
            end = ByteOffset{end.value() + 1};
        }
        name = qs(snapshot->text(text::ByteRange{.begin = begin, .end = end}));
    }
    auto unityFiles = m_services.unityAssetFiles ? m_services.unityAssetFiles() : std::vector<std::string>{};
    std::size_t offset = 0;
    if (m_services.host.documentPath() == path)
        if (const auto snapshot = m_services.host.snapshot())
            if (const auto at = fl::fromLspPosition(*snapshot, position, m_services.server.encoding()))
                offset = at->value();
    start<UnityUsagesOutcome>(
        m_usagesSlot,
        [&server = m_services.server, &fs = m_services.fs, open = m_services.host.openDocuments(),
         path = std::move(path), position, encoding = m_services.server.encoding(), unityFiles = std::move(unityFiles),
         offset](std::stop_token token) {
            return unityUsagesTask(server, fs, open, unityFiles, path, position, encoding, offset, std::move(token));
        },
        [this, name](Result<UnityUsagesOutcome> groups) {
            if (!groups) {
                report(groups.error(), "Find usages");
                return;
            }
            if (!groups->diagnostics.empty())
                emit statusMessage(qs(groups->diagnostics.front()));
            m_usages->setUsages(name.isEmpty() ? QStringLiteral("Usages") : QStringLiteral("Usages of '%1'").arg(name),
                                std::move(groups->groups));
            if (m_usages->usageCount() == 0 && groups->diagnostics.empty()) {
                emit statusMessage(QStringLiteral("No usages found"));
            }
            emit usagesReady();
        });
}

void LanguageController::highlightUsages() {
    m_highlightTimer.stop();
    const auto capabilities = m_services.server.serverCapabilities();
    if (!hasDocument() || !capabilities || !capabilities->documentHighlight) {
        return;
    }
    const std::string path = m_services.host.documentPath();
    const std::int64_t version = m_services.host.documentVersion();
    start<std::vector<fl::DocumentHighlight>>(
        m_highlightSlot,
        [&server = m_services.server, path, position = caretPosition()](std::stop_token token) {
            return fl::requestDocumentHighlights(server, path, position, std::move(token));
        },
        [this, path, version](Result<std::vector<fl::DocumentHighlight>> highlights) {
            // Highlights follow the caret silently: a failure only leaves the old ones cleared.
            if (m_services.host.documentPath() != path || m_services.host.documentVersion() != version) {
                return;
            }
            m_services.host.setUsageHighlights(highlights ? std::move(*highlights)
                                                          : std::vector<fl::DocumentHighlight>{});
        });
}

void LanguageController::showFileStructure() {
    if (!hasDocument()) {
        return;
    }
    const std::string path = m_services.host.documentPath();
    start<std::vector<fl::DocumentSymbol>>(
        m_symbolSlot,
        [&server = m_services.server, path](std::stop_token token) {
            return fl::requestDocumentSymbols(server, path, std::move(token));
        },
        [this, path](Result<std::vector<fl::DocumentSymbol>> symbols) {
            if (!symbols) {
                report(symbols.error(), "File structure");
                return;
            }
            // Structure mode first, so clearing the query filters locally instead of searching.
            m_symbolQueryTimer.stop();
            m_symbols->setStructure(QStringLiteral("Structure of %1").arg(qs(fileName(path))), path,
                                    std::move(*symbols));
            m_symbols->setQuery(QString());
            m_symbols->popup(m_services.host.editorWidget());
        });
}

void LanguageController::gotoClass() {
    m_symbolClassesOnly = true;
    m_symbolSlot.cancel();
    m_symbols->setWorkspaceSymbols(QStringLiteral("Go to Class"), {});
    m_symbols->setQuery(QString());
    m_symbolQueryTimer.stop();
    m_symbols->popup(m_services.host.editorWidget());
}

void LanguageController::gotoSymbol() {
    m_symbolClassesOnly = false;
    m_symbolSlot.cancel();
    m_symbols->setWorkspaceSymbols(QStringLiteral("Go to Symbol"), {});
    m_symbols->setQuery(QString());
    m_symbolQueryTimer.stop();
    m_symbols->popup(m_services.host.editorWidget());
}

void LanguageController::requestWorkspaceSymbols(const QString& query) {
    if (m_symbols->structureMode()) {
        return;
    }
    const QString title = m_symbolClassesOnly ? QStringLiteral("Go to Class") : QStringLiteral("Go to Symbol");
    if (query.isEmpty()) {
        m_symbolSlot.cancel();
        m_symbols->setWorkspaceSymbols(title, {});
        return;
    }
    const bool classesOnly = m_symbolClassesOnly;
    start<std::vector<fl::WorkspaceSymbol>>(
        m_symbolSlot,
        [&server = m_services.server, query = query.toStdString()](std::stop_token token) {
            return fl::requestWorkspaceSymbols(server, query, std::move(token));
        },
        [this, classesOnly, title](Result<std::vector<fl::WorkspaceSymbol>> symbols) {
            if (!symbols) {
                report(symbols.error(), classesOnly ? "Go to class" : "Go to symbol");
                return;
            }
            if (m_symbols->isVisible() && !m_symbols->structureMode()) {
                std::vector<fl::WorkspaceSymbol> filtered =
                    classesOnly ? fl::filterWorkspaceClasses(std::move(*symbols)) : std::move(*symbols);
                m_symbols->setWorkspaceSymbols(title, std::move(filtered));
            }
        });
}

} // namespace frost::ui::lang
