// SPDX-License-Identifier: Apache-2.0
//
// Rename, code actions and formatting (refactoring.hpp).

#include "frost/lang/refactoring.hpp"

#include "frost/core/log.hpp"

#include <algorithm>

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
