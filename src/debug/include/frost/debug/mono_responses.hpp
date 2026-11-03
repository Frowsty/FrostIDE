// SPDX-License-Identifier: Apache-2.0
// Narrow bounded decoders for the Mono 2.58 forms exercised by the standalone oracle.
#pragma once
#include "frost/core/error.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace frost::debug::mono {
struct StopEvent {
    std::uint8_t kind = 0;
    std::uint32_t request = 0;
    std::uint32_t thread = 0;
    std::uint32_t method = 0;
    std::uint64_t rawOffset = 0;
};
struct Frame {
    std::uint32_t id = 0;
    std::uint32_t method = 0;
    std::int32_t ilOffset = 0;
    std::uint8_t flags = 0;
};
struct LocalInfo {
    std::uint32_t type = 0;
    std::string name;
    std::int32_t start = 0;
    std::int32_t end = 0;
};
struct SourceInfo {
    std::string path;
    std::array<std::uint8_t, 16> runtimeHash{};
};
struct SequencePoint {
    std::int32_t ilOffset = 0;
    std::int32_t line = 0;
    std::int32_t source = -1;
    std::int32_t column = 0;
    std::int32_t endLine = 0;
    std::int32_t endColumn = 0;
};
struct MethodDebugInfo {
    std::int32_t codeSize = 0;
    std::vector<SourceInfo> sources;
    std::vector<SequencePoint> points;
};
[[nodiscard]] Result<std::string> decodeString(std::span<const std::uint8_t> payload);
[[nodiscard]] Result<std::uint32_t> decodeId(std::span<const std::uint8_t> payload);
[[nodiscard]] Result<std::vector<std::uint32_t>> decodeIds(std::span<const std::uint8_t> payload);
[[nodiscard]] Result<StopEvent> decodeStop(std::span<const std::uint8_t> payload);
[[nodiscard]] Result<std::vector<Frame>> decodeFrames(std::span<const std::uint8_t> payload);
[[nodiscard]] Result<std::int32_t> decodeInt32(std::span<const std::uint8_t> payload);
[[nodiscard]] Result<std::vector<LocalInfo>> decodeLocals258(std::span<const std::uint8_t> payload);
[[nodiscard]] Result<MethodDebugInfo> decodeDebugInfo258(std::span<const std::uint8_t> payload);
// The expected bytes must be the retained, verified snapshot for this session generation.
[[nodiscard]] Result<void> compareLoadedBlob(std::span<const std::uint8_t> payload,
                                             std::span<const std::uint8_t> expected);
// Frames must come from event.thread; the selected source path and sequence point must already
// have been verified by the caller for the current session generation.
[[nodiscard]] bool matchesBreakpoint(const StopEvent& event, std::span<const Frame> frames, std::uint32_t request,
                                     std::uint32_t method, std::int32_t ilOffset) noexcept;
} // namespace frost::debug::mono
