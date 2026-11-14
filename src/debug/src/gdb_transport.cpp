// SPDX-License-Identifier: Apache-2.0
#include "gdb_transport.hpp"
#include "frost/core/mutex.hpp"
#include <chrono>
#include <condition_variable>
#include <stop_token>

namespace frost::debug {
namespace {
class GdbTransport final : public IDapTransport {
public:
    GdbTransport(io::IProcessLauncher& launcher, std::string executable, DapProcessOutput output)
        : m_launcher(launcher), m_executable(std::move(executable)), m_output(std::move(output)) {}
    ~GdbTransport() override { close(); }

    Result<std::size_t> read(std::span<char> buffer) override {
        if (!m_prepared) {
            m_prepared = true; // Reader-thread owned; factory never starts a process.
            auto prepared = prepare();
            {
                LockGuard lock(m_mutex);
                if (!prepared) m_error = std::move(prepared.error());
                else if (!m_closed) m_transport = std::move(*prepared);
                m_ready = true;
            }
            // A close racing preparation destroys the just-created adapter before any write.
            m_changed.notify_all();
        }
        auto stream = connection();
        if (!stream) return std::unexpected(std::move(stream.error()));
        return (*stream)->read(buffer);
    }
    Result<void> write(std::string_view bytes) override {
        auto stream = connection();
        if (!stream) return std::unexpected(std::move(stream.error()));
        return (*stream)->write(bytes);
    }
    void close() noexcept override {
        m_stop.request_stop(); // Interrupts runToCompletion and kills/reaps its contained probe.
        std::shared_ptr<IDapTransport> stream;
        {
            LockGuard lock(m_mutex);
            m_closed = true;
            stream = m_transport;
        }
        m_changed.notify_all();
        if (stream) stream->close();
    }
    std::optional<int> exitCode() noexcept override {
        std::shared_ptr<IDapTransport> stream;
        { LockGuard lock(m_mutex); stream = m_transport; }
        return stream ? stream->exitCode() : std::nullopt;
    }
private:
    Result<std::shared_ptr<IDapTransport>> prepare() {
        if (m_stop.stop_requested()) return fail(ErrorCode::Cancelled, "GDB preparation cancelled");
        auto probe = gdbDapProcessSpec(m_executable);
        probe.arguments = {"--nx", "--quiet", "--version"};
        auto version = io::runToCompletion(m_launcher, probe, {}, std::chrono::seconds(5), m_stop.get_token());
        if (!version) return std::unexpected(std::move(version.error()).withContext("while checking GDB version"));
        if (version->exitCode != 0 || !validateGdbDapVersion(version->stdoutBytes))
            return fail(ErrorCode::Unsupported, "GDB DAP requires GNU GDB >=16 with Python support");
        if (m_stop.stop_requested()) return fail(ErrorCode::Cancelled, "GDB preparation cancelled");
        auto process = m_launcher.launch(gdbDapProcessSpec(m_executable));
        if (!process) return std::unexpected(std::move(process.error()).withContext("while starting GDB DAP"));
        return std::shared_ptr<IDapTransport>(processTransport(std::move(*process), std::move(m_output)));
    }
    Result<std::shared_ptr<IDapTransport>> connection() {
        UniqueLock lock(m_mutex);
        // The predicate runs only while condition_variable owns this same mutex.
        m_changed.wait(lock.native(), [&]() FROST_NO_THREAD_SAFETY_ANALYSIS { return m_ready || m_closed; });
        if (m_closed) return fail(ErrorCode::Cancelled, "GDB transport closed");
        if (m_error) return std::unexpected(*m_error);
        return m_transport;
    }
    io::IProcessLauncher& m_launcher;
    const std::string m_executable;
    DapProcessOutput m_output;
    bool m_prepared = false;
    std::stop_source m_stop;
    Mutex m_mutex;
    std::condition_variable m_changed;
    bool m_ready FROST_GUARDED_BY(m_mutex) = false;
    bool m_closed FROST_GUARDED_BY(m_mutex) = false;
    std::optional<Error> m_error FROST_GUARDED_BY(m_mutex);
    std::shared_ptr<IDapTransport> m_transport FROST_GUARDED_BY(m_mutex);
};
}
std::unique_ptr<IDapTransport> gdbDapTransport(io::IProcessLauncher& launcher, std::string executable,
                                            DapProcessOutput output) {
    return std::make_unique<GdbTransport>(launcher, std::move(executable), std::move(output));
}
} // namespace frost::debug
