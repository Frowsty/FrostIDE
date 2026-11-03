// SPDX-License-Identifier: Apache-2.0
// Binary loopback transport for Mono. This class does not authenticate an Editor on its own.
#pragma once
#include "frost/core/error.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <stop_token>

namespace frost::debug::mono {
// The verifier must independently bind this established reverse tuple to the selected, still-live
// process identity and current trusted session. It is called after connect, before any bytes, and
// around each I/O operation. It must be bounded, observe the same deadline/cancellation and must
// not call into or destroy the transport. An identity error closes the connection. All methods
// are single-owner; cancellation is via the supplied stop token, not concurrent close().
using MonoDeadline = std::chrono::steady_clock::time_point;
using VerifyPeer = std::function<Result<void>(std::uint16_t localPort, std::uint16_t remotePort, MonoDeadline deadline,
                                              std::stop_token stop)>;
class BinaryLoopback final {
public:
    using Deadline = MonoDeadline;
    [[nodiscard]] static Result<std::unique_ptr<BinaryLoopback>> connect(std::uint16_t port, Deadline deadline,
                                                                         std::stop_token stop, VerifyPeer verify);
    ~BinaryLoopback();
    BinaryLoopback(const BinaryLoopback&) = delete ("one owner");
    BinaryLoopback& operator=(const BinaryLoopback&) = delete ("one owner");
    // Single-owner only. Renew before the current operation deadline expires; the held peer is
    // revalidated before the new deadline takes effect. Failure closes the transport.
    [[nodiscard]] Result<void> renewDeadline(Deadline deadline);
    // A bounded idle poll: false means no bytes became ready by `until`, not a transport failure.
    // The operation deadline, cancellation and peer verification still fail closed.
    [[nodiscard]] Result<bool> pollReadable(Deadline until);
    // On any write error a prefix may already have been sent; the connection is closed.
    [[nodiscard]] Result<void> write(std::span<const std::uint8_t> bytes);
    // Reads 1..buffer.size() bytes; a zero-sized buffer is rejected. EOF is an error.
    [[nodiscard]] Result<std::size_t> read(std::span<std::uint8_t> buffer);
    void close() noexcept;
    struct Impl;

private:
    explicit BinaryLoopback(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> m_impl;
};
} // namespace frost::debug::mono
