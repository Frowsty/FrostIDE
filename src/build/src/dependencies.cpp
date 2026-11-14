// SPDX-License-Identifier: Apache-2.0
#include "frost/build/dependencies.hpp"

#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#ifdef _WIN32
#include "frost/io/utf.hpp"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace frost::build {
namespace {
Result<std::string> canonical(io::IFileSystem& fs, std::string_view path) {
    auto normalized = io::normalizePath(path);
    if (!normalized)
        return std::unexpected(std::move(normalized.error()));
    return fs.canonicalPath(*normalized);
}
Result<void> inputFile(io::IFileSystem& fs, DependencyPlan& plan, const std::string& path) {
    auto bytes = fs.readFileSnapshot(path, 1024 * 1024);
    if (!bytes)
        return std::unexpected(std::move(bytes.error()));
    if (bytes->size() > 1024 * 1024)
        return fail(ErrorCode::InvalidArgument, "Dependency input exceeds 1 MiB");
    auto physical = fs.physicalPath(path);
    if (!physical)
        return std::unexpected(std::move(physical.error()));
    plan.physicalInputs[path] = std::move(*physical);
    plan.inputs[path] = std::move(*bytes);
    plan.trustRoots.emplace_back(io::parentPath(path));
    return {};
}
Result<void> trusted(const project::TrustStore& trust, const DependencyPlan& plan) {
    for (const auto& root : plan.trustRoots)
        if (!trust.isTrusted(root))
            return fail(ErrorCode::Untrusted, "Dependency execution requires trusted input folder: " + root);
    return {};
}
bool contained(std::string root, std::string path) {
#ifdef _WIN32
    std::ranges::transform(root, root.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::ranges::transform(path, path.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
#endif
    if (path == root)
        return true;
    if (!root.ends_with('/') && !root.ends_with('\\'))
        root.push_back(io::kNativePathStyle == io::PathStyle::Windows ? '\\' : '/');
    return path.starts_with(root);
}
Result<std::string> checkedOutput(io::IFileSystem& fs, const std::string& root, std::string_view relative) {
    auto physicalRoot = fs.physicalPath(root);
    if (!physicalRoot)
        return std::unexpected(std::move(physicalRoot.error()));
    std::string current = *physicalRoot;
    bool existing = true;
    while (!relative.empty()) {
        const auto separator = relative.find_first_of("/\\");
        const auto component = relative.substr(0, separator);
        if (!component.empty() && component != ".") {
            auto joined = io::joinPath(current, component);
            if (!joined)
                return std::unexpected(std::move(joined.error()));
            if (existing) {
                // listDirectory observes dangling links too; exists/status follow their targets.
                auto entries = fs.listDirectory(current);
                if (!entries)
                    return std::unexpected(std::move(entries.error()));
                const auto entry = std::ranges::find_if(*entries, [&](const io::DirEntry& item) {
#ifdef _WIN32
                    auto left = io::toWide(item.name), right = io::toWide(component);
                    return left && right &&
                           CompareStringOrdinal(left->data(), static_cast<int>(left->size()), right->data(),
                                                static_cast<int>(right->size()), TRUE) == CSTR_EQUAL;
#else
                    return item.name == component;
#endif
                });
                existing = entry != entries->end();
            }
            current = std::move(*joined);
            if (existing) {
                auto resolved = fs.physicalPath(current);
                if (!resolved)
                    return std::unexpected(std::move(resolved.error()));
                current = std::move(*resolved);
                if (!contained(*physicalRoot, current))
                    return fail(ErrorCode::Untrusted, "Dependency output link leaves the workspace");
                auto status = fs.status(current);
                if (!status || status->type != io::FileType::Directory)
                    return fail(ErrorCode::InvalidArgument, "Dependency output ancestor is not a directory");
            }
        }
        if (separator == std::string_view::npos)
            break;
        relative.remove_prefix(separator + 1);
    }
    return current;
}
bool token(std::string_view text) {
    return !text.empty() && text.size() <= 128 &&
           std::ranges::all_of(text, [](unsigned char c) { return std::isalnum(c) || c == '-' || c == '_'; });
}
} // namespace
Result<DependencyPlan> planDependencies(io::IFileSystem& fs, DependencyRequest request) {
    DependencyPlan plan;
    auto root = canonical(fs, request.root);
    auto executable = canonical(fs, request.executable);
    auto input = canonical(fs, request.input);
    if (!root)
        return std::unexpected(std::move(root.error()));
    if (!executable)
        return std::unexpected(std::move(executable.error()));
    if (!input)
        return std::unexpected(std::move(input.error()));
    if (request.manager != DependencyManager::Vcpkg && request.manager != DependencyManager::Conan &&
        request.manager != DependencyManager::NativeNuget)
        return fail(ErrorCode::InvalidArgument, "Unknown dependency manager");
    auto rootStatus = fs.status(*root);
    if (!rootStatus || rootStatus->type != io::FileType::Directory)
        return fail(ErrorCode::InvalidArgument, "Dependency root must be an existing directory");
    auto executableStatus = fs.status(*executable);
    if (!executableStatus || executableStatus->type != io::FileType::Regular)
        return fail(ErrorCode::InvalidArgument, "Dependency manager must be an absolute regular executable");
    // The requested output stays physically inside the workspace. Package recipes remain trusted
    // executable inputs and may also use their manager's ordinary cache.
    const auto relative = request.outputDirectory;
    if (relative.empty() || io::isAbsolutePath(relative) || relative.find("..") != std::string::npos)
        return fail(ErrorCode::InvalidArgument,
                    "Dependency output must be a workspace-relative directory without '..'");
    auto output = checkedOutput(fs, *root, relative);
    if (!output)
        return std::unexpected(std::move(output.error()));
    request.root = *root;
    request.executable = *executable;
    request.input = *input;
    plan.request = request;
    plan.resolvedOutput = *output;
    auto physicalExecutable = fs.physicalPath(*executable), physicalRoot = fs.physicalPath(*root);
    if (!physicalExecutable)
        return std::unexpected(std::move(physicalExecutable.error()));
    if (!physicalRoot)
        return std::unexpected(std::move(physicalRoot.error()));
    plan.physicalInputs[*executable] = *physicalExecutable;
    plan.physicalInputs[*root] = *physicalRoot;
    plan.process.executable = *physicalExecutable;
    plan.process.workingDirectory = *physicalRoot;
    plan.process.stdinMode = io::Stdio::Null;
    plan.process.killOnClose = true;
    plan.trustRoots = {*root};
    if (auto read = inputFile(fs, plan, *input); !read)
        return std::unexpected(std::move(read.error()));
    if (request.manager == DependencyManager::Vcpkg) {
        if (io::fileName(*input) != "vcpkg.json" || !token(request.triplet))
            return fail(ErrorCode::InvalidArgument, "vcpkg needs vcpkg.json and an explicit triplet");
        auto toolchain = io::joinPath(io::parentPath(*physicalExecutable), "scripts/buildsystems/vcpkg.cmake");
        if (!toolchain)
            return std::unexpected(std::move(toolchain.error()));
        if (auto read = inputFile(fs, plan, *toolchain); !read)
            return std::unexpected(std::move(read.error()));
        plan.process.environment = {{"VCPKG_BINARY_SOURCES", std::string("clear")},
                                    {"VCPKG_DISABLE_METRICS", std::string("1")},
                                    {"VCPKG_OVERLAY_PORTS", std::nullopt},
                                    {"VCPKG_OVERLAY_TRIPLETS", std::nullopt}};
        plan.process.arguments = {"install",
                                  "--x-manifest-root=" + std::string(io::parentPath(plan.physicalInputs.at(*input))),
                                  "--x-install-root=" + *output,
                                  "--triplet=" + request.triplet,
                                  "--no-downloads",
                                  "--disable-metrics"};
        if (!request.localFeed.empty()) {
            auto feed = canonical(fs, request.localFeed);
            if (!feed)
                return std::unexpected(std::move(feed.error()));
            auto physicalFeed = fs.physicalPath(*feed);
            if (!physicalFeed)
                return std::unexpected(std::move(physicalFeed.error()));
            plan.physicalInputs[*feed] = *physicalFeed;
            plan.request.localFeed = *feed;
            plan.trustRoots.push_back(*feed);
            plan.process.arguments.push_back("--overlay-ports=" + plan.physicalInputs.at(*feed));
        }
        auto configuration = io::joinPath(io::parentPath(*input), "vcpkg-configuration.json");
        if (configuration) {
            auto exists = fs.exists(*configuration);
            if (exists && *exists)
                if (auto read = inputFile(fs, plan, *configuration); !read)
                    return std::unexpected(std::move(read.error()));
        }
        plan.cmakeCache = {"CMAKE_TOOLCHAIN_FILE:FILEPATH=" + *toolchain,
                           "VCPKG_TARGET_TRIPLET:STRING=" + request.triplet, "VCPKG_INSTALLED_DIR:PATH=" + *output,
                           "VCPKG_MANIFEST_DIR:PATH=" + std::string(io::parentPath(plan.physicalInputs.at(*input))),
                           "VCPKG_MANIFEST_INSTALL:BOOL=OFF"};
    } else if (request.manager == DependencyManager::Conan) {
        const auto filename = io::fileName(*input);
        if (filename != "conanfile.py" && filename != "conanfile.txt")
            return fail(ErrorCode::InvalidArgument, "Conan 2 needs conanfile.py or conanfile.txt");
        auto host = canonical(fs, request.hostProfile), build = canonical(fs, request.buildProfile);
        if (!host)
            return std::unexpected(std::move(host.error()));
        if (!build)
            return std::unexpected(std::move(build.error()));
        plan.request.hostProfile = *host;
        plan.request.buildProfile = *build;
        if (auto read = inputFile(fs, plan, *host); !read)
            return std::unexpected(std::move(read.error()));
        if (auto read = inputFile(fs, plan, *build); !read)
            return std::unexpected(std::move(read.error()));
        plan.process.arguments = {"install",
                                  plan.physicalInputs.at(*input),
                                  "--output-folder=" + *output,
                                  "--profile:host=" + plan.physicalInputs.at(*host),
                                  "--profile:build=" + plan.physicalInputs.at(*build),
                                  "--generator=CMakeToolchain",
                                  "--generator=CMakeDeps",
                                  "--no-remote",
                                  "--build=missing"};
        auto toolchain = io::joinPath(*output, "conan_toolchain.cmake");
        plan.cmakeCache = {"CMAKE_TOOLCHAIN_FILE:FILEPATH=" + *toolchain};
    } else {
        if (io::fileName(*input) != "packages.config")
            return fail(ErrorCode::Unsupported, "Native NuGet currently restores existing packages.config only; "
                                                "PackageReference/CPM edits are not supported");
        if (request.localFeed.empty())
            return fail(ErrorCode::InvalidArgument, "Native NuGet requires an explicit local feed");
        auto feed = canonical(fs, request.localFeed);
        if (!feed)
            return std::unexpected(std::move(feed.error()));
        auto physicalFeed = fs.physicalPath(*feed);
        if (!physicalFeed)
            return std::unexpected(std::move(physicalFeed.error()));
        plan.physicalInputs[*feed] = *physicalFeed;
        plan.request.localFeed = *feed;
        plan.trustRoots.push_back(*feed);
        plan.process.arguments = {"restore",
                                  plan.physicalInputs.at(*input),
                                  "-Source",
                                  plan.physicalInputs.at(*feed),
                                  "-PackagesDirectory",
                                  *output,
                                  "-NonInteractive",
                                  "-NoCache",
                                  "-ForceEnglishOutput"};
    }
    std::ranges::sort(plan.trustRoots);
    plan.trustRoots.erase(std::unique(plan.trustRoots.begin(), plan.trustRoots.end()), plan.trustRoots.end());
    return plan;
}
Result<DependencyConsent> consentDependencies(const DependencyPlan& plan, const project::TrustStore& trust) {
    if (auto allowed = trusted(trust, plan); !allowed)
        return std::unexpected(std::move(allowed.error()));
    return DependencyConsent(plan, trust.snapshot(plan.request.root).revision);
}
Result<DependencyPlan> validateDependencyConsent(io::IFileSystem& fs, const project::TrustStore& trust,
                                                 const DependencyConsent& consent) {
    auto fresh = planDependencies(fs, consent.m_plan.request);
    if (!fresh)
        return std::unexpected(std::move(fresh.error()));
    const auto& approved = consent.m_plan;
    if (fresh->inputs != approved.inputs || fresh->physicalInputs != approved.physicalInputs ||
        fresh->request != approved.request || fresh->resolvedOutput != approved.resolvedOutput ||
        fresh->trustRoots != approved.trustRoots || fresh->cmakeCache != approved.cmakeCache ||
        fresh->process.executable != approved.process.executable ||
        fresh->process.workingDirectory != approved.process.workingDirectory ||
        fresh->process.arguments != approved.process.arguments ||
        fresh->process.environment != approved.process.environment ||
        fresh->process.cleanEnvironment != approved.process.cleanEnvironment ||
        fresh->process.stdinMode != approved.process.stdinMode ||
        fresh->process.stdoutMode != approved.process.stdoutMode ||
        fresh->process.stderrMode != approved.process.stderrMode ||
        fresh->process.killOnClose != approved.process.killOnClose)
        return fail(ErrorCode::Cancelled, "Dependency inputs changed after consent; review the command again");
    if (auto allowed = trusted(trust, *fresh); !allowed)
        return std::unexpected(std::move(allowed.error()));
    if (trust.snapshot(fresh->request.root).revision != consent.m_revision)
        return fail(ErrorCode::Untrusted, "Trust changed after dependency consent");
    return fresh;
}
Result<io::ProcessResult> installDependencies(io::IFileSystem& fs, io::IProcessLauncher& launcher,
                                              const project::TrustStore& trust, const DependencyConsent& consent,
                                              std::stop_token stop) {
    if (stop.stop_requested())
        return fail(ErrorCode::Cancelled, "Dependency operation cancelled");
    auto fresh = validateDependencyConsent(fs, trust, consent);
    if (!fresh)
        return std::unexpected(std::move(fresh.error()));
#ifndef _WIN32
    if (fresh->request.manager == DependencyManager::NativeNuget)
        return fail(ErrorCode::Unsupported, "Native NuGet.exe restore is supported on Windows only");
#endif
    if (stop.stop_requested())
        return fail(ErrorCode::Cancelled, "Dependency operation cancelled");
    bool unused = false;
    if (!consent.m_used->compare_exchange_strong(unused, true))
        return fail(ErrorCode::PermissionDenied,
                    "Dependency consent was already used; review each invocation separately");
    return io::runToCompletion(launcher, fresh->process, {}, std::chrono::minutes(30), stop);
}
project::CMakeProfileRecord dependencyCmakeProfile(const DependencyPlan& plan) {
    return {.name =
                plan.request.manager == DependencyManager::Vcpkg ? "FrostDependenciesVcpkg" : "FrostDependenciesConan",
            .buildType = "Debug",
            .generator = "Ninja",
            .buildDirectory =
                plan.request.manager == DependencyManager::Vcpkg ? ".frost/build/vcpkg" : ".frost/build/conan",
            .cacheVariables = plan.cmakeCache,
            .root = plan.request.root};
}
} // namespace frost::build
