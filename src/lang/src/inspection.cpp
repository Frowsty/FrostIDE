// SPDX-License-Identifier: Apache-2.0
//
// C++ inspection framework (inspection.hpp, epic CPP-10): locating and running the real clang-tidy
// binary via frost::io::IProcessLauncher, resolving compile_commands.json scopes, applying profile
// severities, and exporting/validating SARIF 2.1.0 reports.

#include "frost/lang/inspection.hpp"

#include "frost/io/path.hpp"
#include "frost/lang/uri.hpp"
#include "frost/project/cmake_compile_database.hpp"

#include <algorithm>
#include <charconv>
#include <format>
#include <set>
#include <stop_token>
#include <thread>

namespace frost::lang {
namespace {

#ifdef _WIN32
constexpr std::string_view kClangTidyName = "clang-tidy.exe";
constexpr char kPathListSeparator = ';';
#else
constexpr std::string_view kClangTidyName = "clang-tidy";
constexpr char kPathListSeparator = ':';
#endif

[[nodiscard]] char asciiLower(char ch) noexcept {
    return (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a') : ch;
}

[[nodiscard]] std::string toLowerAscii(std::string_view text) {
    std::string out(text.size(), '\0');
    for (std::size_t i = 0; i < text.size(); ++i) {
        out[i] = asciiLower(text[i]);
    }
    return out;
}

[[nodiscard]] std::string_view trimWhitespace(std::string_view text) noexcept {
    while (!text.empty() &&
           (text.front() == ' ' || text.front() == '\t' || text.front() == '\r' || text.front() == '\n')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r' || text.back() == '\n')) {
        text.remove_suffix(1);
    }
    return text;
}

[[nodiscard]] bool isRegularFile(io::IFileSystem& fs, std::string_view path) {
    auto status = fs.status(path);
    return status && status->type == io::FileType::Regular;
}

[[nodiscard]] bool isDirectoryPath(io::IFileSystem& fs, std::string_view path) {
    auto status = fs.status(path);
    return status && status->type == io::FileType::Directory;
}

[[nodiscard]] std::optional<std::string> candidateTidyIn(std::string_view directory) {
    std::string dir(directory);
    if (dir.size() >= 2 && dir.front() == '"' && dir.back() == '"') {
        dir = dir.substr(1, dir.size() - 2);
    }
    if (dir.empty() || !io::isAbsolutePath(dir)) {
        return std::nullopt;
    }
    auto normalized = io::normalizePath(dir);
    if (!normalized) {
        return std::nullopt;
    }
    auto joined = io::joinPath(*normalized, kClangTidyName);
    return joined ? std::optional<std::string>(std::move(*joined)) : std::nullopt;
}

[[nodiscard]] std::string parseVersionOutput(std::string_view text) {
    const std::string lower = toLowerAscii(text);
    if (const std::size_t pos = lower.find("version "); pos != std::string::npos) {
        std::string_view rest = trimWhitespace(text.substr(pos + 8));
        std::size_t end = 0;
        while (end < rest.size() && rest[end] != ' ' && rest[end] != '\t' && rest[end] != '\r' && rest[end] != '\n') {
            ++end;
        }
        if (end > 0) {
            return std::string(rest.substr(0, end));
        }
    }
    std::string_view trimmed = trimWhitespace(text);
    const std::size_t newline = trimmed.find_first_of("\r\n");
    return std::string(trimmed.substr(0, newline));
}

[[nodiscard]] Result<ClangTidyLocation> locateClangTidyBinary(io::IFileSystem& fs, const ClangdSearchOptions& search) {
    if (!search.configuredPath.empty()) {
        if (!io::isAbsolutePath(search.configuredPath)) {
            return fail(ErrorCode::InvalidArgument,
                        std::format("clang-tidy path '{}' is not an absolute path", search.configuredPath));
        }
        auto normalized = io::normalizePath(search.configuredPath);
        if (!normalized) {
            return fail(ErrorCode::InvalidArgument,
                        std::format("clang-tidy path '{}' is malformed", search.configuredPath));
        }
        std::string candidate = *normalized;
        if (!isRegularFile(fs, candidate)) {
            return fail(ErrorCode::NotFound, std::format("clang-tidy path '{}' does not exist", candidate));
        }
        return ClangTidyLocation{.path = std::move(candidate), .origin = ClangdOrigin::Setting, .version = {}};
    }

    std::vector<std::string> tried;
    for (const std::string& directory : search.sdkDirectories) {
        if (auto candidate = candidateTidyIn(directory)) {
            if (isRegularFile(fs, *candidate)) {
                return ClangTidyLocation{
                    .path = std::move(*candidate), .origin = ClangdOrigin::PinnedSdk, .version = {}};
            }
            tried.push_back(std::move(*candidate));
        }
    }
    std::string_view rest = search.pathVariable;
    while (!rest.empty()) {
        const std::size_t end = std::min(rest.find(kPathListSeparator), rest.size());
        const std::string_view entry = rest.substr(0, end);
        rest = end < rest.size() ? rest.substr(end + 1) : std::string_view();
        if (auto candidate = candidateTidyIn(entry)) {
            if (isRegularFile(fs, *candidate)) {
                return ClangTidyLocation{.path = std::move(*candidate), .origin = ClangdOrigin::Path, .version = {}};
            }
        }
    }
    std::string message = "clang-tidy not found; tried";
    for (const std::string& path : tried) {
        message += ' ';
        message += path;
    }
    message += " and PATH";
    return fail(ErrorCode::NotFound, std::move(message));
}

[[nodiscard]] bool globMatches(std::string_view pattern, std::string_view text) noexcept {
    std::size_t p = 0;
    std::size_t t = 0;
    std::size_t starIdx = std::string_view::npos;
    std::size_t matchIdx = 0;

    while (t < text.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == text[t])) {
            ++p;
            ++t;
        } else if (p < pattern.size() && pattern[p] == '*') {
            starIdx = p++;
            matchIdx = t;
        } else if (starIdx != std::string_view::npos) {
            p = starIdx + 1;
            t = ++matchIdx;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') {
        ++p;
    }
    return p == pattern.size();
}

[[nodiscard]] bool ruleMatchesId(std::string_view pattern, std::string_view ruleId) noexcept {
    if (globMatches(pattern, ruleId)) {
        return true;
    }
    // clang-tidy can emit comma-separated aliases in `[rule-a,rule-b]`.
    std::string_view rest = ruleId;
    while (!rest.empty()) {
        const std::size_t comma = rest.find(',');
        const std::string_view part = trimWhitespace(comma == std::string_view::npos ? rest : rest.substr(0, comma));
        if (!part.empty() && globMatches(pattern, part)) {
            return true;
        }
        if (comma == std::string_view::npos) {
            break;
        }
        rest.remove_prefix(comma + 1);
    }
    return false;
}

[[nodiscard]] std::string normalizeSlashesAndCase(std::string_view path) {
    std::string out;
    if (io::isAbsolutePath(path)) {
        if (auto norm = io::normalizePath(path)) {
            out = std::move(*norm);
        }
    }
    if (out.empty()) {
        out.assign(path);
    }
#ifdef _WIN32
    // normalizePath returns native backslashes. Comparison keys always use '/', including
    // directory boundary checks; returned file paths retain their native spelling.
    std::ranges::replace(out, '\\', '/');
    out = toLowerAscii(out);
#endif
    while (out.size() > 1 && out.back() == '/') {
        out.pop_back();
    }
    return out;
}

[[nodiscard]] bool pathsEqualNormalized(std::string_view a, std::string_view b) {
    return normalizeSlashesAndCase(a) == normalizeSlashesAndCase(b);
}

[[nodiscard]] bool isUnderDirectory(std::string_view filePath, std::string_view dirPath) {
    const std::string fileNorm = normalizeSlashesAndCase(filePath);
    const std::string dirNorm = normalizeSlashesAndCase(dirPath);
    if (dirNorm.empty()) {
        return false;
    }
    if (fileNorm == dirNorm) {
        return true;
    }
    if (!fileNorm.starts_with(dirNorm)) {
        return false;
    }
    if (dirNorm.back() == '/') {
        return true;
    }
    return fileNorm.size() > dirNorm.size() && fileNorm[dirNorm.size()] == '/';
}

[[nodiscard]] std::string resolveEntryFilePath(const CompileCommand& entry) {
    if (io::isAbsolutePath(entry.file)) {
        if (auto norm = io::normalizePath(entry.file)) {
            return std::move(*norm);
        }
        return entry.file;
    }
    if (!entry.directory.empty() && io::isAbsolutePath(entry.directory)) {
        if (auto joined = io::joinPath(entry.directory, entry.file)) {
            return std::move(*joined);
        }
    }
    return entry.file;
}

[[nodiscard]] bool matchesTargetName(const CompileCommand& entry, std::string_view targetName) {
    if (targetName.empty()) {
        return false;
    }
    const std::string lowerTarget = toLowerAscii(targetName);
    const std::string cmakeMarker = "cmakefiles/" + lowerTarget + ".dir/";

    auto textContainsTarget = [&](std::string_view raw) {
        std::string norm = toLowerAscii(raw);
        std::ranges::replace(norm, '\\', '/');
        if (norm.find(cmakeMarker) != std::string::npos) {
            return true;
        }
        if (norm.find("/" + lowerTarget + ".dir/") != std::string::npos) {
            return true;
        }
        return false;
    };

    for (const std::string& arg : entry.arguments) {
        if (textContainsTarget(arg)) {
            return true;
        }
    }

    return false;
}

[[nodiscard]] bool parsePositiveUint32(std::string_view text, std::uint32_t& out) noexcept {
    if (text.empty()) {
        return false;
    }
    std::uint32_t value = 0;
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || ptr != text.data() + text.size() || value == 0) {
        return false;
    }
    out = value;
    return true;
}

struct ParsedDiagHeader {
    std::string file;
    std::uint32_t line = 1;
    std::uint32_t column = 1;
    DiagnosticSeverity toolSeverity = DiagnosticSeverity::Warning;
    std::string message;
    std::string ruleId;
};

[[nodiscard]] std::optional<ParsedDiagHeader> parseDiagnosticLine(std::string_view rawLine) {
    std::string_view line = trimWhitespace(rawLine);
    if (line.empty() || line.back() != ']') {
        return std::nullopt;
    }
    const std::size_t openBracket = line.rfind(" [");
    if (openBracket == std::string_view::npos || openBracket + 2 >= line.size() - 1) {
        return std::nullopt;
    }
    const std::string_view ruleId = line.substr(openBracket + 2, line.size() - openBracket - 3);
    if (ruleId.empty() || ruleId.find(' ') != std::string_view::npos) {
        return std::nullopt;
    }

    const std::string_view beforeRule = line.substr(0, openBracket);
    struct SevToken {
        std::string_view token;
        DiagnosticSeverity severity;
    };
    constexpr SevToken kSeverities[] = {
        {": fatal error: ", DiagnosticSeverity::Error}, {": error: ", DiagnosticSeverity::Error},
        {": warning: ", DiagnosticSeverity::Warning},   {": remark: ", DiagnosticSeverity::Information},
        {": note: ", DiagnosticSeverity::Information},
    };

    for (const auto& candidate : kSeverities) {
        std::size_t searchFrom = 0;
        while (searchFrom < beforeRule.size()) {
            const std::size_t sevPos = beforeRule.find(candidate.token, searchFrom);
            if (sevPos == std::string_view::npos) {
                break;
            }
            const std::string_view prefix = beforeRule.substr(0, sevPos);
            const std::size_t colColon = prefix.rfind(':');
            if (colColon != std::string_view::npos && colColon > 0) {
                const std::string_view beforeCol = prefix.substr(0, colColon);
                const std::size_t lineColon = beforeCol.rfind(':');
                std::uint32_t parsedLine = 0;
                std::uint32_t parsedCol = 0;
                if (lineColon != std::string_view::npos && lineColon > 0 &&
                    parsePositiveUint32(beforeCol.substr(lineColon + 1), parsedLine) &&
                    parsePositiveUint32(prefix.substr(colColon + 1), parsedCol)) {
                    if (candidate.token == ": note: ") {
                        return std::nullopt;
                    }
                    std::string fileStr(beforeCol.substr(0, lineColon));
                    if (io::isAbsolutePath(fileStr)) {
                        if (auto norm = io::normalizePath(fileStr)) {
                            fileStr = std::move(*norm);
                        }
                    }
                    std::string_view msg = trimWhitespace(beforeRule.substr(sevPos + candidate.token.size()));
                    return ParsedDiagHeader{
                        .file = std::move(fileStr),
                        .line = parsedLine,
                        .column = parsedCol,
                        .toolSeverity = candidate.severity,
                        .message = std::string(msg),
                        .ruleId = std::string(ruleId),
                    };
                }
            }
            searchFrom = sevPos + 1;
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::string formatCommandLine(std::string_view executable, std::span<const std::string> args) {
#ifdef _WIN32
    std::vector<std::string> arguments(args.begin(), args.end());
    if (auto line = io::buildWindowsCommandLine(executable, arguments)) {
        return std::move(*line);
    }
    return std::string(executable);
#else
    auto quote = [](std::string_view arg) {
        if (arg.find_first_of(" \t\n'\"") == std::string_view::npos) {
            return std::string(arg);
        }
        std::string out = "'";
        for (const char ch : arg) {
            if (ch == '\'') {
                out += "'\\''";
            } else {
                out += ch;
            }
        }
        out += '\'';
        return out;
    };
    std::string out = quote(executable);
    for (const std::string& arg : args) {
        out += ' ';
        out += quote(arg);
    }
    return out;
#endif
}

[[nodiscard]] bool commandsUseMsvcPch(std::span<const CompileCommand> commands) {
    for (const CompileCommand& entry : commands) {
        for (const std::string& arg : entry.arguments) {
            if (arg.starts_with("/Yu") || arg.starts_with("-Yu") || arg.starts_with("/Fp") || arg.starts_with("-Fp")) {
                return true;
            }
        }
    }
    return false;
}

[[nodiscard]] bool isValidSarifLevel(std::string_view level) noexcept {
    return level == "error" || level == "warning" || level == "note" || level == "none";
}

} // namespace

// --- Locating clang-tidy -------------------------------------------------------------------------

Result<std::string> queryClangTidyVersion(io::IProcessLauncher& launcher, std::string_view clangTidyPath) {
    if (clangTidyPath.empty() || !io::isAbsolutePath(clangTidyPath)) {
        return fail(ErrorCode::InvalidArgument, "clang-tidy path must be an absolute path");
    }
    io::ProcessSpec spec;
    spec.executable = std::string(clangTidyPath);
    spec.arguments = {"--version"};
    auto process = launcher.launch(spec);
    if (!process) {
        return fail(process.error().code(), process.error().message());
    }
    auto result = (*process)->communicate({}, std::chrono::seconds{15});
    if (!result) {
        return fail(result.error().code(), result.error().message());
    }
    if (result->exitCode != 0) {
        return fail(ErrorCode::IoError, std::format("clang-tidy --version exited with code {}", result->exitCode));
    }
    std::string version = parseVersionOutput(result->stdoutBytes);
    if (version.empty()) {
        version = parseVersionOutput(result->stderrBytes);
    }
    if (version.empty()) {
        return fail(ErrorCode::Corrupt, "clang-tidy --version returned no version");
    }
    return version;
}

Result<ClangTidyLocation> findClangTidy(io::IFileSystem& fs, io::IProcessLauncher& launcher,
                                        const ClangdSearchOptions& options) {
    auto located = locateClangTidyBinary(fs, options);
    if (!located) {
        return fail(located.error().code(), located.error().message());
    }
    auto version = queryClangTidyVersion(launcher, located->path);
    if (!version) {
        return fail(version.error().code(), version.error().message());
    }
    located->version = std::move(*version);
    return located;
}

Result<ClangTidyLocation> findClangTidy(io::IFileSystem& fs, const ClangdSearchOptions& options) {
    auto located = locateClangTidyBinary(fs, options);
    if (!located) {
        return fail(located.error().code(), located.error().message());
    }
    // This overload only discovers the path. It does not invent version metadata when no real
    // process query was made; callers that need a version use the launcher overload.
    return located;
}

Result<ClangTidyLocation> findClangTidy(const io::IFileSystem& fs, const ClangdSearchOptions& options) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
    return findClangTidy(const_cast<io::IFileSystem&>(fs), options);
}

// --- Profiles, Severities, and Scopes ------------------------------------------------------------

std::string_view toString(InspectionSeverity severity) noexcept {
    switch (severity) {
    case InspectionSeverity::Error: return "error";
    case InspectionSeverity::Warning: return "warning";
    case InspectionSeverity::WeakWarning: return "weak_warning";
    case InspectionSeverity::Info: return "info";
    case InspectionSeverity::Disabled: return "disabled";
    }
    return "warning";
}

std::string_view toSarifLevel(InspectionSeverity severity) noexcept {
    switch (severity) {
    case InspectionSeverity::Error: return "error";
    case InspectionSeverity::Warning: return "warning";
    case InspectionSeverity::WeakWarning:
    case InspectionSeverity::Info: return "note";
    case InspectionSeverity::Disabled: return "none";
    }
    return "warning";
}

InspectionSeverity fromDiagnosticSeverity(DiagnosticSeverity severity) noexcept {
    switch (severity) {
    case DiagnosticSeverity::Error: return InspectionSeverity::Error;
    case DiagnosticSeverity::Warning: return InspectionSeverity::Warning;
    case DiagnosticSeverity::Information: return InspectionSeverity::Info;
    case DiagnosticSeverity::Hint: return InspectionSeverity::WeakWarning;
    }
    return InspectionSeverity::Warning;
}

std::string InspectionProfile::toChecksArgument() const {
    if (rules.empty()) {
        return respectProjectClangTidy ? std::string() : std::string("-*");
    }
    std::string out;
    if (!respectProjectClangTidy) {
        out = "-*";
    }
    for (const InspectionRuleConfig& rule : rules) {
        std::string_view pattern = trimWhitespace(rule.checkPattern);
        if (pattern.empty()) {
            continue;
        }
        const bool startsNegative = pattern.front() == '-';
        if (startsNegative || pattern.front() == '+') {
            pattern.remove_prefix(1);
        }
        if (pattern.empty()) {
            continue;
        }
        const bool disabled = startsNegative || !rule.enabled || rule.severity == InspectionSeverity::Disabled;
        if (!out.empty()) {
            out += ',';
        }
        if (disabled) {
            out += '-';
        }
        out.append(pattern);
    }
    return out;
}

InspectionSeverity InspectionProfile::effectiveSeverity(std::string_view ruleId,
                                                        DiagnosticSeverity toolSeverity) const noexcept {
    std::optional<InspectionSeverity> matched;
    for (const InspectionRuleConfig& rule : rules) {
        std::string_view pattern = trimWhitespace(rule.checkPattern);
        if (pattern.empty()) {
            continue;
        }
        const bool startsNegative = pattern.front() == '-';
        if (startsNegative || pattern.front() == '+') {
            pattern.remove_prefix(1);
        }
        if (pattern.empty()) {
            continue;
        }
        if (ruleMatchesId(pattern, ruleId)) {
            if (startsNegative || !rule.enabled || rule.severity == InspectionSeverity::Disabled) {
                matched = InspectionSeverity::Disabled;
            } else {
                matched = rule.severity;
            }
        }
    }
    if (matched) {
        return *matched;
    }
    return fromDiagnosticSeverity(toolSeverity);
}

Result<std::vector<CompileCommand>> loadCompileCommands(io::IFileSystem& fs,
                                                        std::string_view compileCommandsPathOrDir) {
    if (compileCommandsPathOrDir.empty()) {
        return fail(ErrorCode::InvalidArgument, "compile_commands.json path is empty");
    }
    std::string target(compileCommandsPathOrDir);
    if (isDirectoryPath(fs, target) || !toLowerAscii(target).ends_with(".json")) {
        auto joined = io::joinPath(target, "compile_commands.json");
        if (!joined) {
            return fail(ErrorCode::InvalidArgument, std::format("invalid compile_commands directory '{}'", target));
        }
        target = std::move(*joined);
    }
    auto contents = fs.readFile(target);
    if (!contents) {
        return std::unexpected(std::move(contents.error()));
    }
    auto parsed = parseJson(*contents);
    if (!parsed) {
        return fail(ErrorCode::Corrupt,
                    std::format("compile commands '{}' are invalid JSON: {}", target, parsed.error().message()));
    }
    if (!parsed->isArray()) {
        return fail(ErrorCode::Corrupt, std::format("compile commands '{}' must be a JSON array", target));
    }
    std::vector<CompileCommand> commands;
    commands.reserve(parsed->size());
    for (const Json& item : parsed->items()) {
        if (!item.isObject()) {
            return fail(ErrorCode::Corrupt, "a compile command entry must be a JSON object");
        }
        const std::string_view directory = item.stringAt("directory");
        const std::string_view file = item.stringAt("file");
        if (directory.empty() || file.empty()) {
            return fail(ErrorCode::Corrupt, "a compile command entry requires non-empty directory and file fields");
        }
        CompileCommand command{.directory = std::string(directory), .file = std::string(file), .arguments = {}};
        if (const Json* arguments = item.find("arguments"); arguments != nullptr) {
            if (!arguments->isArray()) {
                return fail(ErrorCode::Corrupt, "compile command 'arguments' must be an array");
            }
            command.arguments.reserve(arguments->size());
            for (const Json& argument : arguments->items()) {
                if (!argument.isString()) {
                    return fail(ErrorCode::Corrupt, "compile command arguments must all be strings");
                }
                command.arguments.emplace_back(*argument.asString());
            }
        } else if (const Json* text = item.find("command"); text != nullptr && text->isString()) {
            // Preserve argument boundaries for scope matching and MSVC PCH detection even when
            // the database uses its shell-escaped command form. clang-tidy reads the original file.
            command.arguments = project::splitCompileCommandFragment(*text->asString(), project::nativeCMakeShell());
        } else {
            return fail(ErrorCode::Corrupt, "a compile command entry requires an arguments array or command string");
        }
        commands.push_back(std::move(command));
    }
    return commands;
}

std::vector<std::string> resolveInspectionScopeFiles(std::span<const CompileCommand> commands,
                                                     const InspectionScope& scope) {
    std::vector<std::string> resolved;
    std::set<std::string> seenNormalized;

    auto addUnique = [&](std::string path) {
        if (path.empty()) {
            return;
        }
        if (io::isAbsolutePath(path)) {
            if (auto norm = io::normalizePath(path)) {
                path = std::move(*norm);
            }
        }
        const std::string key = normalizeSlashesAndCase(path);
        if (seenNormalized.insert(key).second) {
            resolved.push_back(std::move(path));
        }
    };

    for (const CompileCommand& entry : commands) {
        std::string tuPath = resolveEntryFilePath(entry);
        if (tuPath.empty()) {
            continue;
        }
        switch (scope.kind) {
        case InspectionScopeKind::Project: addUnique(std::move(tuPath)); break;
        case InspectionScopeKind::File:
            for (const std::string& scopeFile : scope.paths) {
                if (pathsEqualNormalized(tuPath, scopeFile)) {
                    addUnique(std::move(tuPath));
                    break;
                }
            }
            break;
        case InspectionScopeKind::Directory:
            for (const std::string& scopeDir : scope.paths) {
                if (isUnderDirectory(tuPath, scopeDir)) {
                    addUnique(std::move(tuPath));
                    break;
                }
            }
            break;
        case InspectionScopeKind::Target: {
            bool matched = matchesTargetName(entry, scope.targetName);
            if (!matched) {
                for (const std::string& scopePath : scope.paths) {
                    if (pathsEqualNormalized(tuPath, scopePath) || isUnderDirectory(tuPath, scopePath)) {
                        matched = true;
                        break;
                    }
                }
            }
            if (matched) {
                addUnique(std::move(tuPath));
            }
            break;
        }
        }
    }

    if (commands.empty() && (scope.kind == InspectionScopeKind::File || scope.kind == InspectionScopeKind::Project)) {
        for (const std::string& path : scope.paths) {
            addUnique(path);
        }
    }
    return resolved;
}

Result<std::vector<std::string>> resolveInspectionScopeFiles(io::IFileSystem& fs,
                                                             std::string_view compileCommandsPathOrDir,
                                                             const InspectionScope& scope) {
    auto commands = loadCompileCommands(fs, compileCommandsPathOrDir);
    if (!commands) {
        return fail(commands.error().code(), commands.error().message());
    }
    return resolveInspectionScopeFiles(*commands, scope);
}

// --- Real clang-tidy Runner ----------------------------------------------------------------------

std::vector<InspectionFinding> parseClangTidyOutput(std::string_view output, const InspectionProfile& profile) {
    std::vector<InspectionFinding> findings;
    std::size_t pos = 0;
    while (pos < output.size()) {
        const std::size_t eol = output.find('\n', pos);
        std::string_view line = eol == std::string_view::npos ? output.substr(pos) : output.substr(pos, eol - pos);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        pos = eol == std::string_view::npos ? output.size() : eol + 1;

        if (auto parsed = parseDiagnosticLine(line)) {
            const InspectionSeverity effective = profile.effectiveSeverity(parsed->ruleId, parsed->toolSeverity);
            if (effective == InspectionSeverity::Disabled) {
                continue;
            }
            findings.push_back(InspectionFinding{
                .file = std::move(parsed->file),
                .line = parsed->line,
                .column = parsed->column,
                .endLine = parsed->line,
                .endColumn = parsed->column,
                .ruleId = std::move(parsed->ruleId),
                .message = std::move(parsed->message),
                .severity = effective,
                .toolSeverity = parsed->toolSeverity,
                .fixIt = std::nullopt,
            });
            continue;
        }

        // Caret range line (`   ^~~~`) immediately following a diagnostic refines `endColumn`.
        if (!findings.empty()) {
            const std::string_view trimmed = trimWhitespace(line);
            if (!trimmed.empty() && trimmed.front() == '^') {
                std::uint32_t spanLen = 0;
                for (char ch : trimmed) {
                    if (ch == '^' || ch == '~') {
                        ++spanLen;
                    } else {
                        break;
                    }
                }
                if (spanLen > 1) {
                    findings.back().endColumn = findings.back().column + spanLen;
                }
            }
        }
    }
    return findings;
}

Result<InspectionReport> runClangTidyInspection(io::IProcessLauncher& launcher, io::IFileSystem& fs,
                                                const InspectionRequest& request) {
    std::string clangTidyPath = request.clangTidyPath;
    if (clangTidyPath.empty()) {
        auto located = findClangTidy(fs);
        if (!located) {
            return fail(located.error().code(), located.error().message());
        }
        clangTidyPath = std::move(located->path);
    } else {
        if (!io::isAbsolutePath(clangTidyPath)) {
            return fail(ErrorCode::InvalidArgument,
                        std::format("clang-tidy path '{}' is not an absolute path", clangTidyPath));
        }
        auto normalized = io::normalizePath(clangTidyPath);
        if (!normalized || !isRegularFile(fs, *normalized)) {
            return fail(ErrorCode::NotFound, std::format("clang-tidy path '{}' does not exist", clangTidyPath));
        }
        clangTidyPath = std::move(*normalized);
    }

    if (request.compileCommandsDir.empty()) {
        return fail(ErrorCode::InvalidArgument, "compileCommandsDir is required for clang-tidy");
    }
    if (!io::isAbsolutePath(request.compileCommandsDir)) {
        return fail(ErrorCode::InvalidArgument, "compileCommandsDir must be an absolute path");
    }
    std::string compileCommandsDir = request.compileCommandsDir;
    if (toLowerAscii(compileCommandsDir).ends_with(".json")) {
        compileCommandsDir = std::string(io::parentPath(compileCommandsDir));
    }
    if (io::isAbsolutePath(compileCommandsDir)) {
        if (auto norm = io::normalizePath(compileCommandsDir)) {
            compileCommandsDir = std::move(*norm);
        }
    }

    auto loaded = loadCompileCommands(fs, request.compileCommandsDir);
    if (!loaded) {
        return fail(loaded.error().code(), loaded.error().message());
    }
    std::vector<CompileCommand> commands = std::move(*loaded);
    if (commands.empty()) {
        return fail(ErrorCode::NotFound, "compile_commands.json contains no translation units");
    }

    std::vector<std::string> files = resolveInspectionScopeFiles(commands, request.scope);
    if (files.empty()) {
        return fail(ErrorCode::NotFound, "no source files matched the inspection scope");
    }

    std::optional<sched::HeavyLegLease> lease;
    if (request.governor != nullptr) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes{30};
        while (!request.stopToken.stop_requested()) {
            auto acquired = request.governor->tryAcquireHeavyLeg(sched::HeavyLegKind::Inspection);
            if (acquired) {
                lease = std::move(*acquired);
                break;
            }
            if (acquired.error().code() != ErrorCode::Busy) {
                return fail(acquired.error().code(), acquired.error().message());
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                return fail(ErrorCode::Timeout, "timed out waiting for clang-tidy inspection resources");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
        }
        if (!lease) {
            return fail(ErrorCode::Cancelled, "clang-tidy inspection was cancelled");
        }
    }

    auto queried = queryClangTidyVersion(launcher, clangTidyPath);
    if (!queried) {
        return fail(queried.error().code(), queried.error().message());
    }
    if (request.stopToken.stop_requested()) {
        return fail(ErrorCode::Cancelled, "clang-tidy inspection was cancelled");
    }
    std::string toolVersion = std::move(*queried);

    std::vector<std::string> args;
    args.emplace_back("-p");
    args.push_back(compileCommandsDir);
    if (const std::string checks = request.profile.toChecksArgument(); !checks.empty()) {
        args.push_back("--checks=" + checks);
    }
    args.emplace_back("--quiet");
    if (!request.exportFixesPath.empty()) {
        auto path = io::normalizePath(request.exportFixesPath);
        if (!path)
            return std::unexpected(path.error());
        auto exists = fs.exists(*path);
        if (!exists)
            return std::unexpected(exists.error());
        if (*exists)
            return fail(ErrorCode::AlreadyExists, "clang-tidy replacement export must use a fresh owned file");
        args.push_back("--export-fixes=" + *path);
    }

    if (commandsUseMsvcPch(commands)) {
        const bool hasYMinus = std::ranges::any_of(
            request.extraArgs, [](const std::string& arg) { return arg.find("/Y-") != std::string::npos; });
        if (!hasYMinus) {
            args.emplace_back("--extra-arg=/Y-");
            args.emplace_back("--extra-arg=-Wno-unused-command-line-argument");
        }
    }

    for (const std::string& extra : request.extraArgs) {
        args.push_back(extra);
    }
    for (const std::string& file : files) {
        args.push_back(file);
    }

    io::ProcessSpec spec;
    spec.executable = clangTidyPath;
    spec.arguments = args;
    spec.workingDirectory = compileCommandsDir;
    auto child = launcher.launch(spec);
    if (!child) {
        return fail(child.error().code(), child.error().message());
    }
    std::stop_callback cancel(request.stopToken, [&child] {
        if (*child) {
            static_cast<void>((*child)->terminate());
        }
    });
    auto proc = (*child)->communicate({}, request.timeout);
    if (!proc) {
        if (request.stopToken.stop_requested()) {
            return fail(ErrorCode::Cancelled, "clang-tidy inspection was cancelled");
        }
        return fail(proc.error().code(), proc.error().message());
    }
    if (request.stopToken.stop_requested()) {
        return fail(ErrorCode::Cancelled, "clang-tidy inspection was cancelled");
    }

    std::string combined = proc->stdoutBytes;
    if (!proc->stderrBytes.empty()) {
        if (!combined.empty() && combined.back() != '\n') {
            combined += '\n';
        }
        combined += proc->stderrBytes;
    }

    InspectionReport report;
    report.profileName = request.profile.name;
    report.toolPath = std::move(clangTidyPath);
    report.toolVersion = std::move(toolVersion);
    report.commandLine = formatCommandLine(report.toolPath, args);
    report.arguments = std::move(args);
    report.inspectedFiles = std::move(files);
    report.findings = parseClangTidyOutput(combined, request.profile);
    report.exitCode = proc->exitCode;
    report.executionSuccessful = (proc->exitCode == 0);
    report.rawStdout = std::move(proc->stdoutBytes);
    report.rawStderr = std::move(proc->stderrBytes);
    return report;
}

// --- SARIF 2.1.0 Export and Schema Validation ----------------------------------------------------

Json exportSarifJson(const InspectionReport& report) {
    Json rulesArray = Json::array();
    std::set<std::string> seenRuleIds;
    for (const InspectionFinding& finding : report.findings) {
        if (finding.ruleId.empty() || !seenRuleIds.insert(finding.ruleId).second) {
            continue;
        }
        Json defaultConfig = Json::object().set("level", std::string(toSarifLevel(finding.severity)));
        rulesArray.push(Json::object().set("id", finding.ruleId).set("defaultConfiguration", std::move(defaultConfig)));
    }

    Json driver = Json::object().set("name", "clang-tidy");
    if (!report.toolVersion.empty()) {
        driver.set("version", report.toolVersion);
    }
    driver.set("informationUri", "https://clang.llvm.org/extra/clang-tidy/").set("rules", std::move(rulesArray));

    Json tool = Json::object().set("driver", std::move(driver));

    Json invocation =
        Json::object().set("executionSuccessful", report.executionSuccessful).set("commandLine", report.commandLine);
    Json invocations = Json::array().push(std::move(invocation));

    Json resultsArray = Json::array();
    for (const InspectionFinding& finding : report.findings) {
        std::string uriStr;
        if (auto uri = pathToUri(finding.file)) {
            uriStr = std::move(*uri);
        } else {
            uriStr = "file:///" + finding.file;
        }

        Json artifactLocation = Json::object().set("uri", std::move(uriStr));

        Json region = Json::object()
                          .set("startLine", static_cast<std::int64_t>(finding.line))
                          .set("startColumn", static_cast<std::int64_t>(finding.column));
        if (finding.endLine >= finding.line && (finding.endLine > finding.line || finding.endColumn > finding.column)) {
            region.set("endLine", static_cast<std::int64_t>(finding.endLine))
                .set("endColumn", static_cast<std::int64_t>(finding.endColumn));
        }

        Json physicalLocation =
            Json::object().set("artifactLocation", std::move(artifactLocation)).set("region", std::move(region));

        Json locations = Json::array().push(Json::object().set("physicalLocation", std::move(physicalLocation)));

        Json resultObj = Json::object()
                             .set("ruleId", finding.ruleId)
                             .set("level", std::string(toSarifLevel(finding.severity)))
                             .set("message", Json::object().set("text", finding.message))
                             .set("locations", std::move(locations));
        resultsArray.push(std::move(resultObj));
    }

    Json run = Json::object()
                   .set("tool", std::move(tool))
                   .set("invocations", std::move(invocations))
                   .set("results", std::move(resultsArray));

    return Json::object()
        .set("$schema", "https://docs.oasis-open.org/sarif/sarif/v2.1.0/errata01/os/schemas/"
                        "sarif-schema-2.1.0.json")
        .set("version", "2.1.0")
        .set("runs", Json::array().push(std::move(run)));
}

Result<void> validateSarifSchema(const Json& sarif) {
    if (!sarif.isObject()) {
        return fail(ErrorCode::Corrupt, "SARIF root must be a JSON object");
    }
    const auto* version = sarif.find("version");
    if (version == nullptr || !version->isString() || *version->asString() != "2.1.0") {
        return fail(ErrorCode::Corrupt, "SARIF 'version' must be '2.1.0'");
    }
    const auto* schema = sarif.find("$schema");
    if (schema == nullptr || !schema->isString() || schema->asString()->empty()) {
        return fail(ErrorCode::Corrupt, "SARIF '$schema' must be a non-empty string");
    }
    const auto* runs = sarif.find("runs");
    if (runs == nullptr || !runs->isArray() || runs->items().empty()) {
        return fail(ErrorCode::Corrupt, "SARIF 'runs' must be a non-empty array");
    }

    for (const Json& run : runs->items()) {
        if (!run.isObject()) {
            return fail(ErrorCode::Corrupt, "SARIF run entry must be an object");
        }
        const auto* tool = run.find("tool");
        if (tool == nullptr || !tool->isObject()) {
            return fail(ErrorCode::Corrupt, "SARIF run.tool must be an object");
        }
        const auto* driver = tool->find("driver");
        if (driver == nullptr || !driver->isObject()) {
            return fail(ErrorCode::Corrupt, "SARIF run.tool.driver must be an object");
        }
        const auto* driverName = driver->find("name");
        if (driverName == nullptr || !driverName->isString() || driverName->asString()->empty()) {
            return fail(ErrorCode::Corrupt, "SARIF run.tool.driver.name must be a non-empty string");
        }
        const auto* rules = driver->find("rules");
        if (rules == nullptr || !rules->isArray()) {
            return fail(ErrorCode::Corrupt, "SARIF run.tool.driver.rules must be an array");
        }
        std::set<std::string> ruleIds;
        for (const Json& rule : rules->items()) {
            if (!rule.isObject()) {
                return fail(ErrorCode::Corrupt, "SARIF run.tool.driver.rules entry must be an object");
            }
            const auto* id = rule.find("id");
            if (id == nullptr || !id->isString() || id->asString()->empty()) {
                return fail(ErrorCode::Corrupt, "SARIF rule 'id' must be a non-empty string");
            }
            if (!ruleIds.insert(*id->asString()).second) {
                return fail(ErrorCode::Corrupt, std::format("duplicate SARIF rule id '{}'", *id->asString()));
            }
            if (const auto* defCfg = rule.find("defaultConfiguration")) {
                if (!defCfg->isObject()) {
                    return fail(ErrorCode::Corrupt, "SARIF rule.defaultConfiguration must be an object");
                }
                if (const auto* lvl = defCfg->find("level")) {
                    if (!lvl->isString() || !isValidSarifLevel(*lvl->asString())) {
                        return fail(ErrorCode::Corrupt, "SARIF rule.defaultConfiguration.level is invalid");
                    }
                }
            }
        }

        if (const auto* invocations = run.find("invocations")) {
            if (!invocations->isArray()) {
                return fail(ErrorCode::Corrupt, "SARIF run.invocations must be an array");
            }
            for (const Json& inv : invocations->items()) {
                if (!inv.isObject()) {
                    return fail(ErrorCode::Corrupt, "SARIF invocation entry must be an object");
                }
                const auto* ok = inv.find("executionSuccessful");
                if (ok == nullptr || !ok->isBool()) {
                    return fail(ErrorCode::Corrupt, "SARIF invocation.executionSuccessful must be a boolean");
                }
            }
        }

        const auto* results = run.find("results");
        if (results == nullptr || !results->isArray()) {
            return fail(ErrorCode::Corrupt, "SARIF run.results must be an array");
        }
        for (const Json& result : results->items()) {
            if (!result.isObject()) {
                return fail(ErrorCode::Corrupt, "SARIF result entry must be an object");
            }
            const auto* ruleId = result.find("ruleId");
            if (ruleId == nullptr || !ruleId->isString() || ruleId->asString()->empty()) {
                return fail(ErrorCode::Corrupt, "SARIF result.ruleId must be a non-empty string");
            }
            const auto* level = result.find("level");
            if (level == nullptr || !level->isString() || !isValidSarifLevel(*level->asString())) {
                return fail(ErrorCode::Corrupt, "SARIF result.level must be error, warning, note, or none");
            }
            const auto* message = result.find("message");
            if (message == nullptr || !message->isObject()) {
                return fail(ErrorCode::Corrupt, "SARIF result.message must be an object");
            }
            const auto* text = message->find("text");
            if (text == nullptr || !text->isString() || text->asString()->empty()) {
                return fail(ErrorCode::Corrupt, "SARIF result.message.text must be a non-empty string");
            }
            const auto* locations = result.find("locations");
            if (locations == nullptr || !locations->isArray() || locations->items().empty()) {
                return fail(ErrorCode::Corrupt, "SARIF result.locations must be a non-empty array");
            }
            for (const Json& loc : locations->items()) {
                if (!loc.isObject()) {
                    return fail(ErrorCode::Corrupt, "SARIF location must be an object");
                }
                const auto* phys = loc.find("physicalLocation");
                if (phys == nullptr || !phys->isObject()) {
                    return fail(ErrorCode::Corrupt, "SARIF location.physicalLocation must be an object");
                }
                const auto* artifact = phys->find("artifactLocation");
                if (artifact == nullptr || !artifact->isObject()) {
                    return fail(ErrorCode::Corrupt, "SARIF physicalLocation.artifactLocation must be an object");
                }
                const auto* uri = artifact->find("uri");
                if (uri == nullptr || !uri->isString() || uri->asString()->empty()) {
                    return fail(ErrorCode::Corrupt, "SARIF artifactLocation.uri must be a non-empty string");
                }
                const auto* region = phys->find("region");
                if (region == nullptr || !region->isObject()) {
                    return fail(ErrorCode::Corrupt, "SARIF physicalLocation.region must be an object");
                }
                const auto* startLine = region->find("startLine");
                const auto* startCol = region->find("startColumn");
                if (startLine == nullptr || startLine->kind() != Json::Kind::Integer ||
                    startLine->asInteger().value_or(0) < 1) {
                    return fail(ErrorCode::Corrupt, "SARIF region.startLine must be an integer >= 1");
                }
                if (startCol == nullptr || startCol->kind() != Json::Kind::Integer ||
                    startCol->asInteger().value_or(0) < 1) {
                    return fail(ErrorCode::Corrupt, "SARIF region.startColumn must be an integer >= 1");
                }
            }
        }
    }
    return {};
}

Result<void> writeSarifReport(io::IFileSystem& fs, std::string_view outputPath, const InspectionReport& report) {
    Json sarif = exportSarifJson(report);
    if (auto valid = validateSarifSchema(sarif); !valid) {
        return valid;
    }
    const std::string serialized = sarif.dump();
    return fs.writeFileAtomic(outputPath, serialized);
}

} // namespace frost::lang
