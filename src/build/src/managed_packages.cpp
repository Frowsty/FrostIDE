// SPDX-License-Identifier: Apache-2.0
#include "frost/build/managed_packages.hpp"

#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/project/trust_store.hpp"

#include "json.hpp"

#include <algorithm>
#include <utility>

namespace frost::build {
namespace {
bool safe(std::string_view text) { return text.find_first_of(std::string("\0\r\n", 3)) == std::string_view::npos; }
using Json = detail::JsonValue;
bool array(const Json* value) { return value && value->kind == Json::Kind::Array; }
Result<void> packages(ManagedPackageReport& report, const Json* rows, std::string framework, std::string source,
                      bool transitive, bool search) {
    if (!array(rows))
        return fail(ErrorCode::Corrupt, "NuGet response has no package array");
    for (const auto& row : rows->items) {
        if (row.kind != Json::Kind::Object || row.stringMember("id").empty())
            return fail(ErrorCode::Corrupt, "NuGet response has an invalid package");
        ManagedPackage package;
        package.id = row.stringMember("id");
        package.framework = framework;
        package.source = source;
        package.transitive = transitive;
        package.version = row.stringMember(search ? "latestVersion" : "resolvedVersion");
        if (search && package.version.empty())
            package.version = row.stringMember("version");
        if (package.version.empty())
            return fail(ErrorCode::Corrupt, "NuGet package has no reported version");
        package.requested = row.stringMember("requestedVersion");
        package.latest = row.stringMember("latestVersion");
        if (const auto* vulnerabilities = row.find("vulnerabilities"); array(vulnerabilities))
            for (const auto& issue : vulnerabilities->items) {
                const std::string severity = issue.stringMember("severity"),
                                  advisory = issue.stringMember("advisoryurl");
                if (!package.status.empty())
                    package.status += "\n";
                package.status += severity + (advisory.empty() ? "" : ": " + advisory);
            }
        if (const auto* deprecation = row.find("deprecation"); deprecation && deprecation->kind == Json::Kind::Object) {
            package.status += deprecation->stringMember("message");
            if (const auto* reasons = deprecation->find("reasons"); array(reasons))
                for (const auto& reason : reasons->items)
                    if (reason.kind == Json::Kind::String)
                        package.status += " " + reason.text;
        }
        if (const auto* reasons = row.find("deprecationReasons"); array(reasons)) {
            package.status += "Deprecated";
            for (const auto& reason : reasons->items) {
                if (reason.kind != Json::Kind::String)
                    return fail(ErrorCode::Corrupt, "invalid NuGet deprecation reason");
                package.status += " " + reason.text;
            }
            if (const auto* alternative = row.find("alternativePackage");
                alternative && alternative->kind == Json::Kind::Object)
                package.status += " Alternative: " + alternative->stringMember("id") + " " +
                                  alternative->stringMember("versionRange");
        }
        report.packages.push_back(std::move(package));
    }
    return {};
}
} // namespace
Result<io::ProcessSpec> managedPackageSpec(const ManagedPackageRequest& request) {
    auto spec = managedProjectQuerySpec(request.project);
    if (!spec)
        return std::unexpected(std::move(spec.error()));
    if (!safe(request.search) || !safe(request.source) || !safe(request.configFile) ||
        (!request.configFile.empty() && !io::isAbsolutePath(request.configFile)))
        return fail(ErrorCode::InvalidArgument, "invalid NuGet query or config path");
    if (request.query == PackageQuery::Search) {
        if (request.search.empty() || request.search.front() == '-')
            return fail(ErrorCode::InvalidArgument, "enter a NuGet search term");
        spec->arguments = {"package", "search", request.search, "--format", "json", "--take", "30"};
        if (request.prerelease)
            spec->arguments.push_back("--prerelease");
        if (request.exactMatch)
            spec->arguments.push_back("--exact-match");
        if (!request.configFile.empty())
            spec->arguments.insert(spec->arguments.end(), {"--configfile", request.configFile});
    } else if (request.query == PackageQuery::Restore) {
        spec->arguments = {"restore", request.project.project, "--nologo",
                           "-property:Configuration=" + request.project.configuration};
        if (!request.project.targetFramework.empty())
            spec->arguments.push_back("-property:TargetFramework=" + request.project.targetFramework);
        if (!request.project.runtimeIdentifier.empty())
            spec->arguments.push_back("-property:RuntimeIdentifier=" + request.project.runtimeIdentifier);
        if (!request.configFile.empty())
            spec->arguments.insert(spec->arguments.end(), {"--configfile", request.configFile});
    } else {
        // .NET 10+ explicit no-restore: package inspection must not silently execute restore targets.
        spec->arguments = {"package",          "list", "--project",    request.project.project, "--format", "json",
                           "--output-version", "1",    "--no-restore", "--include-transitive"};
        if (!request.project.targetFramework.empty())
            spec->arguments.insert(spec->arguments.end(), {"--framework", request.project.targetFramework});
        if (request.query == PackageQuery::Outdated)
            spec->arguments.push_back("--outdated");
        if (request.query == PackageQuery::Vulnerable)
            spec->arguments.push_back("--vulnerable");
        if (request.query == PackageQuery::Deprecated)
            spec->arguments.push_back("--deprecated");
        if (request.prerelease && request.query == PackageQuery::Outdated)
            spec->arguments.push_back("--include-prerelease");
        if (!request.configFile.empty()) {
            if (request.query != PackageQuery::Outdated)
                return fail(ErrorCode::Unsupported, "this NuGet query uses the project NuGet.config hierarchy; "
                                                    "explicit config override requires Outdated, Search or Restore");
            spec->arguments.insert(spec->arguments.end(), {"--config", request.configFile});
        }
    }
    if (!request.source.empty()) {
        if (request.query != PackageQuery::Search && request.query != PackageQuery::Restore &&
            request.query != PackageQuery::Outdated && request.query != PackageQuery::Deprecated)
            return fail(ErrorCode::Unsupported, "this query uses configured project or audit sources");
        spec->arguments.insert(spec->arguments.end(), {"--source", request.source});
    }
    return spec;
}
Result<ManagedPackageReport> parseManagedPackageReport(std::string_view text, PackageQuery query) {
    auto root = detail::parseJson(text);
    if (!root)
        return std::unexpected(std::move(root.error()));
    ManagedPackageReport report;
    if (const auto* problems = root->find("problems"); array(problems))
        for (const auto& problem : problems->items) {
            const std::string message =
                problem.kind == Json::Kind::String ? problem.text : problem.stringMember("text");
            report.problems.push_back(message.empty() ? "NuGet reported an unspecified feed problem" : message);
        }
    if (query == PackageQuery::Search) {
        const auto* sources = root->find("searchResult");
        if (!array(sources))
            return fail(ErrorCode::Corrupt, "NuGet search response has no searchResult");
        for (const auto& source : sources->items) {
            const std::string name = source.stringMember("sourceName");
            if (name.empty())
                return fail(ErrorCode::Corrupt, "NuGet search source has no identity");
            if (auto parsed = packages(report, source.find("packages"), {}, name, false, true); !parsed)
                return std::unexpected(std::move(parsed.error()));
        }
    } else {
        const auto* projects = root->find("projects");
        if (!array(projects) || projects->items.size() != 1)
            return fail(ErrorCode::Corrupt, "expected one NuGet project report");
        report.projectPath = projects->items.front().stringMember("path");
        if (report.projectPath.empty())
            return fail(ErrorCode::Corrupt, "NuGet report lacks project identity");
        const auto* frameworks = projects->items.front().find("frameworks");
        if (!array(frameworks))
            return fail(ErrorCode::Corrupt, "NuGet report has no frameworks");
        for (const auto& framework : frameworks->items) {
            const std::string name = framework.stringMember("framework");
            if (name.empty())
                return fail(ErrorCode::Corrupt, "NuGet framework has no identity");
            for (const auto transitive : {false, true}) {
                const auto* rows = framework.find(transitive ? "transitivePackages" : "topLevelPackages");
                if (!rows)
                    continue;
                if (auto parsed = packages(report, rows, name, {}, transitive, false); !parsed)
                    return std::unexpected(std::move(parsed.error()));
            }
        }
    }
    return report;
}
Result<std::vector<ManagedPackageInput>>
snapshotManagedPackageInputs(io::IFileSystem& fs, const std::vector<std::string>& paths, std::stop_token stop) {
    if (paths.size() > 4096)
        return fail(ErrorCode::OutOfRange, "Too many MSBuild input files");
    std::vector<ManagedPackageInput> snapshots;
    size_t bytes = 0;
    for (const auto& path : paths) {
        if (stop.stop_requested())
            return fail(ErrorCode::Cancelled, "MSBuild input snapshots cancelled");
        auto contents = fs.readFileSnapshot(path, std::min<size_t>(4 * 1024 * 1024, 32 * 1024 * 1024 - bytes));
        if (!contents)
            return std::unexpected(contents.error());
        bytes += contents->size();
        snapshots.push_back({path, std::move(*contents)});
    }
    return snapshots;
}
Result<std::unique_ptr<BuildSession>> startManagedPackageQuery(io::IProcessLauncher& launcher,
                                                               const project::TrustStore& trust,
                                                               const ManagedPackageRequest& request,
                                                               const std::vector<std::string>& roots,
                                                               BuildCallbacks callbacks, IClock& clock) {
    auto spec = managedPackageSpec(request);
    if (!spec)
        return std::unexpected(std::move(spec.error()));
    if (roots.empty())
        return fail(ErrorCode::InvalidArgument, "NuGet query requires explicit workspace trust roots");
    for (const auto& root : roots)
        if (!io::isAbsolutePath(root) || !trust.isTrusted(root))
            return fail(ErrorCode::Untrusted, "trust the workspace before running NuGet");
    if (!trust.isTrusted(io::parentPath(request.project.project)))
        return fail(ErrorCode::Untrusted, "NuGet project is not trusted");
    return BuildSession::start(launcher, *spec, true, std::move(callbacks), clock);
}
} // namespace frost::build
