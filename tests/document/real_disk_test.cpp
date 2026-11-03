// SPDX-License-Identifier: Apache-2.0
//
// Integration tests for frost::document on the real file system (ADR 0028): a byte-exact save
// through the SaveManager, a read-only target refused and then overwritten on request, the 128 MiB
// refusal before reading, and a kill -9 of document_test_child in the middle of its save loop that
// must leave the old content or the new one, never a torn file, with the killed save's temp file
// swept by the next open.

#include "frost/core/clock.hpp"
#include "frost/document/save_manager.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"

#include "kill_protocol.hpp"
#include "support/scratch_dir.hpp"

// The storage tests' child runner (launch, line reader, kill -9) drives this module's child.
#define FROST_STORAGE_TEST_CHILD FROST_DOCUMENT_TEST_CHILD
#include "storage/support/child_process.hpp"
#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <format>
#include <memory>
#include <string>
#include <system_error>
#include <thread>

namespace frost::document::test {
namespace {

using namespace std::chrono_literals;

class RealDiskTest : public ::testing::Test {
protected:
    RealDiskTest() {
        services.fs = &io::realFileSystem();
        services.clock = &systemClock();
        policies.hotExit = false;
        policies.localHistory = false;
    }

    std::string write(std::string_view name, std::string_view bytes) {
        const std::string file = m_scratch.join(name);
        EXPECT_TRUE(io::realFileSystem().writeFileAtomic(file, bytes));
        return file;
    }

    static std::string read(const std::string& file) {
        auto bytes = io::realFileSystem().readFile(file);
        return bytes ? std::move(*bytes) : std::string("<unreadable>");
    }

    static void setReadOnly(const std::string& file, bool readOnly) {
        std::error_code error;
        const auto native = io::toStdPath(file).value();
        std::filesystem::permissions(
            native,
            std::filesystem::perms::owner_write | std::filesystem::perms::group_write |
                std::filesystem::perms::others_write,
            readOnly ? std::filesystem::perm_options::remove : std::filesystem::perm_options::add, error);
        ASSERT_FALSE(error) << error.message();
    }

    static bool writable(const std::string& file) {
        const auto perms = std::filesystem::status(io::toStdPath(file).value()).permissions();
        return (perms & std::filesystem::perms::owner_write) != std::filesystem::perms::none;
    }

    SaveManagerServices services{};
    SavePolicies policies{};
    frost::test::ScratchDir m_scratch{"document-real-disk"};
};

TEST_F(RealDiskTest, SavesByteExactlyAndRefusesReadOnlyTargets) {
    const std::string file = write("main.cpp", "\xEF\xBB\xBF#include <cstdio>\r\nint main() {}\r\n");
    SaveManager manager(services, {}, policies);
    auto opened = manager.open(file);
    ASSERT_TRUE(opened) << opened.error().describe();
    FileDocument& document = **opened;
    const std::string text = document.document().committedText().text();
    const auto end = text.find('}');
    ASSERT_TRUE(document.document().replace(text::ByteRange{ByteOffset{end}, ByteOffset{end}}, " return 0; ", {}));
    auto saved = manager.save(document);
    ASSERT_TRUE(saved) << saved.error().describe();
    EXPECT_EQ(read(file), "\xEF\xBB\xBF#include <cstdio>\r\nint main() { return 0; }\r\n");

    setReadOnly(file, true);
    ASSERT_TRUE(document.document().replace(text::ByteRange{ByteOffset{0}, ByteOffset{0}}, "// x\r\n", {}));
    auto refused = manager.save(document);
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code(), ErrorCode::PermissionDenied) << refused.error().describe();
    EXPECT_NE(refused.error().describe().find("read-only"), std::string::npos) << refused.error().describe();
    EXPECT_EQ(read(file), "\xEF\xBB\xBF#include <cstdio>\r\nint main() { return 0; }\r\n");
    EXPECT_TRUE(document.dirty());

    SaveOptions overwrite;
    overwrite.overwriteReadOnly = true;
    auto forced = manager.save(document, overwrite);
    ASSERT_TRUE(forced) << forced.error().describe();
    EXPECT_EQ(read(file), "\xEF\xBB\xBF// x\r\n#include <cstdio>\r\nint main() { return 0; }\r\n");
    EXPECT_TRUE(writable(file)); // "Make writable and save".
    EXPECT_FALSE(document.dirty());
}

TEST_F(RealDiskTest, RefusesFilesOf128MiBWithoutReadingThem) {
    const std::string file = m_scratch.join("huge.bin");
    {
        // Sized, not written: the guard must refuse it from its status alone.
        std::error_code error;
        const auto native = io::toStdPath(file).value();
        ASSERT_TRUE(io::realFileSystem().writeFileAtomic(file, ""));
        std::filesystem::resize_file(native, kMaxOpenFileBytes, error);
        ASSERT_FALSE(error) << error.message();
    }
    SaveManager manager(services, {}, policies);
    const auto started = std::chrono::steady_clock::now();
    auto opened = manager.open(file);
    ASSERT_FALSE(opened);
    EXPECT_EQ(opened.error().code(), ErrorCode::OutOfRange) << opened.error().describe();
    EXPECT_LT(std::chrono::steady_clock::now() - started, 5s);
    EXPECT_EQ(manager.documents().size(), 0U);
}

TEST_F(RealDiskTest, KillDuringSavesLeavesAWholeFile) {
    const std::string file = write("victim.txt", killInitialContent());
    auto child = storage::test::StorageChild::start({"save-loop", file});
    ASSERT_NE(child, nullptr);
    ASSERT_TRUE(child->waitFor("saved ", 3).has_value());
    std::this_thread::sleep_for(std::chrono::milliseconds(7)); // Land somewhere inside a later save.
    child->kill();
    const std::int64_t last = child->lastNumber("saved ");
    ASSERT_GE(last, 3);

    // The file holds the last reported save or the one in flight, whole.
    const std::string bytes = read(file);
    const bool reported = bytes == killContent(static_cast<std::uint64_t>(last));
    const bool inFlight = bytes == killContent(static_cast<std::uint64_t>(last) + 1);
    EXPECT_TRUE(reported || inFlight) << "torn file of " << bytes.size() << " bytes after save " << last;

    // The next session sweeps the killed save's temp file (any age, for the test).
    SaveManagerOptions options;
    options.staleTempAge = IClock::Duration::zero();
    SaveManager next(services, options, policies);
    auto reopened = next.open(file);
    ASSERT_TRUE(reopened) << reopened.error().describe();
    auto entries = io::realFileSystem().listDirectory(m_scratch.path());
    ASSERT_TRUE(entries);
    for (const io::DirEntry& entry : *entries) {
        EXPECT_EQ(entry.name.find(".frost-tmp-"), std::string::npos) << entry.name;
    }
}

TEST_F(RealDiskTest, RepeatedKilledSavesAllowNextSessionToSave) {
    for (int iteration = 0; iteration < 10; ++iteration) {
        SCOPED_TRACE(iteration);
        const std::string file = write(std::format("victim-{}.txt", iteration), killInitialContent());
        auto child = storage::test::StorageChild::start({"save-loop", file});
        ASSERT_NE(child, nullptr);
        ASSERT_TRUE(child->waitFor("saved ", 3).has_value());
        std::this_thread::sleep_for(std::chrono::milliseconds(7));
        child->kill();
        SaveManager next(services, {}, policies);
        auto reopened = next.open(file);
        ASSERT_TRUE(reopened) << reopened.error().describe();
        auto& document = (*reopened)->document();
        const text::ByteRange all{ByteOffset{0}, document.committedText().size()};
        ASSERT_TRUE(document.replace(all, "next session", {}));
        auto saved = next.save(**reopened);
        ASSERT_TRUE(saved) << saved.error().describe();
        EXPECT_EQ(read(file), "next session");
    }
}
} // namespace
} // namespace frost::document::test
