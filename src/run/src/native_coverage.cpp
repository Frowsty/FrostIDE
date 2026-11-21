// SPDX-License-Identifier: Apache-2.0
// LLVM source coverage via genuine baseline/export LCOV data and observed source identities.
#include "frost/io/path.hpp"
#include "frost/run/native_analysis.hpp"

#include <algorithm>
#include <charconv>
#include <set>
#include <tuple>
namespace frost::run {
namespace {
template<class T>
bool number(std::string_view text, T& out) {
    const auto result = std::from_chars(text.data(), text.data() + text.size(), out);
    return !text.empty() && result.ec == std::errc{} && result.ptr == text.data() + text.size();
}
std::vector<std::string_view> fields(std::string_view text) {
    std::vector<std::string_view> result;
    while (true) {
        const auto comma = text.find(',');
        result.push_back(text.substr(0, comma));
        if (comma == std::string_view::npos)
            break;
        text.remove_prefix(comma + 1);
    }
    return result;
}
Result<std::string> sourcePath(std::string_view path, std::string_view directory) {
    return io::isAbsolutePath(path) ? io::normalizePath(path) : io::joinPath(directory, path);
}
#ifdef _WIN32
std::string_view trimCoverageLine(std::string_view value) {
    const auto start = value.find_first_not_of(" \t\r");
    return start == value.npos ? std::string_view{} : value.substr(start, value.find_last_not_of(" \t\r") - start + 1);
}

std::string_view nextCoverageLine(std::string_view& input) {
    const auto end = input.find('\n');
    const auto line = trimCoverageLine(input.substr(0, end));
    input = end == input.npos ? std::string_view{} : input.substr(end + 1);
    return line;
}

bool isCoverageId(std::string_view value) {
    return value.size() == 32 && std::ranges::all_of(value, [](char ch) {
               return (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'F') || (ch >= 'a' && ch <= 'f');
           });
}

char lowerHex(char ch) { return ch >= 'A' && ch <= 'F' ? static_cast<char>(ch - 'A' + 'a') : ch; }

std::string_view missingCoffCoverageId(std::string_view message) {
    constexpr std::string_view prefix = "Coverage tool failed or warned: error: failed to load coverage: '";
    constexpr std::string_view separator = "': Missing binary ID: ";
    const auto diagnostic = nextCoverageLine(message);
    const auto pathEnd = diagnostic.rfind(separator);
    if (!diagnostic.starts_with(prefix) || pathEnd == diagnostic.npos || pathEnd <= prefix.size())
        return {};
    const auto id = diagnostic.substr(pathEnd + separator.size());
    if (!isCoverageId(id))
        return {};
    std::string_view line;
    do {
        line = nextCoverageLine(message);
    } while (line.empty() && !message.empty());
    if (line != "error: could not load coverage information")
        return {};
    while (!message.empty())
        if (!nextCoverageLine(message).empty())
            return {};
    return id;
}

Result<std::string> coffCoverageId(std::string_view output) {
    std::size_t coffFormats = 0, codeViews = 0, signatures = 0, guids = 0;
    bool rsds = false;
    std::string_view guid;
    while (!output.empty()) {
        const auto line = nextCoverageLine(output);
        if (line.starts_with("Format: COFF-"))
            ++coffFormats;
        else if (line == "Type: CodeView (0x2)")
            ++codeViews;
        else if (line.starts_with("PDBSignature:")) {
            ++signatures;
            rsds = line == "PDBSignature: 0x53445352";
        } else if (line.starts_with("PDBGUID: ")) {
            ++guids;
            guid = line.substr(9);
        }
    }
    if (coffFormats != 1 || codeViews != 1 || signatures != 1 || guids != 1 || !rsds || guid.size() != 38 ||
        guid.front() != '{' || guid.back() != '}' || guid[9] != '-' || guid[14] != '-' || guid[19] != '-' ||
        guid[24] != '-')
        return fail(ErrorCode::Unsupported, "Expected one COFF CodeView RSDS identifier");
    std::string digits;
    for (const char ch : guid.substr(1, 36))
        if (ch != '-')
            digits += lowerHex(ch);
    if (!isCoverageId(digits) || std::ranges::all_of(digits, [](char ch) { return ch == '0'; }))
        return fail(ErrorCode::Corrupt, "Invalid COFF CodeView RSDS identifier");
    // llvm-profdata prints raw GUID bytes. readobj prints the first three fields in GUID text order.
    std::string id;
    for (const auto offset : {6U, 4U, 2U, 0U, 10U, 8U, 14U, 12U})
        id.append(digits, offset, 2);
    id += digits.substr(16);
    return id;
}

Result<void> requireCoverageId(std::string_view output, std::string_view expected) {
    bool found = false;
    while (!output.empty()) {
        if (nextCoverageLine(output) != "Binary IDs:")
            continue;
        found = true;
        break;
    }
    const auto id = nextCoverageLine(output);
    if (!found || !isCoverageId(id) || !std::ranges::equal(id, expected, {}, lowerHex))
        return fail(ErrorCode::Corrupt, "Coverage profile does not name the selected COFF binary");
    while (!output.empty())
        if (!nextCoverageLine(output).empty())
            return fail(ErrorCode::Unsupported, "Expected exactly one binary identifier in the coverage profile");
    return {};
}
#endif
} // namespace
Result<void> validateAnalysisLlvmVersion(std::string_view banner, std::string_view expected) {
    const std::string marker = "LLVM version " + std::string(expected);
    std::size_t position = 0;
    while ((position = banner.find(marker, position)) != banner.npos) {
        const auto end = position + marker.size();
        if ((position == 0 || banner[position - 1] == ' ' || banner[position - 1] == '\t' ||
             banner[position - 1] == '\n') &&
            (end == banner.size() || banner[end] == ' ' || banner[end] == '\t' || banner[end] == '\r' ||
             banner[end] == '\n'))
            return {};
        position = end;
    }
    return fail(ErrorCode::Unsupported, "Coverage requires pinned LLVM " + std::string(expected));
}
Result<std::vector<CoverageFile>> parseLlvmCoverage(std::string_view lcov, std::string_view compilationDirectory,
                                                    CoverageSummaryMode summaryMode) {
    if (!io::isAbsolutePath(compilationDirectory) || lcov.find('\0') != lcov.npos)
        return fail(ErrorCode::InvalidArgument,
                    "Coverage requires an absolute compilation directory and NUL-free export");
    if (lcov.size() > 32 * 1024 * 1024)
        return fail(ErrorCode::OutOfRange, "Coverage export exceeds 32 MiB");
    std::vector<CoverageFile> result;
    std::set<std::string> paths;
    std::optional<CoverageFile> current;
    std::set<std::uint32_t> lines;
    std::set<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>> branchKeys;
    bool lf = false, lh = false, brf = false, brh = false;
    std::size_t entries = 0;
    while (!lcov.empty()) {
        const auto end = lcov.find('\n');
        auto line = lcov.substr(0, end);
        if (end == std::string_view::npos)
            lcov = {};
        else
            lcov.remove_prefix(end + 1);
        if (line.ends_with('\r'))
            line.remove_suffix(1);
        if (line.empty() || line.starts_with("TN:") || line.starts_with("VER:"))
            continue;
        if (line.starts_with("SF:")) {
            if (current || line.size() == 3 || paths.size() >= 4096)
                return fail(ErrorCode::Corrupt, "Nested or excessive coverage files");
            auto path = sourcePath(line.substr(3), compilationDirectory);
            if (!path)
                return std::unexpected(path.error());
            if (!paths.insert(*path).second)
                return fail(ErrorCode::Corrupt, "Duplicate coverage file");
            current = CoverageFile{};
            current->path = *path;
            lines.clear();
            branchKeys.clear();
            lf = lh = brf = brh = false;
            continue;
        }
        if (!current)
            return fail(ErrorCode::Corrupt, "Coverage record outside source file");
        if (line == "end_of_record") {
            const auto covered = static_cast<std::uint64_t>(
                std::ranges::count_if(current->lines, [](const auto& item) { return item.hits > 0; }));
            const auto branches = static_cast<std::uint64_t>(
                std::ranges::count_if(current->branches, [](const auto& item) { return item.hits && *item.hits > 0; }));
            if (!lf || !lh || current->covered > current->found || brf != brh || (!current->branches.empty() && !brf) ||
                current->branchesCovered > current->branchesFound)
                return fail(ErrorCode::Corrupt, "Missing or invalid coverage summary");
            // LLVM's file report and source projections are calculated separately. Macro expansions and
            // template instantiations can make their totals differ; both values are meaningful.
            current->aggregateDiffersFromDetails =
                current->found != current->lines.size() || current->covered != covered ||
                (brf && (current->branchesFound != current->branches.size() || current->branchesCovered != branches));
            if (current->aggregateDiffersFromDetails && summaryMode == CoverageSummaryMode::RequireRowTotals)
                return fail(ErrorCode::Corrupt, "Coverage summary disagrees with line/branch records");
            std::ranges::sort(current->lines, {}, &CoverageLine::line);
            result.push_back(std::move(*current));
            current.reset();
            continue;
        }
        if (line.starts_with("DA:")) {
            auto values = fields(line.substr(3));
            CoverageLine item;
            if ((values.size() != 2 && values.size() != 3) || !number(values[0], item.line) || !item.line ||
                !number(values[1], item.hits) || !lines.insert(item.line).second)
                return fail(ErrorCode::Corrupt, "Malformed/duplicate coverage line");
            current->lines.push_back(item);
        } else if (line.starts_with("BRDA:")) {
            auto values = fields(line.substr(5));
            CoverageBranch item;
            std::uint64_t hits = 0;
            if (values.size() != 4 || !number(values[0], item.line) || !item.line || !number(values[1], item.block) ||
                !number(values[2], item.branch) || (values[3] != "-" && !number(values[3], hits)) ||
                !branchKeys.emplace(item.line, item.block, item.branch).second)
                return fail(ErrorCode::Corrupt, "Malformed coverage branch");
            if (values[3] != "-")
                item.hits = hits;
            current->branches.push_back(item);
        } else if (line.starts_with("LF:")) {
            if (lf || !number(line.substr(3), current->found))
                return fail(ErrorCode::Corrupt, "Invalid LF");
            lf = true;
        } else if (line.starts_with("LH:")) {
            if (lh || !number(line.substr(3), current->covered))
                return fail(ErrorCode::Corrupt, "Invalid LH");
            lh = true;
        } else if (line.starts_with("BRF:")) {
            if (brf || !number(line.substr(4), current->branchesFound))
                return fail(ErrorCode::Corrupt, "Invalid BRF");
            brf = true;
        } else if (line.starts_with("BRH:")) {
            if (brh || !number(line.substr(4), current->branchesCovered))
                return fail(ErrorCode::Corrupt, "Invalid BRH");
            brh = true;
        } else if (!(line.starts_with("FN:") || line.starts_with("FNDA:") || line.starts_with("FNF:") ||
                     line.starts_with("FNH:"))) {
            return fail(ErrorCode::Unsupported, "Unsupported LLVM coverage record");
        }
        if (++entries > 2'000'000)
            return fail(ErrorCode::OutOfRange, "Coverage record limit exceeded");
    }
    if (current)
        return fail(ErrorCode::Corrupt, "Truncated coverage source record");
    if (result.empty())
        return fail(ErrorCode::Unsupported, "No source coverage mappings; use a Clang instrumented build");
    return result;
}
Result<CoverageReport> collectNativeCoverage(io::IFileSystem& fs, io::IProcessLauncher& launcher,
                                             const CoverageRequest& request, std::stop_token stop,
                                             AnalysisAdmission admission) {
    if (stop.stop_requested())
        return fail(ErrorCode::Cancelled, "Coverage cancelled");
    if (!io::isAbsolutePath(request.llvmCov) || !io::isAbsolutePath(request.llvmProfdata) ||
        !io::isAbsolutePath(request.plan.program) || !io::isAbsolutePath(request.outputDirectory) ||
        !io::isAbsolutePath(request.compilationDirectory))
        return fail(ErrorCode::InvalidArgument, "Coverage paths must be absolute");
    if (admission) {
        auto allowed = admission();
        if (!allowed)
            return std::unexpected(allowed.error());
    }
    auto exists = fs.exists(request.outputDirectory);
    if (!exists)
        return std::unexpected(exists.error());
    if (*exists)
        return fail(ErrorCode::AlreadyExists, "Coverage output directory must be new");
    if (auto made = fs.createDirectories(request.outputDirectory); !made)
        return std::unexpected(made.error());
    auto physicalOutput = fs.physicalPath(request.outputDirectory);
    if (!physicalOutput)
        return std::unexpected(physicalOutput.error());
    std::optional<AnalysisFileIdentity> admittedBinary;
#ifdef _WIN32
    std::optional<AnalysisFileIdentity> admittedMergedProfile, admittedReadobj;
#endif
    std::vector<AnalysisFileIdentity> admittedSources;
    const auto guarded = [&]() -> Result<void> {
        if (stop.stop_requested())
            return fail(ErrorCode::Cancelled, "Coverage cancelled");
        if (admission) {
            auto allowed = admission();
            if (!allowed)
                return allowed;
        }
        if (admittedBinary) {
            auto valid = validateAnalysisIdentity(fs, *admittedBinary);
            if (!valid)
                return valid;
        }
#ifdef _WIN32
        for (const auto* supplemental : {&admittedMergedProfile, &admittedReadobj})
            if (*supplemental)
                if (auto valid = validateAnalysisIdentity(fs, **supplemental); !valid)
                    return valid;
#endif
        for (const auto& source : admittedSources) {
            auto valid = validateAnalysisIdentity(fs, source);
            if (!valid)
                return valid;
        }
        auto now = fs.physicalPath(request.outputDirectory);
        if (!now || *now != *physicalOutput)
            return fail(ErrorCode::Busy, "Coverage output identity changed");
        return {};
    };
    const auto command = [&](std::string program, std::vector<std::string> arguments) -> Result<std::string> {
        io::ProcessSpec spec;
        spec.executable = std::move(program);
        spec.arguments = std::move(arguments);
        spec.workingDirectory = request.compilationDirectory;
        spec.environment.emplace_back("DEBUGINFOD_URLS", "");
        auto result = runAnalysisCommand(launcher, std::move(spec), stop, guarded, std::chrono::minutes(5),
                                         32U * 1024U * 1024U, {}, admission);
        if (!result)
            return std::unexpected(result.error());
        if (result->exitCode != 0 || !result->error.empty())
            return fail(ErrorCode::IoError, "Coverage tool failed or warned: " + result->error);
        return std::move(result->output);
    };
    for (const auto& tool : {request.llvmCov, request.llvmProfdata}) {
        auto version = command(tool, {"--version"});
        if (!version)
            return std::unexpected(version.error());
        if (auto valid = validateAnalysisLlvmVersion(*version, request.expectedLlvmVersion); !valid)
            return std::unexpected(valid.error());
    }
    auto binary = captureAnalysisIdentity(fs, request.plan.program);
    if (!binary)
        return std::unexpected(binary.error());
    admittedBinary = *binary;
    auto baselineText = command(request.llvmCov, {"export", "-format=lcov", "-empty-profile", "-debuginfod=false",
                                                  "-num-threads=2", request.plan.program});
    if (!baselineText)
        return std::unexpected(baselineText.error());
    auto baseline =
        parseLlvmCoverage(*baselineText, request.compilationDirectory, CoverageSummaryMode::LlvmFileAggregates);
    if (!baseline)
        return std::unexpected(baseline.error());
    std::map<std::string, std::optional<AnalysisFileIdentity>> sources;
    std::map<std::string, std::string> unavailable;
    for (const auto& file : *baseline) {
        if (stop.stop_requested())
            return fail(ErrorCode::Cancelled, "Coverage source capture cancelled");
        auto identity = captureAnalysisIdentity(fs, file.path, 8 * 1024 * 1024);
        if (identity) {
            admittedSources.push_back(*identity);
            sources[file.path] = std::move(*identity);
        } else {
            sources[file.path] = std::nullopt;
            unavailable[file.path] = identity.error().message();
        }
    }
    std::optional<int> programExit;
    auto profiles = request.rawProfiles;
    if (request.execute) {
        if (!profiles.empty())
            return fail(ErrorCode::InvalidArgument, "Cannot import profiles while running coverage");
        auto pattern = io::joinPath(request.outputDirectory, "run-%p-%m.profraw");
        if (!pattern)
            return std::unexpected(pattern.error());
        io::ProcessSpec spec;
        spec.executable = request.plan.program;
        spec.arguments = request.plan.arguments;
        spec.workingDirectory = request.plan.workingDirectory;
        for (const auto& entry : request.plan.environment)
            spec.environment.emplace_back(entry.name, entry.value);
        std::erase_if(spec.environment, [](const auto& item) {
            std::string key = item.first;
            std::ranges::transform(key, key.begin(), [](unsigned char c) {
                return static_cast<char>(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c);
            });
            return key == "LLVM_PROFILE_FILE";
        });
        spec.environment.emplace_back("LLVM_PROFILE_FILE", *pattern);
        auto ran = runAnalysisCommand(launcher, std::move(spec), stop, guarded, std::nullopt, 32U * 1024U * 1024U, {},
                                      admission);
        if (!ran)
            return std::unexpected(ran.error());
        programExit = ran->exitCode;
        auto entries = fs.listDirectory(request.outputDirectory);
        if (!entries)
            return std::unexpected(entries.error());
        for (const auto& entry : *entries)
            if (entry.type == io::FileType::Regular && entry.name.starts_with("run-") &&
                entry.name.ends_with(".profraw")) {
                auto path = io::joinPath(request.outputDirectory, entry.name);
                if (!path)
                    return std::unexpected(path.error());
                profiles.push_back(*path);
            }
    }
    if (profiles.empty() || profiles.size() > 256)
        return fail(ErrorCode::Unsupported, "No bounded raw profile set was produced; binary may lack instrumentation");
    std::vector<AnalysisFileIdentity> profileIdentities;
    for (const auto& profile : profiles) {
        if (!io::isAbsolutePath(profile))
            return fail(ErrorCode::InvalidArgument, "Imported profile path must be absolute");
        auto identity = captureAnalysisIdentity(fs, profile);
        if (!identity)
            return std::unexpected(identity.error());
        profileIdentities.push_back(std::move(*identity));
    }
    auto mergedPath = io::joinPath(request.outputDirectory, "coverage.profdata");
    if (!mergedPath)
        return std::unexpected(mergedPath.error());
    std::vector<std::string> args = {"merge", "-sparse", "-num-threads=2", "-o", *mergedPath};
    args.insert(args.end(), profiles.begin(), profiles.end());
    auto merged = command(request.llvmProfdata, std::move(args));
    if (!merged)
        return std::unexpected(merged.error());
    const auto exportProfile = [&](bool checkIds) {
        return command(request.llvmCov, {"export", "-format=lcov", "-num-threads=2",
                                         checkIds ? "-check-binary-ids" : "-check-binary-ids=false",
                                         "-debuginfod=false", "-instr-profile=" + *mergedPath, request.plan.program});
    };
    auto actual = exportProfile(true);
#ifdef _WIN32
    // Pinned llvm-cov cannot match the raw CodeView GUID against its COFF object list. Fall back only
    // for that error, and only after independent identity checks of the selected image and every profile.
    const auto missingId = !actual && actual.error().code() == ErrorCode::IoError
                               ? missingCoffCoverageId(actual.error().message())
                               : std::string_view{};
    if (!missingId.empty()) {
        auto readobj = io::joinPath(io::parentPath(request.llvmCov), "llvm-readobj.exe");
        if (!readobj)
            return std::unexpected(readobj.error());
        auto readobjIdentity = captureAnalysisIdentity(fs, *readobj);
        if (!readobjIdentity)
            return std::unexpected(readobjIdentity.error());
        admittedReadobj = std::move(*readobjIdentity);
        auto mergedIdentity = captureAnalysisIdentity(fs, *mergedPath);
        if (!mergedIdentity)
            return std::unexpected(mergedIdentity.error());
        admittedMergedProfile = std::move(*mergedIdentity);
        auto version = command(*readobj, {"--version"});
        if (!version)
            return std::unexpected(version.error());
        if (auto valid = validateAnalysisLlvmVersion(*version, request.expectedLlvmVersion); !valid)
            return std::unexpected(valid.error());
        auto image = command(*readobj, {"--coff-debug-directory", request.plan.program});
        if (!image)
            return std::unexpected(image.error());
        auto id = coffCoverageId(*image);
        if (!id)
            return std::unexpected(id.error());
        const auto checkProfile = [&](const AnalysisFileIdentity& profile) -> Result<void> {
            if (auto same = validateAnalysisIdentity(fs, profile); !same)
                return same;
            auto ids = command(request.llvmProfdata, {"show", "--binary-ids", profile.path});
            if (!ids)
                return std::unexpected(ids.error());
            return requireCoverageId(*ids, *id);
        };
        for (const auto& profile : profileIdentities)
            if (auto matched = checkProfile(profile); !matched)
                return std::unexpected(matched.error());
        if (auto matched = checkProfile(*admittedMergedProfile); !matched)
            return std::unexpected(matched.error());
        if (!std::ranges::equal(missingId, *id, {}, lowerHex))
            return std::unexpected(actual.error());
        actual = exportProfile(false);
    }
#endif
    if (!actual)
        return std::unexpected(actual.error());
    auto files = parseLlvmCoverage(*actual, request.compilationDirectory, CoverageSummaryMode::LlvmFileAggregates);
    if (!files)
        return std::unexpected(files.error());
    if (files->size() != baseline->size())
        return fail(ErrorCode::Busy, "Coverage file mapping changed");
    if (auto same = guarded(); !same)
        return std::unexpected(same.error());
    for (const auto& identity : profileIdentities)
        if (auto same = validateAnalysisIdentity(fs, identity); !same)
            return std::unexpected(same.error());
    for (auto& file : *files) {
        const auto identity = sources.find(file.path);
        if (identity == sources.end())
            return fail(ErrorCode::Busy, "Coverage mappings changed during collection");
        file.identity = identity->second;
        if (file.identity) {
            auto same = validateAnalysisIdentity(fs, *file.identity);
            if (!same)
                return std::unexpected(same.error());
        } else
            file.sourceUnavailable = unavailable[file.path];
    }
    if (auto allowed = guarded(); !allowed)
        return std::unexpected(allowed.error());
    auto exportPath = io::joinPath(request.outputDirectory, "coverage.info");
    if (!exportPath)
        return std::unexpected(exportPath.error());
    if (auto saved = fs.writeFileAtomic(*exportPath, *actual); !saved)
        return std::unexpected(saved.error());
    return CoverageReport{*binary, std::move(*files), std::move(*actual), *mergedPath, programExit};
}
} // namespace frost::run
