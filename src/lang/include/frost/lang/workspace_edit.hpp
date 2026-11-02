// SPDX-License-Identifier: Apache-2.0
//
// Text edits from a language server (W2, ADR 0041): TextEdit and WorkspaceEdit decoded from LSP
// 3.17, converted to frost::text byte edits, and applied through the IWorkspaceEditApplier seam
// the workbench implements (rename, quick fixes, completion's additional edits, reformat).
/*
 * Conversion. An LSP TextEdit's range is in the negotiated position encoding and refers to the
 * text before the batch; edits of one batch never overlap (LSP 3.17 "TextEdit[]"). toByteEdits
 * maps them onto a snapshot and sorts them for TextBuffer::applyEdits (ascending, inserts at
 * one offset keep their order); overlapping edits are refused.
 *
 * Application (WorkspaceEditApplier, the standard IWorkspaceEditApplier). Every file is checked
 * before anything changes, so an edit applies to all its files or to none:
 *   - an open document is edited through IWorkspaceDocuments::applyEdits, one undo step per
 *     document; it is refused when the edit names an LSP version that is not the document's
 *     (the server computed it for older text), or when the document has unsaved changes and its
 *     file also changed on disk (the "unsaved-on-disk conflict": neither copy can be trusted to
 *     be what the server saw, so the user must save or reload first);
 *   - a file that is not open is read, edited and written back atomically (writeFileAtomic) when
 *     the edit spans several files; a missing or unreadable file refuses the whole edit.
 * The error message names the file and says what to do, for the status bar.
 *
 * Threads: the decoders and conversions are pure. WorkspaceEditApplier runs on the thread that
 * owns the documents (the UI thread); it is not synchronised.
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/lang/document_sync.hpp"
#include "frost/lang/json.hpp"
#include "frost/text/positions.hpp"
#include "frost/text/snapshot.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace frost::io {
class IFileSystem;
} // namespace frost::io

namespace frost::lang {

// LSP TextEdit: replace `range` with `newText`.
struct TextEdit {
    LspRange range;
    std::string newText;
    friend bool operator==(const TextEdit&, const TextEdit&) = default;
};

// A TextEdit, or an InsertReplaceEdit (its insert range); nullopt for anything else.
[[nodiscard]] std::optional<TextEdit> parseTextEdit(const Json& json);
// An array of TextEdits (unusable entries skipped); null gives an empty list.
[[nodiscard]] std::vector<TextEdit> parseTextEdits(const Json& json);

// The edits of one file.
struct FileEdit {
    std::string path; // Absolute.
    std::string uri;
    std::optional<std::int64_t> version; // documentChanges' version, when the server named one.
    std::vector<TextEdit> edits;
    friend bool operator==(const FileEdit&, const FileEdit&) = default;
};

struct WorkspaceEdit {
    std::vector<FileEdit> files; // In the server's order; one entry per file.
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::size_t editCount() const noexcept;
    friend bool operator==(const WorkspaceEdit&, const WorkspaceEdit&) = default;
};

/*
 * Reads a WorkspaceEdit: `documentChanges` (TextDocumentEdits) when present, else `changes`
 * (uri -> TextEdit[]). Edits for the same file are merged. Errors: Unsupported (create, rename
 * or delete file operations), InvalidArgument (a non-file URI, or not an object).
 */
[[nodiscard]] Result<WorkspaceEdit> parseWorkspaceEdit(const Json& json);

// One edit in bytes, for TextBuffer::applyEdits.
struct ByteEdit {
    text::ByteRange range;
    std::string text;
    friend bool operator==(const ByteEdit&, const ByteEdit&) = default;
};

// `edits` mapped onto `snapshot` and sorted as applyEdits wants. Errors: OutOfRange (a line past
// the end), InvalidArgument (overlapping edits). O(edits log edits + edits log n).
[[nodiscard]] Result<std::vector<ByteEdit>> toByteEdits(const text::Snapshot& snapshot, std::span<const TextEdit> edits,
                                                        PositionEncoding encoding);

// `text` with `edits` applied (for files that are not open). Errors: as toByteEdits, and
// InvalidArgument for text that is not UTF-8.
[[nodiscard]] Result<std::string> applyTextEdits(std::string_view text, std::span<const TextEdit> edits,
                                                 PositionEncoding encoding);

// Where a byte offset of the text before `edits` (sorted, as toByteEdits returns) ends up after
// them: edits before it shift it, edits at or after it (inserts at it included) leave it, and an
// offset inside a replaced range moves to the end of the replacement. O(edits).
[[nodiscard]] ByteOffset mapThroughEdits(ByteOffset offset, std::span<const ByteEdit> edits) noexcept;

// The seam rename, quick fixes and reformat apply their edits through. The workbench implements
// it over its open documents and the files on disk.
class IWorkspaceEditApplier {
public:
    virtual ~IWorkspaceEditApplier() = default;
    // Applies `edit` as one user action called `label` ("Rename", "Quick fix"), to every file or
    // to none. Errors: Busy (a conflict, with a message saying what to do), the I/O errors.
    [[nodiscard]] virtual Result<void> apply(const WorkspaceEdit& edit, std::string_view label) = 0;

protected:
    IWorkspaceEditApplier() = default;
    IWorkspaceEditApplier(const IWorkspaceEditApplier&) = default;
    IWorkspaceEditApplier& operator=(const IWorkspaceEditApplier&) = default;
};

// An open document as the applier sees it.
struct OpenDocumentState {
    text::Snapshot snapshot;
    std::optional<std::int64_t> lspVersion; // The version the language server knows, if synced.
    bool dirty = false;                     // Unsaved changes.
    bool changedOnDisk = false;             // The file changed on disk since it was loaded or saved.
};

// The workbench's open documents, for WorkspaceEditApplier.
class IWorkspaceDocuments {
public:
    virtual ~IWorkspaceDocuments() = default;
    // The open document at `path` (absolute), or nullopt when it is not open.
    [[nodiscard]] virtual std::optional<OpenDocumentState> find(std::string_view path) = 0;
    // Applies `edits` (sorted, against find(path)->snapshot) as one undo step called `label`.
    [[nodiscard]] virtual Result<void> applyEdits(std::string_view path, std::span<const ByteEdit> edits,
                                                  std::string_view label) = 0;

protected:
    IWorkspaceDocuments() = default;
    IWorkspaceDocuments(const IWorkspaceDocuments&) = default;
    IWorkspaceDocuments& operator=(const IWorkspaceDocuments&) = default;
};

// The standard applier (see the header comment). `documents` and `fs` must outlive it.
class WorkspaceEditApplier final : public IWorkspaceEditApplier {
public:
    WorkspaceEditApplier(IWorkspaceDocuments& documents, io::IFileSystem& fs, PositionEncoding encoding)
        : m_documents(documents), m_fs(fs), m_encoding(encoding) {}
    // The encoding of the edits applied from now on (it changes when clangd restarts).
    void setEncoding(PositionEncoding encoding) noexcept { m_encoding = encoding; }
    [[nodiscard]] Result<void> apply(const WorkspaceEdit& edit, std::string_view label) override;

private:
    IWorkspaceDocuments& m_documents;
    io::IFileSystem& m_fs;
    PositionEncoding m_encoding;
};

} // namespace frost::lang
