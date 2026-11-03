// SPDX-License-Identifier: Apache-2.0
// Bounded, cancellable TCP connection to the local IPv4 loopback address only.
#pragma once

#include "frost/core/error.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>

namespace frost::io {
// A connection to IPv4 loopback only. The line-exchange mode is owned by one worker; its absolute
// deadline and cancellation cover the connection and every exchange. Duplex mode supports one
// reader and one writer, with a deadline only for connection. Close it and join both threads before
// destruction; closing wakes pending I/O. Do not mix the two modes.
class LoopbackStream {
public:
    using Deadline = std::chrono::steady_clock::time_point;
    [[nodiscard]] static Result<std::unique_ptr<LoopbackStream>> connect(std::uint16_t port, Deadline deadline,
                                                                         std::stop_token stop = {});
    // Connects with a deadline, then permits an indefinitely lived duplex session.
    [[nodiscard]] static Result<std::unique_ptr<LoopbackStream>> connectDuplex(std::uint16_t port, Deadline deadline);
    ~LoopbackStream();
    LoopbackStream(const LoopbackStream&) = delete;
    LoopbackStream& operator=(const LoopbackStream&) = delete;
    // Sends one line and reads one line without its newline. The caller may impose a smaller
    // reply bound; both directions are capped at 16 MiB. Embedded outgoing newlines are refused.
    [[nodiscard]] Result<std::string> exchange(std::string_view line, std::size_t maxReplyBytes = 16 * 1024 * 1024);
    // After a single-threaded handshake, remove the exchange deadline before starting duplex I/O.
    // Precondition: no other operation is running and exchange() has not buffered unread bytes.
    void beginDuplex() noexcept;
    // Duplex mode: one reader and one writer may run concurrently; close may run on any thread.
    // Do not mix with exchange(). The owner joins both threads before destruction.
    [[nodiscard]] Result<std::size_t> read(std::span<char> buffer);
    [[nodiscard]] Result<void> write(std::string_view bytes);
    void close() noexcept;
    struct Impl;

private:
    explicit LoopbackStream(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> m_impl;
};
} // namespace frost::io
