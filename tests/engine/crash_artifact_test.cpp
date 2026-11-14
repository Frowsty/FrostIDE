// SPDX-License-Identifier: Apache-2.0
// Crash-source/format/path fixtures; real CrashHandler acceptance is in crash_artifact_corpus_test.cpp.
#include "frost/engine/crash_artifact.hpp"

#include "support/crash_artifact_fixture.hpp"
#include <gtest/gtest.h>

#include <functional>

namespace frost::engine {
namespace {
using namespace std::chrono_literals;
using rpc::Json;
#ifdef _WIN32
const std::string root = "C:/artifact-test", project = "C:/trusted-project";
#else
const std::string root = "/artifact-test", project = "/trusted-project";
#endif
class PathFaultFs final : public io::IFileSystem {
public:
    explicit PathFaultFs(io::MemFs& inner) : m_inner(inner) {}
    std::string linked;
    unsigned listings = 0;
    std::function<void(std::string_view)> onRead;
    Result<std::string> readFile(std::string_view path) override {
        auto bytes = m_inner.readFile(path);
        if (onRead)
            onRead(path);
        return bytes;
    }
    Result<std::string> readFileSnapshot(std::string_view path, std::size_t limit) override {
        auto bytes = m_inner.readFileSnapshot(path, limit);
        if (onRead)
            onRead(path);
        return bytes;
    }
    Result<void> writeFileAtomic(std::string_view p, std::string_view b) override {
        return m_inner.writeFileAtomic(p, b);
    }
    Result<io::FileStatus> status(std::string_view p) override { return m_inner.status(p); }
    Result<std::vector<io::DirEntry>> listDirectory(std::string_view p) override {
        ++listings;
        auto entries = m_inner.listDirectory(p);
        if (entries && !linked.empty())
            for (auto& entry : *entries) {
                auto joined = io::joinPath(p, entry.name);
                if (joined && *joined == linked)
                    entry.type = io::FileType::Symlink;
            }
        return entries;
    }
    Result<io::DirListing> listDirectoryWithDiagnostics(std::string_view p) override {
        auto entries = listDirectory(p);
        if (!entries)
            return std::unexpected(entries.error());
        return io::DirListing{std::move(*entries), {}};
    }
    Result<void> createDirectories(std::string_view p) override { return m_inner.createDirectories(p); }
    Result<void> removeFile(std::string_view p) override { return m_inner.removeFile(p); }
    Result<void> removeDirectory(std::string_view p) override { return m_inner.removeDirectory(p); }
    Result<bool> exists(std::string_view p) override { return m_inner.exists(p); }

private:
    io::MemFs& m_inner;
};
Json descriptor(const CrashArtifactSource& source) {
    return Json::object()
        .set("protocolVersion", 1)
        .set("sourceId", source.sourceId)
        .set("rootPath", source.rootPath)
        .set("processRole", source.processRole)
        .set("instanceId", source.instanceId)
        .set("generation", source.generation)
        .set("projectPath", source.projectPath)
        .set("processId", source.processId)
        .set("processStartUnixMs", source.processStartUnixMs);
}
TEST(CrashArtifact, AuthenticatedSourceRequiresExactIdentityPidAndSafeMetadata) {
    auto source = test::syntheticCrashSource(root, project);
    auto value = descriptor(source);
    EXPECT_TRUE(
        parseCrashArtifactSource(value, source.instanceId, source.generation, source.projectPath, source.processId));
    for (const auto key : {"instanceId", "generation", "projectPath", "processRole", "rootPath", "sourceId"}) {
        auto broken = value;
        broken.set(key, "invalid");
        EXPECT_FALSE(parseCrashArtifactSource(broken, source.instanceId, source.generation, source.projectPath,
                                              source.processId))
            << key;
    }
    EXPECT_FALSE(parseCrashArtifactSource(value, source.instanceId, source.generation, source.projectPath,
                                          source.processId + 1));
    value.set("processStartUnixMs", 0);
    EXPECT_FALSE(
        parseCrashArtifactSource(value, source.instanceId, source.generation, source.projectPath, source.processId));
    ManualClock clock;
    io::MemFs fs(clock);
    PathFaultFs paths(fs);
    for (const auto remote :
         {"//server/share/crashes", "\\/server/share/crashes", "/\\server/share/crashes", "\\\\server\\share\\crashes",
          "\\\\?\\UNC\\server\\share\\crashes", "\\\\.\\device", "\\\\?\\GLOBALROOT\\Device\\crashes"}) {
        auto untrusted = source;
        untrusted.rootPath = remote;
        const auto metadata = descriptor(untrusted);
        EXPECT_FALSE(parseCrashArtifactSource(metadata, source.instanceId, source.generation, source.projectPath,
                                              source.processId));
        EXPECT_FALSE(CrashArtifactObserver::capture(paths, untrusted, clock));
        EXPECT_FALSE(validateCrashCacheDirectory(paths, remote));
    }
    EXPECT_EQ(paths.listings, 0U);
}
TEST(CrashArtifact, BoundsCheckEveryTruncationAndEmbeddedDumpProcessIdentity) {
    const auto bytes = test::syntheticCrashMinidump();
    ASSERT_TRUE(validateCrashMinidump(bytes, 4242));
    EXPECT_FALSE(validateCrashMinidump(bytes, 4243));
    for (std::size_t size = 0; size < bytes.size(); ++size)
        EXPECT_FALSE(validateCrashMinidump(std::string_view(bytes).substr(0, size), 4242)) << size;
    for (const auto offset : {0U, 4U, 8U, 12U, 40U, 48U, 288U, 448U, 488U}) {
        auto corrupt = bytes;
        test::dumpU32(corrupt, offset, 0xffffffff);
        EXPECT_FALSE(validateCrashMinidump(corrupt, 4242)) << offset;
    }
}
TEST(CrashArtifact, BaselineAndOldOrDistractorReportsNeverProduceAnOffer) {
    ManualClock clock({}, IClock::SystemTime(1001000ms));
    io::MemFs fs(clock);
    auto source = test::syntheticCrashSource(root, project);
    auto invalid = source;
    invalid.sourceId.clear();
    EXPECT_FALSE(CrashArtifactObserver::capture(fs, invalid, clock));
    ASSERT_TRUE(fs.createDirectories(root));
    ASSERT_TRUE(test::writeSyntheticCrashReport(fs, source, "20261111T120000Z-4242"));
    auto observer = CrashArtifactObserver::capture(fs, source, clock);
    ASSERT_TRUE(observer);
    clock.advance(2s);
    auto wrong = source;
    wrong.processStartUnixMs += 1;
    ASSERT_TRUE(test::writeSyntheticCrashReport(fs, wrong, "20261111T120001Z-4242"));
    wrong = source;
    wrong.processRole = "other";
    ASSERT_TRUE(test::writeSyntheticCrashReport(fs, wrong, "20261111T120002Z-4242"));
    ASSERT_TRUE(test::writeSyntheticCrashReport(fs, source, "20261111T120003Z-4242", 1000000));
    auto scan = (*observer)->poll(fs);
    ASSERT_TRUE(scan);
    EXPECT_FALSE(scan->artifact);
    EXPECT_EQ(scan->rejectedReports, 3U);
    ASSERT_TRUE(test::writeSyntheticCrashReport(fs, source, "20261111T120004Z-4242", 1003000));
    scan = (*observer)->poll(fs);
    ASSERT_TRUE(scan);
    ASSERT_TRUE(scan->artifact);
    EXPECT_EQ(scan->artifact->source().instanceId, source.instanceId);
    EXPECT_EQ(scan->artifact->dumpBytes(), test::syntheticCrashMinidump());
    EXPECT_FALSE((*observer)->poll(fs)->artifact);
}
TEST(CrashArtifact, PartialReportAndDumpRetryUntilRealBytesAreComplete) {
    ManualClock clock({}, IClock::SystemTime(1001000ms));
    io::MemFs fs(clock);
    auto source = test::syntheticCrashSource(root, project);
    ASSERT_TRUE(fs.createDirectories(root));
    auto observer = CrashArtifactObserver::capture(fs, source, clock);
    ASSERT_TRUE(observer);
    clock.advance(1s);
    auto dir = test::writeSyntheticCrashReport(fs, source, "20261111T120000Z-4242", 1002000, "MDMP");
    ASSERT_TRUE(dir);
    auto first = (*observer)->poll(fs);
    ASSERT_TRUE(first);
    EXPECT_FALSE(first->artifact);
    EXPECT_GT(first->partialReports, 0U);
    auto dump = io::joinPath(*dir, "crash.dmp");
    ASSERT_TRUE(dump);
    ASSERT_TRUE(fs.writeFileAtomic(*dump, test::syntheticCrashMinidump()));
    auto ready = (*observer)->poll(fs);
    ASSERT_TRUE(ready);
    ASSERT_TRUE(ready->artifact);
    EXPECT_EQ(ready->artifact->sha256().size(), 64U);
}
TEST(CrashArtifact, LinksReparseAndMutationDuringReadCannotPublishASnapshot) {
    ManualClock clock({}, IClock::SystemTime(1001000ms));
    io::MemFs memory(clock);
    PathFaultFs fs(memory);
    auto source = test::syntheticCrashSource(root, project);
    ASSERT_TRUE(fs.createDirectories(root));
    auto observer = CrashArtifactObserver::capture(fs, source, clock);
    ASSERT_TRUE(observer);
    clock.advance(1s);
    auto dir = test::writeSyntheticCrashReport(fs, source);
    ASSERT_TRUE(dir);
    auto dump = io::joinPath(*dir, "crash.dmp");
    ASSERT_TRUE(dump);
    fs.linked = *dump;
    auto linked = (*observer)->poll(fs);
    ASSERT_TRUE(linked);
    EXPECT_FALSE(linked->artifact);
    fs.linked.clear();
    fs.onRead = [&](std::string_view path) {
        if (path == *dump) {
            clock.advance(1ns);
            static_cast<void>(memory.writeFileAtomic(path, "changed"));
        }
    };
    auto changed = (*observer)->poll(fs);
    ASSERT_TRUE(changed);
    EXPECT_FALSE(changed->artifact);
}
TEST(CrashArtifact, MaterializationCopiesImmutableValidatedBytesAndRejectsCacheLinksOrCorruption) {
    auto artifact = test::makeSyntheticCrashArtifact();
    ASSERT_TRUE(artifact);
    ManualClock clock;
    io::MemFs memory(clock);
    PathFaultFs fs(memory);
    ASSERT_TRUE(fs.createDirectories(root));
    auto cache = (*artifact)->materialize(fs, root);
    ASSERT_TRUE(cache);
    EXPECT_EQ(fs.readFile(*cache).value_or(""), (*artifact)->dumpBytes());
    fs.linked = *cache;
    EXPECT_FALSE((*artifact)->materialize(fs, root));
    fs.linked.clear();
    ASSERT_TRUE(fs.writeFileAtomic(*cache, "not the captured dump"));
    EXPECT_FALSE((*artifact)->materialize(fs, root));
    fs.linked = io::normalizePath(root).value();
    EXPECT_FALSE(validateCrashCacheDirectory(fs, root));
}
TEST(CrashArtifact, CancellationAndBoundedDirectoryRefuseUnboundedObservation) {
    ManualClock clock({}, IClock::SystemTime(1001000ms));
    io::MemFs fs(clock);
    auto source = test::syntheticCrashSource(root, project);
    ASSERT_TRUE(fs.createDirectories(root));
    auto observer = CrashArtifactObserver::capture(fs, source, clock);
    ASSERT_TRUE(observer);
    std::stop_source stop;
    stop.request_stop();
    EXPECT_EQ((*observer)->poll(fs, stop.get_token()).error().code(), ErrorCode::Cancelled);
    for (unsigned i = 0; i < 257; ++i)
        ASSERT_TRUE(fs.createDirectories(io::joinPath(root, std::to_string(i)).value()));
    EXPECT_EQ((*observer)->poll(fs).error().code(), ErrorCode::OutOfRange);
}
} // namespace
} // namespace frost::engine
