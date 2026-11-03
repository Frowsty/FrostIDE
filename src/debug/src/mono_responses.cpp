// SPDX-License-Identifier: Apache-2.0
// Decode bounded Mono runtime replies and source-bound stop data.
#include "frost/debug/mono_responses.hpp"

#include "frost/io/utf.hpp"

#include <algorithm>
#include <bit>
#include <limits>

namespace frost::debug::mono {
namespace {
class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> data) : m_data(data) {}
    bool byte(std::uint8_t& value) {
        if (m_pos >= m_data.size())
            return false;
        value = m_data[m_pos++];
        return true;
    }
    bool u32(std::uint32_t& value) {
        if (m_data.size() - m_pos < 4)
            return false;
        value = (std::uint32_t(m_data[m_pos]) << 24) | (std::uint32_t(m_data[m_pos + 1]) << 16) |
                (std::uint32_t(m_data[m_pos + 2]) << 8) | m_data[m_pos + 3];
        m_pos += 4;
        return true;
    }
    bool i32(std::int32_t& value) {
        std::uint32_t raw = 0;
        if (!u32(raw))
            return false;
        value = std::bit_cast<std::int32_t>(raw);
        return true;
    }
    bool u64(std::uint64_t& value) {
        std::uint32_t hi = 0, lo = 0;
        if (!u32(hi) || !u32(lo))
            return false;
        value = (std::uint64_t(hi) << 32) | lo;
        return true;
    }
    bool string(std::string& value) {
        std::uint32_t n = 0;
        if (!u32(n) || n > 4096 || n > m_data.size() - m_pos)
            return false;
        value.assign(reinterpret_cast<const char*>(m_data.data() + m_pos), n);
        m_pos += n;
        return value.find('\0') == std::string::npos && io::validUtf8Prefix(value) == value.size();
    }
    bool hash(std::array<std::uint8_t, 16>& value) {
        if (m_data.size() - m_pos < value.size())
            return false;
        for (auto& byte : value)
            byte = m_data[m_pos++];
        return true;
    }
    [[nodiscard]] bool done() const { return m_pos == m_data.size(); }

private:
    std::span<const std::uint8_t> m_data;
    std::size_t m_pos = 0;
};
} // namespace

Result<std::string> decodeString(std::span<const std::uint8_t> payload) {
    Reader r(payload);
    std::string result;
    if (!r.string(result) || !r.done())
        return fail(ErrorCode::Corrupt, "invalid Mono string");
    return result;
}
Result<std::uint32_t> decodeId(std::span<const std::uint8_t> payload) {
    Reader r(payload);
    std::uint32_t id = 0;
    if (!r.u32(id) || !id || !r.done())
        return fail(ErrorCode::Corrupt, "invalid Mono object ID");
    return id;
}
Result<std::vector<std::uint32_t>> decodeIds(std::span<const std::uint8_t> payload) {
    Reader r(payload);
    std::uint32_t count = 0;
    if (!r.u32(count) || count > 16384)
        return fail(ErrorCode::Corrupt, "invalid Mono ID count");
    std::vector<std::uint32_t> result;
    result.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint32_t id = 0;
        if (!r.u32(id) || !id)
            return fail(ErrorCode::Corrupt, "invalid Mono object ID list");
        result.push_back(id);
    }
    if (!r.done())
        return fail(ErrorCode::Corrupt, "trailing Mono ID data");
    return result;
}

Result<StopEvent> decodeStop(std::span<const std::uint8_t> payload) {
    Reader r(payload);
    std::uint8_t policy = 0;
    std::uint32_t count = 0;
    StopEvent e;
    if (!r.byte(policy) || policy != 2 || !r.u32(count) || count != 1 || !r.byte(e.kind) ||
        (e.kind != 10 && e.kind != 11) || !r.u32(e.request) || !e.request || !r.u32(e.thread) || !e.thread ||
        !r.u32(e.method) || !e.method || !r.u64(e.rawOffset) || !r.done())
        return fail(ErrorCode::Corrupt, "invalid Mono stop event");
    return e;
}

Result<std::vector<Frame>> decodeFrames(std::span<const std::uint8_t> payload) {
    Reader r(payload);
    std::uint32_t count = 0;
    if (!r.u32(count) || count > 128)
        return fail(ErrorCode::Corrupt, "invalid Mono frame count");
    std::vector<Frame> result;
    result.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        Frame f;
        if (!r.u32(f.id) || !r.u32(f.method) || !r.i32(f.ilOffset) || !r.byte(f.flags))
            return fail(ErrorCode::Corrupt, "truncated Mono frame");
        result.push_back(f);
    }
    if (!r.done())
        return fail(ErrorCode::Corrupt, "trailing Mono frames data");
    return result;
}

Result<std::int32_t> decodeInt32(std::span<const std::uint8_t> payload) {
    Reader r(payload);
    std::uint8_t type = 0;
    std::int32_t value = 0;
    if (!r.byte(type) || type != 8 || !r.i32(value) || !r.done())
        return fail(ErrorCode::Corrupt, "invalid Mono Int32 value");
    return value;
}

Result<std::vector<LocalInfo>> decodeLocals258(std::span<const std::uint8_t> payload) {
    Reader r(payload);
    std::uint32_t count = 0;
    if (!r.u32(count) || count > 4096)
        return fail(ErrorCode::Corrupt, "invalid Mono scope count");
    for (std::uint32_t i = 0; i < count; ++i) {
        std::int32_t delta = 0, length = 0;
        if (!r.i32(delta) || !r.i32(length) || length < 0)
            return fail(ErrorCode::Corrupt, "invalid Mono scope");
    }
    if (!r.u32(count) || count > 4096)
        return fail(ErrorCode::Corrupt, "invalid Mono local count");
    std::vector<LocalInfo> result(count);
    for (auto& local : result)
        if (!r.u32(local.type) || !local.type)
            return fail(ErrorCode::Corrupt, "invalid Mono local type");
    for (auto& local : result)
        if (!r.string(local.name))
            return fail(ErrorCode::Corrupt, "invalid Mono local name");
    for (auto& local : result)
        if (!r.i32(local.start) || !r.i32(local.end) || local.start < 0 || local.end < local.start)
            return fail(ErrorCode::Corrupt, "invalid Mono local scope");
    if (!r.done())
        return fail(ErrorCode::Corrupt, "trailing Mono locals data");
    return result;
}

Result<MethodDebugInfo> decodeDebugInfo258(std::span<const std::uint8_t> payload) {
    Reader r(payload);
    MethodDebugInfo result;
    std::uint32_t count = 0;
    if (!r.i32(result.codeSize) || result.codeSize < 0 || !r.u32(count) || count > 1024)
        return fail(ErrorCode::Corrupt, "invalid Mono source count");
    result.sources.resize(count);
    for (auto& source : result.sources)
        if (!r.string(source.path) || !r.hash(source.runtimeHash))
            return fail(ErrorCode::Corrupt, "invalid Mono source record");
    if (!r.u32(count) || count > 16384)
        return fail(ErrorCode::Corrupt, "invalid Mono sequence point count");
    result.points.resize(count);
    for (auto& point : result.points) {
        if (!r.i32(point.ilOffset) || !r.i32(point.line) || !r.i32(point.source) || !r.i32(point.column) ||
            !r.i32(point.endLine) || !r.i32(point.endColumn) || point.ilOffset < 0 ||
            point.ilOffset > result.codeSize || point.source < -1 ||
            (point.source >= 0 && static_cast<std::size_t>(point.source) >= result.sources.size()))
            return fail(ErrorCode::Corrupt, "invalid Mono sequence point");
    }
    if (!r.done())
        return fail(ErrorCode::Corrupt, "trailing Mono method debug data");
    return result;
}

bool matchesBreakpoint(const StopEvent& event, std::span<const Frame> frames, std::uint32_t request,
                       std::uint32_t method, std::int32_t ilOffset) noexcept {
    return event.kind == 10 && event.thread != 0 && request != 0 && method != 0 && ilOffset >= 0 &&
           event.request == request && event.method == method && !frames.empty() && frames.front().method == method &&
           frames.front().ilOffset == ilOffset &&
           (event.rawOffset == 0 || event.rawOffset == static_cast<std::uint64_t>(ilOffset));
}

Result<void> compareLoadedBlob(std::span<const std::uint8_t> payload, std::span<const std::uint8_t> expected) {
    constexpr std::size_t maxBlob = 1024 * 1024 - 15; // Packet header plus byte-array length.
    if (payload.size() < 4 || expected.empty() || expected.size() > maxBlob)
        return fail(ErrorCode::Corrupt, "invalid Mono image or symbol snapshot");
    Reader reader(payload);
    std::uint32_t size = 0;
    if (!reader.u32(size) || size != expected.size() || payload.size() != expected.size() + 4 ||
        !std::equal(expected.begin(), expected.end(), payload.begin() + 4))
        return fail(ErrorCode::Corrupt, "loaded Mono bytes differ from verified snapshot");
    return {};
}
} // namespace frost::debug::mono
