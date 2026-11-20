// SPDX-License-Identifier: Apache-2.0
// Actual native retained-reader fixture: large offsets, bounds, cancellation and mutation.
#include "frost/io/file_system.hpp"
#include "support/scratch_dir.hpp"
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <stop_token>
namespace frost::io::test {
TEST(RandomAccessReader, ActualLargeFixtureReadsHighOffsetsWithoutWholeFileAllocation) {
    frost::test::ScratchDir scratch("large-reader");
    const auto path=scratch.join("large.txt");
    auto& fs=realFileSystem();
    ASSERT_TRUE(fs.writeFileAtomic(path,"header\n"));
    // Materialized only when this deferred test runs. No gigabyte string/line table exists.
    constexpr std::uint64_t size=(std::uint64_t{1}<<30)+17;
    const auto native=std::filesystem::u8path(path);
    std::error_code error; std::filesystem::resize_file(native,size,error); ASSERT_FALSE(error);
    { std::fstream output(native,std::ios::in|std::ios::out|std::ios::binary);
      ASSERT_TRUE(output); output.seekp(static_cast<std::streamoff>(size-5)); output.write("tail!",5);
      ASSERT_TRUE(output); }
    auto opened=fs.openRandomAccess(path); ASSERT_TRUE(opened);
    auto& reader=**opened; EXPECT_EQ(reader.size(),size);
    auto head=reader.read(0,7); ASSERT_TRUE(head); EXPECT_EQ(*head,"header\n");
    auto tail=reader.read(size-5,64); ASSERT_TRUE(tail); EXPECT_EQ(*tail,"tail!");
    auto eof=reader.read(size,1); ASSERT_TRUE(eof); EXPECT_TRUE(eof->empty());
    auto beyond=reader.read(size+1,1); ASSERT_FALSE(beyond); EXPECT_EQ(beyond.error().code(),ErrorCode::OutOfRange);
    auto oversized=reader.read(0,IRandomAccessReader::kMaximumReadBytes+1);
    ASSERT_FALSE(oversized); EXPECT_EQ(oversized.error().code(),ErrorCode::OutOfRange);
    std::stop_source stop; stop.request_stop();
    auto cancelled=reader.read(0,64,stop.get_token()); ASSERT_FALSE(cancelled);
    EXPECT_EQ(cancelled.error().code(),ErrorCode::Cancelled);
}
TEST(RandomAccessReader, OpenerOutlivesFilesystemAndReportsMissingAndNonRegularFiles) {
    auto open=realFileSystem().randomAccessOpener();
    frost::test::ScratchDir scratch("reader-admission");
    auto missing=open(scratch.join("absent")); ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().code(),ErrorCode::NotFound);
    auto directory=open(scratch.path()); ASSERT_FALSE(directory);
    // Windows CreateFile may reject the directory before the regular-file refinement.
    EXPECT_TRUE(directory.error().code()==ErrorCode::InvalidArgument ||
                directory.error().code()==ErrorCode::PermissionDenied);
    EXPECT_FALSE(open("relative.txt"));
}
TEST(RandomAccessReader, ConcurrentWriterIsExcludedOrDetectedAndReplacementNeverSuppliesNewBytes) {
    frost::test::ScratchDir scratch("reader-identity");
    const auto path=scratch.join("identity.txt"); auto& fs=realFileSystem();
    ASSERT_TRUE(fs.writeFileAtomic(path,"original"));
    auto opened=fs.openRandomAccess(path); ASSERT_TRUE(opened);
    auto replaced=fs.writeFileAtomic(path,"different bytes");
    if (replaced) {
        auto page=(*opened)->read(0,8); ASSERT_FALSE(page);
        EXPECT_EQ(page.error().code(),ErrorCode::Busy);
    } else {
        // Windows intentionally retains a read-sharing-only handle, excluding replacement/writes.
        EXPECT_TRUE(replaced.error().code()==ErrorCode::Busy || replaced.error().code()==ErrorCode::PermissionDenied);
        auto page=(*opened)->read(0,8); ASSERT_TRUE(page); EXPECT_EQ(*page,"original");
    }
}
#ifndef _WIN32
TEST(RandomAccessReader, InPlaceMutationInvalidatesRetainedGeneration) {
    frost::test::ScratchDir scratch("reader-inplace"); const auto path=scratch.join("file.txt");
    ASSERT_TRUE(realFileSystem().writeFileAtomic(path,"old"));
    auto opened=realFileSystem().openRandomAccess(path); ASSERT_TRUE(opened);
    { std::ofstream output(std::filesystem::u8path(path),std::ios::binary|std::ios::app); output<<"growth"; }
    auto page=(*opened)->read(0,3); ASSERT_FALSE(page); EXPECT_EQ(page.error().code(),ErrorCode::Busy);
}
#endif
}
