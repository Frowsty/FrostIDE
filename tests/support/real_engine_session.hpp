// SPDX-License-Identifier: Apache-2.0
// Mandatory R6 corpus fixture: pinned build, isolated projects, contained offscreen real editors.
#pragma once

#include "frost/build/cmake_toolchain.hpp"
#include "frost/build/msbuild_locator.hpp"
#include "frost/engine/engine_cli.hpp"
#include "frost/engine/session.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/io/process.hpp"

#include "support/crash_dialogs.hpp"
#include "support/engine_lock.hpp"
#include "support/private_desktop.hpp"
#include "support/scratch_dir.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#ifndef FROST_CORPUS_FROSTENGINE_SHA
#error "R6 acceptance targets must define the locked engine SHA, platform preset and pinned CMake path"
#endif

namespace frost::test {
using namespace std::chrono_literals;

inline std::string engineCorporaRoot() {
#ifdef _WIN32
    char* value = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&value, &length, "FROST_CORPORA_ROOT") != 0 || value == nullptr)
        return {};
    std::string result(value);
    std::free(value);
    return result;
#else
    const char* value = std::getenv("FROST_CORPORA_ROOT");
    return value ? value : "";
#endif
}

// The lock covers all editor and CLI instances, because a build can replace their shared DLLs.
// Missing setup is an error, never a GTEST_SKIP or an environment-dependent success.
class RealEngineBuild {
public:
    [[nodiscard]] Result<void> prepare(std::vector<std::string> targets = {"FrostEditor"}) {
        disableCrashDialogs();
        const std::string root = engineCorporaRoot();
        if (root.empty())
            return fail(ErrorCode::NotFound, "FROST_CORPORA_ROOT missing: run the mandatory corpus tier");
        const std::string sha = FROST_CORPUS_FROSTENGINE_SHA;
        auto path = io::normalizePath(root + "/frostengine-" + sha.substr(0, 12));
        if (!path)
            return std::unexpected(path.error());
        corpus = *path;
        const auto head = io::realFileSystem().readFile(corpus + "/.git/HEAD");
        if (!head || head->substr(0, sha.size()) != sha)
            return fail(ErrorCode::Corrupt,
                        "R6 fixture requires the detached pinned corpus; run corpora.py fetch/verify");
        m_lock = std::make_unique<EngineBuildLock>(root + "/frostengine-" + sha.substr(0, 12) + ".build.lock");
        if (!m_lock->held())
            return fail(ErrorCode::Timeout, "R6 fixture could not acquire the engine corpus build lock");
        io::ProcessSpec buildSpec;
        buildSpec.executable = io::normalizePath(FROST_TEST_CMAKE).value_or("");
        buildSpec.arguments = {"--build", corpus + "/build/" + FROST_CORPUS_FROSTENGINE_PRESET, "--target"};
        buildSpec.arguments.insert(buildSpec.arguments.end(), targets.begin(), targets.end());
        buildSpec.workingDirectory = corpus;
#ifdef _WIN32
        auto instances = build::listVsInstances(io::realProcessLauncher(), io::realFileSystem());
        if (!instances)
            return std::unexpected(instances.error());
        bool found = false;
        for (const auto& instance : *instances) {
            if (!io::realFileSystem()
                     .exists(instance.installationPath + "/VC/Tools/MSVC/" + FROST_PINNED_VC_TOOLS + "/include")
                     .value_or(false))
                continue;
            auto environment =
                build::captureMsvcEnvironment(io::realProcessLauncher(), io::realFileSystem(), instance, "x64");
            if (!environment)
                return std::unexpected(environment.error());
            const auto version = std::ranges::find_if(environment->changes, [](const auto& entry) {
                return _stricmp(entry.first.c_str(), "VCToolsVersion") == 0;
            });
            if (version == environment->changes.end() || !version->second ||
                !version->second->starts_with(FROST_PINNED_VC_TOOLS))
                return fail(ErrorCode::NotFound, "R6 build environment does not select the pinned MSVC toolset");
            buildSpec.environment = std::move(environment->changes);
            found = true;
            break;
        }
        if (!found)
            return fail(ErrorCode::NotFound, "R6 fixture cannot find the pinned MSVC toolset");
#endif
        auto result = io::runToCompletion(io::realProcessLauncher(), buildSpec, {}, 3h);
        if (!result)
            return std::unexpected(result.error());
        if (result->exitCode != 0)
            return fail(ErrorCode::IoError, "mandatory FrostEditor build failed: " + result->stderrBytes +
                                                result->stdoutBytes.substr(result->stdoutBytes.size() > 8000
                                                                               ? result->stdoutBytes.size() - 8000
                                                                               : 0));
        auto binary = engine::findEngineEditorProgram(io::realFileSystem(), corpus, FROST_CORPUS_FROSTENGINE_PRESET);
        if (!binary)
            return std::unexpected(binary.error());
        program = *binary;
        return {};
    }

    [[nodiscard]] Result<rpc::Json> cli(std::string command, const std::string& project,
                                        rpc::Json params = rpc::Json::object()) const {
        engine::EngineCliRequest request;
        request.command = std::move(command);
        request.project = project;
        request.params = std::move(params);
        request.workingDirectory = project;
        return engine::runEngineCliResult(io::realProcessLauncher(), program, request);
    }

    [[nodiscard]] Result<void> copyProject(const std::string& destination) const {
        const auto from = io::toStdPath(corpus + "/TestProject");
        const auto to = io::toStdPath(destination);
        if (!from || !to)
            return fail(ErrorCode::InvalidArgument, "invalid scratch project path");
        std::error_code error;
        std::filesystem::copy(*from, *to, std::filesystem::copy_options::recursive, error);
        if (error)
            return fail(ErrorCode::IoError, "copying pinned TestProject to scratch: " + error.message());
        return {};
    }

    std::string corpus, program;

private:
    std::unique_ptr<EngineBuildLock> m_lock;
};

// Retain a real scene, its camera and registered script asset reference, but remove its other
// scripts so their Application.Quit and test cases cannot end our liveness probe prematurely.
inline Result<void> installAcceptanceScript(const std::string& project, std::string_view source) {
    auto& fs = io::realFileSystem();
    const std::string scenePath = project + "/Assets/Tests/FeatureTest.fscene";
    auto bytes = fs.readFile(scenePath);
    if (!bytes)
        return std::unexpected(bytes.error());
    auto scene = rpc::parseJson(*bytes);
    if (!scene)
        return std::unexpected(scene.error());
    auto* entities = scene->find("Entities");
    if (!entities || !entities->isArray())
        return fail(ErrorCode::Corrupt, "real acceptance scene has no entity array");
    bool retained = false;
    rpc::Json replacement = rpc::Json::array();
    for (const auto& entity : entities->items()) {
        auto changed = entity;
        if (const auto* components = entity.find("Components")) {
            rpc::Json filtered = rpc::Json::object();
            for (std::size_t i = 0; i < components->size(); ++i) {
                const auto& key = components->keys()[i];
                const auto& component = components->items()[i];
                if (key == "Script") {
                    const auto* reference = component.find("Script");
                    if (retained || !reference || reference->stringAt("Path") != "Assets/Scripts/FeatureTest/Hero.lua")
                        continue;
                    retained = true;
                }
                filtered.set(key, component);
            }
            changed.set("Components", std::move(filtered));
        }
        replacement.push(std::move(changed));
    }
    if (!retained)
        return fail(ErrorCode::Corrupt, "pinned scene no longer references the acceptance script asset");
    scene->set("Entities", std::move(replacement));
    auto written = fs.writeFileAtomic(scenePath, scene->dump());
    if (!written)
        return written;
    return fs.writeFileAtomic(project + "/Assets/Scripts/FeatureTest/Hero.lua", source);
}

// Read the editor's startup record, then keep draining stdout. stderr is inherited so a failed
// start leaves real diagnostics without risking a full pipe. No protocol state is inferred here.
class RealEditor {
public:
    ~RealEditor() { stop(); }
    [[nodiscard]] Result<engine::EngineEndpoint> start(const RealEngineBuild& build, const std::string& project,
                                                       const std::string& config) {
        io::ProcessSpec spec;
        spec.executable = build.program;
        spec.arguments = {"--project",         project, "--offscreen",  "--size", "320x240",
                          "--automation-port", "0",     "--config-dir", config};
        spec.workingDirectory = project;
        spec.stderrMode = io::Stdio::Inherit;
        spec.environment.emplace_back("FROST_AUTOMATION_TOKEN", std::nullopt);
#ifdef _WIN32
        if (!m_desktop.valid())
            return fail(ErrorCode::IoError, "private desktop unavailable; refusing a visible engine fixture");
        spec.desktop = m_desktop.name();
#endif
        auto process = io::realProcessLauncher().launch(spec);
        if (!process)
            return std::unexpected(process.error());
        m_process = std::move(*process);
        m_reader = std::thread([this] {
            std::array<char, 4096> buffer{};
            std::string line;
            for (;;) {
                auto count = m_process->readStdout(buffer);
                if (!count || *count == 0)
                    break;
                for (std::size_t i = 0; i < *count; ++i) {
                    if (buffer[i] != '\n') {
                        if (line.size() < 1024 * 1024)
                            line.push_back(buffer[i]);
                        continue;
                    }
                    auto parsed = rpc::parseJson(line);
                    line.clear();
                    const auto* payload = parsed ? parsed->find("result") : nullptr;
                    if (parsed && (!payload || !payload->isObject()))
                        payload = &*parsed;
                    if (payload && payload->unsignedAt("port")) {
                        std::lock_guard lock(m_mutex);
                        if (!m_startup)
                            m_startup = *payload;
                        m_changed.notify_all();
                    }
                }
            }
            std::lock_guard lock(m_mutex);
            m_ended = true;
            m_changed.notify_all();
        });
        std::unique_lock lock(m_mutex);
        if (!m_changed.wait_for(lock, 120s, [&] { return m_startup || m_ended; }) || !m_startup)
            return fail(ErrorCode::Timeout,
                        "real FrostEditor produced no automation startup record within 120 seconds");
        engine::EngineEndpoint endpoint;
        endpoint.port = static_cast<std::uint16_t>(m_startup->unsignedAt("port").value_or(0));
        endpoint.projectPath = std::string(m_startup->stringAt("projectPath"));
        endpoint.instanceId = std::string(m_startup->stringAt("instanceId"));
        endpoint.generation = std::string(m_startup->stringAt("generation"));
        endpoint.secret = std::string(m_startup->stringAt("token"));
        if (endpoint.port == 0 || endpoint.projectPath.empty() || endpoint.instanceId.empty() ||
            endpoint.generation.empty() || endpoint.secret.empty())
            return fail(ErrorCode::Corrupt, "real FrostEditor startup record lacks mandatory v2 identity/credentials");
        return endpoint;
    }
    [[nodiscard]] Result<void> kill() {
        if (!m_process)
            return fail(ErrorCode::InvalidArgument, "editor not started");
        return m_process->terminate();
    }
    void stop() noexcept {
        if (m_process) {
            static_cast<void>(m_process->terminate());
            static_cast<void>(m_process->wait(10s));
            m_process->abandonOutput();
        }
        if (m_reader.joinable())
            m_reader.join();
        m_process.reset();
    }

private:
#ifdef _WIN32
    PrivateDesktop m_desktop;
#endif
    std::unique_ptr<io::IProcess> m_process;
    std::thread m_reader;
    std::mutex m_mutex;
    std::condition_variable m_changed;
    std::optional<rpc::Json> m_startup;
    bool m_ended = false;
};
} // namespace frost::test
