// SPDX-License-Identifier: Apache-2.0
// Encode and parse bounded Mono soft-debugger wire packets.
#include "frost/debug/mono_wire.hpp"

#include "frost/io/utf.hpp"

#include <algorithm>
#include <array>
#include <utility>

namespace frost::debug::mono {
namespace {
constexpr std::size_t header = 11;
std::uint32_t read32(const std::uint8_t* p) {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3];
}
void append32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value >> 24));
    out.push_back(static_cast<std::uint8_t>(value >> 16));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value));
}
} // namespace

void Wire::close() noexcept {
    m_closed = true;
    m_buffer.clear();
    m_pending.clear();
}

Result<std::vector<std::uint8_t>> Wire::request(std::uint8_t commandSet, std::uint8_t command,
                                                std::span<const std::uint8_t> payload, std::string label) {
    if (!ready() || label.empty())
        return fail(ErrorCode::InvalidArgument, "Mono handshake or label missing");
    if (payload.size() > maxPacket - header || m_pending.size() >= maxPending)
        return fail(ErrorCode::OutOfRange, "Mono request limit exceeded");
    auto id = m_nextId;
    while (m_pending.contains(id)) {
        ++id;
        if (id == 0)
            id = 1;
    }
    m_nextId = id + 1;
    if (m_nextId == 0)
        m_nextId = 1;
    m_pending.emplace(id, std::move(label));
    std::vector<std::uint8_t> out;
    out.reserve(header + payload.size());
    append32(out, static_cast<std::uint32_t>(header + payload.size()));
    append32(out, id);
    out.insert(out.end(), {0, commandSet, command});
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

Result<std::vector<std::uint8_t>> Wire::version(std::string label) { return request(1, 1, {}, std::move(label)); }
Result<std::vector<std::uint8_t>> Wire::dispose(std::string label) { return request(1, 6, {}, std::move(label)); }
Result<std::vector<std::uint8_t>> Wire::suspend(std::string label) { return request(1, 3, {}, std::move(label)); }
Result<std::vector<std::uint8_t>> Wire::resume(std::string label) { return request(1, 4, {}, std::move(label)); }
Result<std::vector<std::uint8_t>> Wire::negotiate258(std::string label) {
    constexpr std::array<std::uint8_t, 8> payload{0, 0, 0, 2, 0, 0, 0, 58};
    return request(1, 8, payload, std::move(label));
}

Result<std::vector<std::uint8_t>> Wire::allThreads(std::string label) { return request(1, 2, {}, std::move(label)); }
Result<std::vector<std::uint8_t>> Wire::rootDomain(std::string label) { return request(20, 1, {}, std::move(label)); }
Result<std::vector<std::uint8_t>> Wire::domainAssemblies(std::uint32_t domain, std::string label) {
    if (!domain)
        return fail(ErrorCode::InvalidArgument, "invalid Mono domain ID");
    std::vector<std::uint8_t> p;
    append32(p, domain);
    return request(20, 3, p, std::move(label));
}

Result<std::vector<std::uint8_t>> Wire::types(std::string_view name, std::string label) {
    if (name.empty() || name.size() > 4096 || name.find('\0') != std::string_view::npos ||
        io::validUtf8Prefix(name) != name.size())
        return fail(ErrorCode::InvalidArgument, "invalid Mono type name");
    std::vector<std::uint8_t> p;
    append32(p, static_cast<std::uint32_t>(name.size()));
    p.insert(p.end(), name.begin(), name.end());
    p.push_back(0);
    return request(1, 12, p, std::move(label));
}

Result<std::vector<std::uint8_t>> Wire::metadata(MetadataQuery query, std::uint32_t id, std::string label) {
    if (!id)
        return fail(ErrorCode::InvalidArgument, "invalid Mono metadata ID");
    std::uint8_t set = 0;
    std::uint8_t command = 0;
    switch (query) {
    case MetadataQuery::TypeInfo:
        set = 23;
        command = 1;
        break;
    case MetadataQuery::TypeMethods:
        set = 23;
        command = 2;
        break;
    case MetadataQuery::MethodName:
        set = 22;
        command = 1;
        break;
    case MetadataQuery::MethodDebug:
        set = 22;
        command = 3;
        break;
    case MetadataQuery::MethodLocals:
        set = 22;
        command = 5;
        break;
    case MetadataQuery::AssemblyLocation:
        set = 21;
        command = 1;
        break;
    case MetadataQuery::AssemblyImage:
        set = 21;
        command = 8;
        break;
    case MetadataQuery::AssemblyPdb:
        set = 21;
        command = 10;
        break;
    default: return fail(ErrorCode::InvalidArgument, "invalid Mono metadata query");
    }
    std::vector<std::uint8_t> p;
    append32(p, id);
    return request(set, command, p, std::move(label));
}

Result<std::vector<std::uint8_t>> Wire::methodFromToken(std::uint32_t assembly, std::uint32_t token,
                                                        std::string label) {
    if (!assembly || (token & 0xff000000U) != 0x06000000U || !(token & 0x00ffffffU))
        return fail(ErrorCode::InvalidArgument, "invalid Mono assembly or MethodDef token");
    std::vector<std::uint8_t> p;
    append32(p, assembly);
    append32(p, token);
    return request(21, 12, p, std::move(label));
}

Result<std::vector<std::uint8_t>> Wire::breakpoint(std::uint32_t method, std::uint64_t ilOffset, std::string label) {
    if (!method || ilOffset > 0x7fffffffffffffffULL)
        return fail(ErrorCode::InvalidArgument, "invalid Mono breakpoint location");
    std::vector<std::uint8_t> p{10, 2, 1, 7};
    append32(p, method);
    append32(p, static_cast<std::uint32_t>(ilOffset >> 32));
    append32(p, static_cast<std::uint32_t>(ilOffset));
    return request(15, 1, p, std::move(label));
}

Result<std::vector<std::uint8_t>> Wire::stepOver(std::uint32_t thread, std::string label) {
    if (!thread)
        return fail(ErrorCode::InvalidArgument, "invalid Mono thread ID");
    std::vector<std::uint8_t> p{11, 2, 1, 10};
    append32(p, thread);
    append32(p, 1);
    append32(p, 1);
    append32(p, 0);
    return request(15, 1, p, std::move(label));
}

Result<std::vector<std::uint8_t>> Wire::clearEvent(std::uint8_t kind, std::uint32_t id, std::string label) {
    if ((kind != 10 && kind != 11) || !id)
        return fail(ErrorCode::InvalidArgument, "invalid Mono event request");
    std::vector<std::uint8_t> p{kind};
    append32(p, id);
    return request(15, 2, p, std::move(label));
}

Result<std::vector<std::uint8_t>> Wire::frames(std::uint32_t thread, std::string label) {
    if (!thread)
        return fail(ErrorCode::InvalidArgument, "invalid Mono thread ID");
    std::vector<std::uint8_t> p;
    append32(p, thread);
    append32(p, 0);
    append32(p, 0xffffffffU);
    return request(11, 1, p, std::move(label));
}

Result<std::vector<std::uint8_t>> Wire::local(std::uint32_t thread, std::uint32_t frame, std::uint32_t pdbOrdinal,
                                              std::string label) {
    if (!thread || pdbOrdinal > 0x7fffffffU)
        return fail(ErrorCode::InvalidArgument, "invalid Mono local");
    std::vector<std::uint8_t> p;
    append32(p, thread);
    append32(p, frame);
    append32(p, 1);
    append32(p, pdbOrdinal);
    return request(16, 1, p, std::move(label));
}

Result<std::vector<Packet>> Wire::feed(std::span<const std::uint8_t> bytes) {
    if (m_closed)
        return fail(ErrorCode::Cancelled, "Mono codec closed");
    auto reject = [this](std::string message) -> Result<std::vector<Packet>> {
        close();
        return fail(ErrorCode::Corrupt, std::move(message));
    };
    if (bytes.size() > maxPacket + sizeof(handshake) - 1 - m_buffer.size())
        return reject("Mono input buffer exceeds limit");
    m_buffer.insert(m_buffer.end(), bytes.begin(), bytes.end());
    if (!m_ready) {
        const auto count = std::min(m_buffer.size(), sizeof(handshake) - 1);
        for (std::size_t i = 0; i < count; ++i)
            if (m_buffer[i] != static_cast<std::uint8_t>(handshake[i]))
                return reject("invalid Mono handshake");
        if (count < sizeof(handshake) - 1)
            return std::vector<Packet>{};
        m_buffer.erase(m_buffer.begin(), m_buffer.begin() + (sizeof(handshake) - 1));
        m_ready = true;
    }
    std::vector<Packet> result;
    std::size_t cursor = 0;
    while (m_buffer.size() - cursor >= 4) {
        const auto* current = m_buffer.data() + cursor;
        const auto length = read32(current);
        if (length < header || length > maxPacket)
            return reject("invalid Mono packet length");
        if (m_buffer.size() - cursor < length)
            break;
        Packet p;
        p.id = read32(current + 4);
        const auto flags = current[8];
        if (flags == 0x80) {
            auto it = m_pending.find(p.id);
            if (it == m_pending.end())
                return reject("unexpected Mono reply");
            p.reply = true;
            p.error = static_cast<std::uint16_t>((std::uint16_t(current[9]) << 8) | current[10]);
            p.label = std::move(it->second);
            m_pending.erase(it);
        } else if (flags == 0 && current[9] == 64 && current[10] == 100) {
            p.commandSet = current[9];
            p.command = current[10];
        } else {
            return reject("unexpected Mono packet flags or command");
        }
        p.payload.assign(current + header, current + length);
        result.push_back(std::move(p));
        cursor += length;
    }
    m_buffer.erase(m_buffer.begin(),
                   m_buffer.begin() + static_cast<std::vector<std::uint8_t>::difference_type>(cursor));
    return result;
}
} // namespace frost::debug::mono
