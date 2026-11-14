// SPDX-License-Identifier: Apache-2.0
// Contained real FrostRuntime fixture for its private, bounded initial Lua-debug startup wait.
#pragma once
#include "support/real_engine_session.hpp"

#include <atomic>
#include <random>

namespace frost::test {
class RealLuaPlayer {
public:
    ~RealLuaPlayer() { static_cast<void>(stop()); }
    [[nodiscard]] Result<engine::EngineEndpoint> start(const RealEngineBuild& build, const std::string& project) {
#ifdef _WIN32
        constexpr std::string_view filename = "FrostRuntime.exe";
        if (!m_desktop.valid())
            return fail(ErrorCode::IoError, "private desktop unavailable for real player fixture");
#else
        constexpr std::string_view filename = "FrostRuntime";
#endif
        auto program = io::joinPath(io::parentPath(build.program), filename);
        if (!program)
            return std::unexpected(program.error());
        image = *program;
        if (!io::realFileSystem().exists(image).value_or(false))
            return fail(ErrorCode::NotFound, "mandatory actual FrostRuntime executable is missing");
        constexpr char digits[] = "0123456789abcdef";
        std::random_device random;
        for (unsigned i = 0; i < 64; ++i)
            m_secret += digits[random() & 15U];
        io::ProcessSpec spec;
        spec.executable = image;
        spec.arguments = {"--project", project, "--headless", "--automation-port", "0"};
        spec.workingDirectory = project;
        spec.environment = {{"FROST_AUTOMATION_TOKEN", m_secret}, {"FROST_SCRIPT_DEBUG_WAIT", "1"}};
#ifdef _WIN32
        spec.desktop = m_desktop.name();
#endif
        auto launched = io::realProcessLauncher().launch(spec);
        if (!launched)
            return std::unexpected(launched.error());
        m_process = std::move(*launched);
        m_stdout = std::thread([this] { drain(true); });
        m_stderr = std::thread([this] { drain(false); });
        std::unique_lock lock(m_mutex);
        if (!m_changed.wait_for(lock, 25s, [&] { return m_startup || m_ended || m_corrupt; }) || !m_startup)
            return fail(ErrorCode::Timeout, "actual player did not publish private-wait startup within 25 seconds");
        if (m_corrupt || m_secretLeaked.load())
            return fail(ErrorCode::Corrupt, "actual player startup violated bounded private-output contract");
        const auto port = m_startup->unsignedAt("port").value_or(0);
        engine::EngineEndpoint result;
        result.port = static_cast<std::uint16_t>(port);
        result.projectPath = std::string(m_startup->stringAt("projectPath"));
        result.instanceId = std::string(m_startup->stringAt("instanceId"));
        result.generation = std::string(m_startup->stringAt("generation"));
        result.secret = m_secret;
        if (port == 0 || port > 65535 || result.projectPath.empty() || result.instanceId.empty() ||
            result.generation.empty())
            return fail(ErrorCode::Corrupt, "actual player startup omitted its authenticated identity");
        return result;
    }

    [[nodiscard]] Result<void> stop() {
        if (!m_process)
            return {};
        auto terminated = m_process->terminate();
        auto reaped = m_process->wait(10s);
        m_process->abandonOutput();
        if (m_stdout.joinable())
            m_stdout.join();
        if (m_stderr.joinable())
            m_stderr.join();
        m_process.reset();
        if (!terminated)
            return std::unexpected(terminated.error());
        if (!reaped)
            return std::unexpected(reaped.error());
        if (m_secretLeaked.load())
            return fail(ErrorCode::Corrupt, "actual player printed its transient environment credential");
        return {};
    }
    std::string image;

private:
    void drain(bool stdoutPipe) {
        std::array<char, 4096> buffer{};
        std::string line;
        for (;;) {
            auto count = stdoutPipe ? m_process->readStdout(buffer) : m_process->readStderr(buffer);
            if (!count || *count == 0)
                break;
            for (std::size_t i = 0; i < *count; ++i) {
                if (line.size() >= 1024 * 1024) {
                    std::lock_guard lock(m_mutex);
                    m_corrupt = true;
                    m_changed.notify_all();
                    line.erase(0, line.size() - m_secret.size());
                }
                line.push_back(buffer[i]);
                if (line.size() >= m_secret.size() && line.ends_with(m_secret))
                    m_secretLeaked.store(true);
                if (buffer[i] != '\n')
                    continue;
                auto parsed = stdoutPipe ? rpc::parseJson(line) : Result<rpc::Json>(rpc::Json{});
                line.clear();
                const auto* record = parsed ? parsed->find("result") : nullptr;
                if (record && record->isObject() && record->unsignedAt("port")) {
                    std::lock_guard lock(m_mutex);
                    if (!m_startup)
                        m_startup = *record;
                    m_changed.notify_all();
                }
            }
        }
        if (stdoutPipe) {
            std::lock_guard lock(m_mutex);
            m_ended = true;
            m_changed.notify_all();
        }
    }
#ifdef _WIN32
    PrivateDesktop m_desktop;
#endif
    std::unique_ptr<io::IProcess> m_process;
    std::thread m_stdout, m_stderr;
    std::string m_secret;
    std::atomic<bool> m_secretLeaked = false;
    std::mutex m_mutex;
    std::condition_variable m_changed;
    std::optional<rpc::Json> m_startup;
    bool m_ended = false, m_corrupt = false;
};
} // namespace frost::test
