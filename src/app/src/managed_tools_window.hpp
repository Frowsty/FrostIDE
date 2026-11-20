// SPDX-License-Identifier: Apache-2.0
// Defines the workspace context and factories for managed package and test widgets.
#pragma once
#include "frost/build/managed_evaluation.hpp"
#include "frost/document/refactoring_transaction.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>
class QWidget;
namespace frost::project {
class TrustStore;
}
namespace frost::io {
class IFileSystem;
class IProcessLauncher;
} // namespace frost::io
namespace frost::app {
struct ManagedToolsContext {
    std::string preferredProject{}; // Explicit project action may differ from the global SDK selection.
    io::IProcessLauncher* launcher = nullptr;
    io::IFileSystem* fs = nullptr;
    project::TrustStore* trust = nullptr;
    std::function<std::string()> root{};
    std::function<std::uint64_t()> generation{};
    std::function<std::string()> dotnet{};
    std::function<std::vector<std::string>()> projects{};
    // Immutable actual SDK selection; pointer identity retires operations on clear/re-evaluation.
    std::function<std::shared_ptr<const build::ManagedProjectEvaluation>()> evaluatedProject{};
    std::function<Result<void>()> prepare{};
    std::function<Result<document::RefactoringOutcome>(const document::RefactoringPlan&)> apply{};
    std::function<void(const std::string&, int)> openLocation{};
};
[[nodiscard]] QWidget* createManagedPackagesWidget(QWidget* parent, ManagedToolsContext context);
[[nodiscard]] QWidget* createDotnetTestsWidget(QWidget* parent, ManagedToolsContext context);
} // namespace frost::app
