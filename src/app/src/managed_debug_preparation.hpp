// SPDX-License-Identifier: Apache-2.0
// Private coordinator for one trusted managed console build and PDB source verification.
#pragma once

#include "frost/build/managed_project.hpp"
#include "frost/debug/coreclr_dap.hpp"

#include <QObject>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace frost::project {
class TrustStore;
}

namespace frost::app {

struct PreparedManagedDebug {
    debug::CoreClrTarget target{};
    struct Source {
        std::string path;
        std::string sha256;
    };
    std::string moduleId;
    std::string assemblySha256;
    std::string pdbSha256;
    std::vector<Source> sources{};
};

/* GUI-thread coordinator. The caller must save documents before start and provide a GUI-thread
 * predicate that becomes false on workspace switch, revocation or any relevant source edit. It
 * must continue enforcing source validity after completion and before presenting a paused frame.
 * The coordinator owns each cancellable process tree, stops on failed validation, and posts its
 * one completion to the GUI thread. Dependencies outlive this object. This is not a UI entry point.
 */
class ManagedDebugPreparation final : public QObject {
public:
    using Completion = std::function<void(Result<PreparedManagedDebug>)>;
    using StillCurrent = std::function<bool()>;
    using Output = std::function<void(build::OutputStream, std::string_view)>;

    ManagedDebugPreparation(io::IProcessLauncher& launcher, project::TrustStore& trust, IClock& clock,
                            QObject* parent = nullptr);
    ~ManagedDebugPreparation() override;
    ManagedDebugPreparation(const ManagedDebugPreparation&) = delete;
    ManagedDebugPreparation& operator=(const ManagedDebugPreparation&) = delete;

    // Starts build -> property query -> PDB/source verification; rejects overlapping work.
    [[nodiscard]] Result<void> start(build::ManagedProjectRequest request, std::vector<std::string> trustRoots,
                                     std::string guardAssembly, StillCurrent stillCurrent, Completion done,
                                     Output output = {});
    // Asks the active owned process tree to terminate; completion reports Cancelled.
    void cancel();
    [[nodiscard]] bool active() const noexcept { return m_active; }

private:
    struct Relay;
    enum class Phase : unsigned char { Build, Query, SourceCheck };
    [[nodiscard]] Result<void> launch(Phase phase);
    void finished(Phase phase, build::BuildResult result, std::string output, std::string error, bool overflow);
    void complete(Result<PreparedManagedDebug> result);
    [[nodiscard]] bool current() const;

    io::IProcessLauncher& m_launcher;
    project::TrustStore& m_trust;
    IClock& m_clock;
    std::shared_ptr<Relay> m_relay;
    std::unique_ptr<build::BuildSession> m_session;
    build::ManagedProjectRequest m_request{};
    std::vector<std::string> m_roots{};
    std::string m_guard;
    std::string m_assembly;
    StillCurrent m_stillCurrent{};
    Completion m_done{};
    Output m_output{};
    std::uint64_t m_generation = 0;
    bool m_active = false;
    bool m_cancelled = false;
};

} // namespace frost::app
