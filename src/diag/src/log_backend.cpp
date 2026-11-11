// SPDX-License-Identifier: Apache-2.0
//
// The spdlog-based logging backend (frost/diag/log_backend.hpp). spdlog (MIT) is used
// header-only, from this file alone, with std::format (the vcpkg port without its fmt
// feature): an async_logger on a private one-thread thread_pool, no global registry.

#include "frost/diag/log_backend.hpp"

#include "frost/io/path.hpp"

#include "internal.hpp"
#include <spdlog/async_logger.h>
#include <spdlog/details/thread_pool.h>
#include <spdlog/pattern_formatter.h>
#include <spdlog/sinks/base_sink.h>

#include <atomic>
#include <condition_variable>
#include <ctime>
#include <deque>
#include <format>
#include <mutex>
#include <utility>

namespace frost::diag {
namespace {

// Separates the category from the message inside spdlog's payload (never in either).
constexpr char kCategorySeparator = '\x1f';

std::string_view categoryOf(std::string_view payload) noexcept {
    const std::size_t at = payload.find(kCategorySeparator);
    return at == std::string_view::npos ? std::string_view() : payload.substr(0, at);
}

std::string_view messageOf(std::string_view payload) noexcept {
    const std::size_t at = payload.find(kCategorySeparator);
    return at == std::string_view::npos ? payload : payload.substr(at + 1);
}

// The "%N" flag: the record's category.
class CategoryFlag final : public spdlog::custom_flag_formatter {
public:
    void format(const spdlog::details::log_msg& msg, const std::tm&, spdlog::memory_buf_t& dest) override {
        const std::string_view category = categoryOf(std::string_view(msg.payload.data(), msg.payload.size()));
        dest.append(category.data(), category.data() + category.size());
    }
    [[nodiscard]] std::unique_ptr<custom_flag_formatter> clone() const override {
        return std::make_unique<CategoryFlag>();
    }
};

// The "%V" flag: the record's message.
class MessageFlag final : public spdlog::custom_flag_formatter {
public:
    void format(const spdlog::details::log_msg& msg, const std::tm&, spdlog::memory_buf_t& dest) override {
        const std::string_view message = messageOf(std::string_view(msg.payload.data(), msg.payload.size()));
        dest.append(message.data(), message.data() + message.size());
    }
    [[nodiscard]] std::unique_ptr<custom_flag_formatter> clone() const override {
        return std::make_unique<MessageFlag>();
    }
};

spdlog::level::level_enum toSpdlog(log::Level level) noexcept {
    switch (level) {
    case log::Level::Trace: return spdlog::level::trace;
    case log::Level::Debug: return spdlog::level::debug;
    case log::Level::Info: return spdlog::level::info;
    case log::Level::Warn: return spdlog::level::warn;
    case log::Level::Error: return spdlog::level::err;
    case log::Level::Critical: return spdlog::level::critical;
    case log::Level::Off: return spdlog::level::off;
    }
    return spdlog::level::info;
}

} // namespace

std::string logFileName(IClock::SystemTime start, std::uint32_t pid, unsigned sequence) {
    return std::format("frost-{}-{}-{}.log", compactUtc(start), pid, sequence);
}

/*
 * The state shared by the backend and its sink. The sink runs on the "frost-log" thread only;
 * the counters and the current path are read from other threads.
 */
struct LogBackend::Impl {
    LogBackendOptions options;
    ILogFileOpener* opener = nullptr;
    IClock::SystemTime startTime{};

    std::shared_ptr<spdlog::details::thread_pool> pool;
    std::shared_ptr<spdlog::async_logger> logger;

    std::atomic<std::uint64_t> submitted{0};
    std::atomic<std::uint64_t> written{0};
    std::atomic<std::uint64_t> failed{0};
    std::atomic<unsigned> filesCreated{0};

    // Signalled by the sink after every record, for flushFor().
    std::mutex progressMutex;
    std::condition_variable progress;

    mutable std::mutex pathMutex;
    std::string currentPath; // Guarded by pathMutex.

    // Sink-thread state.
    std::unique_ptr<ILogFile> file;
    std::uint64_t fileBytes = 0;
    unsigned nextSequence = 0;
    bool disabled = false;
    std::deque<std::string> ownFiles;

    void recordDone(bool ok) {
        (ok ? written : failed).fetch_add(1, std::memory_order_relaxed);
        {
            const std::scoped_lock lock(progressMutex);
        }
        progress.notify_all();
    }

    // Opens the next file (exclusive create, moving on past taken names). Sink thread only.
    bool openNext() {
        file.reset();
        fileBytes = 0;
        for (int attempt = 0; attempt < 1000; ++attempt) {
            auto path = io::joinPath(options.directory, logFileName(startTime, options.pid, nextSequence++));
            if (!path) {
                break;
            }
            auto created = opener->create(*path);
            if (created) {
                file = std::move(*created);
                filesCreated.fetch_add(1, std::memory_order_relaxed);
                {
                    const std::scoped_lock lock(pathMutex);
                    currentPath = *path;
                }
                ownFiles.push_back(*path);
                while (ownFiles.size() > options.maxFilesPerProcess) {
                    opener->remove(ownFiles.front());
                    ownFiles.pop_front();
                }
                return true;
            }
            if (created.error().code() != ErrorCode::AlreadyExists) {
                break;
            }
        }
        disabled = true;
        return false;
    }

    void writeRecord(std::string_view bytes) {
        if (disabled) {
            recordDone(false);
            return;
        }
        if ((!file || (fileBytes > 0 && fileBytes + bytes.size() > options.rotateBytes)) && !openNext()) {
            recordDone(false);
            return;
        }
        if (auto wrote = file->write(bytes); !wrote) {
            // A write error (disk full, file removed under us): try one fresh file, then give up.
            if (!openNext() || !file->write(bytes)) {
                disabled = true;
                recordDone(false);
                return;
            }
        }
        fileBytes += bytes.size();
        recordDone(true);
    }
};

namespace {

// The sink: formats with the pattern and hands each record to the Impl. spdlog calls it from
// the pool thread only, so it needs no lock of its own (null_mutex).
class FileSink final : public spdlog::sinks::base_sink<spdlog::details::null_mutex> {
public:
    explicit FileSink(LogBackend::Impl& impl) : m_impl(impl) {}

protected:
    void sink_it_(const spdlog::details::log_msg& msg) override {
        spdlog::memory_buf_t formatted;
        formatter_->format(msg, formatted);
        m_impl.writeRecord(std::string_view(formatted.data(), formatted.size()));
    }
    void flush_() override {}

private:
    LogBackend::Impl& m_impl;
};

} // namespace

Result<std::unique_ptr<LogBackend>> LogBackend::start(const LogBackendOptions& options, ILogFileOpener& opener,
                                                      const IClock& clock) {
    if (options.directory.empty() || options.queueSlots == 0 || options.maxFilesPerProcess == 0) {
        return fail(ErrorCode::InvalidArgument, "log backend needs a directory, queue slots and a file count");
    }
    auto impl = std::make_unique<Impl>();
    impl->options = options;
    impl->opener = &opener;
    impl->startTime = clock.systemNow();
    try {
        impl->pool = std::make_shared<spdlog::details::thread_pool>(options.queueSlots, 1,
                                                                    [] { nameAndGuardThisThread("frost-log"); });
        auto sink = std::make_shared<FileSink>(*impl);
        auto formatter = std::make_unique<spdlog::pattern_formatter>(spdlog::pattern_time_type::utc, "\n");
        formatter->add_flag<CategoryFlag>('N').add_flag<MessageFlag>('V');
        formatter->set_pattern("%Y-%m-%dT%H:%M:%S.%fZ [%l] [%N/%t] %V");
        sink->set_formatter(std::move(formatter));
        impl->logger = std::make_shared<spdlog::async_logger>("frost", std::move(sink), impl->pool,
                                                              spdlog::async_overflow_policy::overrun_oldest);
        impl->logger->set_level(spdlog::level::trace);
        impl->logger->flush_on(spdlog::level::off);
    } catch (const std::exception& error) {
        return fail(ErrorCode::Internal, std::format("spdlog: {}", error.what()));
    }
    return std::unique_ptr<LogBackend>(new LogBackend(std::move(impl)));
}

LogBackend::LogBackend(std::unique_ptr<Impl> impl) noexcept : m_impl(std::move(impl)) {}

LogBackend::~LogBackend() {
    // The logger first, then the pool: the pool's destructor drains the queue and joins.
    m_impl->logger.reset();
    m_impl->pool.reset();
}

void LogBackend::write(log::Level level, std::string_view category, std::string_view message) noexcept {
    try {
        std::string payload;
        payload.reserve(category.size() + 1 + message.size());
        payload.append(category);
        payload.push_back(kCategorySeparator);
        payload.append(message);
        m_impl->submitted.fetch_add(1, std::memory_order_relaxed);
        m_impl->logger->log(toSpdlog(level), std::string_view(payload));
    } catch (...) {
        m_impl->failed.fetch_add(1, std::memory_order_relaxed);
    }
}

void LogBackend::flush() noexcept {
    try {
        m_impl->logger->flush();
    } catch (...) { // NOLINT(bugprone-empty-catch): noexcept flush cannot recursively log a backend failure.
    }
}

bool LogBackend::flushFor(std::chrono::milliseconds deadline) noexcept {
    // Budget: B20.
    const std::uint64_t target = m_impl->submitted.load(std::memory_order_relaxed);
    const auto done = [&] {
        const std::uint64_t overrun = m_impl->pool ? m_impl->pool->overrun_counter() : 0;
        return m_impl->written.load(std::memory_order_relaxed) + m_impl->failed.load(std::memory_order_relaxed) +
                   overrun >=
               target;
    };
    std::unique_lock lock(m_impl->progressMutex);
    return m_impl->progress.wait_for(lock, deadline, done);
}

LogBackendStats LogBackend::stats() const noexcept {
    LogBackendStats stats;
    stats.submitted = m_impl->submitted.load(std::memory_order_relaxed);
    stats.written = m_impl->written.load(std::memory_order_relaxed);
    stats.failed = m_impl->failed.load(std::memory_order_relaxed);
    stats.overrun = m_impl->pool ? m_impl->pool->overrun_counter() : 0;
    stats.filesCreated = m_impl->filesCreated.load(std::memory_order_relaxed);
    return stats;
}

std::string LogBackend::currentFile() const {
    const std::scoped_lock lock(m_impl->pathMutex);
    return m_impl->currentPath;
}

log::Level logLevelFromEnvironment(log::Level fallback) noexcept {
    const std::string value = environmentValue("FROST_LOG_LEVEL");
    log::Level level = fallback;
    if (!value.empty() && log::parseLevel(value, level)) {
        return level;
    }
    return fallback;
}

} // namespace frost::diag
