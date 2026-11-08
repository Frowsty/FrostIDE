// SPDX-License-Identifier: Apache-2.0
//
// LanguageController (frost/ui/lang/language_controller.hpp): navigation, go to declaration (and
// metadata sources), implementation and related file, find and highlight usages, file structure
// and go to class or symbol (split from language_controller.cpp, FND-12a).

#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/lang/navigation.hpp"
#include "frost/lang/symbols.hpp"
#include "frost/lang/uri.hpp"
#include "frost/ui/lang/language_controller.hpp"

#include "language_controller_internal.hpp"

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

bool isClassLikeSymbol(fl::SymbolKind kind) {
    return kind == fl::SymbolKind::Class || kind == fl::SymbolKind::Struct || kind == fl::SymbolKind::Interface ||
           kind == fl::SymbolKind::Enum || kind == fl::SymbolKind::Namespace;
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
        const std::string siblingStr = current.prefixWithoutExt + std::string(targetExt);
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

/*
 * Real metadata-as-source for the symbol at `position` (LTR-1): textDocument/definition answered
 * with a metadata URI, and the server's csharp/metadata answered with its source. The source is
 * written to `cacheDir` (never the workspace) and the location points into that file at the range
 * the server gave. No location when the server has no metadata, or when no cache directory is
 * configured (then nothing more is asked or written, and `offered` tells whether there was some).
 */
sched::Task<Result<MetadataSource>> metadataSourceTask(fl::ILspRequester& server, io::IFileSystem& fs,
                                                       std::string cacheDir, std::string path, fl::LspPosition position,
                                                       std::stop_token token) {
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
    bool metadata = false;        // result.locations holds one location in the metadata cache.
    bool metadataNoCache = false; // The server offered metadata, but no cache is configured (LTR-6).
};

sched::Task<Result<GotoOutcome>> gotoDeclarationTask(fl::ILspRequester& server, io::IFileSystem& fs,
                                                     std::string cacheDir, bool askMetadata, std::string path,
                                                     fl::LspPosition position, std::stop_token token) {
    // The declarations' lines from disk, to pass over a forward type declaration (navigation.hpp).
    auto lineSource = std::make_shared<fl::FileLineSource>(fs, nullptr);
    auto result = co_await fl::gotoDeclarationOrUsages(
        server, path, position, token,
        [lineSource](std::string_view file, std::uint32_t line) { return lineSource->line(file, line); });
    if (!result) {
        co_return std::unexpected(std::move(result.error()));
    }
    GotoOutcome outcome{.result = std::move(*result), .metadata = false};
    if (askMetadata && !outcome.result.showUsages && outcome.result.locations.empty()) {
        const bool noCache = cacheDir.empty();
        auto metadata = co_await metadataSourceTask(server, fs, std::move(cacheDir), path, position, token);
        if (metadata && metadata->location) {
            outcome.result.locations.push_back(std::move(*metadata->location));
            outcome.metadata = true;
        } else if (metadata && metadata->offered && noCache) {
            outcome.metadataNoCache = true;
        }
    }
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
    start<GotoOutcome>(
        m_navigationSlot,
        [&server = m_services.server, &fs = m_services.fs, cacheDir = m_services.metadataCacheDir, askMetadata, path,
         position](std::stop_token token) {
            return gotoDeclarationTask(server, fs, cacheDir, askMetadata, path, position, std::move(token));
        },
        [this, path, position, symbol](Result<GotoOutcome> outcome) {
            if (!outcome) {
                report(outcome.error(), "Go to declaration");
                return;
            }
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
    emit statusMessage(QStringLiteral("Showing metadata source for '%1' from %2 (read-only cache)")
                           .arg(symbol, qs(capabilities().serverName)));
    m_services.host.openReadOnlyLocation(location.path, location.range.start);
}

void LanguageController::decompileExternalSymbol() {
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
        [&server = m_services.server, &fs = m_services.fs, cacheDir = m_services.metadataCacheDir, path,
         position](std::stop_token token) {
            return metadataSourceTask(server, fs, cacheDir, path, position, std::move(token));
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
    start<std::vector<fl::UsageGroup>>(
        m_usagesSlot,
        [&server = m_services.server, &fs = m_services.fs, open = m_services.host.openDocuments(), path, position,
         encoding = m_services.server.encoding()](std::stop_token token) {
            return detail::runFindUsagesTask(server, fs, open, path, position, encoding, std::move(token));
        },
        [this, name](Result<std::vector<fl::UsageGroup>> groups) {
            if (!groups) {
                report(groups.error(), "Find usages");
                return;
            }
            m_usages->setUsages(name.isEmpty() ? QStringLiteral("Usages") : QStringLiteral("Usages of '%1'").arg(name),
                                std::move(*groups));
            if (m_usages->usageCount() == 0) {
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
                std::vector<fl::WorkspaceSymbol> filtered = std::move(*symbols);
                if (classesOnly) {
                    std::erase_if(filtered, [](const fl::WorkspaceSymbol& s) { return !isClassLikeSymbol(s.kind); });
                }
                m_symbols->setWorkspaceSymbols(title, std::move(filtered));
            }
        });
}

} // namespace frost::ui::lang
