// SPDX-License-Identifier: Apache-2.0
// Current-user, project-keyed local IPC for forwarding bounded open requests.
#pragma once
#include "frost/core/error.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace frost::io {
// Messages (request and reply) are UTF-8 bytes with a 64 KiB limit. Handlers run serially on a
// dedicated worker and must finish promptly. A server reservation prevents competing owners;
// start() begins listening after the caller's first frame. Destructor stops and joins the worker.
class LocalIpcOwner {
public:
    using Handler = std::function<Result<std::string>(std::string)>;
    // `key` is exactly 64 lowercase hex bytes (a SHA-256 project identity). Busy means an owner
    // already exists; no listener is created until start().
    [[nodiscard]] static Result<std::unique_ptr<LocalIpcOwner>> claim(std::string_view key);
    ~LocalIpcOwner();
    LocalIpcOwner(const LocalIpcOwner&) = delete;
    LocalIpcOwner& operator=(const LocalIpcOwner&) = delete;
    // Launches the listener once. Errors include IoError and Busy. Thread-safe against clients;
    // owner methods are called on one thread and handler must outlive stop().
    [[nodiscard]] Result<void> start(Handler handler);
    void stop() noexcept;
    struct Impl;

private:
    explicit LocalIpcOwner(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> m_impl;
};
// Bounded request/reply to the current owner, retrying while it starts. Timeout is a wall-clock
// bound for connection, transmission and reply. Errors: InvalidArgument, Timeout, IoError, or the
// server's reported error. Never searches or opens a network endpoint.
[[nodiscard]] Result<std::string> requestLocalIpc(std::string_view key, std::string_view request,
                                                  std::chrono::milliseconds timeout);
} // namespace frost::io
