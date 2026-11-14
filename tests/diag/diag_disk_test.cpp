// SPDX-License-Identifier: Apache-2.0
//
// frost::diag on the real file system: retention prunes aged and oversized fixtures (and a
// real crashpad report, aged through the injected clock) and keeps what it must keep (PLAN.md
// section 3, Privacy and network: "a test ages and oversizes fixtures and asserts the
// pruning"); the log backend writes real files below a long, non-ASCII root, and on Windows
// a running log cannot be deleted; and crashpad's libraries were built by clang (D2).

#include "frost/core/clock.hpp"
#include "frost/core/log.hpp"
#include "frost/diag/log_backend.hpp"
#include "frost/diag/paths.hpp"
#include "frost/diag/retention.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/io/utf.hpp"

#include "diag_test_support.hpp"
#include "support/scratch_dir.hpp"
#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <regex>
#include <string>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace {

using namespace std::chrono_literals;
using frost::test::ScratchDir;

fs::path anyLength(const std::string& utf8) {
#ifdef _WIN32
    return frost::io::toStdPath(frost::io::extendedLengthPath(utf8).value()).value();
#else
    return frost::io::toStdPath(utf8).value();
#endif
}

// Writes `bytes` bytes to `path` (creating parents) and sets its modification time `age` ago
// (std::filesystem::last_write_time: SetFileTime on Windows, utimensat on Linux).
void fixture(const std::string& path, std::size_t bytes, std::chrono::hours age) {
    const fs::path native = anyLength(path);
    fs::create_directories(native.parent_path());
    {
        std::ofstream out(native, std::ios::binary | std::ios::trunc);
        out << std::string(bytes, 'x');
    }
    fs::last_write_time(native, fs::file_time_type::clock::now() - age);
}

bool exists(const std::string& path) { return fs::exists(anyLength(path)); }

TEST(Retention, PrunesAgedAndOversizedFixturesOnDisk) {
    const ScratchDir scratch("diag-retention");
    auto directories = frost::diag::diagnosticsDirectories(scratch.join("cache"));
    ASSERT_TRUE(directories.has_value());
    ASSERT_TRUE(frost::diag::prepareDiagnosticsDirectories(*directories).has_value());
    const auto& d = *directories;

    // A real crashpad report, written now.
    const frost::test::ChildRun run = frost::test::runChild(frost::io::normalizePath(FROST_CRASH_CHILD).value(),
                                                            {"dump-without-crash", d.crashpadDatabase});
    ASSERT_EQ(run.exitCode, 0) << run.output << run.error;
    ASSERT_EQ(frost::test::waitForReports(anyLength(d.crashpadDatabase), 1).finished.size(), 1u);
    auto store = frost::diag::openCrashpadReportStore(d.crashpadDatabase);
    ASSERT_TRUE(store.has_value()) << store.error().describe();
    auto listed = (*store)->list();
    ASSERT_TRUE(listed.has_value());
    ASSERT_EQ(listed->size(), 1u);
    ASSERT_LT(listed->front().size, 299'000u) << "the size step below assumes a small report";

    const auto at = [](const std::string& dir, std::string_view name) {
        return frost::io::joinPath(dir, name).value();
    };
    const std::string freezeOld = at(d.freezes, "freeze-old.json");
    const std::string freezeMid = at(d.freezes, "freeze-mid.json");
    const std::string freezeNew = at(d.freezes, "freeze-new.json");
    const std::string logOld = at(d.logs, "frost-old.log");
    const std::string notes = at(d.logs, "notes.txt");
    const std::string traceOld = frost::io::joinPath(d.traces, "a/b/t.bin").value();
    const std::string traceDeep = frost::io::joinPath(d.traces, "a/b/c/deep.bin").value();
    fixture(freezeOld, 1000, 40 * 24h);
    fixture(freezeMid, 300'000, 48h);
    fixture(freezeNew, 300'000, 24h);
    fixture(logOld, 1000, 40 * 24h);
    fixture(notes, 1000, 40 * 24h);     // Not a frost log name: never managed.
    fixture(traceOld, 1000, 40 * 24h);  // Two directory levels below traces/: managed.
    fixture(traceDeep, 1000, 40 * 24h); // Three levels: out of reach.

    // An active log (the backend's own file, still open), aged too.
    std::unique_ptr<frost::diag::ILogFile> active;
    const std::string activeLog = at(d.logs, "frost-active.log");
    {
        auto created = frost::diag::realLogFileOpener().create(activeLog);
        ASSERT_TRUE(created.has_value()) << created.error().describe();
        active = std::move(*created);
        ASSERT_TRUE(active->write(std::string(1000, 'l')).has_value());
        fs::last_write_time(anyLength(activeLog), fs::file_time_type::clock::now() - 40 * 24h);
    }
#ifndef _WIN32
    // A link with a managed name, to a file outside: neither is ever touched.
    const std::string outside = scratch.join("outside.log");
    fixture(outside, 10, 40 * 24h);
    const std::string link = at(d.logs, "frost-link.log");
    ASSERT_EQ(::symlink(outside.c_str(), link.c_str()), 0);
#endif

    // Prune 1 at the real time, 600 000 bytes allowed: the aged items go, then the oldest
    // removable one (freeze-mid) for size.
    frost::diag::RetentionPolicy policy;
    policy.maxTotalBytes = 600'000;
    auto first = frost::diag::pruneDiagnostics(d, frost::io::realFileSystem(), **store,
                                               frost::diag::realActiveFileProbe(), frost::systemClock(), policy);
    ASSERT_TRUE(first.has_value()) << first.error().describe();
    EXPECT_FALSE(exists(freezeOld));
    EXPECT_FALSE(exists(logOld));
    EXPECT_FALSE(exists(traceOld));
    EXPECT_FALSE(exists(freezeMid));
    EXPECT_TRUE(exists(freezeNew));
    EXPECT_TRUE(exists(activeLog));
    EXPECT_TRUE(exists(notes));
    EXPECT_TRUE(exists(traceDeep));
    EXPECT_EQ((*store)->list()->size(), 1u);
#ifndef _WIN32
    EXPECT_TRUE(fs::is_symlink(fs::symlink_status(link)));
    EXPECT_TRUE(exists(outside));
#endif

    // Prune 2, 31 days later (the injected clock): freeze-new and the crash report are now too
    // old; the active log is still protected.
    const frost::ManualClock later(frost::IClock::SteadyTime{}, frost::systemClock().systemNow() + 31 * 24h);
    auto second =
        frost::diag::pruneDiagnostics(d, frost::io::realFileSystem(), **store, frost::diag::realActiveFileProbe(),
                                      later, frost::diag::RetentionPolicy{});
    ASSERT_TRUE(second.has_value()) << second.error().describe();
    EXPECT_FALSE(exists(freezeNew));
    EXPECT_TRUE(exists(activeLog));
    EXPECT_TRUE(exists(notes));
    // crashpad's own listing no longer shows the report.
    auto reopened = frost::diag::openCrashpadReportStore(d.crashpadDatabase);
    ASSERT_TRUE(reopened.has_value());
    EXPECT_TRUE((*reopened)->list()->empty());
    EXPECT_TRUE(frost::test::listReports(anyLength(d.crashpadDatabase)).finished.empty());
#ifdef _WIN32
    EXPECT_GE(first->busy + second->busy, 1u) << "the active log's deletion is refused (sharing mode)";
#endif
    active.reset();
}

TEST(LogBackend, RealFilesOnLongAndNonAsciiRoots) {
    const ScratchDir scratch("diag-logs");
    std::string root = scratch.join("logs \xC3\xA4 \xE6\xB5\x8B ");
    while (frost::io::utf8ToUtf16(root).value().size() < 240) {
        root += 'x';
    }
    ASSERT_TRUE(frost::io::realFileSystem().createDirectories(root).has_value());
    frost::diag::LogBackendOptions options;
    options.directory = root;
    options.pid = 4242;
    auto backend = frost::diag::LogBackend::start(options, frost::diag::realLogFileOpener(), frost::systemClock());
    ASSERT_TRUE(backend.has_value()) << backend.error().describe();
    (*backend)->write(frost::log::Level::Info, "test", "first record \xC3\xA4");
    (*backend)->write(frost::log::Level::Warn, "test", "second record");
    ASSERT_TRUE((*backend)->flushFor(5s));
    const std::string path = (*backend)->currentFile();
    ASSERT_FALSE(path.empty());
    EXPECT_TRUE(path.ends_with("-4242-0.log")) << path;
    EXPECT_GT(frost::io::utf8ToUtf16(path).value().size(), 260u);
    const std::string text = frost::test::readBytes(anyLength(path));
    const std::regex lines(R"(\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{6}Z \[info\] \[test/\d+\] first record [^\n]+\n)"
                           R"(\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{6}Z \[warning\] \[test/\d+\] second record\n)");
    EXPECT_TRUE(std::regex_match(text, lines)) << text;
    EXPECT_TRUE(text.contains("first record \xC3\xA4\n")) << "the UTF-8 bytes are written as they are";
#ifdef _WIN32
    // FILE_SHARE_READ only: another process, or retention, cannot delete the running log.
    auto removed = frost::io::realFileSystem().removeFile(path);
    ASSERT_FALSE(removed.has_value());
    EXPECT_EQ(removed.error().code(), frost::ErrorCode::Busy) << removed.error().describe();
#else
    EXPECT_TRUE(frost::diag::realActiveFileProbe().isActive(path));
#endif
    backend->reset();
#ifndef _WIN32
    EXPECT_FALSE(frost::diag::realActiveFileProbe().isActive(path));
#endif
}

#ifdef _WIN32
TEST(Toolchain, CrashpadBuiltByClang) {
    /*
     * D2 (clang only): no object in crashpad's static libraries came from an MSVC tool (cl.exe
     * and ml64.exe write the absolute symbol @comp.id into every object; clang-cl and llvm-ml
     * do not), each library holds objects with clang's address-significance table
     * (.llvm_addrsig, which cl.exe never emits; llvm-ml's two assembly objects in util have
     * none), and the handler was linked by lld-link, which writes no "Rich" header into the DOS
     * stub (link.exe always does).
     */
    const std::string libraries = frost::io::normalizePath(FROST_CRASHPAD_LIBRARY_DIR).value();
    for (const char* name : {"vcpkg_crashpad_base.lib", "vcpkg_crashpad_client.lib", "vcpkg_crashpad_client_common.lib",
                             "vcpkg_crashpad_compat.lib", "vcpkg_crashpad_util.lib"}) {
        SCOPED_TRACE(name);
        const frost::test::ChildRun run = frost::test::runChild(
            frost::io::normalizePath(FROST_LLVM_READOBJ).value(),
            {"--sections", "--symbols", frost::io::joinPath(libraries, name).value()}, {}, {}, 120s, false);
        ASSERT_EQ(run.exitCode, 0) << run.output.substr(0, 2000);
        std::size_t members = 0;
        std::size_t marked = 0;
        for (std::size_t at = run.output.find("File: "); at != std::string::npos;
             at = run.output.find("File: ", at + 1)) {
            const std::size_t next = run.output.find("File: ", at + 1);
            const std::string_view member(run.output.data() + at,
                                          (next == std::string::npos ? run.output.size() : next) - at);
            ++members;
            marked += member.contains("Name: .llvm_addrsig") ? 1 : 0;
            EXPECT_FALSE(member.contains("Name: @comp.id")) << member.substr(0, member.find('\n'));
        }
        EXPECT_GT(members, 0u);
        EXPECT_GT(marked, 0u) << "no object from clang in " << members << " objects";
    }
    const std::string handler =
        frost::test::readBytes(anyLength(frost::io::normalizePath(FROST_CRASHPAD_HANDLER).value()));
    ASSERT_GT(handler.size(), 0x40u);
    const auto byteAt = [&](std::size_t at) { return std::uint32_t{static_cast<unsigned char>(handler[at])}; };
    const std::uint32_t peOffset = byteAt(0x3C) | byteAt(0x3D) << 8 | byteAt(0x3E) << 16; // e_lfanew
    ASSERT_LT(peOffset, handler.size());
    EXPECT_FALSE(std::string_view(handler).substr(0, peOffset).contains("Rich"));
}
#endif

} // namespace
