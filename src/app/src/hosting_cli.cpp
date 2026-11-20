// SPDX-License-Identifier: Apache-2.0
// Uses documented provider CLI/REST formats, not hosting SDKs, scraped pages, or fabricated results.
// Contracts: https://cli.github.com/manual/gh_pr_list ; https://docs.gitlab.com/cli/api/
#include "hosting_cli.hpp"
#include "frost/io/path.hpp"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QUrl>
#include <algorithm>
#include <chrono>
#include <set>
#include <limits>

namespace frost::app {
namespace {
bool component(std::string_view value) {
    return !value.empty() && value != "." && value != ".." && value.front() != '-' && std::ranges::all_of(value, [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
    });
}
bool validRepository(std::string_view value, HostingProvider provider) {
    if (value.size() > 512) return false;
    unsigned segments = 0;
    while (true) {
        const auto slash = value.find('/');
        if (!component(value.substr(0, slash))) return false;
        ++segments;
        if (slash == std::string_view::npos) break;
        value.remove_prefix(slash + 1);
    }
    return provider == HostingProvider::GitHub ? segments == 2 : segments >= 2;
}
bool sha(std::string_view value) {
    return (value.size() == 40 || value.size() == 64) && std::ranges::all_of(value, [](unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    });
}
std::string encode(std::string_view value) { return QUrl::toPercentEncoding(QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()))).toStdString(); }
}
Result<HostingCommand> hostingCommand(const HostingRequest& request) {
    if (!io::isAbsolutePath(request.executable) || !io::isAbsolutePath(request.root)) return fail(ErrorCode::InvalidArgument, "Choose an absolute provider CLI path and an open project");
    if (!component(request.host) || request.host.size() > 253 || !validRepository(request.repository, request.provider)) return fail(ErrorCode::InvalidArgument, "Enter a hostname and owner/repository (GitLab subgroups are supported)");
    if (request.page == 0 || request.page > 10000 || request.body.size() > 65536) return fail(ErrorCode::InvalidArgument, "Invalid review page or body size");
    const bool github = request.provider == HostingProvider::GitHub;
    const auto operation = request.operation;
    const bool numbered = operation != HostingOperation::Account && operation != HostingOperation::List;
    if (numbered && request.number == 0) return fail(ErrorCode::InvalidArgument, "Select a provider review first");
    const bool review = operation == HostingOperation::Approve || operation == HostingOperation::RequestChanges;
    if (review && !sha(request.head)) return fail(ErrorCode::InvalidArgument, "Refresh the selected review to capture its exact head commit");
    if ((operation == HostingOperation::Comment || operation == HostingOperation::RequestChanges) && request.body.empty()) return fail(ErrorCode::InvalidArgument, "Enter a review message");
    if (!github && operation == HostingOperation::RequestChanges) return fail(ErrorCode::Unsupported, "Use a GitLab comment or approval; request-changes is unavailable in this CLI profile");
    HostingCommand command;
    command.process.executable = request.executable;
    command.process.workingDirectory = request.root;
    command.process.environment = {{"GH_PROMPT_DISABLED", "1"}, {"GH_PAGER", "cat"}, {"PAGER", "cat"}, {"NO_COLOR", "1"}, {"GLAB_CHECK_UPDATE", "false"}, {"GLAB_DEBUG_HTTP", std::nullopt}, {"GH_DEBUG", std::nullopt}, {"GIT_TERMINAL_PROMPT", "0"}, {"GIT_DIR", std::nullopt}, {"GIT_WORK_TREE", std::nullopt}, {"GIT_INDEX_FILE", std::nullopt}, {"GIT_COMMON_DIR", std::nullopt}, {"GIT_TRACE", std::nullopt}, {"GIT_TRACE_CURL", std::nullopt}, {"GIT_TRACE_PACKET", std::nullopt}, {"GIT_CURL_VERBOSE", std::nullopt}};
    auto& args = command.process.arguments;
    const auto number = std::to_string(request.number);
    const auto project = github ? "repos/" + request.repository : "projects/" + encode(request.repository);
    const auto reviews = github ? "/pulls/" : "/merge_requests/";
    if (operation == HostingOperation::Account) args = {"api", "user", "--hostname", request.host};
    else if (operation == HostingOperation::List) {
        // REST list pages have an explicit upper bound and provider-defined JSON field names.
        args = {"api", project + reviews + "?state=" + (github ? "open" : "opened") + "&per_page=100&page=" + std::to_string(request.page), "--hostname", request.host};
    } else if (operation == HostingOperation::Details) {
        args = {"api", project + reviews + number, "--hostname", request.host};
    } else if (operation == HostingOperation::Diff || operation == HostingOperation::Checkout) {
        args = {github ? "pr" : "mr", operation == HostingOperation::Diff ? "diff" : "checkout", number,
            "--repo", github ? request.host + "/" + request.repository : "https://" + request.host + "/" + request.repository};
        if (operation == HostingOperation::Diff) args.insert(args.end(), {"--color", "never"});
    } else {
        std::string endpoint;
        QJsonObject body;
        if (github) {
            endpoint = project + "/pulls/" + number + "/reviews";
            body.insert(QStringLiteral("body"), QString::fromStdString(request.body));
            body.insert(QStringLiteral("event"), operation == HostingOperation::Approve ? QStringLiteral("APPROVE") : operation == HostingOperation::RequestChanges ? QStringLiteral("REQUEST_CHANGES") : QStringLiteral("COMMENT"));
            if (!sha(request.head)) return fail(ErrorCode::InvalidArgument, "Refresh review details to capture the head commit");
            body.insert(QStringLiteral("commit_id"), QString::fromStdString(request.head));
        } else {
            endpoint = project + "/merge_requests/" + number + (operation == HostingOperation::Approve ? "/approve" : "/notes");
            if (operation == HostingOperation::Approve) body.insert(QStringLiteral("sha"), QString::fromStdString(request.head));
            else body.insert(QStringLiteral("body"), QString::fromStdString(request.body));
        }
        args = {"api", endpoint, "--hostname", request.host, "--method", "POST", "--input", "-"};
        command.input = QJsonDocument(body).toJson(QJsonDocument::Compact).toStdString();
        command.process.stdinMode = io::Stdio::Pipe;
    }
    return command;
}
Result<std::vector<HostedReview>> parseHostedReviews(HostingProvider provider, std::string_view bytes) {
    if (bytes.size() > 4 * 1024 * 1024) return fail(ErrorCode::OutOfRange, "Provider response exceeds 4 MiB");
    QJsonParseError error;
    const auto doc = QJsonDocument::fromJson(QByteArray(bytes.data(), static_cast<qsizetype>(bytes.size())), &error);
    if (error.error != QJsonParseError::NoError || !doc.isArray() || doc.array().size() > 100) return fail(ErrorCode::Corrupt, "Provider returned an invalid review page");
    std::vector<HostedReview> result;
    std::set<std::uint32_t> seen;
    for (const auto& value : doc.array()) {
        if (!value.isObject()) return fail(ErrorCode::Corrupt, "Provider review row is not an object");
        const auto row = value.toObject();
        const bool github = provider == HostingProvider::GitHub;
        const auto id = row.value(github ? QStringLiteral("number") : QStringLiteral("iid")).toInteger(-1);
        const auto title = row.value(QStringLiteral("title"));
        const auto state = row.value(QStringLiteral("state"));
        const auto url = row.value(github ? QStringLiteral("html_url") : QStringLiteral("web_url"));
        const auto author = row.value(github ? QStringLiteral("user") : QStringLiteral("author")).toObject();
        const auto source = github ? row.value(QStringLiteral("head")).toObject().value(QStringLiteral("ref")) : row.value(QStringLiteral("source_branch"));
        const auto target = github ? row.value(QStringLiteral("base")).toObject().value(QStringLiteral("ref")) : row.value(QStringLiteral("target_branch"));
        const auto head = github ? row.value(QStringLiteral("head")).toObject().value(QStringLiteral("sha")) : row.value(QStringLiteral("sha"));
        const auto login = author.value(github ? QStringLiteral("login") : QStringLiteral("username"));
        if (id <= 0 || id > std::numeric_limits<std::uint32_t>::max() || !title.isString() || !state.isString() || !url.isString() || !source.isString() || !target.isString() || !head.isString() || !login.isString() || !sha(head.toString().toStdString()) || !seen.insert(static_cast<std::uint32_t>(id)).second)
            return fail(ErrorCode::Corrupt, "Provider review row is missing required identity or head fields");
        result.push_back({static_cast<std::uint32_t>(id), title.toString().toStdString(), state.toString().toStdString(), login.toString().toStdString(), source.toString().toStdString(), target.toString().toStdString(), head.toString().toStdString(), url.toString().toStdString()});
    }
    return result;
}
Result<std::string> runHosting(io::IProcessLauncher& launcher, const HostingRequest& request, const std::function<bool()>& admitted, std::stop_token stop) {
    auto command = hostingCommand(request);
    if (!command) return std::unexpected(command.error());
    if (stop.stop_requested()) return fail(ErrorCode::Cancelled, "Provider request cancelled");
    if (!admitted || !admitted()) return fail(ErrorCode::Untrusted, "The selected project is no longer trusted or current");
    const auto matchesHead = [&]() -> Result<void> {
        if (!sha(request.head)) return fail(ErrorCode::InvalidArgument, "Refresh review metadata before loading a diff");
        auto details = request; details.operation = HostingOperation::Details;
        auto bytes = runHosting(launcher, details, admitted, stop);
        if (!bytes) return std::unexpected(bytes.error());
        auto rows = parseHostedReviews(request.provider, "[" + *bytes + "]");
        if (!rows) return std::unexpected(rows.error());
        if (rows->size() != 1 || rows->front().number != request.number || rows->front().head != request.head)
            return fail(ErrorCode::Busy, "Review head changed; refresh metadata before reading or reviewing its diff");
        return {};
    };
    if (request.operation == HostingOperation::Diff) if (auto matched = matchesHead(); !matched) return std::unexpected(matched.error());
    auto output = io::runToCompletion(launcher, command->process, command->input, std::chrono::seconds(60), stop);
    if (!output) return std::unexpected(output.error());
    if (stop.stop_requested() || !admitted()) return fail(ErrorCode::Cancelled, "Provider project changed during request");
    if (output->exitCode != 0 || output->signal != 0) return fail(ErrorCode::IoError, "Provider CLI failed: " + output->stderrBytes.substr(0, 4096));
    if (output->stdoutBytes.size() > 8 * 1024 * 1024) return fail(ErrorCode::OutOfRange, "Provider output exceeds 8 MiB");
    if (request.operation == HostingOperation::Diff) if (auto matched = matchesHead(); !matched) return std::unexpected(matched.error());
    return std::move(output->stdoutBytes);
}
}
