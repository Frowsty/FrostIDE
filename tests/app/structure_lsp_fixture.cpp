// SPDX-License-Identifier: Apache-2.0
// Controllable stdio LSP fixture for Structure's application lifecycle tests.

#include "frost/lang/json.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace {
using frost::lang::Json;
void send(const Json& message) {
    const std::string text = message.dump();
    std::cout << "Content-Length: " << text.size() << "\r\n\r\n" << text << std::flush;
}
Json position(int character) { return Json::object().set("line", 0).set("character", character); }
Json range(int start, int end) { return Json::object().set("start", position(start)).set("end", position(end)); }
} // namespace

int main() {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    const bool utf8 = std::filesystem::exists("structure-utf8");
    for (;;) {
        std::string line;
        std::size_t length = 0;
        while (std::getline(std::cin, line)) {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (line.empty())
                break;
            if (line.starts_with("Content-Length: "))
                length = static_cast<std::size_t>(std::stoul(line.substr(16)));
        }
        if (!std::cin || length == 0)
            return 0;
        std::string bytes(length, '\0');
        std::cin.read(bytes.data(), static_cast<std::streamsize>(length));
        auto message = frost::lang::parseJson(bytes);
        if (!message)
            return 2;
        const auto* id = message->find("id");
        const auto method = message->stringAt("method");
        if (std::filesystem::exists("structure-log")) {
            std::ofstream log("structure-notifications", std::ios::app);
            log << (id ? "request " : "notification ") << method << '\n';
        }
        if (!id)
            continue;
        Json result;
        if (method == "initialize") {
            result = Json::object().set("capabilities", Json::object()
                                                            .set("positionEncoding", utf8 ? "utf-8" : "utf-16")
                                                            .set("textDocumentSync", 2)
                                                            .set("documentSymbolProvider", true));
        } else if (method == "textDocument/documentSymbol") {
            std::string name;
            {
                std::ifstream nameFile("structure-name");
                std::getline(nameFile, name);
            }
            std::ofstream("structure-requested") << "requested";
            while (std::filesystem::exists("structure-hold"))
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            const int start = utf8 ? 8 : 6;
            result = Json::array({Json::object()
                                      .set("name", name)
                                      .set("kind", 12)
                                      .set("range", range(start, start + 6))
                                      .set("selectionRange", range(start, start + 6))});
        }
        send(Json::object().set("jsonrpc", "2.0").set("id", *id).set("result", std::move(result)));
    }
}
