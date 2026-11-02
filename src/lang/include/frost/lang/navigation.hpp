// SPDX-License-Identifier: Apache-2.0
//
// Navigation (W2, ADR 0041): go to definition / declaration / implementation (Ctrl+B,
// Ctrl+click, Ctrl+Alt+B), clangd's switch between header and source (Go to Related Files,
// Ctrl+Alt+Home), find usages (Alt+F7: textDocument/references grouped by file with the line
// text) and the usages highlighted under the caret (textDocument/documentHighlight).
/*
 * Ctrl+B follows Rider's "go to declaration or usages": the definition, unless the caret is
 * already on it, then the declaration, unless the caret is on that too, then find usages.
 *
 * Usage lines. groupUsages reads each usage's line through a LineSource, so open documents are
 * read from their snapshots and other files from disk (FileLineSource caches each file once per
 * search). Groups are ordered by path, usages by line and column; the column and highlight are
 * converted from the server's encoding to bytes of the line.
 *
 * Threads: the requests are coroutines; groupUsages and FileLineSource run on one thread (a
 * worker: they read files).
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/lang/json.hpp"
#include "frost/lang/lsp_client.hpp"
#include "frost/lang/lsp_types.hpp"
#include "frost/sched/task.hpp"
#include "frost/text/snapshot.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace frost::io {
class IFileSystem;
} // namespace frost::io

namespace frost::lang {

enum class NavigationTarget : std::uint8_t { Definition, Declaration, Implementation, TypeDefinition };

// "textDocument/definition" and so on.
[[nodiscard]] std::string_view methodFor(NavigationTarget target) noexcept;

// The locations of `target` for the symbol at `position`. Errors: request's.
[[nodiscard]] sched::Task<Result<std::vector<Location>>> requestLocations(ILspRequester& server,
                                                                          NavigationTarget target, std::string path,
                                                                          LspPosition position,
                                                                          std::stop_token token = {});

// Where Ctrl+B goes (see the header comment).
struct GotoResult {
    std::vector<Location> locations; // Empty with showUsages, or when nothing was found.
    bool showUsages = false;         // The caret is on the declaration itself: run find usages.
};

[[nodiscard]] sched::Task<Result<GotoResult>> gotoDeclarationOrUsages(ILspRequester& server, std::string path,
                                                                      LspPosition position, std::stop_token token = {});

// clangd's textDocument/switchSourceHeader: the counterpart of `path`, or nullopt. Errors:
// request's (MethodNotFound from servers other than clangd).
[[nodiscard]] sched::Task<Result<std::optional<std::string>>>
requestSwitchSourceHeader(ILspRequester& server, std::string path, std::stop_token token = {});

// textDocument/references with the declaration included. Errors: request's.
[[nodiscard]] sched::Task<Result<std::vector<Location>>> requestReferences(ILspRequester& server, std::string path,
                                                                           LspPosition position,
                                                                           bool includeDeclaration = true,
                                                                           std::stop_token token = {});

// One line of a file for the usages list: its text without the break, or nullopt when the file or
// line cannot be read.
using LineSource = std::function<std::optional<std::string>(std::string_view path, std::uint32_t line)>;

// Lines from open documents (`openDocument` returns a snapshot for open paths) and, for other
// files, from `fs` (each file read once). Not thread-safe; `fs` must outlive it.
class FileLineSource {
public:
    FileLineSource(io::IFileSystem& fs,
                   std::function<std::optional<text::Snapshot>(std::string_view path)> openDocument)
        : m_fs(fs), m_openDocument(std::move(openDocument)) {}
    [[nodiscard]] std::optional<std::string> line(std::string_view path, std::uint32_t line);
    // As a LineSource; the FileLineSource must outlive the returned function.
    [[nodiscard]] LineSource source() {
        return [this](std::string_view path, std::uint32_t index) { return line(path, index); };
    }

private:
    io::IFileSystem& m_fs;
    std::function<std::optional<text::Snapshot>(std::string_view)> m_openDocument;
    std::map<std::string, std::optional<text::Snapshot>> m_files; // documentKey -> text (nullopt: unreadable).
};

struct Usage {
    Location location;
    std::uint32_t line = 0;           // 1-based.
    std::uint32_t column = 0;         // 1-based, in bytes of the line.
    std::string lineText;             // Without leading indentation.
    std::uint32_t highlightBegin = 0; // Byte range of the usage in lineText.
    std::uint32_t highlightEnd = 0;
};

struct UsageGroup {
    std::string path;
    std::vector<Usage> usages;
};

// The locations grouped by file (see the header comment). O(n log n) plus the line reads.
[[nodiscard]] std::vector<UsageGroup> groupUsages(std::span<const Location> locations, const LineSource& lines,
                                                  PositionEncoding encoding);

// LSP DocumentHighlightKind; the numbers are the protocol's.
enum class HighlightKind : std::uint8_t { Text = 1, Read = 2, Write = 3 };

struct DocumentHighlight {
    LspRange range;
    HighlightKind kind = HighlightKind::Text;
    friend bool operator==(const DocumentHighlight&, const DocumentHighlight&) = default;
};

[[nodiscard]] std::vector<DocumentHighlight> parseDocumentHighlights(const Json& json);

// textDocument/documentHighlight. Errors: request's.
[[nodiscard]] sched::Task<Result<std::vector<DocumentHighlight>>>
requestDocumentHighlights(ILspRequester& server, std::string path, LspPosition position, std::stop_token token = {});

} // namespace frost::lang
