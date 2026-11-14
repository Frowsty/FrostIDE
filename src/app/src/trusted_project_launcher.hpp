// SPDX-License-Identifier: Apache-2.0
// Project tool admission for asynchronous inspections and test jobs. A captured trust revision
// is checked at each child launch, including launches delayed by a resource wait or batch queue.
#pragma once

#include "frost/io/async_process.hpp"
#include "frost/io/process.hpp"
#include "frost/project/trust_store.hpp"

#include <stop_token>
#include <string>
#include <utility>

namespace frost::app {

// Thread-safe immutable launcher adapter. Its launcher and trust store must outlive it and every
// child it starts. Revocation (even followed by a new grant) invalidates the captured admission;
// a stop request rejects later launches with Cancelled. It does not own already-running children.
class TrustedProjectLauncher final : public io::IProcessLauncher {
public:
    TrustedProjectLauncher(io::IProcessLauncher& launcher, project::TrustStore& trust, std::string root,
                           std::uint64_t revision, std::stop_token stop = {})
        : m_launcher(launcher), m_trust(trust), m_root(std::move(root)), m_revision(revision), m_stop(std::move(stop)) {
    }

    [[nodiscard]] Result<std::unique_ptr<io::IProcess>> launch(const io::ProcessSpec& spec) override {
        if (auto allowed = check(); !allowed)
            return std::unexpected(allowed.error());
        return m_launcher.launch(spec);
    }

    [[nodiscard]] Result<std::unique_ptr<io::IAsyncProcess>> launchAsync(const io::ProcessSpec& spec,
                                                                         const io::AsyncContext& context) override {
        if (auto allowed = check(); !allowed)
            return std::unexpected(allowed.error());
        return m_launcher.launchAsync(spec, context);
    }

private:
    [[nodiscard]] Result<void> check() const {
        if (m_stop.stop_requested())
            return fail(ErrorCode::Cancelled, "Project tool operation was cancelled");
        const auto current = m_trust.snapshot(m_root);
        if (!current.trusted || current.revision != m_revision)
            return fail(ErrorCode::Untrusted, "Project trust changed before the tool could start");
        return {};
    }

    io::IProcessLauncher& m_launcher;
    project::TrustStore& m_trust;
    const std::string m_root;
    const std::uint64_t m_revision;
    const std::stop_token m_stop;
};

} // namespace frost::app
