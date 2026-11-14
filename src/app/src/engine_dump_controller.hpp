// SPDX-License-Identifier: Apache-2.0
// FER-16: offer immutable locally observed crash bytes, then open an explicit read-only dump.
#pragma once

#include "frost/debug/debug_session.hpp"
#include "frost/engine/crash_artifact.hpp"
#include "frost/ui/debug_session_registry.hpp"
#include "frost/ui/shell/notifications.hpp"

#include <QObject>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace frost::app {

// Explicit worker lifetime barrier. shutdown() on the controller is nonblocking; the host MUST
// retain and drain this batch after GUI/controller teardown and before borrowed services die.
// Only one observation/materialization is pending at once; cleanup is independently queued and
// bounded by 144 retained cache leases. drain() also waits for all leases to release, so drain
// debugger retirements first. No new observation/materialization after drain begins. Thread-safe.
class EngineDumpWorkBatch final {
public:
    // An optional worker-only cleanup seam supports blocked-I/O lifecycle tests. Captured services
    // outlive drain(); production removes only bounded entries from a revalidated private child.
    explicit EngineDumpWorkBatch(std::function<void(std::string)> cleanup = {});
    ~EngineDumpWorkBatch();
    EngineDumpWorkBatch(const EngineDumpWorkBatch&) = delete;
    EngineDumpWorkBatch& operator=(const EngineDumpWorkBatch&) = delete;
    void drain();
    [[nodiscard]] std::size_t pending() const noexcept;

private:
    friend class EngineDumpController;
    friend class EngineCrashObserver;
    [[nodiscard]] Result<void> submit(std::function<void()> work);
    [[nodiscard]] Result<std::function<void(std::string)>> reserveCleanup();
    struct State;
    std::shared_ptr<State> m_state;
};

struct EngineDumpAdmission {
    std::string projectRoot; // Captured physical trusted root, matching source.projectPath.
    // Thread-safe, bounded and nonblocking: checks cached physical identity, trust revision and
    // explicit revocation on GUI and immediately before actual DAP launch transmission. No I/O,
    // QObject access or GUI-thread capture. Borrowed services outlive debugger retirement drain.
    // Unexpected process loss must not itself revoke this historical artifact admission.
    std::function<Result<void>()> current;
    // Worker only: revalidate the captured physical project/source identity without QObject
    // access. Services captured here outlive WorkBatch::drain(). GUI checks never perform I/O.
    std::function<Result<void>()> validatePhysical;
    // Optional explicit caller-admitted local symbol directories; never guessed from dump contents.
    std::vector<std::string> localSymbolPaths;
};

struct EngineDumpControllerOptions {
    ui::NotificationCenter* notifications = nullptr;
    ui::DebugSessionRegistry* sessions = nullptr;
    io::IProcessLauncher* launcher = nullptr;
    std::shared_ptr<EngineDumpWorkBatch> work;
    std::string adapterExecutable; // Absolute staged FrostIDE adapter, not an engine-supplied path.
    std::string cacheDirectory;    // Existing private IDE diagnostics directory, never a workspace.
    // Tests may supply the existing fake-DAP factory. Production leaves this empty.
    debug::AdapterFactory adapterFactory;
};

// GUI-thread coordinator. The caller supplies only artifacts from CrashArtifactObserver. No push
// from a dead engine is claimed: provenance is previous authentication plus local correlation.
// All borrowed GUI/launcher services outlive this object and active debugger sessions. A decorated
// real client retains the private dump copy until asynchronous adapter/session retirement finishes.
class EngineDumpController final : public QObject {
public:
    explicit EngineDumpController(EngineDumpControllerOptions options, QObject* parent = nullptr);
    ~EngineDumpController() override;
    // Posts one ordinary notification with an Open dump action; never starts an adapter. At most
    // eight pending offers and 256 MiB of pending immutable bytes. Duplicate source/hash dedupes.
    [[nodiscard]] Result<std::uint64_t> offer(std::shared_ptr<const engine::ValidatedCrashArtifact> artifact,
                                              EngineDumpAdmission admission);
    // One-use user action: starts bounded background materialization, rechecks admission, then
    // creates a read-only registry session. No executable is inferred or launched as a debuggee.
    [[nodiscard]] Result<void> open(std::uint64_t ticket);
    // Explicit disconnect/project/trust/source replacement invalidates old notification actions.
    void revoke();
    void shutdown(); // Revoke and stop GUI polling; worker cleanup remains batch-owned.

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace frost::app
