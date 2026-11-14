// SPDX-License-Identifier: Apache-2.0
// Structurally valid synthetic parser fixtures, never real-engine crash acceptance evidence.
#pragma once

#include "frost/engine/crash_artifact.hpp"
#include "frost/io/mem_fs.hpp"

#include <array>

namespace frost::test {
inline void dumpU32(std::string& bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i)
        bytes[offset + i] = static_cast<char>((value >> (i * 8)) & 255);
}
inline std::string syntheticCrashMinidump(std::uint32_t pid = 4242) {
    std::string bytes(560, '\0');
    dumpU32(bytes, 0, 0x504d444d);
    dumpU32(bytes, 4, 0xa793);
    dumpU32(bytes, 8, 5);
    dumpU32(bytes, 12, 32);
    const std::array<std::array<std::uint32_t, 3>, 5> streams{
        {{3, 52, 92}, {4, 112, 144}, {6, 168, 256}, {7, 56, 424}, {15, 24, 480}}};
    for (std::size_t i = 0; i < streams.size(); ++i)
        for (std::size_t j = 0; j < 3; ++j)
            dumpU32(bytes, 32 + i * 12 + j * 4, streams[i][j]);
    dumpU32(bytes, 92, 1);
    dumpU32(bytes, 96, 9);
    dumpU32(bytes, 136, 32);
    dumpU32(bytes, 140, 504);
    dumpU32(bytes, 144, 1);
    dumpU32(bytes, 148, 0x1000);
    dumpU32(bytes, 156, 4096);
    dumpU32(bytes, 168, 536);
    dumpU32(bytes, 256, 9);
    dumpU32(bytes, 264, 0xc0000005);
    dumpU32(bytes, 416, 32);
    dumpU32(bytes, 420, 504);
    dumpU32(bytes, 424, 9);
    dumpU32(bytes, 480, 24);
    dumpU32(bytes, 484, 1);
    dumpU32(bytes, 488, pid);
    dumpU32(bytes, 536, 20);
    const std::string module = "engine.exe";
    for (std::size_t i = 0; i < module.size(); ++i)
        bytes[540 + i * 2] = module[i];
    return bytes;
}
inline engine::CrashArtifactSource syntheticCrashSource(std::string root, std::string project,
                                                        std::int64_t pid = 4242) {
    return engine::CrashArtifactSource{std::string(32, 'a'),
                                       std::move(root),
                                       "artifact-test-child",
                                       std::string(32, 'b'),
                                       std::string(32, 'c'),
                                       std::move(project),
                                       pid,
                                       1000000};
}
inline Result<std::string> writeSyntheticCrashReport(io::IFileSystem& fs, const engine::CrashArtifactSource& source,
                                                     std::string_view name = "20261111T120000Z-4242",
                                                     std::int64_t crashUnixMs = 1002000,
                                                     std::string bytes = syntheticCrashMinidump()) {
    auto dir = io::joinPath(source.rootPath, name);
    if (!dir)
        return std::unexpected(dir.error());
    auto created = fs.createDirectories(*dir);
    if (!created)
        return std::unexpected(created.error());
    auto report = io::joinPath(*dir, "crash.json"), dump = io::joinPath(*dir, "crash.dmp");
    if (!report || !dump)
        return fail(ErrorCode::InvalidArgument, "invalid fixture paths");
    auto metadata = rpc::Json::object()
                        .set("Version", 1)
                        .set("ProcessId", source.processId)
                        .set("StartUnixMs", source.processStartUnixMs)
                        .set("ProcessRole", source.processRole)
                        .set("TimeUnixMs", crashUnixMs)
                        .set("Complete", false);
    auto written = fs.writeFileAtomic(*report, metadata.dump());
    if (!written)
        return std::unexpected(written.error());
    written = fs.writeFileAtomic(*dump, bytes);
    if (!written)
        return std::unexpected(written.error());
    return dir;
}
inline Result<std::shared_ptr<const engine::ValidatedCrashArtifact>> makeSyntheticCrashArtifact() {
    using namespace std::chrono_literals;
    ManualClock clock({}, IClock::SystemTime(1001000ms));
    io::MemFs fs(clock);
#ifdef _WIN32
    const std::string root = "C:/artifact-fixture", project = "C:/trusted-project";
#else
    const std::string root = "/artifact-fixture", project = "/trusted-project";
#endif
    auto source = syntheticCrashSource(root, project);
    auto created = fs.createDirectories(root);
    if (!created)
        return std::unexpected(created.error());
    auto observer = engine::CrashArtifactObserver::capture(fs, source, clock);
    if (!observer)
        return std::unexpected(observer.error());
    clock.advance(1s);
    auto written = writeSyntheticCrashReport(fs, source);
    if (!written)
        return std::unexpected(written.error());
    auto scan = (*observer)->poll(fs);
    if (!scan)
        return std::unexpected(scan.error());
    if (!scan->artifact)
        return fail(ErrorCode::Corrupt, "synthetic crash fixture did not validate");
    return scan->artifact;
}
} // namespace frost::test
