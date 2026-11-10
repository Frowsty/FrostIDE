// SPDX-License-Identifier: Apache-2.0
//
// Safe solution and project edits (frost/project/project_edit.hpp, SLN-6): the edit protocol's
// request, decision and answer lines, the project edits implied by a path change on disk, and the
// blocking runner.

#include "frost/project/project_edit.hpp"

#include "frost/io/path.hpp"
#include "frost/project/solution_model.hpp"
#include "frost/project/vcxproj.hpp"

#include "json.hpp"
#include "paths.hpp"
#include "text_util.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <format>
#include <mutex>
#include <stop_token>
#include <thread>

namespace frost::project {
namespace {

// Appends `"key":"value"` with JSON escaping (UTF-8 passes through; controls are \u escaped).
void appendMember(std::string& out, std::string_view key, std::string_view value) {
    const auto quoted = [&out](std::string_view text) {
        out += '"';
        for (const char c : text) {
            switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    out += std::format("\\u{:04x}", static_cast<unsigned>(static_cast<unsigned char>(c)));
                } else {
                    out += c;
                }
            }
        }
        out += '"';
    };
    if (out.size() > 1) {
        out += ',';
    }
    quoted(key);
    out += ':';
    quoted(value);
}

bool isGuid(std::string_view id) {
    if (id.size() != 36) {
        return false;
    }
    for (std::size_t i = 0; i < id.size(); ++i) {
        const bool dash = i == 8 || i == 13 || i == 18 || i == 23;
        if (dash ? id[i] != '-' : std::isxdigit(static_cast<unsigned char>(id[i])) == 0) {
            return false;
        }
    }
    return true;
}

bool hasExtension(std::string_view path, std::string_view extension) {
    return path.size() > extension.size() &&
           detail::pathKey(path.substr(path.size() - extension.size())) == detail::pathKey(extension);
}

// An absolute path the request can carry: no '|' (the list separator) and no control character.
Result<void> checkPath(std::string_view path, std::string_view what) {
    if (!io::isAbsolutePath(path) ||
        std::ranges::any_of(path, [](char c) { return c == '|' || static_cast<unsigned char>(c) < 0x20; })) {
        return fail(ErrorCode::InvalidArgument, std::format("{} must be an absolute path without '|': {}", what, path));
    }
    return {};
}

Result<std::string> joinPaths(const std::vector<std::string>& paths, std::string_view what) {
    if (paths.empty()) {
        return fail(ErrorCode::InvalidArgument, std::format("the edit names no {}", what));
    }
    std::string out;
    for (const std::string& path : paths) {
        if (auto checked = checkPath(path, what); !checked) {
            return std::unexpected(std::move(checked.error()));
        }
        out += (out.empty() ? "" : "|") + path;
    }
    return out;
}

} // namespace

Result<std::string> projectEditRequest(const ProjectEdit& edit) {
    for (const auto& [path, what] : {std::pair{std::string_view(edit.root), "the trusted root"},
                                     std::pair{std::string_view(edit.definition), "the definition"}}) {
        if (auto checked = checkPath(path, what); !checked) {
            return std::unexpected(std::move(checked.error()));
        }
    }
    if (!isGuid(edit.transactionId)) {
        return fail(ErrorCode::InvalidArgument, "the transaction id must be a GUID");
    }
    const bool solutionEdit = edit.kind == ProjectEditKind::AddProject || edit.kind == ProjectEditKind::RemoveProject;
    if (solutionEdit ? !(hasExtension(edit.definition, ".sln") || hasExtension(edit.definition, ".slnx"))
                     : !hasExtension(edit.definition, ".vcxproj")) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("{} is not a {}", edit.definition, solutionEdit ? "solution" : ".vcxproj project"));
    }
    std::string out = "{";
    appendMember(out, solutionEdit ? "Solution" : "Project", edit.definition);
    appendMember(out, "Root", edit.root);
    appendMember(out, "Trusted", "true"); // The caller edits only a trusted root; the helper checks the flag.
    appendMember(out, "Commit", edit.commit ? "true" : "false");
    appendMember(out, "TransactionId", edit.transactionId);
    if (solutionEdit) {
        if (auto checked = checkPath(edit.project, "the project"); !checked) {
            return std::unexpected(std::move(checked.error()));
        }
        appendMember(out, edit.kind == ProjectEditKind::AddProject ? "AddProject" : "RemoveProject", edit.project);
        out += '}';
        return out;
    }
    if (edit.configuration.empty() || edit.platform.empty()) {
        return fail(ErrorCode::InvalidArgument, "a file edit needs the project's configuration and platform");
    }
    appendMember(out, "Configuration", edit.configuration);
    appendMember(out, "Platform", edit.platform);
    auto files = joinPaths(edit.files, "file");
    if (!files) {
        return std::unexpected(std::move(files.error()));
    }
    if (edit.kind != ProjectEditKind::AddFiles && !edit.filter.empty()) {
        return fail(ErrorCode::InvalidArgument, "only an added file is placed in a filter");
    }
    switch (edit.kind) {
    case ProjectEditKind::AddFiles:
        appendMember(out, "Add", *files);
        appendMember(out, "Filter", edit.filter);
        break;
    case ProjectEditKind::RemoveFiles: appendMember(out, "Remove", *files); break;
    case ProjectEditKind::RenameFiles: {
        auto renamed = joinPaths(edit.renamedTo, "new file name");
        if (!renamed || edit.renamedTo.size() != edit.files.size()) {
            return fail(ErrorCode::InvalidArgument, "a rename needs one new path per file");
        }
        appendMember(out, "RenameFrom", *files);
        appendMember(out, "RenameTo", *renamed);
        break;
    }
    default: break;
    }
    out += '}';
    return out;
}

std::string projectEditDecision(std::string_view transactionId, bool accept) {
    std::string out = "{";
    appendMember(out, "Accept", accept ? "true" : "false");
    appendMember(out, "TransactionId", transactionId);
    out += '}';
    return out;
}

Result<ProjectEditAnswer> parseProjectEditAnswer(std::string_view line) {
    auto json = detail::parseJson(line);
    if (!json || !json->isObject()) {
        return fail(ErrorCode::Corrupt, std::format("ProjectHost wrote an unreadable answer: {}", line.substr(0, 200)));
    }
    ProjectEditAnswer answer;
    answer.ready = json->boolAt("Ready");
    if (answer.ready) {
        answer.transactionId = json->stringAt("TransactionId");
        answer.journalDigest = json->stringAt("JournalDigest");
        if (!isGuid(answer.transactionId) || answer.journalDigest.size() != 64 ||
            answer.journalDigest.find_first_not_of("0123456789ABCDEF") != std::string::npos) {
            return fail(ErrorCode::Corrupt, "ProjectHost announced an invalid transaction");
        }
        return answer;
    }
    const detail::JsonNode* error = json->find("Error");
    if (error == nullptr) {
        return fail(ErrorCode::Corrupt, "ProjectHost's answer has no Error member");
    }
    if (!error->isNull()) {
        answer.error = error->isString() ? error->text : std::string("ProjectHost failed");
    }
    return answer;
}

std::vector<ProjectPathChange> projectEditsForPathChange(const SolutionModel& solution, std::string_view oldPath,
                                                         std::string_view newPath, bool directory) {
    std::vector<ProjectPathChange> changes;
    const std::string oldKey = detail::pathKey(oldPath);
    for (const ProjectInfo& project : solution.projects()) {
        if (project.kind != ProjectKind::Cpp || !project.vcxproj || project.path.empty()) {
            continue;
        }
        ProjectPathChange change{.project = project.path, .files = {}, .renamedTo = {}};
        for (const ProjectItem& item : project.vcxproj->items) {
            if (item.fromWildcard) {
                continue;
            }
            const std::string key = detail::pathKey(item.path);
            const bool exact = key == oldKey;
            const bool under = directory && key.size() > oldKey.size() && key.starts_with(oldKey) &&
                               (key[oldKey.size()] == '/' || key[oldKey.size()] == '\\');
            if (!exact && !under) {
                continue;
            }
            change.files.push_back(item.path);
            if (!newPath.empty()) {
                // The key keeps the spelling's length (ASCII lower-casing), so the suffix index is the item's.
                change.renamedTo.push_back(std::string(newPath) +
                                           item.path.substr(std::min(oldPath.size(), item.path.size())));
            }
        }
        if (!change.files.empty()) {
            changes.push_back(std::move(change));
        }
    }
    return changes;
}

Result<void> runProjectEdit(io::IProcessLauncher& launcher, const ProjectEditLaunch& launch, const ProjectEdit& edit,
                            const std::function<bool(std::string_view digest)>& admit) {
    auto request = projectEditRequest(edit);
    if (!request) {
        return std::unexpected(std::move(request.error()));
    }
    if (!io::isAbsolutePath(launch.hostPath) || !io::isAbsolutePath(launch.msbuildPath)) {
        return fail(ErrorCode::InvalidArgument, "ProjectHost and MSBuild paths must be absolute");
    }
    io::ProcessSpec spec;
    spec.executable = launch.hostPath;
    spec.arguments = {std::string(io::parentPath(launch.msbuildPath))};
    spec.stdinMode = io::Stdio::Pipe;
    spec.killOnClose = true;
    auto launched = launcher.launch(spec);
    if (!launched) {
        return std::unexpected(std::move(launched.error()));
    }
    std::unique_ptr<io::IProcess> process = std::move(*launched);
    // The stderr drain keeps the helper from blocking on a full pipe; its tail explains a crash.
    std::string stderrTail;
    std::mutex stderrMutex;
    std::jthread drain([&process, &stderrTail, &stderrMutex] {
        std::array<char, 4096> buffer{};
        for (;;) {
            auto count = process->readStderr(buffer);
            if (!count || *count == 0) {
                return;
            }
            std::lock_guard lock(stderrMutex);
            stderrTail.append(buffer.data(), *count);
            if (stderrTail.size() > 4096) {
                stderrTail.erase(0, stderrTail.size() - 4096);
            }
        }
    });
    // The watchdog ends the helper when the whole edit takes longer than the launch allows.
    std::atomic<bool> timedOut{false};
    std::jthread watchdog([&process, &timedOut, timeout = launch.timeout](std::stop_token stop) {
        std::mutex mutex;
        std::condition_variable_any wake;
        std::unique_lock lock(mutex);
        if (!wake.wait_for(lock, stop, timeout, [] { return false; }) && !stop.stop_requested()) {
            timedOut = true;
            static_cast<void>(process->terminate());
        }
    });
    const auto finish = [&](Result<void> result) -> Result<void> {
        process->closeStdin();
        auto status = process->wait(std::chrono::seconds(30));
        watchdog.request_stop();
        watchdog.join();
        if (!status) {
            static_cast<void>(process->terminate());
        }
        drain.join();
        if (timedOut) {
            return fail(ErrorCode::Timeout, "ProjectHost did not finish the edit in time; it was ended");
        }
        if (result && status && status->exitCode != 0) {
            return fail(ErrorCode::IoError, std::format("ProjectHost ended with exit code {}", status->exitCode));
        }
        return result;
    };
    if (auto written = process->writeStdin(*request + "\n"); !written) {
        return finish(std::unexpected(std::move(written.error())));
    }
    std::string pending;
    std::array<char, 65536> buffer{};
    bool decided = false;
    for (;;) {
        std::size_t newline = pending.find('\n');
        while (newline == std::string::npos) {
            auto count = process->readStdout(buffer);
            if (!count || *count == 0) {
                std::lock_guard lock(stderrMutex);
                return finish(fail(ErrorCode::IoError,
                                   std::format("ProjectHost ended without an answer: {}", detail::trim(stderrTail))));
            }
            pending.append(buffer.data(), *count);
            newline = pending.find('\n');
        }
        std::string line = pending.substr(0, newline);
        pending.erase(0, newline + 1);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        auto answer = parseProjectEditAnswer(line);
        if (!answer) {
            return finish(std::unexpected(std::move(answer.error())));
        }
        if (answer->ready) {
            if (decided || answer->transactionId != edit.transactionId) {
                return finish(fail(ErrorCode::Corrupt, "ProjectHost announced an unexpected transaction"));
            }
            decided = true;
            const bool accept = admit && admit(answer->journalDigest);
            if (auto written = process->writeStdin(projectEditDecision(edit.transactionId, accept) + "\n"); !written) {
                return finish(std::unexpected(std::move(written.error())));
            }
            process->closeStdin();
            if (!accept) {
                // The helper deletes its journal and answers "Commit rejected"; nothing was written.
                auto rest = finish({});
                return rest ? Result<void>(fail(ErrorCode::Cancelled, "the edit was not admitted; nothing was written"))
                            : rest;
            }
            continue;
        }
        if (answer->error) {
            return finish(fail(ErrorCode::IoError, *answer->error));
        }
        if (edit.commit && !decided) {
            return finish(fail(ErrorCode::Corrupt, "ProjectHost answered without asking to commit"));
        }
        return finish({});
    }
}

} // namespace frost::project
