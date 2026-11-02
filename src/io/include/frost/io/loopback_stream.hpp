// SPDX-License-Identifier: Apache-2.0
// Bounded, cancellable TCP connection to the local IPv4 loopback address only.
#pragma once

#include "frost/core/error.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>

namespace frost::io {
// One connection, owned and used by a worker thread. No name resolution or remote addresses.
// A single absolute deadline covers connect and every request on the connection. Destruction
// closes the socket; cancellation and deadline are observed while waiting for I/O.
class LoopbackStream {
public:
    using Deadline = std::chrono::steady_clock::time_point;
    [[nodiscard]] static Result<std::unique_ptr<LoopbackStream>> connect(std::uint16_t port, Deadline deadline,
                                                                         std::stop_token stop = {});
    ~LoopbackStream();
    LoopbackStream(const LoopbackStream&) = delete;
    LoopbackStream& operator=(const LoopbackStream&) = delete;
    // Sends one line and reads one line without its newline. Both are bounded to 16 MiB;
    // embedded newlines in the outgoing message are refused.
    [[nodiscard]] Result<std::string> exchange(std::string_view line);
    struct Impl;

private:
    explicit LoopbackStream(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> m_impl;
};
} // namespace frost::io
