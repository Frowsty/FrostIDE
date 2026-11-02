// SPDX-License-Identifier: Apache-2.0
//
// Parameter info and quick documentation (W2, ADR 0041): textDocument/signatureHelp (Ctrl+P, and
// automatically on '(' and ',') and textDocument/hover (Ctrl+Q), decoded into small models the
// popups render. The hover text stays Markdown; the UI turns it into rich text.
/*
 * Parameter labels. FrostIDE announces labelOffsetSupport = false, so servers send each
 * parameter's label as a substring of the signature label; parameterRange finds them left to
 * right (a later parameter is searched after the previous one's end, so "f(int x, int x)" still
 * highlights the second "int x"). Offset pairs ([start, end), UTF-16 units) are honoured too.
 * Threads: the requests are coroutines; the rest is pure.
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/lang/json.hpp"
#include "frost/lang/lsp_client.hpp"
#include "frost/lang/lsp_types.hpp"
#include "frost/sched/task.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace frost::lang {

struct ParameterInfo {
    std::string label;                                              // The text, or empty when the server sent offsets.
    std::optional<std::pair<std::uint32_t, std::uint32_t>> offsets; // UTF-16 [start, end) in the signature.
    Markup documentation;
};

struct SignatureInfo {
    std::string label; // "int add(int a, int b)"
    Markup documentation;
    std::vector<ParameterInfo> parameters;
    std::optional<std::uint32_t> activeParameter; // Per-signature override (LSP 3.16).
};

struct SignatureHelp {
    std::vector<SignatureInfo> signatures;
    std::uint32_t activeSignature = 0;
    std::uint32_t activeParameter = 0;
    // The active parameter of signature `index`: its own override, else activeParameter.
    [[nodiscard]] std::uint32_t activeParameterOf(std::size_t index) const noexcept;
};

// A SignatureHelp; nullopt for null or a help without signatures.
[[nodiscard]] std::optional<SignatureHelp> parseSignatureHelp(const Json& json);

// The byte range [first, second) of parameter `parameter` in `signature.label`, or nullopt.
[[nodiscard]] std::optional<std::pair<std::size_t, std::size_t>> parameterRange(const SignatureInfo& signature,
                                                                                std::size_t parameter);

// True for the characters that open parameter info by themselves: '(' and ','.
[[nodiscard]] bool isSignatureHelpTrigger(char c) noexcept;

// textDocument/signatureHelp. `triggerCharacter` is the typed character, if any; `retrigger`
// says a popup is already showing. nullopt when the caret is not in a call. Errors: request's.
[[nodiscard]] sched::Task<Result<std::optional<SignatureHelp>>>
requestSignatureHelp(ILspRequester& server, std::string path, LspPosition position,
                     std::optional<char> triggerCharacter, bool retrigger, std::stop_token token = {});

struct HoverInfo {
    Markup contents;
    std::optional<LspRange> range; // What the hover is about, when the server says.
};

// A Hover; nullopt for null or empty contents.
[[nodiscard]] std::optional<HoverInfo> parseHover(const Json& json);

// textDocument/hover; nullopt when there is nothing to show. Errors: request's.
[[nodiscard]] sched::Task<Result<std::optional<HoverInfo>>>
requestHover(ILspRequester& server, std::string path, LspPosition position, std::stop_token token = {});

} // namespace frost::lang
