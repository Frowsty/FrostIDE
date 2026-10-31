// SPDX-License-Identifier: Apache-2.0
//
// Tests for MemFs, the in-memory IFileSystem double: the shared semantics
// (errors, sorted listings, atomic writes), virtual-clock mtimes, and fault
// injection, including the "old content survives a failed atomic write" rule.

#include "frost/core/clock.hpp"
#include "frost/io/mem_fs.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using frost::ErrorCode;
using frost::io::DirEntry;
using frost::io::FileType;
using frost::io::FsOp;
using frost::io::MemFs;
using frost::io::PathStyle;

class MemFsTest : public ::testing::Test {
protected:
    frost::ManualClock clock{frost::IClock::SteadyTime{}, frost::IClock::SystemTime{1000s}};
    MemFs fs{clock, PathStyle::Posix};
    // Production code sees only the interface.
    frost::io::IFileSystem& iface = fs;

    void SetUp() override { ASSERT_TRUE(iface.createDirectories("/proj/src").has_value()); }

    // True when no writeFileAtomic temp entry is left anywhere in the tree.
    [[nodiscard]] bool noTempFiles() const {
        for (const std::string& path : fs.allPaths()) {
            if (path.find(".frost-tmp-") != std::string::npos) {
                return false;
            }
        }
        return true;
    }
};

TEST_F(MemFsTest, WriteReadAndStatus) {
    const std::string data("bytes\r\n\0\xff", 9);
    ASSERT_TRUE(iface.writeFileAtomic("/proj/src/a.cpp", data).has_value());
    EXPECT_EQ(iface.readFile("/proj/src/a.cpp").value(), data);
    const auto status = iface.status("/proj/src/a.cpp");
    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(status->type, FileType::Regular);
    EXPECT_EQ(status->size, data.size());
    EXPECT_EQ(status->mtime, frost::IClock::SystemTime{1000s});
    EXPECT_TRUE(iface.exists("/proj/src/a.cpp").value());
    EXPECT_FALSE(iface.exists("/proj/src/b.cpp").value());
    EXPECT_TRUE(noTempFiles());
}

TEST_F(MemFsTest, ModificationTimesComeFromTheClock) {
    ASSERT_TRUE(iface.writeFileAtomic("/proj/a", "1").has_value());
    clock.advance(5s);
    ASSERT_TRUE(iface.writeFileAtomic("/proj/a", "2").has_value());
    EXPECT_EQ(iface.status("/proj/a")->mtime, frost::IClock::SystemTime{1005s});
    // Creating an entry also touches its parent directory.
    EXPECT_EQ(iface.status("/proj")->mtime, frost::IClock::SystemTime{1005s});
    EXPECT_EQ(iface.status("/proj/src")->mtime, frost::IClock::SystemTime{1000s});
}

TEST_F(MemFsTest, ListingIsSortedAndTyped) {
    ASSERT_TRUE(iface.writeFileAtomic("/proj/b.txt", "").has_value());
    ASSERT_TRUE(iface.writeFileAtomic("/proj/a.txt", "").has_value());
    ASSERT_TRUE(iface.createDirectories("/proj/c/deep/er").has_value());
    const std::vector<DirEntry> expected{{"a.txt", FileType::Regular},
                                         {"b.txt", FileType::Regular},
                                         {"c", FileType::Directory},
                                         {"src", FileType::Directory}};
    EXPECT_EQ(iface.listDirectory("/proj").value(), expected);
    EXPECT_EQ(iface.listDirectory("/").value(), (std::vector<DirEntry>{{"proj", FileType::Directory}}));
    EXPECT_TRUE(iface.listDirectory("/proj/c/deep/er").value().empty());
}

TEST_F(MemFsTest, ErrorsMatchTheRealFileSystem) {
    ASSERT_TRUE(iface.writeFileAtomic("/proj/file", "x").has_value());
    EXPECT_EQ(iface.readFile("/proj/missing").error().code(), ErrorCode::NotFound);
    EXPECT_EQ(iface.readFile("/proj/src").error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(iface.writeFileAtomic("/nowhere/file", "x").error().code(), ErrorCode::NotFound);
    EXPECT_EQ(iface.writeFileAtomic("/proj/src", "x").error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(iface.listDirectory("/proj/file").error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(iface.listDirectory("/proj/missing").error().code(), ErrorCode::NotFound);
    EXPECT_EQ(iface.createDirectories("/proj/file").error().code(), ErrorCode::AlreadyExists);
    EXPECT_EQ(iface.createDirectories("/proj/file/below").error().code(), ErrorCode::AlreadyExists);
    EXPECT_EQ(iface.removeFile("/proj/src").error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(iface.removeFile("/proj/missing").error().code(), ErrorCode::NotFound);
    EXPECT_EQ(iface.removeDirectory("/proj").error().code(), ErrorCode::IoError); // Not empty.
    EXPECT_EQ(iface.removeDirectory("/proj/file").error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(iface.status("/proj/missing").error().code(), ErrorCode::NotFound);
    for (const auto& result :
         {iface.readFile("relative").error(), iface.status("").error(), iface.listDirectory("a/b").error()}) {
        EXPECT_EQ(result.code(), ErrorCode::InvalidArgument);
    }
    EXPECT_EQ(iface.writeFileAtomic("rel", "x").error().code(), ErrorCode::InvalidArgument);
}

TEST_F(MemFsTest, RemoveFileAndDirectory) {
    ASSERT_TRUE(iface.writeFileAtomic("/proj/src/x", "1").has_value());
    ASSERT_TRUE(iface.removeFile("/proj/src/x").has_value());
    EXPECT_FALSE(iface.exists("/proj/src/x").value());
    ASSERT_TRUE(iface.removeDirectory("/proj/src").has_value());
    ASSERT_TRUE(iface.removeDirectory("/proj").has_value());
    EXPECT_TRUE(fs.allPaths().empty());
}

TEST_F(MemFsTest, CreateDirectoriesIsIdempotent) {
    ASSERT_TRUE(iface.createDirectories("/proj/src").has_value());
    ASSERT_TRUE(iface.createDirectories("/a//b/./c/").has_value());
    EXPECT_EQ(iface.status("/a/b/c")->type, FileType::Directory);
}

TEST_F(MemFsTest, InjectedWriteFailureKeepsOldContent) {
    ASSERT_TRUE(iface.writeFileAtomic("/proj/a.cpp", "old").has_value());
    fs.injectFault(FsOp::Write, "/proj/a.cpp", ErrorCode::PermissionDenied);
    EXPECT_EQ(fs.pendingFaults(), 1u);
    const auto result = iface.writeFileAtomic("/proj/a.cpp", "new");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), ErrorCode::PermissionDenied);
    EXPECT_NE(result.error().message().find("injected"), std::string::npos);
    EXPECT_EQ(iface.readFile("/proj/a.cpp").value(), "old");
    EXPECT_EQ(fs.pendingFaults(), 0u);
    EXPECT_TRUE(noTempFiles());
    // The fault was one-shot.
    ASSERT_TRUE(iface.writeFileAtomic("/proj/a.cpp", "new").has_value());
    EXPECT_EQ(iface.readFile("/proj/a.cpp").value(), "new");
}

TEST_F(MemFsTest, InjectedRenameFailureKeepsOldContentAndRemovesTheTempFile) {
    ASSERT_TRUE(iface.writeFileAtomic("/proj/a.cpp", "old").has_value());
    clock.advance(1s);
    fs.injectFault(FsOp::AtomicRename, "/proj/a.cpp", ErrorCode::IoError);
    const auto result = iface.writeFileAtomic("/proj/a.cpp", "new content");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), ErrorCode::IoError);
    EXPECT_EQ(iface.readFile("/proj/a.cpp").value(), "old");
    EXPECT_EQ(iface.status("/proj/a.cpp")->mtime, frost::IClock::SystemTime{1000s});
    EXPECT_TRUE(noTempFiles());
    EXPECT_EQ(iface.listDirectory("/proj").value().size(), 2u); // a.cpp and src only.
}

TEST_F(MemFsTest, FaultsMatchPathOpAndCount) {
    ASSERT_TRUE(iface.writeFileAtomic("/proj/a", "a").has_value());
    ASSERT_TRUE(iface.writeFileAtomic("/proj/b", "b").has_value());
    fs.injectFault(FsOp::Read, "/proj/./a", ErrorCode::Busy, 2); // Matched after normalisation.
    EXPECT_EQ(iface.readFile("/proj/b").value(), "b");           // Other path.
    EXPECT_TRUE(iface.status("/proj/a").has_value());            // Other operation.
    EXPECT_EQ(iface.readFile("/proj/a").error().code(), ErrorCode::Busy);
    EXPECT_EQ(iface.readFile("/proj/a").error().code(), ErrorCode::Busy);
    EXPECT_EQ(iface.readFile("/proj/a").value(), "a");
    // An empty path matches any path.
    fs.injectFault(FsOp::List, "", ErrorCode::IoError);
    EXPECT_EQ(iface.listDirectory("/proj/src").error().code(), ErrorCode::IoError);
    EXPECT_TRUE(iface.listDirectory("/proj/src").has_value());
    // Faults on every operation kind, in injection order.
    fs.injectFault(FsOp::Status, "/proj/a", ErrorCode::PermissionDenied);
    fs.injectFault(FsOp::Exists, "/proj/a", ErrorCode::PermissionDenied);
    fs.injectFault(FsOp::RemoveFile, "/proj/a", ErrorCode::Busy);
    fs.injectFault(FsOp::RemoveEmptyDirectory, "/proj/src", ErrorCode::Busy);
    fs.injectFault(FsOp::CreateDirectories, "/proj/new", ErrorCode::PermissionDenied);
    EXPECT_EQ(iface.status("/proj/a").error().code(), ErrorCode::PermissionDenied);
    EXPECT_EQ(iface.exists("/proj/a").error().code(), ErrorCode::PermissionDenied);
    EXPECT_EQ(iface.removeFile("/proj/a").error().code(), ErrorCode::Busy);
    EXPECT_EQ(iface.removeDirectory("/proj/src").error().code(), ErrorCode::Busy);
    EXPECT_EQ(iface.createDirectories("/proj/new").error().code(), ErrorCode::PermissionDenied);
    EXPECT_EQ(fs.pendingFaults(), 0u);
    EXPECT_TRUE(iface.exists("/proj/a").value());
    EXPECT_FALSE(iface.exists("/proj/new").value());
}

TEST(MemFs, WindowsStyleTreeOnAnyPlatform) {
    frost::ManualClock clock;
    MemFs fs(clock, PathStyle::Windows);
    ASSERT_TRUE(fs.createDirectories("C:/Users/dev/proj").has_value());
    ASSERT_TRUE(fs.writeFileAtomic("C:\\Users\\dev\\proj\\main.cpp", "int main() {}").has_value());
    // Either separator and "." / ".." name the same file.
    EXPECT_EQ(fs.readFile("C:/Users/dev/./x/../proj/main.cpp").value(), "int main() {}");
    EXPECT_EQ(fs.listDirectory("C:\\Users\\dev\\proj").value(),
              (std::vector<DirEntry>{{"main.cpp", FileType::Regular}}));
    EXPECT_EQ(fs.listDirectory("C:\\").value(), (std::vector<DirEntry>{{"Users", FileType::Directory}}));
    EXPECT_EQ(fs.readFile("c:\\users\\dev\\proj\\main.cpp").error().code(), ErrorCode::NotFound); // Case-sensitive...
    EXPECT_EQ(fs.readFile("c:\\Users\\dev\\proj\\main.cpp").value(), "int main() {}"); // ...except the drive.
    EXPECT_EQ(fs.readFile("/Users/dev/proj/main.cpp").error().code(), ErrorCode::InvalidArgument); // Not absolute.
    // Names Win32 would rewrite are refused, as by the real file system.
    EXPECT_EQ(fs.writeFileAtomic("C:\\Users\\trail.", "x").error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(fs.writeFileAtomic("C:\\Users\\NUL.txt", "x").error().code(), ErrorCode::InvalidArgument);
}

TEST(MemFs, OnlyAddedRootsExist) {
    frost::ManualClock clock;
    MemFs fs(clock, PathStyle::Windows);
    EXPECT_TRUE(fs.exists("C:\\").value());
    EXPECT_FALSE(fs.exists("Z:\\").value());
    EXPECT_EQ(fs.status("Z:\\").error().code(), ErrorCode::NotFound);
    EXPECT_EQ(fs.listDirectory("Z:\\").error().code(), ErrorCode::NotFound);
    EXPECT_EQ(fs.writeFileAtomic("Z:\\x.txt", "x").error().code(), ErrorCode::NotFound);
    EXPECT_EQ(fs.createDirectories("Z:\\a\\b").error().code(), ErrorCode::NotFound);
    EXPECT_EQ(fs.writeFileAtomic("\\\\server\\share\\x.txt", "unc").error().code(), ErrorCode::NotFound);

    fs.addRoot("z:/");
    fs.addRoot("\\\\server\\share");
    ASSERT_TRUE(fs.createDirectories("Z:\\a\\b").has_value());
    ASSERT_TRUE(fs.writeFileAtomic("\\\\server\\share\\x.txt", "unc").has_value());
    EXPECT_EQ(fs.readFile("//server/share/x.txt").value(), "unc");
    EXPECT_EQ(fs.removeDirectory("Z:\\").error().code(), ErrorCode::PermissionDenied);
    EXPECT_TRUE(fs.listDirectory("\\\\server\\other\\").error().code() == ErrorCode::NotFound);

    MemFs posix(clock, PathStyle::Posix);
    EXPECT_TRUE(posix.exists("/").value());
}

TEST_F(MemFsTest, PosixDotDotResolvesThroughExistingDirectoriesOnly) {
    ASSERT_TRUE(iface.writeFileAtomic("/proj/a.txt", "a").has_value());
    // Like the kernel: "/proj/src/.." is /proj because /proj/src exists.
    EXPECT_EQ(iface.readFile("/proj/src/../a.txt").value(), "a");
    EXPECT_EQ(iface.readFile("/proj/src/../../proj/a.txt").value(), "a");
    EXPECT_EQ(iface.readFile("/../../proj/a.txt").value(), "a"); // ".." at the root stays there.
    // Not through a missing directory or through a file (ENOENT / ENOTDIR).
    EXPECT_EQ(iface.readFile("/proj/missing/../a.txt").error().code(), ErrorCode::NotFound);
    EXPECT_EQ(iface.readFile("/proj/a.txt/../a.txt").error().code(), ErrorCode::NotFound);
    EXPECT_FALSE(iface.exists("/proj/missing/../a.txt").value());
    ASSERT_TRUE(iface.writeFileAtomic("/proj/src/../b.txt", "b").has_value());
    EXPECT_EQ(iface.readFile("/proj/b.txt").value(), "b");
    EXPECT_EQ(iface.listDirectory("/proj/src/..").value().size(), 3u); // a.txt, b.txt, src.
    // A final ".." names a directory, never an entry to create or remove.
    for (const char* path : {"/proj/src/..", "/proj/.."}) {
        EXPECT_EQ(iface.writeFileAtomic(path, "x").error().code(), ErrorCode::InvalidArgument) << path;
        EXPECT_EQ(iface.removeFile(path).error().code(), ErrorCode::InvalidArgument) << path;
        EXPECT_EQ(iface.removeDirectory(path).error().code(), ErrorCode::InvalidArgument) << path;
    }
    EXPECT_TRUE(iface.exists("/proj/src").value());
}

TEST(MemFs, ConcurrentWritersDoNotInterfere) {
    frost::ManualClock clock;
    MemFs fs(clock, PathStyle::Posix);
    ASSERT_TRUE(fs.createDirectories("/t").has_value());
    std::vector<std::jthread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&fs, t] {
            for (int i = 0; i < 200; ++i) {
                const std::string path = "/t/f" + std::to_string(t);
                static_cast<void>(fs.writeFileAtomic(path, std::to_string(i)));
                static_cast<void>(fs.readFile(path));
                static_cast<void>(fs.listDirectory("/t"));
            }
        });
    }
    threads.clear();
    const auto entries = fs.listDirectory("/t");
    ASSERT_TRUE(entries.has_value());
    EXPECT_EQ(entries->size(), 4u);
    for (int t = 0; t < 4; ++t) {
        EXPECT_EQ(fs.readFile("/t/f" + std::to_string(t)).value(), "199");
    }
}

} // namespace
