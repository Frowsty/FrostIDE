// SPDX-License-Identifier: Apache-2.0
//
// The git differential runner (git_runner.hpp): environment, command lines, the success criteria,
// the -U<k> output parser, the dialect probe and `git apply`.

#include "diff/support/git_runner.hpp"

#include "frost/io/path.hpp"

#include "diff/support/files.hpp"

#include <charconv>
#include <format>
#include <optional>
#include <utility>

namespace frost::diff::test {
namespace {

constexpr std::chrono::seconds kGitTimeout{60};

std::vector<std::string_view> splitLf(std::string_view text) {
    std::vector<std::string_view> lines;
    std::size_t start = 0;
    while (start < text.size()) {
        const std::size_t lf = text.find('\n', start);
        if (lf == std::string_view::npos) {
            lines.push_back(text.substr(start));
            break;
        }
        lines.push_back(text.substr(start, lf - start));
        start = lf + 1;
    }
    return lines;
}

// "-s,c" or "+s" (after the sign) as a half-open 0-based range.
bool parseSide(std::string_view text, std::uint32_t& begin, std::uint32_t& end) {
    const std::size_t comma = text.find(',');
    const std::string_view first = text.substr(0, comma);
    std::uint32_t start = 0;
    std::uint32_t count = 1;
    const auto head = std::from_chars(first.data(), first.data() + first.size(), start);
    if (first.empty() || head.ec != std::errc() || head.ptr != first.data() + first.size()) {
        return false;
    }
    if (comma != std::string_view::npos) {
        const std::string_view second = text.substr(comma + 1);
        const auto parsed = std::from_chars(second.data(), second.data() + second.size(), count);
        if (second.empty() || parsed.ec != std::errc() || parsed.ptr != second.data() + second.size()) {
            return false;
        }
    }
    if (count != 0 && start == 0) {
        return false;
    }
    begin = count == 0 ? start : start - 1;
    end = begin + count;
    return true;
}

// "@@ -R +R @@..." → the hunk and the header with the text after the second "@@" stripped.
std::optional<std::pair<HunkTuple, std::string_view>> parseHeader(std::string_view line) {
    if (!line.starts_with("@@ -")) {
        return std::nullopt;
    }
    const std::size_t space = line.find(' ', 4);
    if (space == std::string_view::npos || line.substr(space).size() < 2 || line[space + 1] != '+') {
        return std::nullopt;
    }
    const std::size_t close = line.find(' ', space + 2);
    if (close == std::string_view::npos || line.substr(close, 3) != " @@") {
        return std::nullopt;
    }
    HunkTuple hunk{};
    if (!parseSide(line.substr(4, space - 4), hunk[0], hunk[1]) ||
        !parseSide(line.substr(space + 2, close - space - 2), hunk[2], hunk[3])) {
        return std::nullopt;
    }
    return std::pair{hunk, line.substr(0, close + 3)};
}

// The case index of a "c%06d" name, or nullopt.
std::optional<std::size_t> caseIndex(std::string_view name) {
    if (name.size() != 7 || name[0] != 'c') {
        return std::nullopt;
    }
    std::size_t index = 0;
    const auto parsed = std::from_chars(name.data() + 1, name.data() + name.size(), index);
    if (parsed.ec != std::errc() || parsed.ptr != name.data() + name.size()) {
        return std::nullopt;
    }
    return index;
}

std::string stderrNote(std::string_view stderrBytes) {
    return stderrBytes.empty() ? std::string("(stderr empty)") : std::format("stderr: {}", stderrBytes);
}

} // namespace

std::string_view dialectName(GitDialect dialect) noexcept { return dialect == GitDialect::Classic ? "Classic" : "New"; }

Result<std::vector<GitSection>> parseGitDiff(std::string_view stdoutBytes, std::span<const GitCase> cases) {
    std::vector<GitSection> sections(cases.size());
    GitSection* current = nullptr;
    std::string_view currentName;
    bool inBody = false;
    for (const std::string_view line : splitLf(stdoutBytes)) {
        if (line.starts_with("diff --git ")) {
            // "diff --git a/a/<name> b/b/<name>".
            const std::string_view rest = line.substr(11);
            const std::size_t split = rest.find(" b/b/");
            if (!rest.starts_with("a/a/") || split == std::string_view::npos) {
                return fail(ErrorCode::InvalidArgument, std::format("unexpected section header: {}", line));
            }
            const std::string_view name = rest.substr(4, split - 4);
            const std::optional<std::size_t> index = caseIndex(name);
            if (rest.substr(split + 5) != name || !index || *index >= cases.size()) {
                return fail(ErrorCode::InvalidArgument, std::format("unknown case name in: {}", line));
            }
            if (sections[*index].present) {
                return fail(ErrorCode::InvalidArgument, std::format("duplicate section for {}", name));
            }
            current = &sections[*index];
            current->present = true;
            currentName = name;
            inBody = false;
            continue;
        }
        if (current == nullptr) {
            return fail(ErrorCode::InvalidArgument, std::format("output before the first section: {}", line));
        }
        if (line.starts_with("@@")) {
            const auto header = parseHeader(line);
            if (!header) {
                return fail(ErrorCode::InvalidArgument,
                            std::format("malformed hunk header in {}: {}", currentName, line));
            }
            current->hunks.push_back(header->first);
            current->body += header->second;
            current->body += '\n';
            inBody = true;
            continue;
        }
        if (!inBody) {
            if (line.starts_with("Binary files ")) {
                return fail(ErrorCode::InvalidArgument, std::format("binary section for {}", currentName));
            }
            continue; // index, mode, --- and +++ lines.
        }
        if (line.empty() || (line[0] != ' ' && line[0] != '-' && line[0] != '+' && line[0] != '\\')) {
            return fail(ErrorCode::InvalidArgument, std::format("unexpected body line in {}: '{}'", currentName, line));
        }
        current->body += line;
        current->body += '\n';
    }
    for (std::size_t k = 0; k < cases.size(); ++k) {
        const bool differ = cases[k].a != cases[k].b;
        if (differ && !sections[k].present) {
            return fail(ErrorCode::InvalidArgument, std::format("no section for differing case {}", caseName(k)));
        }
        if (!differ && sections[k].present) {
            return fail(ErrorCode::InvalidArgument, std::format("a section for byte-identical case {}", caseName(k)));
        }
        if (differ && sections[k].hunks.empty()) {
            return fail(ErrorCode::InvalidArgument, std::format("a section without hunks for {}", caseName(k)));
        }
    }
    return sections;
}

std::string gitStyleOutput(std::span<const std::string> bodies) {
    std::string out;
    for (std::size_t k = 0; k < bodies.size(); ++k) {
        if (bodies[k].empty()) {
            continue;
        }
        const std::string name = caseName(k);
        out +=
            std::format("diff --git a/a/{0} b/b/{0}\nindex 0000000..1111111 100644\n--- a/a/{0}\n+++ b/b/{0}\n", name);
        out += bodies[k];
    }
    return out;
}

GitRunner::GitRunner(io::IProcessLauncher& launcher, std::string git, std::string scratch)
    : m_launcher(launcher), m_git(std::move(git)), m_scratch(std::move(scratch)) {}

Result<void> GitRunner::prepare() {
    if (auto written = writeBinaryFile(joinUtf8(m_scratch, "empty.gitconfig"), ""); !written) {
        return written;
    }
    return makeDirectories(joinUtf8(m_scratch, "no-hooks"));
}

Result<io::ProcessResult> GitRunner::run(std::vector<std::string> arguments, std::string workingDirectory) {
    auto config = io::joinPath(m_scratch, "empty.gitconfig");
    auto hooks = io::joinPath(m_scratch, "no-hooks");
    if (!config || !hooks) {
        return std::unexpected(std::move(!config ? config.error() : hooks.error()));
    }
    io::ProcessSpec spec;
    spec.executable = m_git;
    spec.workingDirectory = std::move(workingDirectory);
    spec.arguments = {"-c", "core.autocrlf=false",
                      "-c", "core.safecrlf=false",
                      "-c", "core.quotepath=false",
                      "-c", "core.fsmonitor=false",
                      "-c", std::format("core.hooksPath={}", *hooks),
                      "-c", "diff.suppressBlankEmpty=false",
                      "-c", "diff.noprefix=false",
                      "-c", "diff.mnemonicPrefix=false",
                      "-c", "diff.relative=false"};
    spec.arguments.insert(spec.arguments.end(), std::make_move_iterator(arguments.begin()),
                          std::make_move_iterator(arguments.end()));
    spec.environment = {
        {"GIT_CONFIG_NOSYSTEM", "1"},
        {"GIT_CONFIG_GLOBAL", *config},
        {"HOME", m_scratch},
        {"XDG_CONFIG_HOME", m_scratch},
        {"GIT_CEILING_DIRECTORIES", std::string(io::parentPath(m_scratch))},
        {"GIT_ATTR_NOSYSTEM", "1"},
        {"GIT_TERMINAL_PROMPT", "0"},
        {"LC_ALL", "C"},
        {"GIT_DIR", std::nullopt},
        {"GIT_WORK_TREE", std::nullopt},
        {"GIT_EXTERNAL_DIFF", std::nullopt},
        {"GIT_DIFF_OPTS", std::nullopt},
        {"GIT_CONFIG", std::nullopt},
        {"GIT_CONFIG_PARAMETERS", std::nullopt},
        {"GIT_CONFIG_COUNT", std::nullopt},
    };
    const auto start = std::chrono::steady_clock::now();
    auto process = m_launcher.launch(spec);
    if (!process) {
        return fail(ErrorCode::NotFound, std::format("git not found: {} could not be started ({}); configure with "
                                                     "-DFROST_GIT_EXECUTABLE=<absolute path> or set FROST_GIT",
                                                     m_git, process.error().describe()));
    }
    auto result = (*process)->communicate("", kGitTimeout);
    m_gitTime += std::chrono::steady_clock::now() - start;
    if (!result) {
        return std::unexpected(std::move(result.error().withContext(std::format("running {}", m_git))));
    }
    return result;
}

Result<std::string> GitRunner::freshDirectory(std::string_view prefix) {
    auto directory = io::joinPath(m_scratch, std::format("{}{}", prefix, m_runs++));
    if (!directory) {
        return directory;
    }
    if (auto made = makeDirectories(*directory); !made) {
        return std::unexpected(std::move(made.error()));
    }
    return directory;
}

Result<std::string> GitRunner::version() {
    auto result = run({"--version"}, m_scratch);
    if (!result) {
        return std::unexpected(std::move(result.error()));
    }
    if (result->exitCode != 0 || !result->stderrBytes.empty()) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("git --version exited {}; {}", result->exitCode, stderrNote(result->stderrBytes)));
    }
    std::string text = std::move(result->stdoutBytes);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.pop_back();
    }
    m_version = text;
    return text;
}

Result<std::vector<GitSection>> GitRunner::diff(std::span<const GitCase> cases, std::uint32_t context) {
    const std::uint32_t contexts[] = {context};
    auto runs = diffContexts(cases, contexts);
    if (!runs) {
        return std::unexpected(std::move(runs.error()));
    }
    return std::move(runs->front());
}

Result<std::vector<std::vector<GitSection>>> GitRunner::diffContexts(std::span<const GitCase> cases,
                                                                     std::span<const std::uint32_t> contexts) {
    auto directory = freshDirectory("run");
    if (!directory) {
        return std::unexpected(std::move(directory.error()));
    }
    const auto start = std::chrono::steady_clock::now();
    if (auto written = writeBatch(*directory, cases); !written) {
        return std::unexpected(std::move(written.error()));
    }
    m_writeTime += std::chrono::steady_clock::now() - start;
    std::vector<std::vector<GitSection>> runs;
    for (const std::uint32_t context : contexts) {
        auto result = run({"diff", "--no-index", "--histogram", "--no-indent-heuristic", "--inter-hunk-context=0",
                           std::format("-U{}", context), "--no-color", "--no-ext-diff", "--no-textconv", "--no-renames",
                           "-a", "a", "b"},
                          *directory);
        if (!result) {
            return std::unexpected(std::move(result.error()));
        }
        if ((result->exitCode != 0 && result->exitCode != 1) || result->signal != 0 || !result->stderrBytes.empty()) {
            return fail(ErrorCode::InvalidArgument, std::format("git diff exited {} (signal {}); {}", result->exitCode,
                                                                result->signal, stderrNote(result->stderrBytes)));
        }
        auto sections = parseGitDiff(result->stdoutBytes, cases);
        if (!sections) {
            return std::unexpected(std::move(sections.error().withContext(stderrNote(result->stderrBytes))));
        }
        runs.push_back(std::move(*sections));
    }
    return runs;
}

Result<GitDialect> GitRunner::probeDialect(std::span<const DialectGolden> canaries, std::string& report) {
    std::vector<GitCase> cases;
    for (const DialectGolden& canary : canaries) {
        cases.push_back(GitCase{canary.a, canary.b});
    }
    auto sections = diff(cases, 0);
    if (!sections) {
        return std::unexpected(std::move(sections.error()));
    }
    std::size_t classic = 0;
    std::size_t fresh = 0;
    report.clear();
    for (std::size_t k = 0; k < canaries.size(); ++k) {
        const HunkList& got = (*sections)[k].hunks;
        const bool isClassic = got == canaries[k].classic;
        const bool isNew = got == canaries[k].fresh;
        classic += isClassic;
        fresh += isNew;
        report += std::format("  {}: git {} ({})\n", canaries[k].name, formatHunks(got),
                              isClassic ? "classic"
                              : isNew   ? "new"
                                        : "neither");
    }
    if (!canaries.empty() && classic == canaries.size()) {
        return GitDialect::Classic;
    }
    if (!canaries.empty() && fresh == canaries.size()) {
        return GitDialect::New;
    }
    return fail(ErrorCode::InvalidArgument,
                std::format("unknown git dialect ({}): {} classic and {} new of {} canaries\n{}",
                            m_version.empty() ? "version not read" : m_version, classic, fresh, canaries.size(),
                            report));
}

Result<std::vector<std::string>> GitRunner::apply(std::span<const GitCase> cases, std::string_view patch,
                                                  bool unidiffZero) {
    auto directory = freshDirectory("apply");
    if (!directory) {
        return std::unexpected(std::move(directory.error()));
    }
    const auto start = std::chrono::steady_clock::now();
    if (auto written = writeSideA(*directory, cases); !written) {
        return std::unexpected(std::move(written.error()));
    }
    auto patchPath = io::joinPath(m_scratch, std::format("patch{}.diff", m_runs++));
    if (!patchPath) {
        return std::unexpected(std::move(patchPath.error()));
    }
    if (auto written = writeBinaryFile(*patchPath, patch); !written) {
        return std::unexpected(std::move(written.error()));
    }
    m_writeTime += std::chrono::steady_clock::now() - start;
    std::vector<std::string> arguments = {"apply", "--whitespace=nowarn"};
    if (unidiffZero) {
        arguments.push_back("--unidiff-zero");
    }
    arguments.push_back(*patchPath);
    auto result = run(std::move(arguments), *directory);
    if (!result) {
        return std::unexpected(std::move(result.error()));
    }
    if (result->exitCode != 0 || result->signal != 0 || !result->stderrBytes.empty()) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("git apply exited {}; {}", result->exitCode, stderrNote(result->stderrBytes)));
    }
    std::vector<std::string> files;
    files.reserve(cases.size());
    for (std::size_t k = 0; k < cases.size(); ++k) {
        auto bytes = readBinaryFile(joinUtf8(*directory, caseName(k)));
        if (!bytes) {
            return std::unexpected(std::move(bytes.error()));
        }
        files.push_back(std::move(*bytes));
    }
    return files;
}

} // namespace frost::diff::test
