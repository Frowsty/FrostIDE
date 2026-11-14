// SPDX-License-Identifier: Apache-2.0
// Real System32 DbgEng peer and pinned MSVC fixture helper. Used only by native acceptance tests.
#pragma once
#include "frost/build/cmake_toolchain.hpp"
#include "frost/build/msbuild_locator.hpp"
#include "frost/debug/dap_types.hpp"
#include "frost/debug/dbgeng_dap.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"

#include "support/scratch_dir.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <format>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>
namespace frost::debug::native_test {
using Json = rpc::Json;
using namespace std::chrono_literals;
inline bool sameText(std::string_view a, std::string_view b) {
    return std::ranges::equal(a, b, [](char x, char y) {
        return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
    });
}
inline bool contains(std::string_view text, std::string_view part) { return text.find(part) != text.npos; }
inline Result<project::CMakeEnvironment> pinnedMsvcEnvironment() {
    auto& fs = io::realFileSystem();
    auto& launcher = io::realProcessLauncher();
    auto instances = build::listVsInstances(launcher, fs);
    if (!instances) {
        return std::unexpected(std::move(instances.error()));
    }
    const std::string pinned = FROST_PINNED_VC_TOOLS;
    for (const build::VsInstance& instance : *instances) {
        if (!fs.exists(instance.installationPath + "\\VC\\Tools\\MSVC\\" + pinned + "\\include").value_or(false)) {
            continue;
        }
        auto captured = build::captureMsvcEnvironment(launcher, fs, instance, "x64");
        if (!captured) {
            return std::unexpected(std::move(captured.error()));
        }
        const auto version = std::ranges::find_if(
            captured->changes, [](const auto& entry) { return sameText(entry.first, "VCToolsVersion"); });
        if (version == captured->changes.end() || !version->second || !contains(*version->second, pinned)) {
            return fail(ErrorCode::NotFound,
                        std::format("{}'s default MSVC toolset is not the pinned {}", instance.displayName, pinned));
        }
        return std::move(captured->changes);
    }
    return fail(ErrorCode::NotFound, std::format("no Visual Studio instance has the pinned MSVC toolset {}", pinned));
}

// cl.exe on the captured PATH.
inline Result<std::string> findCl(const project::CMakeEnvironment& environment) {
    auto& fs = io::realFileSystem();
    for (const auto& [name, value] : environment) {
        if (!sameText(name, "PATH") || !value) {
            continue;
        }
        std::size_t start = 0;
        while (start <= value->size()) {
            const std::size_t end = std::min(value->find(';', start), value->size());
            const std::string directory = value->substr(start, end - start);
            if (!directory.empty()) {
                if (auto cl = io::joinPath(directory, "cl.exe"); cl && fs.exists(*cl).value_or(false)) {
                    return io::normalizePath(*cl);
                }
            }
            start = end + 1;
        }
    }
    return fail(ErrorCode::NotFound, "cl.exe is not on the vcvarsall PATH");
}

inline Result<std::string> buildNativeFixture(std::string source, frost::test::ScratchDir& scratch) {
    auto environment = pinnedMsvcEnvironment();
    if (!environment)
        return std::unexpected(std::move(environment.error()));
    auto compiler = findCl(*environment);
    if (!compiler)
        return std::unexpected(std::move(compiler.error()));
    io::ProcessSpec spec;
    spec.executable = *compiler;
    spec.workingDirectory = scratch.path();
    spec.environment = *environment;
    spec.arguments = {"/nologo",
                      "/EHsc",
                      "/Od",
                      "/Zi",
                      "/MDd",
                      "/std:c++20",
                      std::move(source),
                      "/Fefixture.exe",
                      "/Fdfixture_compile.pdb",
                      "/Fofixture.obj",
                      "/link",
                      "/DEBUG",
                      "/INCREMENTAL:NO"};
    auto result = io::runToCompletion(io::realProcessLauncher(), spec, {}, 10min);
    if (!result)
        return std::unexpected(std::move(result.error()));
    if (result->exitCode != 0)
        return fail(ErrorCode::IoError, "Pinned MSVC fixture failed: " + result->stdoutBytes + result->stderrBytes);
    return scratch.join("fixture.exe");
}
class Peer {
public:
    ~Peer() {
        if (client) {
            client->close();
            client.reset();
        }
    }
    Result<Json> start(std::string adapter) {
        auto opened = dbgEngDapAdapterFactory(io::realProcessLauncher(), std::move(adapter))(
            {.onEvent = [this](std::string_view name, const Json& body) {
                std::scoped_lock lock(mutex);
                if (name == "output") {
                    if (output.size() < 1024 * 1024)
                        output += body.stringAt("output");
                } else if (events.size() < 4096)
                    events.emplace_back(std::string(name), body);
            }});
        if (!opened)
            return std::unexpected(std::move(opened.error()));
        client = std::move(*opened);
        return request("initialize", dap::initializeArguments(kDbgEngAdapterId));
    }
    Result<Json> request(std::string command, Json arguments = Json::object()) {
        auto promise = std::make_shared<std::promise<Result<Json>>>();
        auto future = promise->get_future();
        client->request(std::move(command), std::move(arguments),
                        [promise](Result<Json> result) { promise->set_value(std::move(result)); });
        if (future.wait_for(60s) != std::future_status::ready)
            return fail(ErrorCode::Timeout, "Native DAP request timed out");
        return future.get();
    }
    std::size_t count(std::string_view name) {
        std::scoped_lock lock(mutex);
        return std::ranges::count_if(events, [&](const auto& event) { return event.first == name; });
    }
    std::optional<Json> last(std::string_view name) {
        std::scoped_lock lock(mutex);
        for (auto it = events.rbegin(); it != events.rend(); ++it)
            if (it->first == name)
                return it->second;
        return std::nullopt;
    }
    std::string text() {
        std::scoped_lock lock(mutex);
        return output;
    }

private:
    std::unique_ptr<IDebugAdapterClient> client;
    std::mutex mutex;
    std::vector<std::pair<std::string, Json>> events;
    std::string output;
};
} // namespace frost::debug::native_test
