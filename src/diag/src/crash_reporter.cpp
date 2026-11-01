// SPDX-License-Identifier: Apache-2.0
//
// The platform-independent parts of the crash reporter (frost/diag/crash_reporter.hpp): the
// handler's command line and environment, the crashpad annotations, the reporter object and
// the in-memory test double. crashpad (Apache-2.0) supplies the annotations.

#include "frost/diag/crash_reporter.hpp"

#include "frost/core/log.hpp"
#include "frost/io/path.hpp"

#include "crash_internal.hpp"
#include "internal.hpp"
#include <client/annotation.h>
#include <client/annotation_list.h>
#include <client/crashpad_info.h>
#include <cpptrace/basic.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <format>
#include <thread>

namespace frost::diag {

std::string_view toString(FatalReason reason) noexcept {
    switch (reason) {
    case FatalReason::ContractViolation: return "contract-violation";
    case FatalReason::Terminate: return "terminate";
    case FatalReason::Abort: return "abort";
    case FatalReason::InvalidParameter: return "invalid-parameter";
    case FatalReason::PureCall: return "purecall";
    case FatalReason::QtFatal: return "qt-fatal";
    case FatalReason::SelfTest: return "selftest";
    }
    return "unknown";
}

std::vector<std::string> crashpadHandlerArguments(const CrashReporterConfig& config, std::string_view pipeName,
                                                  bool windowsRules) {
    std::vector<std::string> arguments;
    if (windowsRules) {
        const std::string database = io::extendedLengthPath(config.databasePath).value_or(config.databasePath);
        arguments.push_back("--database=" + database);
        arguments.push_back(std::format("--pipe-name={}", pipeName));
    } else {
        arguments.push_back("--database=" + config.databasePath);
        arguments.push_back("--initial-client-fd=3");
        // SetHandlerSocket's client speaks the shared-socket protocol (it signals the request and
        // waits for SIGCONT instead of reading a reply), so the handler must expect it; crashpad's
        // own CrashpadClient::StartHandler pairs the two options the same way.
        arguments.emplace_back("--shared-client-connection");
    }
    arguments.emplace_back("--no-periodic-tasks");
    arguments.emplace_back("--annotation=prod=FrostIDE");
    arguments.push_back("--annotation=ver=" + config.version);
    arguments.push_back(windowsRules ? "--annotation=plat=win" : "--annotation=plat=linux");
    return arguments;
}

std::vector<std::pair<std::string, std::optional<std::string>>>
crashpadHandlerEnvironment(const std::function<std::optional<std::string>(std::string_view)>& parent,
                           bool windowsRules) {
    static constexpr std::string_view kWindows[] = {"SystemRoot", "SystemDrive", "windir", "TEMP", "TMP"};
    static constexpr std::string_view kPosix[] = {"TMPDIR"};
    static constexpr std::string_view kSanitizers[] = {"ASAN_OPTIONS", "UBSAN_OPTIONS", "LSAN_OPTIONS", "TSAN_OPTIONS"};
    std::vector<std::pair<std::string, std::optional<std::string>>> environment;
    const auto keep = [&](std::string_view name) {
        if (std::optional<std::string> value = parent(name)) {
            environment.emplace_back(std::string(name), std::move(value));
        }
    };
    if (windowsRules) {
        std::ranges::for_each(kWindows, keep);
    } else {
        std::ranges::for_each(kPosix, keep);
    }
    std::ranges::for_each(kSanitizers, keep);
    return environment;
}

namespace detail {
namespace {

// The client-side annotations (crashpad::StringAnnotation, which must never be destroyed:
// constinit and trivially destructible).
constinit crashpad::StringAnnotation<4096> g_imagePath("frost.image_path");
constinit crashpad::StringAnnotation<4096> g_restartedFrom("frost.restarted_from");
constinit crashpad::StringAnnotation<32> g_selfTest("frost.selftest");
constinit crashpad::StringAnnotation<64> g_reason("frost.fatal_reason");
constinit crashpad::StringAnnotation<1024> g_message("frost.message");
constinit crashpad::StringAnnotation<4096> g_frames("frost.frames");

constinit std::atomic<bool> g_registered{false};
constinit std::atomic<bool> g_framesSet{false};
constinit std::atomic<std::uint64_t> g_fatalOwner{0};

// Removes NULs, which StringAnnotation::Set refuses, by cutting the value at the first one.
std::string_view withoutNul(std::string_view value) noexcept { return value.substr(0, value.find('\0')); }

} // namespace

void registerAnnotations() noexcept {
    if (!g_registered.exchange(true, std::memory_order_acq_rel)) {
        crashpad::AnnotationList::Register();
    }
}

void setAnnotation(AnnotationKey key, std::string_view value) noexcept {
    if (!g_registered.load(std::memory_order_acquire)) {
        return;
    }
    value = withoutNul(value);
    switch (key) {
    case AnnotationKey::ImagePath: g_imagePath.Set(value); break;
    case AnnotationKey::RestartedFrom: g_restartedFrom.Set(value); break;
    case AnnotationKey::SelfTest: g_selfTest.Set(value); break;
    case AnnotationKey::Reason: g_reason.Set(value); break;
    case AnnotationKey::Message: g_message.Set(value); break;
    case AnnotationKey::Frames:
        g_frames.Set(value);
        g_framesSet.store(true, std::memory_order_release);
        break;
    }
}

bool framesAnnotated() noexcept { return g_framesSet.load(std::memory_order_acquire); }

std::string describeStack(std::size_t skip) {
    // Allocates (cpptrace resolves each frame's object); only for log records.
    std::string text;
    try {
        const cpptrace::object_trace trace = cpptrace::generate_object_trace(skip + 1, 32);
        for (const cpptrace::object_frame& frame : trace.frames) {
            const std::string_view path = frame.object_path;
            const std::size_t slash = path.find_last_of("/\\");
            text += std::format("\n    {}+0x{:x}", slash == std::string_view::npos ? path : path.substr(slash + 1),
                                frame.object_address);
        }
    } catch (...) {
        text += "\n    <stack unavailable>";
    }
    return text;
}

std::size_t formatFrames(const std::uintptr_t* frames, std::size_t count, char* buffer, std::size_t capacity) noexcept {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::size_t length = 0;
    for (std::size_t i = 0; i < count; ++i) {
        char digits[2 * sizeof(std::uintptr_t)];
        std::size_t n = 0;
        std::uintptr_t value = frames[i];
        do {
            digits[n++] = kDigits[value & 0xF];
            value >>= 4;
        } while (value != 0);
        const std::size_t needed = (i == 0 ? 0 : 1) + 2 + n;
        if (length + needed > capacity) {
            break;
        }
        if (i != 0) {
            buffer[length++] = ' ';
        }
        buffer[length++] = '0';
        buffer[length++] = 'x';
        while (n > 0) {
            buffer[length++] = digits[--n];
        }
    }
    return length;
}

void enterFatalSection() noexcept {
    const std::uint64_t self = currentThreadId();
    std::uint64_t expected = 0;
    if (g_fatalOwner.compare_exchange_strong(expected, self, std::memory_order_acq_rel)) {
        return;
    }
    if (expected == self) {
        writeToStderr("frost: fatal error while reporting a fatal error\n");
        endWithoutReport();
    }
    for (;;) {
        // Another thread is writing the report and will end the process.
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

} // namespace detail

namespace {

class CrashpadReporter final : public ICrashReporter {
public:
    CrashpadReporter(io::IProcessLauncher& launcher, const IClock& clock, ICrashpadConnector& connector) noexcept
        : m_launcher(launcher), m_clock(clock), m_connector(connector) {}

    Result<void> start(const CrashReporterConfig& config) override {
        const std::scoped_lock lock(m_mutex);
        if (m_active.load(std::memory_order_acquire)) {
            return fail(ErrorCode::InvalidArgument, "the crash reporter is already started");
        }
        if (m_connector.processElevated()) {
            log::warn("diag", "crash handler not started: frost runs elevated (dumps would hold administrator memory)");
            return fail(ErrorCode::Unsupported, "frost runs elevated; no crash handler is started");
        }
        if (!io::isAbsolutePath(config.handlerPath) || !io::isAbsolutePath(config.databasePath)) {
            return fail(ErrorCode::InvalidArgument, "the handler and database paths must be absolute");
        }
        detail::registerAnnotations();
        crashpad::CrashpadInfo::GetCrashpadInfo()->set_system_crash_reporter_forwarding(crashpad::TriState::kDisabled);
        const IClock::SteadyTime begin = m_clock.steadyNow();
        auto connection = detail::launchAndRegister(m_launcher, m_clock, m_connector, config);
        if (!connection) {
            log::error("diag", "crash handler not started: {}", connection.error().describe());
            return std::unexpected(std::move(connection.error()));
        }
        m_process = std::move(connection->process);
        m_status.active = true;
        m_status.handlerPid = m_process->pid();
        m_status.handlerPath = config.handlerPath;
        m_status.databasePath = config.databasePath;
        m_status.pipeName = std::move(connection->pipeName);
        m_status.startDuration = m_clock.steadyNow() - begin;
        m_active.store(true, std::memory_order_release);
        log::info("diag", "crash handler pid {} ready in {} us ({})", m_status.handlerPid,
                  std::chrono::duration_cast<std::chrono::microseconds>(m_status.startDuration).count(),
                  config.handlerPath);
        return {};
    }

    [[nodiscard]] bool active() const noexcept override { return m_active.load(std::memory_order_acquire); }

    [[nodiscard]] ReporterStatus status() const override {
        const std::scoped_lock lock(m_mutex);
        return m_status;
    }

    void annotate(AnnotationKey key, std::string_view value) noexcept override {
        detail::registerAnnotations();
        detail::setAnnotation(key, value);
    }

    Result<void> dumpWithoutCrash(std::string_view message) override {
        if (!active()) {
            return fail(ErrorCode::Unsupported, "the crash reporter is not started");
        }
        detail::setAnnotation(AnnotationKey::Message, message.substr(0, 1024));
        detail::dumpThroughHandler();
        return {};
    }

    [[noreturn]] void crashNow(FatalReason reason, std::string_view message) noexcept override {
        detail::enterFatalSection();
        if (!active()) {
            detail::FixedText<1200> line;
            line.append("frost: fatal ").append(toString(reason)).append(": ").append(message.substr(0, 1024));
            line.append("\n");
            detail::writeToStderr(line.view());
            detail::endWithoutReport();
        }
        detail::setAnnotation(AnnotationKey::Reason, toString(reason));
        detail::setAnnotation(AnnotationKey::Message, message.substr(0, 1024));
        if (!detail::framesAnnotated()) {
            std::uintptr_t frames[64];
            const std::size_t count = detail::captureFrames(frames, 64, 1);
            char text[64 * 19];
            const std::size_t length = detail::formatFrames(frames, count, text, sizeof(text));
            detail::setAnnotation(AnnotationKey::Frames, std::string_view(text, length));
        }
        detail::crashThroughHandler();
    }

private:
    io::IProcessLauncher& m_launcher;
    const IClock& m_clock;
    ICrashpadConnector& m_connector;
    mutable std::mutex m_mutex;
    std::atomic<bool> m_active{false};
    std::unique_ptr<io::IProcess> m_process; // Guarded by m_mutex; kept for the process's life.
    ReporterStatus m_status;                 // Guarded by m_mutex.
};

} // namespace

std::unique_ptr<ICrashReporter> createCrashpadReporter(io::IProcessLauncher& launcher, const IClock& clock,
                                                       ICrashpadConnector& connector) {
    return std::make_unique<CrashpadReporter>(launcher, clock, connector);
}

std::unique_ptr<ICrashReporter> createCrashpadReporter(io::IProcessLauncher& launcher, const IClock& clock) {
    return createCrashpadReporter(launcher, clock, realCrashpadConnector());
}

Result<void> InMemoryCrashReporter::start(const CrashReporterConfig& config) {
    if (failWith) {
        return fail(*failWith, "InMemoryCrashReporter: scripted failure");
    }
    const std::scoped_lock lock(m_mutex);
    m_config = config;
    m_active.store(true, std::memory_order_release);
    return {};
}

ReporterStatus InMemoryCrashReporter::status() const {
    const std::scoped_lock lock(m_mutex);
    ReporterStatus status;
    status.active = m_active.load(std::memory_order_acquire);
    status.handlerPath = m_config.handlerPath;
    status.databasePath = m_config.databasePath;
    return status;
}

void InMemoryCrashReporter::annotate(AnnotationKey key, std::string_view value) noexcept {
    try {
        const std::scoped_lock lock(m_mutex);
        m_annotations.emplace_back(key, std::string(value));
    } catch (...) {
    }
}

Result<void> InMemoryCrashReporter::dumpWithoutCrash(std::string_view message) {
    if (!active()) {
        return fail(ErrorCode::Unsupported, "not started");
    }
    const std::scoped_lock lock(m_mutex);
    m_dumps.emplace_back(message);
    return {};
}

void InMemoryCrashReporter::crashNow(FatalReason reason, std::string_view message) noexcept {
    detail::FixedText<1200> line;
    line.append("InMemoryCrashReporter: fatal ").append(toString(reason)).append(": ").append(message.substr(0, 1024));
    line.append("\n");
    detail::writeToStderr(line.view());
    std::_Exit(kInMemoryFatalExit);
}

std::optional<std::string> InMemoryCrashReporter::annotation(AnnotationKey key) const {
    const std::scoped_lock lock(m_mutex);
    for (auto it = m_annotations.rbegin(); it != m_annotations.rend(); ++it) {
        if (it->first == key) {
            return it->second;
        }
    }
    return std::nullopt;
}

std::vector<std::string> InMemoryCrashReporter::dumps() const {
    const std::scoped_lock lock(m_mutex);
    return m_dumps;
}

} // namespace frost::diag
