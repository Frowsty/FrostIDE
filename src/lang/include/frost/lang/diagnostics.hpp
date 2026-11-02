// SPDX-License-Identifier: Apache-2.0
//
// Live diagnostics (PLAN.md section 3.4 "Diagnostics"; W2, ADR 0036): textDocument/
// publishDiagnostics decoded into LiveDiagnostic records, mapped to frost::text byte ranges of
// the document version they were computed for, and DiagnosticsStore, the per-file set the
// Problems tool window and the editor read.
/*
 * Shape. A LiveDiagnostic carries the fields of the build error list's record (frost::build's
 * Diagnostic, W1: severity, code, message, file, 1-based line and column), so the Problems window
 * can list build and live diagnostics side by side, plus what only a language server provides:
 * the source ("clang", "clang-tidy"), the LSP range, the byte range in a text snapshot, and the
 * related information (notes such as "previous definition is here").
 *
 * Versions. A publish names the LSP document version it was computed for (versionSupport). The
 * client maps it back to the frost::text snapshot it sent as that version; a publish for an older
 * version than the latest is kept but marked stale (the editor re-anchors or hides it; PLAN.md
 * 3.4). Files that are not open keep line and column only.
 *
 * Thread safety: DiagnosticsStore is internally synchronised; listeners run on the publishing
 * thread (a scheduler worker), outside the store's lock, and must be quick.
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/core/mutex.hpp"
#include "frost/lang/document_sync.hpp"
#include "frost/lang/json.hpp"
#include "frost/text/positions.hpp"
#include "frost/text/snapshot.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace frost::lang {

// LSP DiagnosticSeverity; the numbers are the protocol's.
enum class DiagnosticSeverity : std::uint8_t { Error = 1, Warning = 2, Information = 3, Hint = 4 };

// "error", "warning", "information" or "hint".
[[nodiscard]] std::string_view toString(DiagnosticSeverity severity) noexcept;

// DiagnosticRelatedInformation: a location and a message.
struct RelatedInformation {
    std::string file; // Absolute path; "" when the URI is not a file URI.
    LspRange range;
    int line = 0;   // 1-based (range.start.line + 1).
    int column = 0; // 1-based (range.start.character + 1).
    std::string message;
    friend bool operator==(const RelatedInformation&, const RelatedInformation&) = default;
};

struct LiveDiagnostic {
    // The error-list fields, as frost::build::Diagnostic has them.
    DiagnosticSeverity severity = DiagnosticSeverity::Error;
    std::string code;    // "undeclared_var_use", "-Wunused-variable", "misc-unused" or "".
    std::string message; // The server's text (clangd's first line is the summary).
    std::string file;    // Absolute path.
    int line = 0;        // 1-based.
    int column = 0;      // 1-based, in the negotiated encoding's units (UTF-8 bytes with clangd).
    // Language-server fields.
    std::string source; // "clang", "clang-tidy", or "".
    LspRange lspRange;  // As published (0-based).
    // The byte range in the snapshot of `textVersion`, when the document is open in the client.
    std::optional<text::ByteRange> range;
    std::optional<std::uint64_t> textVersion;
    std::vector<RelatedInformation> related;
    friend bool operator==(const LiveDiagnostic&, const LiveDiagnostic&) = default;
};

// Every diagnostic of one file, as the last publish left them.
struct FileDiagnostics {
    std::string file; // Absolute path.
    std::string uri;
    std::optional<std::int64_t> lspVersion;   // The document version the server named, if any.
    std::optional<std::uint64_t> textVersion; // The matching frost::text snapshot version, if known.
    bool stale = false;                       // Computed for an older version than the client's latest.
    std::vector<LiveDiagnostic> diagnostics;
};

// Decodes publishDiagnostics params (ranges not mapped yet). Diagnostics without a usable range
// are skipped. Errors: InvalidArgument (no uri or no diagnostics array; a non-file URI).
[[nodiscard]] Result<FileDiagnostics> decodePublishDiagnostics(const Json& params);

// Fills `range` and `textVersion` of every diagnostic from `snapshot`, the text the server saw.
// Diagnostics whose positions fall outside it keep range empty. O(diagnostics × log n).
void mapToSnapshot(FileDiagnostics& diagnostics, const text::Snapshot& snapshot, PositionEncoding encoding);

class DiagnosticsStore {
public:
    using Listener = std::function<void(const FileDiagnostics&)>;

    // Replaces `diagnostics.file`'s set (an empty list clears it) and notifies the listeners.
    void publish(FileDiagnostics diagnostics);
    // Removes every file's diagnostics (e.g. the server stopped), notifying with empty sets.
    void clear();

    // The current set of one file (by documentKey), if any was published.
    [[nodiscard]] std::optional<FileDiagnostics> forFile(std::string_view path) const;
    // Every file with at least one diagnostic, ordered by path.
    [[nodiscard]] std::vector<FileDiagnostics> files() const;
    // Every diagnostic flattened in Problems-window order: file, line, column.
    [[nodiscard]] std::vector<LiveDiagnostic> problems() const;
    [[nodiscard]] std::size_t count(DiagnosticSeverity severity) const;
    // Increments on every publish or clear, so a waiter can tell that something arrived.
    [[nodiscard]] std::uint64_t generation() const;

    // Adds a listener; returns its id for removeListener(). Listeners are called in id order.
    std::uint64_t addListener(Listener listener);
    void removeListener(std::uint64_t id);

private:
    void notify(const FileDiagnostics& diagnostics) const;

    mutable Mutex m_mutex;
    std::map<std::string, FileDiagnostics> m_files FROST_GUARDED_BY(m_mutex); // documentKey -> set.
    std::uint64_t m_generation FROST_GUARDED_BY(m_mutex) = 0;
    std::map<std::uint64_t, Listener> m_listeners FROST_GUARDED_BY(m_mutex);
    std::uint64_t m_nextListener FROST_GUARDED_BY(m_mutex) = 1;
};

} // namespace frost::lang
