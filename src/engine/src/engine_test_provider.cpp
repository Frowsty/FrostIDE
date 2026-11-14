// SPDX-License-Identifier: Apache-2.0
// FER-10: each discovery/run is an actual contained engine command, with captured project admission.
#include "frost/engine/engine_test_provider.hpp"

#include "frost/io/path.hpp"
#include "frost/lang/uri.hpp"

#include <algorithm>
#include <set>

namespace frost::engine {
namespace {
bool same(std::string_view a, std::string_view b) { return lang::documentKey(a) == lang::documentKey(b); }
bool beneath(std::string_view path, std::string_view root) {
    auto prefix = lang::documentKey(root);
    if (!prefix.ends_with('/') && !prefix.ends_with('\\'))
        prefix += io::kNativePathStyle == io::PathStyle::Windows ? '\\' : '/';
    return lang::documentKey(path).starts_with(prefix);
}
Result<std::string> checkpoint(io::IFileSystem& fs, const EngineTestProviderOptions& o,
                               const std::function<bool()>& admitted, std::stop_token stop) {
    if (stop.stop_requested())
        return fail(ErrorCode::Cancelled, "engine tests cancelled");
    if (!admitted || !admitted())
        return fail(ErrorCode::Untrusted, "engine test project admission changed");
    if (!io::normalizePath(o.program) || !io::normalizePath(o.project) || !io::normalizePath(o.expectedProjectRoot) ||
        o.frames == 0 || o.discoveryTimeout.count() <= 0 || (o.runTimeout && o.runTimeout->count() <= 0))
        return fail(
            ErrorCode::InvalidArgument,
            "engine tests require an absolute tool, project and captured physical root, frames and positive deadlines");
    if (o.expectedProgram.empty())
        return fail(ErrorCode::PermissionDenied, "engine test CLI has no discovery physical receipt");
    auto executable = fs.physicalPath(o.program);
    if (!executable)
        return std::unexpected(executable.error());
    if (!same(*executable, o.expectedProgram))
        return fail(ErrorCode::PermissionDenied, "engine test physical CLI changed");
    auto program = fs.status(*executable);
    if (!program)
        return std::unexpected(program.error());
    if (program->type != io::FileType::Regular)
        return fail(ErrorCode::InvalidArgument, "engine test tool is not a regular file");
    auto root = fs.physicalPath(o.expectedProjectRoot), target = fs.physicalPath(o.project);
    if (!root)
        return std::unexpected(root.error());
    if (!target)
        return std::unexpected(target.error());
    if (!same(*root, o.expectedProjectRoot))
        return fail(ErrorCode::PermissionDenied, "engine test physical root changed");
    auto status = fs.status(*target);
    if (!status)
        return std::unexpected(status.error());
    const auto targetRoot = status->type == io::FileType::Directory ? *target : std::string(io::parentPath(*target));
    if (!same(targetRoot, *root))
        return fail(ErrorCode::PermissionDenied, "engine test selected project changed");
    if (o.expectedProjectFile) {
        auto file = fs.physicalPath(*o.expectedProjectFile);
        if (!file)
            return std::unexpected(file.error());
        if (!same(io::parentPath(*file), *root) ||
            (status->type != io::FileType::Directory && !same(*file, *target)))
            return fail(ErrorCode::PermissionDenied, "engine test project file changed");
        // Preserve the selected file, rather than allowing directory selection to choose another one.
        target = std::move(file);
    } else if (status->type != io::FileType::Directory) {
        return fail(ErrorCode::InvalidArgument, "engine test file selection requires a captured project file");
    }
    if (stop.stop_requested())
        return fail(ErrorCode::Cancelled, "engine tests cancelled");
    if (!admitted())
        return fail(ErrorCode::Untrusted, "engine test project admission changed");
    return target;
}
struct AdmittedCommandResult {
    rpc::Json result;
    std::string target;
};
Result<AdmittedCommandResult> command(io::IFileSystem& fs, io::IProcessLauncher& launcher, const EngineTestProviderOptions& o,
                          const std::function<bool()>& admitted, std::stop_token stop, std::string name,
                          rpc::Json params, bool run, std::string_view expectedTarget = {}) {
    auto target = checkpoint(fs, o, admitted, stop);
    if (!target)
        return std::unexpected(target.error());
    if (!expectedTarget.empty() && !same(*target, expectedTarget))
        return fail(ErrorCode::PermissionDenied, "engine test selected file changed before command");
    EngineCliRequest request;
    request.command = std::move(name);
    request.params = std::move(params);
    request.project = *target;
    request.workingDirectory = o.expectedProjectRoot;
    request.stopToken = stop;
    request.timeout = run ? o.runTimeout : std::optional(o.discoveryTimeout);
    auto outcome = runEngineCliCommand(launcher, o.program, request);
    // Cancellation/source changes win over a just-finished command: no stale publication.
    auto still = checkpoint(fs, o, admitted, stop);
    if (!still)
        return std::unexpected(still.error());
    if (!same(*still, *target))
        return fail(ErrorCode::PermissionDenied, "engine test selected file changed during command");
    if (!outcome)
        return std::unexpected(outcome.error());
    if (outcome->error) {
        const auto code = outcome->error->code;
        const auto category = code == -32602   ? ErrorCode::InvalidArgument
                              : code == -32001 ? ErrorCode::NotFound
                              : code == -32003 ? ErrorCode::Busy
                              : code == -32601 ? ErrorCode::Unsupported
                                               : ErrorCode::IoError;
        return fail(category, "FrostEngine " + request.command + " refused (" + std::to_string(outcome->error->code) +
                                  "): " + outcome->error->message);
    }
    if (!outcome->result)
        return fail(ErrorCode::Corrupt, "engine test command returned no result");
    if (!run && outcome->exitCode != 0)
        return fail(ErrorCode::Corrupt, "test.list returned a nonzero exit code");
    if (run) {
        const auto passed = outcome->result->boolAt("passed");
        if (!passed || outcome->exitCode != (*passed ? 0 : 3))
            return fail(ErrorCode::Corrupt, "test.run exit code contradicts its result");
    }
    return AdmittedCommandResult{std::move(*outcome->result), std::move(*target)};
}
Result<void> listingIdentity(io::IFileSystem& fs, const EngineTestProviderOptions& o,
                             const EngineTestListing& listing, std::string_view admittedTarget) {
    auto root = fs.physicalPath(listing.projectPath), file = fs.physicalPath(listing.projectFile);
    if (!root)
        return std::unexpected(root.error());
    if (!file)
        return std::unexpected(file.error());
    auto status = fs.status(*file);
    if (!status)
        return std::unexpected(status.error());
    if (!same(*root, o.expectedProjectRoot) || !same(io::parentPath(*file), *root) ||
        status->type != io::FileType::Regular || (o.expectedProjectFile && !same(*file, admittedTarget)))
        return fail(ErrorCode::PermissionDenied, "test.list returned a different physical project");
    return {};
}
} // namespace
Result<EngineTestListing> discoverEngineSceneTests(io::IFileSystem& fs, io::IProcessLauncher& launcher,
                                                   const EngineTestProviderOptions& o,
                                                   const std::function<bool()>& admitted, std::stop_token stop) {
    auto result =
        command(fs, launcher, o, admitted, stop, "test.list", rpc::Json::object().set("folder", o.folder), false);
    if (!result)
        return std::unexpected(result.error());
    auto listing = parseEngineTestListing(result->result);
    if (!listing)
        return std::unexpected(listing.error());
    auto identity = listingIdentity(fs, o, *listing, result->target);
    if (!identity)
        return std::unexpected(identity.error());
    return listing;
}
EngineTestExecution runEngineSceneTests(io::IFileSystem& fs, io::IProcessLauncher& launcher,
                                        const EngineTestProviderOptions& o, const EngineTestListing& listing,
                                        std::span<const std::string> ids, const std::function<bool()>& admitted,
                                        std::stop_token stop) {
    EngineTestExecution out;
    const auto refuse = [&](Error error) {
        out.cancelled = error.code() == ErrorCode::Cancelled;
        out.error = std::move(error);
    };
    auto current = checkpoint(fs, o, admitted, stop);
    if (!current) {
        refuse(current.error());
        return out;
    }
    auto identity = listingIdentity(fs, o, listing, *current);
    if (!identity) {
        refuse(identity.error());
        return out;
    }
    if (listing.tests.size() > 100000) {
        refuse(Error(ErrorCode::InvalidArgument, "too many engine scene tests"));
        return out;
    }
    auto encoded = rpc::Json::array();
    for (const auto& test : listing.tests)
        encoded.push(rpc::Json::object().set("id", test.id).set("scene", test.scene));
    auto validated = parseEngineTestListing(rpc::Json::object()
                                                .set("version", 1)
                                                .set("granularity", "scene")
                                                .set("projectPath", listing.projectPath)
                                                .set("projectFile", listing.projectFile)
                                                .set("count", listing.tests.size())
                                                .set("tests", std::move(encoded)));
    if (!validated) {
        refuse(validated.error());
        return out;
    }
    std::vector<std::string> selected;
    std::set<std::string> available, seen;
    for (const auto& t : listing.tests) {
        if (t.id != t.scene || !available.insert(t.id).second) {
            refuse(Error(ErrorCode::InvalidArgument, "invalid engine scene listing"));
            return out;
        }
    }
    if (ids.empty())
        for (const auto& t : listing.tests)
            selected.push_back(t.id);
    else
        selected.assign(ids.begin(), ids.end());
    for (const auto& id : selected)
        if (!available.contains(id) || !seen.insert(id).second) {
            refuse(Error(ErrorCode::InvalidArgument, "engine test selection contains an unlisted or duplicate scene"));
            return out;
        }
    if (selected.empty())
        return out;
    auto scenes = rpc::Json::array();
    for (const auto& id : selected)
        scenes.push(id);
    // Native RunTests shares one test SaveData directory across the selected scenes. One command
    // preserves those semantics and the engine's actual all/failed selection behavior. The CLI emits
    // only its final report: interrupted commands cannot provide trustworthy partial scene results.
    auto params = rpc::Json::object().set("scenes", std::move(scenes)).set("frames", o.frames).set("mode", "headless");
    auto report = command(fs, launcher, o, admitted, stop, "test.run", std::move(params), true, *current);
    if (!report) {
        refuse(report.error());
        return out;
    }
    auto parsed = parseEngineTestRun(report->result, selected);
    if (!parsed) {
        refuse(parsed.error());
        return out;
    }
    out.results = std::move(*parsed);
    if (!*report->result.boolAt("passed") &&
        std::ranges::all_of(out.results, [](const auto& r) { return r.outcome == EngineTestOutcome::Passed; }))
        refuse(Error(ErrorCode::IoError, "test.run reported a command-level requirement failure; see actual report"));
    return out;
}
std::vector<std::string> failedEngineSceneTests(const EngineTestListing& listing,
                                                std::span<const EngineSceneTestResult> results) {
    std::vector<std::string> out;
    for (const auto& t : listing.tests)
        if (std::ranges::any_of(
                results, [&](const auto& r) { return r.scene == t.scene && r.outcome == EngineTestOutcome::Failed; }))
            out.push_back(t.id);
    return out;
}
Result<std::string> resolveEngineTestLocation(io::IFileSystem& fs, std::string_view physicalProject,
                                              const EngineTestLocation& location) {
    // Reuse strict parser location validation without inferring a path from human-readable text.
    if (location.line == 0 || !location.path.starts_with("Assets/") ||
        location.path.find_first_of("\\:") != location.path.npos)
        return fail(ErrorCode::InvalidArgument, "engine test has no safe structured source location");
    auto root = fs.physicalPath(physicalProject);
    if (!root)
        return std::unexpected(root.error());
    if (!same(*root, physicalProject))
        return fail(ErrorCode::PermissionDenied, "engine test project root changed");
    auto assets = io::joinPath(*root, "Assets");
    if (!assets)
        return std::unexpected(assets.error());
    auto physicalAssets = fs.physicalPath(*assets);
    if (!physicalAssets)
        return std::unexpected(physicalAssets.error());
    if (!beneath(*physicalAssets, *root))
        return fail(ErrorCode::PermissionDenied, "engine Assets directory escapes project");
    auto joined = io::joinPath(*root, location.path);
    if (!joined)
        return std::unexpected(joined.error());
    auto target = fs.physicalPath(*joined);
    if (!target)
        return std::unexpected(target.error());
    if (!beneath(*target, *physicalAssets))
        return fail(ErrorCode::PermissionDenied, "engine test location escapes Assets");
    auto status = fs.status(*target);
    if (!status)
        return std::unexpected(status.error());
    if (status->type != io::FileType::Regular)
        return fail(ErrorCode::InvalidArgument, "engine test location is not a regular file");
    return target;
}
} // namespace frost::engine
