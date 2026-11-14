// SPDX-License-Identifier: Apache-2.0
#include "frost/lang/inspection_scope_fixes.hpp"
#include "frost/io/path.hpp"
#include "frost/lang/uri.hpp"
namespace frost::lang {
Result<InspectionScopeFixProposal> runInspectionScopeFixes(io::IProcessLauncher& launcher, io::IFileSystem& fs,
    InspectionRequest request, const InspectionScopeCapture& capture, std::string helper, std::string scratchDirectory) {
    auto scratch = fs.physicalPath(scratchDirectory);
    if (!scratch || !io::isAbsolutePath(helper)) return fail(ErrorCode::InvalidArgument, "A private scratch directory and absolute LLVM helper are required");
    const auto root = documentKey(capture.physicalRoot), temp = documentKey(*scratch);
    if (temp == root || (temp.starts_with(root) && temp.size() > root.size() && (temp[root.size()] == '/' || temp[root.size()] == '\\')))
        return fail(ErrorCode::InvalidArgument, "Inspection scratch must be outside the source project");
    auto entries = fs.listDirectory(*scratch);
    if (!entries || !entries->empty()) return fail(ErrorCode::AlreadyExists, "Inspection scratch directory must be uniquely owned and empty");
    if (request.stopToken.stop_requested()) return fail(ErrorCode::Cancelled, "Inspection cancelled");
    if (auto current = validateInspectionCapture(fs, capture); !current) return std::unexpected(current.error());
    auto roots = Json::array();
    std::string databaseBytes;
    std::size_t index = 0;
    for (const auto& image : capture.images) {
        const auto file = io::joinPath(*scratch, "input-" + std::to_string(index++) + ".txt");
        if (!file) return std::unexpected(file.error());
        if (auto written = fs.writeFileAtomic(*file, image.contents); !written) return std::unexpected(written.error());
        roots.push(Json::object().set("type", "file").set("name", image.path).set("external-contents", *file));
        if (image.path == capture.databasePath) databaseBytes = image.contents;
    }
    if (databaseBytes.empty()) return fail(ErrorCode::Corrupt, "Compilation database was not captured");
    const auto database = io::joinPath(*scratch, "compile_commands.json").value_or("");
    const auto overlay = io::joinPath(*scratch, "inputs.json").value_or("");
    const auto exported = io::joinPath(*scratch, "fixes.yaml").value_or("");
    if (auto written = fs.writeFileAtomic(database, databaseBytes); !written) return std::unexpected(written.error());
    const auto mapping = Json::object().set("version", 0).set("case-sensitive", io::kNativePathStyle != io::PathStyle::Windows)
        .set("use-external-names", false).set("roots", std::move(roots));
    if (auto written = fs.writeFileAtomic(overlay, mapping.dump()); !written) return std::unexpected(written.error());
    for (const auto& arg : request.extraArgs) {
        auto option = std::string_view(arg); const auto dash = option.find_first_not_of('-');
        if (dash == option.npos) return fail(ErrorCode::Unsupported, "Inspection option separators cannot change the owned command");
        option.remove_prefix(dash); option = option.substr(0, option.find('='));
        if (option == "fix" || option == "fix-errors" || option == "fix-notes" || option == "export-fixes" || option == "vfsoverlay" || option == "header-filter" || option == "p")
            return fail(ErrorCode::Unsupported, "Scope fix analysis cannot mutate sources or override owned outputs");
    }
    request.compileCommandsDir = *scratch;
    request.exportFixesPath = exported;
    if (!request.profile.respectProjectClangTidy) request.extraArgs.push_back("--config={}");
    std::string headerFilter = "^";
    for (const char c : capture.root) {
        if (c == '/' || c == '\\') headerFilter += "[/\\\\]";
        else { if (std::string_view("^$.*+?[](){}|\\").find(c) != std::string_view::npos) headerFilter += '\\'; headerFilter += c; }
    }
    headerFilter += "[/\\\\]";
    request.extraArgs.push_back("--header-filter=" + headerFilter);
    request.extraArgs.push_back("--vfsoverlay=" + overlay);
    auto report = runClangTidyInspection(launcher, fs, request);
    if (!report) return std::unexpected(report.error());
    if (!report->executionSuccessful) return fail(ErrorCode::IoError, "clang-tidy did not complete the whole selected scope: " + report->rawStderr);
    auto yaml = fs.readFileSnapshot(exported, 64U * 1024U * 1024U);
    if (!yaml) return fail(ErrorCode::Unsupported, "clang-tidy supplied no machine-readable exported fix report; no scope fix can be applied");
    io::ProcessSpec spec; spec.executable = std::move(helper); spec.arguments = {"--decode-tidy-fixes"}; spec.killOnClose = true;
    auto child = launcher.launch(spec); if (!child) return std::unexpected(child.error());
    std::stop_callback cancel(request.stopToken, [&] { static_cast<void>((*child)->terminate()); });
    auto decoded = (*child)->communicate(*yaml, request.timeout);
    if (request.stopToken.stop_requested()) return fail(ErrorCode::Cancelled, "Inspection cancelled");
    if (!decoded) return std::unexpected(decoded.error());
    if (decoded->exitCode != 0) return fail(ErrorCode::Corrupt, "LLVM could not decode clang-tidy exported fixes: " + decoded->stderrBytes);
    auto body = parseJson(decoded->stdoutBytes); if (!body) return std::unexpected(body.error());
    if (auto current = validateInspectionCapture(fs, capture); !current) return std::unexpected(current.error());
    return planInspectionScopeFixes(*body, capture, request.profile);
}
}
