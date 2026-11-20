// SPDX-License-Identifier: Apache-2.0
// Real bounded stdio LSP example. Counts only received open-buffer text; no file/network access.
#include "frost/rpc/framing.hpp"
#include "frost/rpc/json.hpp"

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <map>
#include <string>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif
using frost::rpc::Json;
namespace {
void reply(const Json& id, Json result) {
    const auto body = Json::object().set("jsonrpc", "2.0").set("id", id).set("result", std::move(result)).dump();
    std::cout << frost::rpc::encodeFrame(body) << std::flush;
}
void refuse(const Json& id) {
    const auto body =
        Json::object()
            .set("jsonrpc", "2.0")
            .set("id", id)
            .set("error", Json::object().set("code", -32601).set("message", "Unsupported sample method or command"))
            .dump();
    std::cout << frost::rpc::encodeFrame(body) << std::flush;
}
bool whitespace(unsigned char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }
} // namespace
int main() {
#ifdef _WIN32
    if (_setmode(_fileno(stdin), _O_BINARY) == -1 || _setmode(_fileno(stdout), _O_BINARY) == -1)
        return 2;
#endif
    std::map<std::string, std::string> documents;
    bool initialized = false, shutdown = false;
    while (std::cin && std::cout) {
        std::size_t length = 0, headerBytes = 0;
        bool hasLength = false;
        std::string line;
        for (;;) {
            line.clear();
            char character{};
            for (;;) {
                if (!std::cin.get(character))
                    return shutdown ? 0 : 2;
                if (++headerBytes > 8192)
                    return 2;
                if (character == '\n')
                    break;
                line.push_back(character);
            }
            if (line.empty() || line.back() != '\r')
                return 2;
            line.pop_back();
            if (line.empty())
                break;
            if (line.starts_with("Content-Length: ")) {
                if (hasLength)
                    return 2;
                hasLength = true;
                const auto number = std::string_view(line).substr(16);
                const auto parsed = std::from_chars(number.data(), number.data() + number.size(), length);
                if (parsed.ec != std::errc{} || parsed.ptr != number.data() + number.size())
                    return 2;
            }
        }
        if (!hasLength || length > 4 * 1024 * 1024)
            return 2;
        std::string body(length, '\0');
        if (!std::cin.read(body.data(), static_cast<std::streamsize>(body.size())))
            return 2;
        auto message = frost::rpc::parseJson(body);
        if (!message || !message->isObject())
            return 2;
        const auto method = message->stringAt("method");
        const auto* id = message->find("id");
        const auto* params = message->find("params");
        if (method == "exit")
            return shutdown ? 0 : 2;
        if (method == "initialize" && id && !initialized) {
            initialized = true;
            reply(*id,
                  Json::object()
                      .set("serverInfo", Json::object().set("name", "Frost text-statistics sample").set("version", "1"))
                      .set("capabilities",
                           Json::object()
                               .set("positionEncoding", "utf-16")
                               .set("textDocumentSync", 1)
                               .set("executeCommandProvider",
                                    Json::object().set("commands", Json::array({"frost.sample.statistics"})))));
        } else if (method == "shutdown" && id) {
            shutdown = true;
            reply(*id, {});
        } else if (!initialized || shutdown) {
            if (id)
                refuse(*id);
        } else if (params && (method == "textDocument/didOpen" || method == "textDocument/didChange" ||
                              method == "textDocument/didClose")) {
            const auto* document = params->find("textDocument");
            if (!document)
                continue;
            const auto uri = std::string(document->stringAt("uri"));
            if (uri.empty() || uri.size() > 32768)
                continue;
            if (method == "textDocument/didClose") {
                documents.erase(uri);
                continue;
            }
            const Json* text = document->find("text");
            if (method == "textDocument/didChange") {
                const auto* changes = params->find("contentChanges");
                if (!changes || !changes->isArray() || changes->size() != 1 || changes->items()[0].find("range"))
                    continue;
                text = changes->items()[0].find("text");
            }
            if (text && text->isString() && text->asString()->size() <= 1024 * 1024 &&
                (documents.contains(uri) || documents.size() < 16))
                documents[uri] = *text->asString();
        } else if (method == "workspace/executeCommand" && id && params &&
                   params->stringAt("command") == "frost.sample.statistics") {
            auto rows = Json::array();
            for (const auto& [uri, text] : documents) {
                std::uint64_t words = 0, lines = text.empty() ? 0 : 1;
                bool word = false;
                for (char raw : text) {
                    const auto c = static_cast<unsigned char>(raw);
                    if (c == '\n')
                        ++lines;
                    const bool next = !whitespace(c);
                    if (next && !word)
                        ++words;
                    word = next;
                }
                rows.push(Json::object()
                              .set("uri", uri)
                              .set("utf8Bytes", text.size())
                              .set("lines", lines)
                              .set("whitespaceSeparatedWords", words));
            }
            reply(*id,
                  Json::object().set("source", "received open-buffer text only").set("documents", std::move(rows)));
        } else if (id)
            refuse(*id);
    }
    return shutdown ? 0 : 2;
}
