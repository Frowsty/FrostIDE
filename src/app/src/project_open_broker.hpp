// SPDX-License-Identifier: Apache-2.0
// Project-scoped native IPC adapter for external editor open requests.
#pragma once
#include "frost/core/error.hpp"
#include "frost/io/local_ipc.hpp"

#include <QObject>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace frost::app {
class Workbench;
class MainWindow;
// Canonical directory of an existing project root or .frostproj. Errors: NotFound, InvalidArgument.
[[nodiscard]] Result<std::string> externalProjectRoot(std::string_view project);
// Infers the canonical project root directory for an external script or source file when
// launched without --project (e.g. from FrostEngine); returns nullopt if none can be inferred.
[[nodiscard]] std::optional<std::string> inferExternalProjectRootForFile(std::string_view filePath);
// SHA-256 of the canonical document key; no raw user path is exposed in IPC names.
[[nodiscard]] std::string externalProjectKey(std::string_view canonicalRoot);
// Opens an absolute file in the running project; zero line/column keeps the existing coordinate.
[[nodiscard]] Result<void> openExternalFile(Workbench& workbench, std::string_view path, int line, int column,
                                            bool columnUtf16 = false);
// Secondary process: sends one bounded request and waits for the owning window's acknowledgement.
[[nodiscard]] Result<void> forwardExternalOpen(std::string_view root, std::string_view file, int line, int column,
                                               bool columnUtf16 = false);

// GUI thread owns this adapter. The handler runs on the IPC worker, posts to this object, and
// waits at most two seconds for the UI; destruction cancels the server and joins its worker.
class ProjectOpenBroker final : public QObject {
public:
    ProjectOpenBroker(Workbench& workbench, MainWindow& window, std::string root,
                      std::unique_ptr<io::LocalIpcOwner> owner);
    ~ProjectOpenBroker() override;
    [[nodiscard]] Result<void> start(); // Call only after first frame.
    void projectChanged();              // Rebind on a project switch; already-owned roots remain unchanged.
private:
    [[nodiscard]] Result<std::string> accept(std::string request);
    void claimCurrent(unsigned attempts);
    Workbench& m_workbench;
    MainWindow& m_window;
    std::string m_root;
    std::unique_ptr<io::LocalIpcOwner> m_owner;
    bool m_firstFrame = false;
    std::vector<std::jthread> m_retired;
};
} // namespace frost::app
