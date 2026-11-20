// SPDX-License-Identifier: Apache-2.0
// CPP-13 bounded wire decoding and owned actual preprocessor request.
#include "frost/lang/cpp_preprocessor.hpp"

#include "frost/io/path.hpp"
#include "frost/io/spawn_log.hpp"
namespace frost::lang {
Result<CppPreprocessorResult> parseCppPreprocessorResult(std::string_view bytes) {
    if (bytes.size() > 64U * 1024U * 1024U)
        return fail(ErrorCode::OutOfRange, "Preprocessor result exceeds64MiB");
    auto parsed = parseJson(bytes);
    if (!parsed)
        return std::unexpected(parsed.error());
    if (parsed->stringAt("kind") != "frost-preprocessor" || !parsed->find("schema") ||
        parsed->find("schema")->asUnsigned() != 1)
        return fail(ErrorCode::Unsupported, "Unknown preprocessor protocol");
    if (parsed->stringAt("status") != "ready")
        return fail(ErrorCode::Unsupported, std::string(parsed->stringAt("error")));
    const auto* text = parsed->find("text");
    const auto* includes = parsed->find("includes");
    const auto* macros = parsed->find("macros");
    if (!text || !text->asString() || text->asString()->size() > 16U * 1024U * 1024U || !includes ||
        !includes->isArray() || includes->items().size() > 100000 || !macros || !macros->isArray() ||
        macros->items().size() > 200000)
        return fail(ErrorCode::InvalidArgument, "Missing or oversized preprocessor output");
    const auto location = [](const Json* row) {
        if (!row || !row->isObject())
            return false;
        const auto path = row->find("path");
        if (!path || !path->asString() || (!path->asString()->empty() && !io::isAbsolutePath(*path->asString())))
            return false;
        for (const auto key : {"offset", "line", "column"})
            if (!row->find(key) || !row->find(key)->asUnsigned())
                return false;
        return true;
    };
    for (const auto& row : includes->items())
        if (!location(row.find("at")) || !row.find("name") || !row.find("name")->asString() || !row.find("target") ||
            !row.find("target")->asString() ||
            (!row.stringAt("target").empty() && !io::isAbsolutePath(row.stringAt("target"))) || !row.boolAt("angled") ||
            !row.boolAt("moduleImported"))
            return fail(ErrorCode::InvalidArgument, "Invalid include event");
    for (const auto& row : macros->items()) {
        if (!location(row.find("at")) || !location(row.find("end")) || !location(row.find("definition")) ||
            row.stringAt("name").empty() || !row.boolAt("functionLike") || !row.boolAt("variadic") ||
            !row.boolAt("nested"))
            return fail(ErrorCode::InvalidArgument, "Invalid macro event");
        for (const auto key : {"parameters", "definitionTokens"}) {
            const auto values = row.find(key);
            if (!values || !values->isArray())
                return fail(ErrorCode::InvalidArgument, "Missing macro tokens");
            for (const auto& token : values->items())
                if (!token.asString())
                    return fail(ErrorCode::InvalidArgument, "Invalid macro token");
        }
    }
    auto receipt = Json(*parsed)
                       .set("kind", "frost-semantic-index")
                       .set("symbols", Json::array())
                       .set("occurrences", Json::array())
                       .set("relations", Json::array())
                       .set("text", "")
                       .set("includes", Json::array())
                       .set("macros", Json::array());
    auto unit = parseCppIndexUnit(receipt.dump());
    if (!unit)
        return std::unexpected(unit.error());
    return CppPreprocessorResult{std::move(*unit), std::string(*text->asString()), *includes, *macros};
}
sched::Task<Result<CppPreprocessorResult>> requestCppPreprocessor(io::IProcessLauncher& launcher,
                                                                  io::AsyncContext context, std::string helper,
                                                                  CppIndexRequest request, IClock::Duration timeout,
                                                                  std::stop_token stop) {
    if (!io::isAbsolutePath(helper) || !io::isAbsolutePath(request.projectRoot) ||
        (!request.llvmRoot.empty() && !io::isAbsolutePath(request.llvmRoot)) || request.configuration.empty())
        co_return fail(ErrorCode::InvalidArgument,
                       "Preprocessor request needs absolute helper/project and configuration");
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
        co_return fail(ErrorCode::OutOfRange, "Preprocessor request exceeds32MiB");
    io::ProcessSpec spec;
    spec.executable = std::move(helper);
    spec.arguments = {"--preprocess-unit"};
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
    auto unit = parseCppPreprocessorResult(output->stdoutBytes);
    if (!unit)
        co_return std::unexpected(unit.error());
    if (output->exitCode || output->signal)
        co_return fail(ErrorCode::IoError, "Preprocessor helper did not exit successfully");
    if (unit->receipt.databaseSha256 != io::sha256Hex(request.compilationDatabase) ||
        unit->receipt.commandIndex != request.commandIndex || unit->receipt.configuration != request.configuration)
        co_return fail(ErrorCode::InvalidArgument, "Preprocessor output belongs to different captured commands");
    co_return std::move(*unit);
}
} // namespace frost::lang
