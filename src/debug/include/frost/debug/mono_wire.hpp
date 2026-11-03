// SPDX-License-Identifier: Apache-2.0
// Incremental Mono soft-debugger framing. Transport, endpoint identity and lifecycle are owned by
// the caller; this component never opens a socket or starts/terminates a process.
#pragma once

#include "frost/core/error.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace frost::debug::mono {

enum class MetadataQuery {
    TypeInfo,
    TypeMethods,
    MethodName,
    MethodDebug,
    MethodLocals,
    AssemblyLocation,
    AssemblyImage,
    AssemblyPdb
};

struct Packet {
    std::uint32_t id = 0;
    bool reply = false;
    std::uint16_t error = 0;
    std::uint8_t commandSet = 0;
    std::uint8_t command = 0;
    std::string label;
    std::vector<std::uint8_t> payload;
};

// Single-owner; all methods must be called serially. A protocol error permanently closes the codec.
// The runtime and peer command IDs occupy separate namespaces. Bounds are independent of transport.
class Wire final {
public:
    Wire() = default;
    Wire(const Wire&) = delete ("a Mono connection has one owner");
    Wire& operator=(const Wire&) = delete ("a Mono connection has one owner");
    Wire(Wire&&) = delete ("a Mono connection has one owner");
    Wire& operator=(Wire&&) = delete ("a Mono connection has one owner");
    static constexpr std::size_t maxPacket = 1024 * 1024;
    static constexpr std::size_t maxPending = 128;
    static constexpr char handshake[] = "DWP-Handshake";

    [[nodiscard]] Result<std::vector<Packet>> feed(std::span<const std::uint8_t> bytes);
    [[nodiscard]] Result<std::vector<std::uint8_t>> version(std::string label);
    [[nodiscard]] Result<std::vector<std::uint8_t>> negotiate258(std::string label);
    [[nodiscard]] Result<std::vector<std::uint8_t>> dispose(std::string label);
    [[nodiscard]] Result<std::vector<std::uint8_t>> suspend(std::string label);
    [[nodiscard]] Result<std::vector<std::uint8_t>> resume(std::string label);
    [[nodiscard]] Result<std::vector<std::uint8_t>> allThreads(std::string label);
    [[nodiscard]] Result<std::vector<std::uint8_t>> rootDomain(std::string label);
    [[nodiscard]] Result<std::vector<std::uint8_t>> domainAssemblies(std::uint32_t domain, std::string label);
    [[nodiscard]] Result<std::vector<std::uint8_t>> types(std::string_view name, std::string label);
    [[nodiscard]] Result<std::vector<std::uint8_t>> metadata(MetadataQuery query, std::uint32_t id, std::string label);
    [[nodiscard]] Result<std::vector<std::uint8_t>> methodFromToken(std::uint32_t assembly, std::uint32_t token,
                                                                    std::string label);
    [[nodiscard]] Result<std::vector<std::uint8_t>> breakpoint(std::uint32_t method, std::uint64_t ilOffset,
                                                               std::string label);
    [[nodiscard]] Result<std::vector<std::uint8_t>> stepOver(std::uint32_t thread, std::string label);
    [[nodiscard]] Result<std::vector<std::uint8_t>> clearEvent(std::uint8_t kind, std::uint32_t id, std::string label);
    [[nodiscard]] Result<std::vector<std::uint8_t>> frames(std::uint32_t thread, std::string label);
    [[nodiscard]] Result<std::vector<std::uint8_t>> local(std::uint32_t thread, std::uint32_t frame,
                                                          std::uint32_t pdbOrdinal, std::string label);
    [[nodiscard]] bool ready() const noexcept { return m_ready && !m_closed; }
    void close() noexcept;

private:
    [[nodiscard]] Result<std::vector<std::uint8_t>> request(std::uint8_t commandSet, std::uint8_t command,
                                                            std::span<const std::uint8_t> payload, std::string label);
    std::vector<std::uint8_t> m_buffer;
    std::unordered_map<std::uint32_t, std::string> m_pending;
    std::uint32_t m_nextId = 1;
    bool m_ready = false;
    bool m_closed = false;
};

} // namespace frost::debug::mono
