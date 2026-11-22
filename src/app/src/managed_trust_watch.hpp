// SPDX-License-Identifier: Apache-2.0
// Signals managed tool operations when their workspace trust revision changes.
#pragma once
#include "frost/project/trust_store.hpp"

#include <chrono>
#include <stop_token>
#include <string>
#include <thread>
namespace frost::app {
// Scoped to a worker, joined before borrowed services die. Observes revision even if
// revoke and regrant happen between GUI ticks; active subprocesses share its stop source.
inline std::jthread watchManagedTrust(project::TrustStore& trust, std::string root, std::uint64_t revision,
                                      std::stop_source operation) {
    return std::jthread([&trust, root = std::move(root), revision,
                         operation = std::move(operation)](const std::stop_token& end) mutable {
        while (!end.stop_requested() && !operation.stop_requested()) {
            const auto current = trust.snapshot(root);
            if (!current.trusted || current.revision != revision) {
                operation.request_stop();
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
    });
}
} // namespace frost::app
