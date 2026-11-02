// SPDX-License-Identifier: Apache-2.0
//
// Signature help and hover (code_assist.hpp).

#include "frost/lang/code_assist.hpp"

#include <algorithm>

namespace frost::lang {
namespace {

// The byte index of UTF-16 offset `units` in `text` (clamped to its size).
[[nodiscard]] std::size_t byteIndexOfUtf16(std::string_view text, std::uint32_t units) {
    return byteColumn(text, units, PositionEncoding::Utf16);
}

} // namespace

std::uint32_t SignatureHelp::activeParameterOf(std::size_t index) const noexcept {
    if (index < signatures.size() && signatures[index].activeParameter) {
        return *signatures[index].activeParameter;
    }
    return activeParameter;
}

std::optional<SignatureHelp> parseSignatureHelp(const Json& json) {
    if (!json.isObject()) {
        return std::nullopt;
    }
    SignatureHelp help;
    if (const Json* signatures = json.find("signatures"); signatures != nullptr) {
        for (const Json& item : signatures->items()) {
            SignatureInfo signature;
            signature.label = std::string(item.stringAt("label"));
            if (const Json* documentation = item.find("documentation"); documentation != nullptr) {
                signature.documentation = parseMarkup(*documentation);
            }
            if (const std::optional<std::int64_t> active = item.integerAt("activeParameter"); active && *active >= 0) {
                signature.activeParameter = static_cast<std::uint32_t>(*active);
            }
            if (const Json* parameters = item.find("parameters"); parameters != nullptr) {
                for (const Json& parameterJson : parameters->items()) {
                    ParameterInfo parameter;
                    if (const Json* label = parameterJson.find("label"); label != nullptr) {
                        if (const std::string* text = label->asString()) {
                            parameter.label = *text;
                        } else if (label->isArray() && label->size() == 2) {
                            const std::int64_t start = label->items()[0].asInteger().value_or(0);
                            const std::int64_t end = label->items()[1].asInteger().value_or(0);
                            if (start >= 0 && end >= start) {
                                parameter.offsets = {static_cast<std::uint32_t>(start),
                                                     static_cast<std::uint32_t>(end)};
                            }
                        }
                    }
                    if (const Json* documentation = parameterJson.find("documentation"); documentation != nullptr) {
                        parameter.documentation = parseMarkup(*documentation);
                    }
                    signature.parameters.push_back(std::move(parameter));
                }
            }
            help.signatures.push_back(std::move(signature));
        }
    }
    if (help.signatures.empty()) {
        return std::nullopt;
    }
    help.activeSignature = static_cast<std::uint32_t>(std::clamp<std::int64_t>(
        json.integerAt("activeSignature").value_or(0), 0, static_cast<std::int64_t>(help.signatures.size()) - 1));
    help.activeParameter =
        static_cast<std::uint32_t>(std::max<std::int64_t>(json.integerAt("activeParameter").value_or(0), 0));
    return help;
}

std::optional<std::pair<std::size_t, std::size_t>> parameterRange(const SignatureInfo& signature,
                                                                  std::size_t parameter) {
    if (parameter >= signature.parameters.size()) {
        return std::nullopt;
    }
    const std::string_view label = signature.label;
    // Search from where the previous parameter ended; start after the opening parenthesis.
    std::size_t from = label.find('(');
    from = from == std::string_view::npos ? 0 : from + 1;
    for (std::size_t i = 0; i <= parameter; ++i) {
        const ParameterInfo& info = signature.parameters[i];
        std::size_t begin = 0;
        std::size_t end = 0;
        if (info.offsets) {
            begin = byteIndexOfUtf16(label, info.offsets->first);
            end = byteIndexOfUtf16(label, info.offsets->second);
        } else {
            if (info.label.empty()) {
                return std::nullopt;
            }
            begin = label.find(info.label, from);
            if (begin == std::string_view::npos) {
                begin = label.find(info.label);
            }
            if (begin == std::string_view::npos) {
                return std::nullopt;
            }
            end = begin + info.label.size();
        }
        if (i == parameter) {
            return std::pair{begin, end};
        }
        from = end;
    }
    return std::nullopt;
}

bool isSignatureHelpTrigger(char c) noexcept { return c == '(' || c == ','; }

sched::Task<Result<std::optional<SignatureHelp>>> requestSignatureHelp(ILspRequester& server, std::string path,
                                                                       LspPosition position,
                                                                       std::optional<char> triggerCharacter,
                                                                       bool retrigger, std::stop_token token) {
    auto params = textDocumentPosition(path, position);
    if (!params) {
        co_return std::unexpected(std::move(params.error()));
    }
    Json context = Json::object().set("triggerKind", triggerCharacter ? 2 : 1).set("isRetrigger", retrigger);
    if (triggerCharacter) {
        context.set("triggerCharacter", std::string(1, *triggerCharacter));
    }
    params->set("context", std::move(context));
    auto result = co_await server.request("textDocument/signatureHelp", std::move(*params), std::move(token));
    if (!result) {
        co_return std::unexpected(std::move(result.error()));
    }
    co_return parseSignatureHelp(*result);
}

std::optional<HoverInfo> parseHover(const Json& json) {
    if (!json.isObject()) {
        return std::nullopt;
    }
    const Json* contents = json.find("contents");
    if (contents == nullptr) {
        return std::nullopt;
    }
    HoverInfo hover{.contents = parseMarkup(*contents), .range = std::nullopt};
    if (hover.contents.text.empty()) {
        return std::nullopt;
    }
    if (const Json* range = json.find("range"); range != nullptr) {
        hover.range = parseRange(*range);
    }
    return hover;
}

sched::Task<Result<std::optional<HoverInfo>>> requestHover(ILspRequester& server, std::string path,
                                                           LspPosition position, std::stop_token token) {
    auto params = textDocumentPosition(path, position);
    if (!params) {
        co_return std::unexpected(std::move(params.error()));
    }
    auto result = co_await server.request("textDocument/hover", std::move(*params), std::move(token));
    if (!result) {
        co_return std::unexpected(std::move(result.error()));
    }
    co_return parseHover(*result);
}

} // namespace frost::lang
