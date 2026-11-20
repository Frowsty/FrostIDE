// SPDX-License-Identifier: Apache-2.0
// CPP-8 typed real-frontend unit decoding, owned async request and fresh-input admission.
#include "frost/lang/cpp_index.hpp"

#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/io/spawn_log.hpp"

#include <algorithm>
#include <map>
#include <set>

namespace frost::lang {
namespace {
bool text(const Json& row, std::string_view key, std::size_t maximum = 32768) {
    const auto* value = row.find(key);
    return value && value->asString() && !value->asString()->empty() && value->asString()->size() <= maximum &&
           value->asString()->find('\0') == std::string_view::npos;
}
bool number(const Json& row, std::string_view key) {
    const auto* value = row.find(key);
    return value && value->asUnsigned() && *value->asUnsigned() <= static_cast<std::uint64_t>(INT64_MAX);
}
} // namespace
Result<CppIndexUnit> parseCppIndexUnit(std::string_view bytes) {
    if (bytes.size() > 64 * 1024 * 1024)
        return fail(ErrorCode::OutOfRange, "Semantic index unit exceeds64MiB");
    auto parsed = parseJson(bytes);
    if (!parsed)
        return std::unexpected(parsed.error());
    if (parsed->stringAt("kind") != "frost-semantic-index" || !parsed->find("schema") ||
        parsed->find("schema")->asUnsigned() != 1)
        return fail(ErrorCode::Unsupported, "Unknown semantic index protocol");
    if (parsed->stringAt("status") != "ready")
        return fail(ErrorCode::Unsupported, std::string(parsed->stringAt("error")));
    for (const auto key : {"commandIdentity", "configuration", "source", "compilationDatabaseSha256"})
        if (!text(*parsed, key))
            return fail(ErrorCode::InvalidArgument, "Missing semantic unit identity");
    if (!number(*parsed, "commandIndex") || parsed->stringAt("commandIdentity").size() != 64 ||
        parsed->stringAt("compilationDatabaseSha256").size() != 64 || !io::isAbsolutePath(parsed->stringAt("source")))
        return fail(ErrorCode::InvalidArgument, "Invalid semantic unit identity");
    const auto* symbols = parsed->find("symbols");
    const auto* occurrences = parsed->find("occurrences");
    const auto* relations = parsed->find("relations");
    const auto* dependencies = parsed->find("dependencies");
    if (!symbols || !symbols->isArray() || symbols->items().size() > 100000 || !occurrences ||
        !occurrences->isArray() || occurrences->items().size() > 250000 || !relations || !relations->isArray() ||
        relations->items().size() > 1000000 || !dependencies || !dependencies->isArray() ||
        dependencies->items().empty() || dependencies->items().size() > 32768)
        return fail(ErrorCode::InvalidArgument, "Semantic unit rows missing or oversized");
    std::set<std::string> identities;
    for (const auto& row : symbols->items())
        if (!text(row, "usr") || !text(row, "name") || !text(row, "kind") || !text(row, "qualifiedName") ||
            !identities.insert(std::string(row.stringAt("usr"))).second)
            return fail(ErrorCode::InvalidArgument, "Invalid/duplicate Clang symbol identity");
    for (const auto& row : occurrences->items())
        if (!identities.contains(std::string(row.stringAt("usr"))) || !text(row, "path") ||
            !io::isAbsolutePath(row.stringAt("path")) || !number(row, "offset") || !number(row, "line") ||
            !number(row, "column") || !number(row, "roles") || !row.boolAt("macro"))
            return fail(ErrorCode::InvalidArgument, "Invalid Clang occurrence");
    for (const auto& row : relations->items()) {
        const auto* index = row.find("occurrence");
        if (!index || !index->asUnsigned() || *index->asUnsigned() >= occurrences->items().size() ||
            !number(row, "roles") || !identities.contains(std::string(row.stringAt("target"))) ||
            row.stringAt("source") != occurrences->items()[*index->asUnsigned()].stringAt("usr"))
            return fail(ErrorCode::InvalidArgument, "Invalid Clang relation");
    }
    std::set<std::string> paths;
    std::map<std::string, std::uint64_t> inputSizes;
    bool sourceCaptured = false;
    for (const auto& row : dependencies->items()) {
        if (!text(row, "path") || !io::isAbsolutePath(row.stringAt("path")) || !text(row, "sha256", 64) ||
            row.stringAt("sha256").size() != 64 || !number(row, "bytes") ||
            !paths.insert(std::string(row.stringAt("path"))).second)
            return fail(ErrorCode::InvalidArgument, "Invalid input dependency");
        sourceCaptured |= row.stringAt("path") == parsed->stringAt("source");
        inputSizes.emplace(std::string(row.stringAt("path")), *row.find("bytes")->asUnsigned());
    }
    if (!sourceCaptured)
        return fail(ErrorCode::InvalidArgument, "Source has no captured dependency receipt");
    for (const auto& row : occurrences->items()) {
        const auto input = inputSizes.find(std::string(row.stringAt("path")));
        if (input == inputSizes.end() || *row.find("offset")->asUnsigned() > input->second ||
            !*row.find("line")->asUnsigned() || !*row.find("column")->asUnsigned() ||
            *row.find("line")->asUnsigned() > input->second + 1 ||
            *row.find("column")->asUnsigned() > input->second + 1)
            return fail(ErrorCode::InvalidArgument, "Occurrence has no captured source range");
    }
    for (const auto key : {"missingDependencies", "directoryDependencies"}) {
        const auto* values = parsed->find(key);
        if (!values || !values->isArray() || values->items().size() + paths.size() > 32768)
            return fail(ErrorCode::InvalidArgument, "Missing/oversized namespace dependencies");
        for (const auto& value : values->items())
            if (!value.asString() || !io::isAbsolutePath(*value.asString()) ||
                !paths.insert(std::string(*value.asString())).second)
                return fail(ErrorCode::InvalidArgument, "Invalid/overlapping namespace dependency");
    }
    CppIndexUnit result{
        std::string(parsed->stringAt("commandIdentity")), std::string(parsed->stringAt("configuration")),
        std::string(parsed->stringAt("source")),          std::string(parsed->stringAt("compilationDatabaseSha256")),
        *parsed->find("commandIndex")->asUnsigned(),      std::move(*parsed)};
    return result;
}
sched::Task<Result<CppIndexUnit>> requestCppIndexUnit(io::IProcessLauncher& launcher, io::AsyncContext context,
                                                      std::string helper, CppIndexRequest request,
                                                      IClock::Duration timeout, std::stop_token stop) {
    if (!io::isAbsolutePath(helper) || !io::isAbsolutePath(request.projectRoot) ||
        (!request.llvmRoot.empty() && !io::isAbsolutePath(request.llvmRoot)) || request.configuration.empty())
        co_return fail(ErrorCode::InvalidArgument, "Index request needs absolute helper/project and configuration");
    std::vector<Json> overlays;
    for (const auto& overlay : request.overlays)
        overlays.push_back(Json::object().set("path", overlay.path).set("contents", overlay.contents));
    auto payload = Json::object()
                       .set("projectRoot", request.projectRoot)
                       .set("compilationDatabase", request.compilationDatabase)
                       .set("configuration", request.configuration)
                       .set("commandIndex", request.commandIndex)
                       .set("overlays", Json::array(std::move(overlays)))
                       .dump();
    if (payload.size() > 32 * 1024 * 1024)
        co_return fail(ErrorCode::OutOfRange, "Index request exceeds32MiB");
    io::ProcessSpec spec;
    spec.executable = std::move(helper);
    spec.arguments = {"--index-unit"};
    spec.workingDirectory = request.projectRoot;
    spec.stdinMode = io::Stdio::Pipe;
    spec.belowNormalPriority = true;
    if (!request.llvmRoot.empty())
        spec.environment.emplace_back("FROST_LLVM_ROOT", request.llvmRoot);
    auto child = launcher.launchAsync(spec, context);
    if (!child)
        co_return std::unexpected(child.error());
    auto output = co_await (*child)->communicate(std::move(payload), timeout, stop);
    if (!output)
        co_return std::unexpected(output.error());
    auto unit = parseCppIndexUnit(output->stdoutBytes);
    if (!unit)
        co_return std::unexpected(unit.error());
    if (output->exitCode || output->signal)
        co_return fail(ErrorCode::IoError, "Index helper did not exit successfully");
    if (unit->databaseSha256 != io::sha256Hex(request.compilationDatabase) ||
        unit->commandIndex != request.commandIndex || unit->configuration != request.configuration)
        co_return fail(ErrorCode::InvalidArgument, "Index unit belongs to different captured commands");
    co_return std::move(*unit);
}
Result<void> validateCppIndexInputs(io::IFileSystem& fs, const CppIndexUnit& unit,
                                    std::span<const CppRenameOverlay> overlays, std::stop_token stop) {
    auto decoded = parseCppIndexUnit(unit.data.dump());
    if (!decoded)
        return std::unexpected(decoded.error());
    if (decoded->commandIdentity != unit.commandIdentity || decoded->configuration != unit.configuration ||
        decoded->source != unit.source || decoded->databaseSha256 != unit.databaseSha256 ||
        decoded->commandIndex != unit.commandIndex)
        return fail(ErrorCode::InvalidArgument, "Semantic unit metadata differs from validated payload");
    std::map<std::string, std::string_view> buffers;
    for (const auto& overlay : overlays) {
        auto path = io::normalizePath(overlay.path);
        if (!path || !buffers.emplace(*path, overlay.contents).second)
            return fail(ErrorCode::InvalidArgument, "Duplicate index overlay");
    }
    for (const auto& dependency : unit.data.find("dependencies")->items()) {
        if (stop.stop_requested())
            return fail(ErrorCode::Cancelled, "Index input validation cancelled");
        const auto path = io::normalizePath(dependency.stringAt("path"));
        if (!path)
            return std::unexpected(path.error());
        const auto found = buffers.find(*path);
        const auto disk = found == buffers.end() ? fs.readFileSnapshot(*path, 64U * 1024U * 1024U)
                                                 : Result<std::string>{std::string(found->second)};
        if (!disk || disk->size() != *dependency.find("bytes")->asUnsigned() ||
            io::sha256Hex(*disk) != dependency.stringAt("sha256"))
            return fail(ErrorCode::Busy, "Index dependency changed: " + *path);
    }
    for (const auto& value : unit.data.find("missingDependencies")->items()) {
        if (stop.stop_requested())
            return fail(ErrorCode::Cancelled, "Index validation cancelled");
        if (buffers.contains(std::string(*value.asString())))
            return fail(ErrorCode::Busy, "Previously absent input is now open");
        const auto status = fs.status(*value.asString());
        if (status || status.error().code() != ErrorCode::NotFound)
            return fail(ErrorCode::Busy, "Absent index input changed");
    }
    for (const auto& value : unit.data.find("directoryDependencies")->items()) {
        if (stop.stop_requested())
            return fail(ErrorCode::Cancelled, "Index validation cancelled");
        const auto status = fs.status(*value.asString());
        if (!status || status->type != io::FileType::Directory)
            return fail(ErrorCode::Busy, "Index directory probe changed");
    }
    return {};
}
} // namespace frost::lang
