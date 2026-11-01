// SPDX-License-Identifier: Apache-2.0
//
// The logging backend (frost/diag/log_backend.hpp) over a file-opener double: file names, the
// exclusive-create retry past taken names, rotation and the per-process file limit, a failed
// create disabling the output without failing a caller, and the queue's overrun policy: a
// writer stuck on the disk never blocks a logging call, and flushFor() keeps its deadline
// (Budget: B20).

#include "frost/core/clock.hpp"
#include "frost/core/log.hpp"
#include "frost/diag/log_backend.hpp"
#include "frost/io/path.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <set>
#include <string>
#include <vector>

namespace {

using namespace std::chrono_literals;
using frost::ErrorCode;
using frost::diag::LogBackend;
using frost::diag::LogBackendOptions;

constexpr bool kWindows = frost::io::kNativePathStyle == frost::io::PathStyle::Windows;
const std::string kLogs = kWindows ? "C:\\cache\\logs" : "/cache/logs";

/*
 * Files in memory. `taken` names fail with AlreadyExists, `failWith` fails every create;
 * `gate` closed makes every write wait until it opens (a disk that does not answer).
 * Thread-safe: the backend's thread writes while the test reads.
 */
class FakeOpener final : public frost::diag::ILogFileOpener {
public:
    std::set<std::string> taken;
    std::optional<ErrorCode> failWith;

    class File final : public frost::diag::ILogFile {
    public:
        File(FakeOpener& owner, std::string path) : m_owner(owner), m_path(std::move(path)) {}
        frost::Result<void> write(std::string_view bytes) noexcept override {
            std::unique_lock lock(m_owner.m_mutex);
            m_owner.m_gateChanged.wait(lock, [&] { return m_owner.m_gateOpen; });
            m_owner.m_files[m_path].append(bytes);
            return {};
        }

    private:
        FakeOpener& m_owner;
        std::string m_path;
    };

    frost::Result<std::unique_ptr<frost::diag::ILogFile>> create(const std::string& path) override {
        const std::scoped_lock lock(m_mutex);
        m_created.push_back(path);
        if (failWith) {
            return frost::fail(*failWith, path);
        }
        if (taken.contains(std::string(frost::io::fileName(path))) || m_files.contains(path)) {
            return frost::fail(ErrorCode::AlreadyExists, path);
        }
        m_files[path];
        return std::make_unique<File>(*this, path);
    }
    void remove(const std::string& path) noexcept override {
        const std::scoped_lock lock(m_mutex);
        m_files.erase(path);
    }

    void setGate(bool open) {
        {
            const std::scoped_lock lock(m_mutex);
            m_gateOpen = open;
        }
        m_gateChanged.notify_all();
    }
    std::map<std::string, std::string> files() const {
        const std::scoped_lock lock(m_mutex);
        return m_files;
    }
    std::vector<std::string> created() const {
        const std::scoped_lock lock(m_mutex);
        return m_created;
    }

private:
    mutable std::mutex m_mutex;
    std::condition_variable m_gateChanged;
    bool m_gateOpen = true;
    std::map<std::string, std::string> m_files;
    std::vector<std::string> m_created;
};

const frost::IClock::SystemTime kStart =
    frost::IClock::SystemTime(std::chrono::sys_days(std::chrono::year(2026) / 11 / 1)) + 18h + 59min + 12s;

struct Rig {
    frost::ManualClock clock{frost::IClock::SteadyTime{}, kStart};
    FakeOpener opener;

    std::unique_ptr<LogBackend> start(LogBackendOptions options = {}) {
        options.directory = kLogs;
        options.pid = 1234;
        auto backend = LogBackend::start(options, opener, clock);
        EXPECT_TRUE(backend.has_value()) << backend.error().describe();
        return backend ? std::move(*backend) : nullptr;
    }
};

std::string logPath(unsigned sequence) {
    return frost::io::joinPath(kLogs, std::format("frost-20261101T185912Z-1234-{}.log", sequence)).value();
}

TEST(LogBackend, FileNames) {
    EXPECT_EQ(frost::diag::logFileName(kStart, 1234, 0), "frost-20261101T185912Z-1234-0.log");
    EXPECT_EQ(frost::diag::logFileName(kStart + 999ms, 7, 12), "frost-20261101T185912Z-7-12.log");
}

TEST(LogBackend, NoFileUntilTheFirstRecordThenOneLinePerRecord) {
    Rig rig;
    auto backend = rig.start();
    ASSERT_NE(backend, nullptr);
    EXPECT_TRUE(rig.opener.created().empty());
    EXPECT_EQ(backend->currentFile(), "");
    backend->write(frost::log::Level::Info, "app", "hello");
    backend->write(frost::log::Level::Error, "io", "line\twith tab");
    ASSERT_TRUE(backend->flushFor(5s));
    EXPECT_EQ(backend->currentFile(), logPath(0));
    const std::string text = rig.opener.files().at(logPath(0));
    const std::regex expected(R"(\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{6}Z \[info\] \[app/\d+\] hello\n)"
                              R"(\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{6}Z \[error\] \[io/\d+\] line\twith tab\n)");
    EXPECT_TRUE(std::regex_match(text, expected)) << text;
    const frost::diag::LogBackendStats stats = backend->stats();
    EXPECT_EQ(stats.submitted, 2u);
    EXPECT_EQ(stats.written, 2u);
    EXPECT_EQ(stats.filesCreated, 1u);
}

TEST(LogBackend, TakenNamesMoveOnToTheNextSequence) {
    Rig rig;
    rig.opener.taken = {"frost-20261101T185912Z-1234-0.log", "frost-20261101T185912Z-1234-1.log"};
    auto backend = rig.start();
    ASSERT_NE(backend, nullptr);
    backend->write(frost::log::Level::Info, "app", "x");
    ASSERT_TRUE(backend->flushFor(5s));
    EXPECT_EQ(backend->currentFile(), logPath(2));
    EXPECT_EQ(rig.opener.created(), (std::vector<std::string>{logPath(0), logPath(1), logPath(2)}));
}

TEST(LogBackend, RotatesAndKeepsAtMostFourFiles) {
    Rig rig;
    LogBackendOptions options;
    options.rotateBytes = 200;
    auto backend = rig.start(options);
    ASSERT_NE(backend, nullptr);
    for (int i = 0; i < 60; ++i) {
        backend->write(frost::log::Level::Info, "app", std::string(60, 'a' + static_cast<char>(i % 26)));
    }
    ASSERT_TRUE(backend->flushFor(5s));
    const auto files = rig.opener.files();
    EXPECT_EQ(files.size(), 4u);
    for (const auto& [path, text] : files) {
        EXPECT_LE(text.size(), 200u) << path; // A record never splits; each file stays under the limit.
        EXPECT_GT(text.size(), 0u) << path;
    }
    // The newest four of the files it created, the last one current.
    const auto created = rig.opener.created();
    ASSERT_GT(created.size(), 4u);
    EXPECT_EQ(backend->currentFile(), created.back());
    EXPECT_TRUE(files.contains(created[created.size() - 4]));
    EXPECT_FALSE(files.contains(created.front()));
    EXPECT_EQ(backend->stats().written, 60u);
}

TEST(LogBackend, ACreateFailureDisablesTheOutputWithoutFailingCallers) {
    Rig rig;
    rig.opener.failWith = ErrorCode::PermissionDenied;
    auto backend = rig.start();
    ASSERT_NE(backend, nullptr);
    for (int i = 0; i < 10; ++i) {
        backend->write(frost::log::Level::Warn, "app", "dropped");
    }
    ASSERT_TRUE(backend->flushFor(5s));
    EXPECT_EQ(backend->stats().failed, 10u);
    EXPECT_EQ(backend->stats().written, 0u);
    EXPECT_EQ(rig.opener.created().size(), 1u); // Not retried for every record.
}

TEST(LogBackend, AStuckDiskNeverBlocksALoggingCall) {
    Rig rig;
    LogBackendOptions options;
    options.queueSlots = 8;
    auto backend = rig.start(options);
    ASSERT_NE(backend, nullptr);
    rig.opener.setGate(false);
    // Opens the gate before the backend's destructor drains the queue, even after a failed ASSERT.
    struct OpenGate {
        FakeOpener& opener;
        ~OpenGate() { opener.setGate(true); }
    } const openGate{rig.opener};
    const auto begin = std::chrono::steady_clock::now();
    for (int i = 0; i < 2000; ++i) {
        backend->write(frost::log::Level::Info, "app", "record");
    }
    const auto elapsed = std::chrono::steady_clock::now() - begin;
    EXPECT_LT(elapsed, 2s) << "logging calls waited for the disk";
    // The deadline holds while the writer is stuck (Budget: B20).
    const auto flushBegin = std::chrono::steady_clock::now();
    EXPECT_FALSE(backend->flushFor(50ms));
    EXPECT_LT(std::chrono::steady_clock::now() - flushBegin, 1s);
    rig.opener.setGate(true);
    ASSERT_TRUE(backend->flushFor(10s));
    const frost::diag::LogBackendStats stats = backend->stats();
    EXPECT_GT(stats.overrun, 0u);
    EXPECT_EQ(stats.written + stats.overrun + stats.failed, 2000u);
}

TEST(LogBackend, StartRejectsBadOptions) {
    Rig rig;
    LogBackendOptions options;
    EXPECT_EQ(LogBackend::start(options, rig.opener, rig.clock).error().code(), ErrorCode::InvalidArgument);
    options.directory = kLogs;
    options.queueSlots = 0;
    EXPECT_EQ(LogBackend::start(options, rig.opener, rig.clock).error().code(), ErrorCode::InvalidArgument);
}

} // namespace
