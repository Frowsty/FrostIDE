// SPDX-License-Identifier: Apache-2.0
// Windows selected-process/reverse-TCP tuple verifier for the Mono transport.
#pragma once
#include "frost/core/error.hpp"
#include "frost/debug/mono_transport.hpp"

#include <cstdint>
#include <functional>
#include <memory>

namespace frost::debug::mono {
// `pid` and `creationTime` must come from an independently authenticated selected Editor/fixture.
// `current` must recheck trust and the selected project/session generation without blocking.
// This verifier binds TCP ownership; it does not authenticate the project bridge or source bytes.
class WindowsPeer final {
public:
    [[nodiscard]] static Result<std::shared_ptr<WindowsPeer>> open(std::uint32_t pid, std::uint64_t creationTime,
                                                                   std::function<bool()> current);
    ~WindowsPeer();
    WindowsPeer(const WindowsPeer&) = delete ("one held process identity");
    WindowsPeer& operator=(const WindowsPeer&) = delete ("one held process identity");
    [[nodiscard]] Result<void> verify(std::uint16_t localPort, std::uint16_t remotePort, MonoDeadline deadline,
                                      std::stop_token stop) const;
    struct Impl;

private:
    explicit WindowsPeer(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> m_impl;
};
} // namespace frost::debug::mono
