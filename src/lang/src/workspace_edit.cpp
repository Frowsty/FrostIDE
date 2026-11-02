// SPDX-License-Identifier: Apache-2.0
//
// TextEdit / WorkspaceEdit decoding, byte conversion and the standard applier (workspace_edit.hpp).

#include "frost/lang/workspace_edit.hpp"

#include "frost/io/file_system.hpp"
#include "frost/lang/uri.hpp"
#include "frost/text/text_buffer.hpp"

#include <algorithm>
#include <format>
#include <map>

namespace frost::lang {
namespace {

// Appends `edits` to the entry for `uri` in `out` (creating it), keeping the first version seen.
Result<void> addFileEdits(WorkspaceEdit& out, std::string_view uri, std::optional<std::int64_t> version,
                          std::vector<TextEdit> edits) {
    auto path = uriToPath(uri);
    if (!path) {
        return std::unexpected(std::move(path.error().withContext("a workspace edit")));
    }
    const std::string key = documentKey(*path);
    for (FileEdit& file : out.files) {
        if (documentKey(file.path) == key) {
            file.edits.insert(file.edits.end(), std::make_move_iterator(edits.begin()),
                              std::make_move_iterator(edits.end()));
            return {};
        }
    }
    out.files.push_back(
        FileEdit{.path = std::move(*path), .uri = std::string(uri), .version = version, .edits = std::move(edits)});
    return {};
}

} // namespace

std::optional<TextEdit> parseTextEdit(const Json& json) {
    if (!json.isObject()) {
        return std::nullopt;
    }
    const Json* range = json.find("range");
    if (range == nullptr) {
        range = json.find("insert"); // InsertReplaceEdit.
    }
    if (range == nullptr) {
        return std::nullopt;
    }
    auto parsed = parseRange(*range);
    if (!parsed) {
        return std::nullopt;
    }
    return TextEdit{.range = *parsed, .newText = std::string(json.stringAt("newText"))};
}

std::vector<TextEdit> parseTextEdits(const Json& json) {
    std::vector<TextEdit> out;
    for (const Json& item : json.items()) {
        if (auto edit = parseTextEdit(item)) {
            out.push_back(std::move(*edit));
        }
    }
    return out;
}

bool WorkspaceEdit::empty() const noexcept { return editCount() == 0; }

std::size_t WorkspaceEdit::editCount() const noexcept {
    std::size_t count = 0;
    for (const FileEdit& file : files) {
        count += file.edits.size();
    }
    return count;
}

Result<WorkspaceEdit> parseWorkspaceEdit(const Json& json) {
    if (!json.isObject()) {
        return fail(ErrorCode::InvalidArgument, "a workspace edit must be an object");
    }
    WorkspaceEdit out;
    if (const Json* changes = json.find("documentChanges"); changes != nullptr && changes->isArray()) {
        for (const Json& change : changes->items()) {
            if (change.find("kind") != nullptr) {
                return fail(ErrorCode::Unsupported,
                            std::format("file operation '{}' in a workspace edit", change.stringAt("kind")));
            }
            const Json* document = change.find("textDocument");
            const Json* edits = change.find("edits");
            if (document == nullptr || edits == nullptr) {
                continue;
            }
            if (auto added = addFileEdits(out, document->stringAt("uri"), document->integerAt("version"),
                                          parseTextEdits(*edits));
                !added) {
                return std::unexpected(std::move(added.error()));
            }
        }
        return out;
    }
    if (const Json* changes = json.find("changes"); changes != nullptr && changes->isObject()) {
        for (std::size_t i = 0; i < changes->size(); ++i) {
            if (auto added = addFileEdits(out, changes->keys()[i], std::nullopt, parseTextEdits(changes->items()[i]));
                !added) {
                return std::unexpected(std::move(added.error()));
            }
        }
    }
    return out;
}

Result<std::vector<ByteEdit>> toByteEdits(const text::Snapshot& snapshot, std::span<const TextEdit> edits,
                                          PositionEncoding encoding) {
    struct Indexed {
        ByteEdit edit;
        std::size_t order;
    };
    std::vector<Indexed> converted;
    converted.reserve(edits.size());
    for (std::size_t i = 0; i < edits.size(); ++i) {
        auto begin = fromLspPosition(snapshot, edits[i].range.start, encoding);
        auto end = fromLspPosition(snapshot, edits[i].range.end, encoding);
        if (!begin) {
            return std::unexpected(std::move(begin.error()));
        }
        if (!end) {
            return std::unexpected(std::move(end.error()));
        }
        if (*end < *begin) {
            return fail(ErrorCode::InvalidArgument, std::format("edit {} ends before it starts", i));
        }
        converted.push_back(Indexed{.edit = ByteEdit{.range = {*begin, *end}, .text = edits[i].newText}, .order = i});
    }
    std::ranges::stable_sort(converted, [](const Indexed& a, const Indexed& b) {
        if (a.edit.range.begin != b.edit.range.begin) {
            return a.edit.range.begin < b.edit.range.begin;
        }
        // An insert at an offset comes before a range that starts there (applyEdits' rule).
        return a.edit.range.empty() && !b.edit.range.empty();
    });
    std::vector<ByteEdit> out;
    out.reserve(converted.size());
    for (std::size_t i = 0; i < converted.size(); ++i) {
        if (i > 0) {
            const ByteEdit& previous = converted[i - 1].edit;
            const ByteEdit& next = converted[i].edit;
            if (previous.range.end > next.range.begin) {
                return fail(ErrorCode::InvalidArgument, "the server sent overlapping edits");
            }
        }
        out.push_back(std::move(converted[i].edit));
    }
    return out;
}

Result<std::string> applyTextEdits(std::string_view text, std::span<const TextEdit> edits, PositionEncoding encoding) {
    auto buffer = text::TextBuffer::fromUtf8(std::string(text));
    if (!buffer) {
        return std::unexpected(std::move(buffer.error()));
    }
    auto bytes = toByteEdits(buffer->current(), edits, encoding);
    if (!bytes) {
        return std::unexpected(std::move(bytes.error()));
    }
    std::vector<text::Edit> plain;
    plain.reserve(bytes->size());
    for (const ByteEdit& edit : *bytes) {
        plain.push_back(text::Edit{.range = edit.range, .text = edit.text});
    }
    if (auto applied = buffer->applyEdits(plain); !applied) {
        return std::unexpected(std::move(applied.error()));
    }
    return buffer->current().text();
}

ByteOffset mapThroughEdits(ByteOffset offset, std::span<const ByteEdit> edits) noexcept {
    std::int64_t delta = 0;
    for (const ByteEdit& edit : edits) {
        if (edit.range.begin >= offset) {
            break; // Edits at or after the offset (inserts there included) leave it in place.
        }
        if (offset < edit.range.end) {
            // Inside a replaced range: the end of the replacement.
            return ByteOffset{static_cast<std::uint64_t>(static_cast<std::int64_t>(edit.range.begin.value()) + delta +
                                                         static_cast<std::int64_t>(edit.text.size()))};
        }
        delta += static_cast<std::int64_t>(edit.text.size()) - static_cast<std::int64_t>(edit.range.size());
    }
    return ByteOffset{static_cast<std::uint64_t>(static_cast<std::int64_t>(offset.value()) + delta)};
}

Result<void> WorkspaceEditApplier::apply(const WorkspaceEdit& edit, std::string_view label) {
    struct Planned {
        const FileEdit* file = nullptr;
        bool open = false;
        std::vector<ByteEdit> bytes; // Open documents.
        std::string newText;         // Closed files.
    };
    std::vector<Planned> plan;
    plan.reserve(edit.files.size());
    // 1. Check every file and compute the new contents; nothing changes yet.
    for (const FileEdit& file : edit.files) {
        if (file.edits.empty()) {
            continue;
        }
        Planned planned{.file = &file, .open = false, .bytes = {}, .newText = {}};
        if (std::optional<OpenDocumentState> open = m_documents.find(file.path)) {
            if (file.version && open->lspVersion && *file.version != *open->lspVersion) {
                return fail(
                    ErrorCode::Busy,
                    std::format("{} changed after the language server computed the edit; try again", file.path));
            }
            if (open->dirty && open->changedOnDisk) {
                return fail(ErrorCode::Busy, std::format("{} has unsaved changes and also changed on disk; save or "
                                                         "reload it before applying this edit",
                                                         file.path));
            }
            auto bytes = toByteEdits(open->snapshot, file.edits, m_encoding);
            if (!bytes) {
                return std::unexpected(std::move(bytes.error().withContext(file.path)));
            }
            planned.open = true;
            planned.bytes = std::move(*bytes);
        } else {
            auto content = m_fs.readFile(file.path);
            if (!content) {
                return std::unexpected(std::move(
                    content.error().withContext(std::format("{} is not open and could not be read", file.path))));
            }
            auto edited = applyTextEdits(*content, file.edits, m_encoding);
            if (!edited) {
                return std::unexpected(std::move(edited.error().withContext(file.path)));
            }
            planned.newText = std::move(*edited);
        }
        plan.push_back(std::move(planned));
    }
    // 2. Apply. Open documents first (their undo steps are cheap to revert by the user), then files.
    for (const Planned& planned : plan) {
        if (planned.open) {
            if (auto applied = m_documents.applyEdits(planned.file->path, planned.bytes, label); !applied) {
                return std::unexpected(std::move(applied.error().withContext(planned.file->path)));
            }
        }
    }
    for (const Planned& planned : plan) {
        if (!planned.open) {
            if (auto written = m_fs.writeFileAtomic(planned.file->path, planned.newText); !written) {
                return std::unexpected(std::move(written.error().withContext(planned.file->path)));
            }
        }
    }
    return {};
}

} // namespace frost::lang
