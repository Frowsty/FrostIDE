// SPDX-License-Identifier: Apache-2.0
//
// Rename, code actions and formatting (refactoring.hpp).

#include "frost/lang/refactoring.hpp"

#include "frost/core/log.hpp"
#include "frost/lang/clangd_actions.hpp"
#include "frost/lang/uri.hpp"

#include <algorithm>
#include <utility>

namespace frost::lang {
namespace {

constexpr std::string_view kLogCategory = "lang";

[[nodiscard]] bool overlapsOrTouches(const LspRange& a, const LspRange& b) noexcept {
    return !(a.end < b.start) && !(b.end < a.start);
}

} // namespace

sched::Task<Result<std::optional<PrepareRenameResult>>>
requestPrepareRename(ILspRequester& server, std::string path, LspPosition position, std::stop_token token) {
    const std::optional<ServerCapabilities> capabilities = server.serverCapabilities();
    if (capabilities && !capabilities->prepareRename) {
        co_return std::optional<PrepareRenameResult>(
            PrepareRenameResult{.range = LspRange{position, position}, .placeholder = {}, .defaultBehavior = true});
    }
    auto params = textDocumentPosition(path, position);
    if (!params) {
        co_return std::unexpected(std::move(params.error()));
    }
    auto result = co_await server.request("textDocument/prepareRename", std::move(*params), std::move(token));
    if (!result) {
        co_return std::unexpected(std::move(result.error()));
    }
    if (result->isNull()) {
        co_return std::optional<PrepareRenameResult>();
    }
    if (result->boolAt("defaultBehavior").value_or(false)) {
        co_return std::optional<PrepareRenameResult>(
            PrepareRenameResult{.range = LspRange{position, position}, .placeholder = {}, .defaultBehavior = true});
    }
    // Range, or {range, placeholder}.
    const Json* range = result->find("range");
    auto parsed = parseRange(range != nullptr ? *range : *result);
    if (!parsed) {
        co_return std::optional<PrepareRenameResult>();
    }
    co_return std::optional<PrepareRenameResult>(PrepareRenameResult{
        .range = *parsed, .placeholder = std::string(result->stringAt("placeholder")), .defaultBehavior = false});
}

sched::Task<Result<WorkspaceEdit>> requestRename(ILspRequester& server, std::string path, LspPosition position,
                                                 std::string newName, std::stop_token token) {
    if (newName.empty()) {
        co_return fail(ErrorCode::InvalidArgument, "the new name is empty");
    }
    auto params = textDocumentPosition(path, position);
    if (!params) {
        co_return std::unexpected(std::move(params.error()));
    }
    params->set("newName", newName);
    auto result = co_await server.request("textDocument/rename", std::move(*params), std::move(token));
    if (!result) {
        co_return std::unexpected(std::move(result.error()));
    }
    if (result->isNull()) {
        co_return WorkspaceEdit{};
    }
    co_return parseWorkspaceEdit(*result);
}

Json diagnosticToJson(const LiveDiagnostic& diagnostic) {
    Json out = Json::object()
                   .set("range", toJson(diagnostic.lspRange))
                   .set("severity", static_cast<int>(diagnostic.severity))
                   .set("message", diagnostic.message);
    if (!diagnostic.code.empty()) {
        out.set("code", diagnostic.code);
    }
    if (!diagnostic.source.empty()) {
        out.set("source", diagnostic.source);
    }
    return out;
}

std::vector<CodeActionItem> parseCodeActions(const Json& json) {
    std::vector<CodeActionItem> out;
    for (const Json& item : json.items()) {
        if (!item.isObject() || item.find("title") == nullptr) {
            continue;
        }
        WorkspaceEdit parsedEdit;
        if (const Json* edit = item.find("edit"); edit != nullptr && !edit->isNull()) {
            auto parsed = parseWorkspaceEdit(*edit);
            if (!parsed) {
                log::warn(kLogCategory, "skipped code action '{}': {}", item.stringAt("title"),
                          parsed.error().describe());
                continue;
            }
            parsedEdit = std::move(*parsed);
        }
        std::string commandName;
        std::vector<Json> commandArguments;
        if (const Json* cmd = item.find("command"); cmd != nullptr) {
            if (const std::string* name = cmd->asString()) {
                commandName = *name;
                if (const Json* args = item.find("arguments"); args != nullptr && args->isArray()) {
                    for (const Json& arg : args->items()) {
                        commandArguments.push_back(arg);
                    }
                }
            } else if (cmd->isObject()) {
                commandName = std::string(cmd->stringAt("command"));
                if (const Json* args = cmd->find("arguments"); args != nullptr && args->isArray()) {
                    for (const Json& arg : args->items()) {
                        commandArguments.push_back(arg);
                    }
                }
            }
        }
        if (parsedEdit.empty() && commandName.empty()) {
            continue;
        }
        out.push_back(CodeActionItem{.title = std::string(item.stringAt("title")),
                                     .kind = std::string(item.stringAt("kind")),
                                     .preferred = item.boolAt("isPreferred").value_or(false),
                                     .edit = std::move(parsedEdit),
                                     .commandName = std::move(commandName),
                                     .commandArguments = std::move(commandArguments)});
    }
    std::ranges::stable_sort(out, [](const CodeActionItem& a, const CodeActionItem& b) {
        const auto rank = [](const CodeActionItem& action) {
            if (action.preferred) {
                return 0;
            }
            return action.kind.starts_with("quickfix") ? 1 : 2;
        };
        return rank(a) < rank(b);
    });
    return out;
}

namespace {
sched::Task<Result<Json>> invalidExecuteCommand() {
    co_return fail(ErrorCode::InvalidArgument, "the command name is empty");
}
} // namespace

sched::Task<Result<Json>> requestExecuteCommand(LspRequestSender& server, std::string_view command,
                                                const std::vector<Json>& arguments, std::stop_token token) {
    if (command.empty()) {
        return invalidExecuteCommand();
    }
    Json args = Json::array();
    for (const Json& arg : arguments) {
        args.push(arg);
    }
    Json params = Json::object().set("command", command).set("arguments", std::move(args));
    return server.request("workspace/executeCommand", std::move(params), std::move(token));
}

namespace {

[[nodiscard]] char asciiLowerChar(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

[[nodiscard]] std::string toLowerAsciiString(std::string_view s) {
    std::string out(s.size(), '\0');
    for (std::size_t i = 0; i < s.size(); ++i) {
        out[i] = asciiLowerChar(s[i]);
    }
    return out;
}

Result<void> mergeTextEditIntoFile(FileEdit& targetFile, const TextEdit& incoming) {
    for (TextEdit& existing : targetFile.edits) {
        if (existing.range == incoming.range && existing.newText == incoming.newText) {
            return {};
        }
        const bool samePointInsert = incoming.range.start == incoming.range.end &&
                                     existing.range.start == existing.range.end &&
                                     incoming.range.start == existing.range.start;
        const bool overlap = incoming.range.start < existing.range.end && existing.range.start < incoming.range.end;
        const bool insertInside = incoming.range.start == incoming.range.end &&
                                  existing.range.start <= incoming.range.start &&
                                  incoming.range.start < existing.range.end;
        const bool existingInsertInside = existing.range.start == existing.range.end &&
                                          incoming.range.start <= existing.range.start &&
                                          existing.range.start < incoming.range.end;
        if (samePointInsert || overlap || insertInside || existingInsertInside) {
            return fail(ErrorCode::InvalidArgument, "clangd returned overlapping include edits");
        }
    }
    targetFile.edits.push_back(incoming);
    return {};
}

Result<void> mergeWorkspaceEdit(WorkspaceEdit& dest, const WorkspaceEdit& src) {
    for (const FileEdit& srcFile : src.files) {
        if (srcFile.edits.empty()) {
            continue;
        }
        const std::string key = documentKey(srcFile.path);
        FileEdit* target = nullptr;
        for (FileEdit& existingFile : dest.files) {
            if (documentKey(existingFile.path) == key) {
                target = &existingFile;
                break;
            }
        }
        if (target == nullptr) {
            dest.files.push_back(FileEdit{
                .path = srcFile.path,
                .uri = srcFile.uri,
                .version = srcFile.version,
                .edits = {},
            });
            target = &dest.files.back();
        } else if (target->version != srcFile.version) {
            return fail(ErrorCode::InvalidArgument, "clangd returned include edits for different document versions");
        }
        for (const TextEdit& edit : srcFile.edits) {
            if (auto merged = mergeTextEditIntoFile(*target, edit); !merged) {
                return merged;
            }
        }
    }
    return {};
}

[[nodiscard]] int includeActionPriority(const CodeActionItem& action) {
    if (action.edit.empty()) {
        return -1;
    }
    const std::string lower = toLowerAsciiString(action.title);
    if (lower == "fix all includes") {
        return 100;
    }
    if (lower == "remove all unused includes" || lower == "add all missing includes") {
        return 90;
    }
    if (lower.starts_with("remove #include")) {
        return 80;
    }
    if (lower.starts_with("#include ") || lower.starts_with("add #include") || lower.starts_with("insert #include")) {
        return action.preferred ? 75 : 70;
    }
    if (action.kind == "source.organizeImports") {
        return 60;
    }
    return -1;
}

} // namespace

sched::Task<Result<WorkspaceEdit>> requestOptimizeIncludes(ILspRequester& server, std::string path,
                                                           std::span<const LiveDiagnostic> diagnostics,
                                                           std::stop_token token) {
    WorkspaceEdit merged;
    bool removedAllUnused = false;
    bool addedAllMissing = false;

    std::vector<LiveDiagnostic> cleanerDiags;
    for (const LiveDiagnostic& d : diagnostics) {
        if (isIncludeCleanerDiagnostic(d)) {
            cleanerDiags.push_back(d);
        }
    }

    if (!cleanerDiags.empty()) {
        LspRange cleanerSpan = cleanerDiags.front().lspRange;
        for (const LiveDiagnostic& d : cleanerDiags) {
            if (d.lspRange.start < cleanerSpan.start) {
                cleanerSpan.start = d.lspRange.start;
            }
            if (cleanerSpan.end < d.lspRange.end) {
                cleanerSpan.end = d.lspRange.end;
            }
        }
        auto spanActions = co_await requestCodeActions(server, path, cleanerSpan, cleanerDiags, token);
        if (!spanActions) {
            co_return std::unexpected(std::move(spanActions.error()));
        }
        for (const CodeActionItem& action : *spanActions) {
            if (toLowerAsciiString(action.title) == "fix all includes" && !action.edit.empty()) {
                co_return action.edit;
            }
        }
        for (const CodeActionItem& action : *spanActions) {
            const std::string title = toLowerAsciiString(action.title);
            if ((title == "remove all unused includes" || title == "add all missing includes") &&
                !action.edit.empty()) {
                if (auto mergedResult = mergeWorkspaceEdit(merged, action.edit); !mergedResult) {
                    co_return std::unexpected(std::move(mergedResult.error()));
                }
                removedAllUnused = removedAllUnused || title == "remove all unused includes";
                addedAllMissing = addedAllMissing || title == "add all missing includes";
            }
        }
    }

    for (const LiveDiagnostic& diag : diagnostics) {
        if (!isIncludeCleanerDiagnostic(diag)) {
            continue;
        }
        if ((diag.code == "unused-includes" && removedAllUnused) ||
            (diag.code == "missing-includes" && addedAllMissing)) {
            continue;
        }
        auto actions = co_await requestCodeActions(server, path, diag.lspRange, {diag}, token);
        if (!actions) {
            co_return std::unexpected(std::move(actions.error()));
        }
        const CodeActionItem* best = nullptr;
        int bestPriority = -1;
        for (const CodeActionItem& candidate : *actions) {
            const int prio = includeActionPriority(candidate);
            if (prio > bestPriority) {
                bestPriority = prio;
                best = &candidate;
            }
        }
        if (best != nullptr) {
            if (auto mergedResult = mergeWorkspaceEdit(merged, best->edit); !mergedResult) {
                co_return std::unexpected(std::move(mergedResult.error()));
            }
        }
    }

    if (diagnostics.empty()) {
        auto docActions = co_await requestCodeActions(server, path, LspRange{{0, 0}, {0, 0}}, {}, std::move(token));
        if (!docActions) {
            co_return std::unexpected(std::move(docActions.error()));
        }
        const CodeActionItem* best = nullptr;
        int bestPriority = -1;
        for (const CodeActionItem& candidate : *docActions) {
            const int priority = includeActionPriority(candidate);
            if (priority > bestPriority) {
                best = &candidate;
                bestPriority = priority;
            }
        }
        if (best != nullptr) {
            if (auto mergedResult = mergeWorkspaceEdit(merged, best->edit); !mergedResult) {
                co_return std::unexpected(std::move(mergedResult.error()));
            }
        }
    }

    for (FileEdit& file : merged.files) {
        std::ranges::stable_sort(file.edits, [](const TextEdit& a, const TextEdit& b) {
            if (a.range.start != b.range.start) {
                return b.range.start < a.range.start;
            }
            return b.range.end < a.range.end;
        });
    }

    co_return merged;
}

sched::Task<Result<std::vector<CodeActionItem>>> requestCodeActions(ILspRequester& server, std::string path,
                                                                    LspRange range,
                                                                    std::vector<LiveDiagnostic> diagnostics,
                                                                    std::stop_token token) {
    auto document = textDocumentIdentifier(path);
    if (!document) {
        co_return std::unexpected(std::move(document.error()));
    }
    Json list = Json::array();
    for (const LiveDiagnostic& diagnostic : diagnostics) {
        list.push(diagnosticToJson(diagnostic));
    }
    Json params = Json::object()
                      .set("textDocument", std::move(*document))
                      .set("range", toJson(range))
                      .set("context", Json::object().set("diagnostics", std::move(list)).set("triggerKind", 1));
    auto result = co_await server.request("textDocument/codeAction", std::move(params), std::move(token));
    if (!result) {
        co_return std::unexpected(std::move(result.error()));
    }
    co_return parseCodeActions(*result);
}

std::vector<LiveDiagnostic> diagnosticsAt(std::span<const LiveDiagnostic> all, LspRange range) {
    std::vector<LiveDiagnostic> out;
    for (const LiveDiagnostic& diagnostic : all) {
        if (overlapsOrTouches(diagnostic.lspRange, range)) {
            out.push_back(diagnostic);
        }
    }
    return out;
}

sched::Task<Result<std::vector<TextEdit>>> requestFormatting(ILspRequester& server, std::string path,
                                                             std::optional<LspRange> range, FormattingOptions options,
                                                             std::stop_token token) {
    auto document = textDocumentIdentifier(path);
    if (!document) {
        co_return std::unexpected(std::move(document.error()));
    }
    Json params = Json::object()
                      .set("textDocument", std::move(*document))
                      .set("options", Json::object()
                                          .set("tabSize", static_cast<std::int64_t>(options.tabSize))
                                          .set("insertSpaces", options.insertSpaces));
    std::string method = "textDocument/formatting";
    if (range) {
        params.set("range", toJson(*range));
        method = "textDocument/rangeFormatting";
    }
    auto result = co_await server.request(std::move(method), std::move(params), std::move(token));
    if (!result) {
        co_return std::unexpected(std::move(result.error()));
    }
    co_return parseTextEdits(*result);
}

} // namespace frost::lang
