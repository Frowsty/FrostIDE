// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "frost/core/error.hpp"
#include "frost/io/process.hpp"
#include "frost/io/file_system.hpp"
#include "frost/project/cmake_profile.hpp"
#include "frost/project/trust_store.hpp"
#include <map>
#include <memory>
#include <atomic>
#include <stop_token>

namespace frost::build {
enum class DependencyManager { Vcpkg, Conan, NativeNuget };
struct DependencyRequest {
    DependencyManager manager = DependencyManager::Vcpkg;
    std::string root, executable, input, outputDirectory, localFeed, triplet, hostProfile, buildProfile;
    friend bool operator==(const DependencyRequest&, const DependencyRequest&) = default;
};
struct DependencyPlan {
    DependencyRequest request;
    std::string resolvedOutput;
    io::ProcessSpec process;
    std::map<std::string, std::string> inputs;
    std::map<std::string, std::string> physicalInputs;
    std::vector<std::string> trustRoots;
    std::vector<std::string> cmakeCache;
};
// Pure planning plus bounded reads. Does not invoke a manager or alter project/SDK files.
[[nodiscard]] Result<DependencyPlan> planDependencies(io::IFileSystem& fs, DependencyRequest request);
class DependencyConsent {
public:
    DependencyConsent(const DependencyConsent&) = default;
private:
    DependencyConsent(DependencyPlan plan, std::uint64_t revision) : m_plan(std::move(plan)), m_revision(revision) {}
    DependencyPlan m_plan;
    std::uint64_t m_revision;
    std::shared_ptr<std::atomic<bool>> m_used = std::make_shared<std::atomic<bool>>(false);
    friend Result<DependencyConsent> consentDependencies(const DependencyPlan&, const project::TrustStore&);
    friend Result<DependencyPlan> validateDependencyConsent(io::IFileSystem&, const project::TrustStore&, const DependencyConsent&);
    friend Result<io::ProcessResult> installDependencies(io::IFileSystem&, io::IProcessLauncher&,
        const project::TrustStore&, const DependencyConsent&, std::stop_token);
};
// Call only after displaying this exact command/input/output plan and receiving explicit consent.
[[nodiscard]] Result<DependencyConsent> consentDependencies(const DependencyPlan& plan, const project::TrustStore& trust);
// Revalidate the same receipt before applying generated integration files/properties.
[[nodiscard]] Result<DependencyPlan> validateDependencyConsent(io::IFileSystem& fs, const project::TrustStore& trust, const DependencyConsent& consent);
// Blocking worker-only operation. Replans inputs, rechecks current trust/revision and honors cancellation.
[[nodiscard]] Result<io::ProcessResult> installDependencies(io::IFileSystem& fs, io::IProcessLauncher& launcher,
    const project::TrustStore& trust, const DependencyConsent& consent, std::stop_token stop = {});
[[nodiscard]] project::CMakeProfileRecord dependencyCmakeProfile(const DependencyPlan& plan);
} // namespace frost::build
