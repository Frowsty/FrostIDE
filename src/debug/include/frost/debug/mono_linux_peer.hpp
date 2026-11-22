// SPDX-License-Identifier: Apache-2.0
// Bind an actual Linux Mono socket inode to a held pidfd and selected process identity.
#pragma once
#include "frost/debug/mono_transport.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
namespace frost::debug::mono {
struct LinuxProcessIdentity {
    std::uint32_t pid = 0;
    std::uint64_t startTime = 0;
    std::string image;
};
// Pure bounded /proc/net/tcp parser. Tuple ports are from the IDE side; ownership is checked separately.
[[nodiscard]] Result<std::uint64_t> linuxMonoSocketInode(std::string_view table, std::uint16_t localPort,
                                                         std::uint16_t remotePort);
// Select never connects or sends bytes. Requires Linux pidfd/procfs, same UID/network namespace.
// Single immutable process selection; verify may run serially from the transport worker. current
// must be thread-safe and recheck exact trust/session revision. Missing ownership evidence refuses.
class LinuxPeer final {
public:
    [[nodiscard]] static Result<std::shared_ptr<LinuxPeer>> select(std::uint32_t pid, std::function<bool()> current);
    [[nodiscard]] LinuxProcessIdentity identity() const;
    [[nodiscard]] Result<void> verify(std::uint16_t localPort, std::uint16_t remotePort, MonoDeadline deadline,
                                      const std::stop_token& stop) const;
    ~LinuxPeer();
    struct Impl;

private:
    explicit LinuxPeer(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> m_impl;
};
} // namespace frost::debug::mono
