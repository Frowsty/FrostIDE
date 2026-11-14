// SPDX-License-Identifier: Apache-2.0
// FEC-13 independent raw pinned-clangd oracle; no Frost language-service or mapping code participates.
#pragma once

#include "frost/io/path.hpp"
#include "frost/io/process.hpp"
#include "frost/rpc/framing.hpp"
#include "frost/rpc/json.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace frost::test {

inline std::string bindingFixtureUri(std::string_view path) {
    constexpr std::string_view hex = "0123456789ABCDEF";
    std::string uri = path.starts_with('/') ? "file://" : "file:///";
    for (const char value : path) {
        const auto ch = static_cast<unsigned char>(value);
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '/' ||
            ch == ':' || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            uri += static_cast<char>(ch);
        } else {
            uri += '%';
            uri += hex[ch >> 4];
            uri += hex[ch & 15];
        }
    }
    return uri;
}

// Source is the ASCII-only fixture. Each offset is a native declaration's actual identifier token,
// independently selected from the fixture, never a location or USR supplied by the mapping tool.
inline std::optional<std::vector<std::string>> rawBindingUsrs(std::string_view compiler, const std::string& workspace,
                                                              const std::string& file, const std::string& source,
                                                              const std::vector<std::size_t>& offsets) {
#ifdef _WIN32
    constexpr std::string_view executable = "clangd.exe";
#else
    constexpr std::string_view executable = "clangd";
#endif
    const auto normalized = io::normalizePath(compiler);
    const auto path = normalized ? io::joinPath(io::parentPath(*normalized), executable) : normalized;
    if (!path) {
        ADD_FAILURE() << "cannot locate pinned clangd beside pinned compiler";
        return std::nullopt;
    }
    io::ProcessSpec spec;
    spec.executable = *path;
    spec.workingDirectory = workspace;
    spec.arguments = {"--background-index=false",
                      "--enable-config=false",
                      "--clang-tidy=false",
                      "--log=error",
                      "--pch-storage=memory",
                      "-j=1",
                      "--compile-commands-dir=" + workspace};
    spec.stdinMode = io::Stdio::Pipe;
    spec.stderrMode = io::Stdio::Null;
    auto process = io::realProcessLauncher().launch(spec);
    if (!process) {
        ADD_FAILURE() << "pinned raw-clangd launch failed: " << process.error().describe();
        return std::nullopt;
    }
    struct Guard {
        io::IProcess& process;
        bool exited = false;
        ~Guard() {
            if (!exited) {
                process.closeStdin();
                (void)process.terminate();
                (void)process.wait(std::chrono::seconds(5));
            }
        }
    } guard{**process};
    // Bound blocking stdout reads as well as final shutdown; no orphaned oracle on an assertion.
    std::jthread deadline([&](std::stop_token stop) {
        std::mutex mutex;
        std::condition_variable_any wake;
        std::unique_lock lock(mutex);
        (void)wake.wait_for(lock, stop, std::chrono::seconds(60), [] { return false; });
        if (!stop.stop_requested())
            (void)(*process)->terminate();
    });
    rpc::FrameDecoder decoder;
    const auto send = [&](const rpc::Json& message) {
        auto written = (*process)->writeStdin(rpc::encodeFrame(message.dump()));
        if (!written)
            ADD_FAILURE() << written.error().describe();
        return written.has_value();
    };
    const auto response = [&](std::int64_t id) -> std::optional<rpc::Json> {
        std::array<char, 4096> bytes{};
        for (;;) {
            while (auto frame = decoder.next()) {
                if (!*frame) {
                    ADD_FAILURE() << frame->error().describe();
                    return std::nullopt;
                }
                auto parsed = rpc::parseJson(**frame);
                if (!parsed) {
                    ADD_FAILURE() << parsed.error().describe();
                    return std::nullopt;
                }
                if (parsed->integerAt("id") == id) {
                    if (parsed->find("error")) {
                        ADD_FAILURE() << "raw-clangd error: " << parsed->dump();
                        return std::nullopt;
                    }
                    return std::move(*parsed);
                }
            }
            auto read = (*process)->readStdout(bytes);
            if (!read || *read == 0) {
                ADD_FAILURE() << "raw-clangd closed or timed out waiting for " << id;
                return std::nullopt;
            }
            decoder.feed(std::string_view(bytes.data(), *read));
        }
    };
    using rpc::Json;
    const auto request = [&](std::int64_t id, std::string_view method, Json params) {
        return Json::object()
            .set("jsonrpc", "2.0")
            .set("id", id)
            .set("method", method)
            .set("params", std::move(params));
    };
    if (!send(request(1, "initialize",
                      Json::object()
                          .set("processId", Json())
                          .set("rootUri", bindingFixtureUri(workspace))
                          .set("capabilities", Json::object()))))
        return std::nullopt;
    const auto initialized = response(1);
    const Json* initialization = initialized ? initialized->find("result") : nullptr;
    const Json* server = initialization ? initialization->find("serverInfo") : nullptr;
    constexpr std::string_view pinnedVersion = "clangd version 23.1.4";
    const auto version = server ? server->stringAt("version") : std::string_view{};
    if (!server || server->stringAt("name") != "clangd" || !version.starts_with(pinnedVersion) ||
        (version.size() > pinnedVersion.size() && version[pinnedVersion.size()] != ' ' &&
         version[pinnedVersion.size()] != '\n')) {
        ADD_FAILURE() << "raw-clangd is not the pinned LLVM 23.1.4 oracle: "
                      << (initialized ? initialized->dump() : "no initialization");
        return std::nullopt;
    }
    const auto notification = [&](std::string_view method, Json params) {
        return Json::object().set("jsonrpc", "2.0").set("method", method).set("params", std::move(params));
    };
    const std::string uri = bindingFixtureUri(file);
    if (!send(notification("initialized", Json::object())) ||
        !send(notification(
            "textDocument/didOpen",
            Json::object().set(
                "textDocument",
                Json::object().set("uri", uri).set("languageId", "cpp").set("version", 1).set("text", source)))))
        return std::nullopt;
    std::vector<std::string> usrs;
    std::int64_t id = 2;
    for (const auto offset : offsets) {
        if (offset >= source.size()) {
            ADD_FAILURE() << "invalid independent oracle token offset";
            return std::nullopt;
        }
        std::uint32_t line = 0;
        std::uint32_t character = 0;
        for (std::size_t i = 0; i < offset; ++i) {
            if (source[i] == '\n') {
                ++line;
                character = 0;
            } else {
                ++character;
            }
        }
        if (!send(request(id, "textDocument/symbolInfo",
                          Json::object()
                              .set("textDocument", Json::object().set("uri", uri))
                              .set("position", Json::object().set("line", line).set("character", character)))))
            return std::nullopt;
        auto answer = response(id++);
        const Json* symbols = answer ? answer->find("result") : nullptr;
        if (!symbols || !symbols->isArray() || symbols->size() != 1 ||
            symbols->items().front().stringAt("usr").empty()) {
            ADD_FAILURE() << "pinned raw-clangd did not identify the exact native declaration: "
                          << (answer ? answer->dump() : "no response");
            return std::nullopt;
        }
        usrs.emplace_back(symbols->items().front().stringAt("usr"));
    }
    if (!send(request(id, "shutdown", Json())) || !response(id) || !send(notification("exit", Json())))
        return std::nullopt;
    (*process)->closeStdin();
    const auto exited = (*process)->wait(std::chrono::seconds(5));
    if (!exited || exited->exitCode != 0) {
        ADD_FAILURE() << "raw-clangd did not shut down cleanly";
        return std::nullopt;
    }
    guard.exited = true;
    return usrs;
}

} // namespace frost::test
