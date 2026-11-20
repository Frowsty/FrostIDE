// SPDX-License-Identifier: Apache-2.0
// Decode actual dotnet MSBuild item/property reports; never infer SDK globs from project XML.
#include "frost/build/managed_evaluation.hpp"

#include "frost/io/path.hpp"
#include "frost/project/trust_store.hpp"

#include "json.hpp"

#include <algorithm>
#include <set>
namespace frost::build {
namespace {
constexpr std::string_view kinds[] = {"Compile",         "ProjectReference",  "PackageReference", "Analyzer",
                                      "AdditionalFiles", "EditorConfigFiles", "Reference",        "PackageVersion"};
Result<std::vector<std::string>> frameworks(std::string_view value) {
    std::vector<std::string> result;
    while (!value.empty()) {
        const auto end = value.find(';');
        const auto entry = value.substr(0, end);
        if (entry.empty() || entry.size() > 256 ||
            entry.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-_") !=
                std::string_view::npos)
            return fail(ErrorCode::Corrupt, "invalid evaluated target framework list");
        if (std::ranges::find(result, entry) != result.end())
            return fail(ErrorCode::Corrupt, "duplicate evaluated target framework");
        result.emplace_back(entry);
        if (result.size() > 256)
            return fail(ErrorCode::OutOfRange, "too many evaluated target frameworks");
        if (end == std::string_view::npos)
            break;
        value.remove_prefix(end + 1);
        if (value.empty())
            return fail(ErrorCode::Corrupt, "empty evaluated target framework");
    }
    return result;
}
} // namespace
Result<io::ProcessSpec> managedProjectEvaluationSpec(const ManagedProjectRequest& request) {
    auto spec = managedProjectQuerySpec(request);
    if (!spec)
        return std::unexpected(spec.error());
    spec->arguments[3] = "-getProperty:TargetPath,TargetFramework,TargetFrameworks,OutputType,RuntimeIdentifier,"
                         "MSBuildProjectFullPath,UsingMicrosoftNETSdk,AssemblyName,LangVersion,Nullable,"
                         "DefineConstants,IsTestProject,ManagePackageVersionsCentrally,"
                         "CentralPackageVersionOverrideEnabled,NuGetPackageRoot,MSBuildProjectExtensionsPath";
    spec->arguments.push_back("-getItem:Compile,ProjectReference,PackageReference,Analyzer,AdditionalFiles,"
                              "EditorConfigFiles,Reference,PackageVersion");
    spec->arguments.push_back("-property:BuildProjectReferences=false");
    return spec;
}
Result<ManagedProjectEvaluation> parseManagedProjectEvaluation(const ManagedProjectRequest& request,
                                                               std::string_view output) {
    if (output.size() > 32U * 1024U * 1024U)
        return fail(ErrorCode::OutOfRange, "managed evaluation exceeds 32 MiB");
    auto admission = managedProjectEvaluationSpec(request);
    if (!admission)
        return std::unexpected(admission.error());
    auto document = detail::parseJson(output);
    if (!document)
        return std::unexpected(document.error());
    auto properties = parseManagedProjectProperties(output);
    if (!properties)
        return std::unexpected(properties.error());
    const auto* values = document->find("Properties");
    const auto* items = document->find("Items");
    if (!values || !items || items->kind != detail::JsonValue::Kind::Object)
        return fail(ErrorCode::Corrupt, "managed evaluation lacks structured properties/items");
    for (const auto name : {"MSBuildProjectFullPath", "UsingMicrosoftNETSdk", "AssemblyName", "LangVersion", "Nullable",
                            "DefineConstants"}) {
        const auto* field = values->find(name);
        if (!field || field->kind != detail::JsonValue::Kind::String || field->text.find('\0') != std::string::npos)
            return fail(ErrorCode::Corrupt, "managed evaluation lacks a requested string property");
    }
    if (values->stringMember("UsingMicrosoftNETSdk") != "true")
        return fail(ErrorCode::Unsupported, "managed project is not SDK-style");
    const auto file = values->stringMember("MSBuildProjectFullPath");
    const auto expected = io::normalizePath(request.project), actual = io::normalizePath(file);
    if (!io::isAbsolutePath(file) || !expected || !actual || *expected != *actual)
        return fail(ErrorCode::Corrupt, "managed evaluation returned another project");
    if ((!request.targetFramework.empty() && properties->targetFramework != request.targetFramework) ||
        (!request.runtimeIdentifier.empty() && properties->runtimeIdentifier != request.runtimeIdentifier))
        return fail(ErrorCode::Corrupt, "managed evaluation differs from the selected TFM/RID");
    auto targets =
        frameworks(properties->targetFrameworks.empty() ? properties->targetFramework : properties->targetFrameworks);
    if (!targets)
        return std::unexpected(targets.error());
    ManagedProjectEvaluation result{request,
                                    *properties,
                                    file,
                                    values->stringMember("AssemblyName"),
                                    values->stringMember("LangVersion"),
                                    values->stringMember("Nullable"),
                                    values->stringMember("DefineConstants"),
                                    *targets,
                                    {},
                                    {}};
    for (std::size_t i = 0; i < values->memberNames.size(); ++i) {
        const auto& value = values->memberValues[i];
        if (value.kind != detail::JsonValue::Kind::String ||
            !result.properties.emplace(values->memberNames[i], value.text).second)
            return fail(ErrorCode::Corrupt, "invalid or duplicate evaluated managed property");
    }
    std::size_t count = 0;
    for (const auto kind : kinds) {
        const auto* list = items->find(kind);
        if (!list || list->kind != detail::JsonValue::Kind::Array)
            return fail(ErrorCode::Corrupt, "managed evaluation lacks a requested item list");
        auto& destination = result.items[std::string(kind)];
        for (const auto& value : list->items) {
            if (++count > 200000 || value.kind != detail::JsonValue::Kind::Object)
                return fail(ErrorCode::OutOfRange, "managed evaluation item budget exceeded");
            ManagedEvaluatedItem entry;
            for (std::size_t i = 0; i < value.memberNames.size(); ++i) {
                const auto& name = value.memberNames[i];
                const auto& metadata = value.memberValues[i];
                if (metadata.kind != detail::JsonValue::Kind::String || metadata.text.find('\0') != std::string::npos ||
                    !entry.metadata.emplace(name, metadata.text).second)
                    return fail(ErrorCode::Corrupt, "invalid or duplicate managed item metadata");
            }
            const auto identity = entry.metadata.find("Identity");
            if (identity == entry.metadata.end() || identity->second.empty())
                return fail(ErrorCode::Corrupt, "evaluated managed item has no Identity");
            entry.identity = identity->second;
            destination.push_back(std::move(entry));
        }
    }
    return result;
}
Result<std::unique_ptr<BuildSession>> startManagedProjectEvaluation(io::IProcessLauncher& launcher,
                                                                    const project::TrustStore& trust,
                                                                    const ManagedProjectRequest& request,
                                                                    const std::vector<std::string>& roots,
                                                                    BuildCallbacks callbacks, IClock& clock) {
    auto spec = managedProjectEvaluationSpec(request);
    if (!spec)
        return std::unexpected(spec.error());
    if (roots.empty())
        return fail(ErrorCode::InvalidArgument, "managed evaluation requires explicit trust roots");
    for (const auto& root : roots)
        if (!io::isAbsolutePath(root) || !trust.isTrusted(root))
            return fail(ErrorCode::Untrusted, "managed evaluation root is not trusted");
    if (!trust.isTrusted(io::parentPath(request.project)))
        return fail(ErrorCode::Untrusted, "managed evaluation project is not trusted");
    return BuildSession::start(launcher, *spec, true, std::move(callbacks), clock);
}
} // namespace frost::build
