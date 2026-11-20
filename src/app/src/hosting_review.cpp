// SPDX-License-Identifier: Apache-2.0
// Actual provider file-patch anchors, immutable diff refs and existing review discussion contracts.
// https://docs.github.com/rest/pulls/comments ; https://docs.gitlab.com/api/discussions/
#include "hosting_cli.hpp"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QUrl>

#include <algorithm>
#include <charconv>
#include <limits>
#include <set>
namespace frost::app {
namespace {
bool sha(std::string_view value) {
    return (value.size() == 40 || value.size() == 64) &&
           value.find_first_not_of("0123456789abcdefABCDEF") == value.npos;
}
bool path(std::string_view value) {
    if (value.empty() || value.size() > 32768 || value.front() == '/' || value.front() == '\\' ||
        value.find_first_of("\0\r\n", 0, 3) != value.npos || value.find('\\') != value.npos ||
        value.find(':') != value.npos)
        return false;
    while (true) {
        const auto end = value.find('/');
        const auto part = value.substr(0, end);
        if (part.empty() || part == "." || part == "..")
            return false;
        if (end == value.npos)
            break;
        value.remove_prefix(end + 1);
    }
    return true;
}
std::string value(const QJsonObject& row, const char* name) {
    return row.value(QLatin1String(name)).toString().toStdString();
}
Result<QJsonDocument> document(std::string_view bytes, bool array) {
    if (bytes.size() > 8U * 1024U * 1024U)
        return fail(ErrorCode::OutOfRange, "Hosted review response exceeds8MiB");
    QJsonParseError error;
    auto result = QJsonDocument::fromJson(QByteArray(bytes.data(), static_cast<qsizetype>(bytes.size())), &error);
    if (error.error != QJsonParseError::NoError || (array ? !result.isArray() : !result.isObject()))
        return fail(ErrorCode::Corrupt, "Malformed hosted review response");
    return result;
}
Result<std::optional<std::uint32_t>> line(const QJsonObject& row, const char* name) {
    const auto field = row.value(QLatin1String(name));
    if (field.isUndefined() || field.isNull())
        return std::optional<std::uint32_t>{};
    const auto number = field.toInteger(-1);
    if (number <= 0 || number > std::numeric_limits<std::uint32_t>::max())
        return fail(ErrorCode::Corrupt, "Invalid provider line number");
    return std::optional<std::uint32_t>{static_cast<std::uint32_t>(number)};
}
bool integer(std::string_view& input, std::uint32_t& number) {
    const auto parsed = std::from_chars(input.data(), input.data() + input.size(), number);
    if (parsed.ec != std::errc{} || parsed.ptr == input.data())
        return false;
    input.remove_prefix(static_cast<std::size_t>(parsed.ptr - input.data()));
    return true;
}
Result<std::vector<HostedDiffLine>> patchLines(std::string_view patch) {
    if (patch.find('\0') != patch.npos)
        return fail(ErrorCode::Unsupported, "Provider patch is not NUL-free text");
    if (patch.size() > 8U * 1024U * 1024U)
        return fail(ErrorCode::OutOfRange, "Provider patch exceeds8MiB");
    std::vector<HostedDiffLine> result;
    std::uint32_t old = 0, next = 0, oldLeft = 0, newLeft = 0;
    bool hunk = false;
    std::set<std::uint32_t> oldSeen, newSeen;
    while (!patch.empty()) {
        const auto end = patch.find('\n');
        const auto row = patch.substr(0, end);
        if (end == patch.npos)
            patch = {};
        else
            patch.remove_prefix(end + 1);
        if (row.starts_with("@@ -")) {
            if (oldLeft || newLeft)
                return fail(ErrorCode::Corrupt, "Truncated provider diff hunk");
            auto header = row.substr(4);
            std::uint32_t oldCount = 1, newCount = 1;
            if (!integer(header, old))
                return fail(ErrorCode::Corrupt, "Invalid old hunk range");
            if (header.starts_with(',')) {
                header.remove_prefix(1);
                if (!integer(header, oldCount))
                    return fail(ErrorCode::Corrupt, "Invalid old hunk count");
            }
            if (!header.starts_with(" +"))
                return fail(ErrorCode::Corrupt, "Invalid provider hunk separator");
            header.remove_prefix(2);
            if (!integer(header, next))
                return fail(ErrorCode::Corrupt, "Invalid new hunk range");
            if (header.starts_with(',')) {
                header.remove_prefix(1);
                if (!integer(header, newCount))
                    return fail(ErrorCode::Corrupt, "Invalid new hunk count");
            }
            if (!header.starts_with(" @@") || (oldCount && !old) || (newCount && !next) ||
                oldCount > UINT32_MAX - old || newCount > UINT32_MAX - next)
                return fail(ErrorCode::Corrupt, "Invalid or overflowing provider hunk");
            oldLeft = oldCount;
            newLeft = newCount;
            hunk = true;
            continue;
        }
        if (row == "\\ No newline at end of file")
            continue;
        if (!hunk || row.empty() || (row.front() != ' ' && row.front() != '+' && row.front() != '-'))
            return fail(ErrorCode::Corrupt, "Unsupported provider patch line");
        HostedDiffLine entry;
        entry.text = std::string(row);
        if (row.front() != '+') {
            if (!oldLeft || !oldSeen.insert(old).second)
                return fail(ErrorCode::Corrupt, "Overlapping/overflowing old hunk anchor");
            entry.oldLine = old++;
            --oldLeft;
        }
        if (row.front() != '-') {
            if (!newLeft || !newSeen.insert(next).second)
                return fail(ErrorCode::Corrupt, "Overlapping/overflowing new hunk anchor");
            entry.newLine = next++;
            --newLeft;
        }
        result.push_back(std::move(entry));
        if (result.size() > 20000)
            return fail(ErrorCode::OutOfRange, "Provider file exceeds20000 review lines");
    }
    if (!hunk || oldLeft || newLeft)
        return fail(ErrorCode::Corrupt, "Incomplete provider patch");
    return result;
}
template<class Evidence>
bool sameContext(const HostingRequest& request, const Evidence& evidence) {
    return request.provider == evidence.provider && request.executable == evidence.executable &&
           request.root == evidence.root && request.host == evidence.host &&
           request.repository == evidence.repository && request.number == evidence.number;
}
} // namespace
Result<HostedDiffIdentity> parseHostedDiffIdentity(HostingProvider provider, std::string_view bytes) {
    auto doc = document(bytes, false);
    if (!doc)
        return std::unexpected(doc.error());
    const auto row = doc->object();
    HostedDiffIdentity result;
    if (provider == HostingProvider::GitHub) {
        result.head = value(row.value("head").toObject(), "sha");
        result.base = value(row.value("base").toObject(), "sha");
        result.start = result.base;
    } else {
        const auto refs = row.value("diff_refs").toObject();
        result.head = value(refs, "head_sha");
        result.base = value(refs, "base_sha");
        result.start = value(refs, "start_sha");
        if (value(row, "sha") != result.head)
            return fail(ErrorCode::Busy, "GitLab diff refs are not ready for the selected head");
    }
    if (!sha(result.head) || !sha(result.base) || !sha(result.start))
        return fail(ErrorCode::Unsupported,
                    "Provider has no complete immutable diff refs; refresh when its diff is ready");
    return result;
}
Result<HostedDiffSnapshot> parseHostedDiffFiles(const HostingRequest& request, HostedDiffIdentity identity,
                                                std::string_view bytes) {
    auto doc = document(bytes, true);
    if (!doc)
        return std::unexpected(doc.error());
    if (doc->array().size() > 100)
        return fail(ErrorCode::OutOfRange, "Provider file page exceeds100 files");
    if (!sha(identity.head) || !sha(identity.base) || !sha(identity.start) || identity.head != request.head)
        return fail(ErrorCode::Busy, "Provider file evidence belongs to another review head");
    HostedDiffSnapshot result{request.provider, request.executable,  request.root,
                              request.host,     request.repository,  request.number,
                              request.page,     std::move(identity), {}};
    std::set<std::string> paths;
    std::size_t count = 0;
    for (const auto& element : doc->array()) {
        if (!element.isObject())
            return fail(ErrorCode::Corrupt, "Provider file row is not an object");
        const auto row = element.toObject();
        HostedDiffFile file;
        const bool github = request.provider == HostingProvider::GitHub;
        file.newPath = value(row, github ? "filename" : "new_path");
        file.oldPath = github ? value(row, "previous_filename") : value(row, "old_path");
        if (github) {
            const auto status = value(row, "status");
            if (status != "added" && status != "removed" && status != "modified" && status != "renamed" &&
                status != "copied" && status != "changed" && status != "unchanged")
                return fail(ErrorCode::Unsupported, "Unknown provider file change type");
            if ((status == "renamed" || status == "copied") && file.oldPath.empty())
                return fail(ErrorCode::Corrupt, "Provider renamed/copied file lacks its original path");
            if (file.oldPath.empty())
                file.oldPath = file.newPath;
        }
        if (!path(file.newPath) || !path(file.oldPath) || !paths.insert(file.newPath).second)
            return fail(ErrorCode::Corrupt, "Invalid or duplicate provider repository path");
        const auto patch = row.value(github ? "patch" : "diff");
        if (!patch.isString() || patch.toString().isEmpty() || row.value("collapsed").toBool() ||
            row.value("too_large").toBool())
            file.unavailable = "Provider supplied no complete inline-review patch (binary, collapsed or too large).";
        else {
            file.patch = patch.toString().toStdString();
            auto lines = patchLines(file.patch);
            if (!lines)
                file.unavailable = "Inline anchors unavailable: " + lines.error().message();
            else {
                if (github) {
                    const auto added = row.value("additions").toInteger(-1),
                               removed = row.value("deletions").toInteger(-1),
                               changes = row.value("changes").toInteger(-1);
                    const auto actualAdded =
                        std::ranges::count_if(*lines, [](const auto& line) { return line.newLine && !line.oldLine; });
                    const auto actualRemoved =
                        std::ranges::count_if(*lines, [](const auto& line) { return line.oldLine && !line.newLine; });
                    if (added < 0 || removed < 0 || added > UINT32_MAX || removed > UINT32_MAX ||
                        changes != added + removed || actualAdded != added || actualRemoved != removed) {
                        file.unavailable =
                            "Provider patch/count evidence is incomplete or truncated; inline anchors unavailable.";
                        result.files.push_back(std::move(file));
                        continue;
                    }
                }
                count += lines->size();
                if (count > 20000)
                    return fail(ErrorCode::OutOfRange,
                                "Provider page exceeds20000 displayed review lines; choose another file page");
                file.lines = std::move(*lines);
            }
        }
        result.files.push_back(std::move(file));
    }
    return result;
}
Result<HostedCommentPage> parseHostedComments(const HostingRequest& request, std::string_view bytes) {
    auto doc = document(bytes, true);
    if (!doc)
        return std::unexpected(doc.error());
    if (doc->array().size() > 100)
        return fail(ErrorCode::OutOfRange, "Provider comment page exceeds100 threads");
    HostedCommentPage result{request.provider,   request.executable, request.root, request.host,
                             request.repository, request.number,     request.page, {}};
    std::set<std::uint64_t> ids;
    const auto append = [&](const QJsonObject& note, std::string discussion) -> Result<void> {
        HostedComment comment;
        const auto id = note.value("id").toInteger(-1);
        if (id <= 0 || !ids.insert(static_cast<std::uint64_t>(id)).second)
            return fail(ErrorCode::Corrupt, "Invalid or duplicate provider comment id");
        comment.id = static_cast<std::uint64_t>(id);
        comment.discussion = std::move(discussion);
        const auto author = note.value(request.provider == HostingProvider::GitHub ? "user" : "author").toObject();
        comment.author = value(author, request.provider == HostingProvider::GitHub ? "login" : "username");
        if (!note.value("body").isString())
            return fail(ErrorCode::Corrupt, "Provider comment lacks body/author");
        comment.body = value(note, "body");
        if (comment.body.size() > 65536)
            return fail(ErrorCode::OutOfRange, "Provider comment body exceeds64KiB");
        const auto position = request.provider == HostingProvider::GitHub ? note : note.value("position").toObject();
        comment.path = value(position, request.provider == HostingProvider::GitHub ? "path" : "new_path");
        comment.head = value(position, request.provider == HostingProvider::GitHub ? "commit_id" : "head_sha");
        comment.url = value(note, "html_url");
        comment.originalHead = value(note, "original_commit_id");
        if (!comment.originalHead.empty() && !sha(comment.originalHead))
            return fail(ErrorCode::Corrupt, "Provider comment has an invalid original commit");
        const auto resolved = note.value("resolved");
        if (resolved.isBool())
            comment.resolved = resolved.toBool();
        if (!comment.path.empty() && !path(comment.path))
            return fail(ErrorCode::Corrupt, "Provider comment has an invalid path");
        if (!comment.head.empty() && !sha(comment.head))
            return fail(ErrorCode::Corrupt, "Provider comment has an invalid commit");
        if (request.provider == HostingProvider::GitHub) {
            const auto parent = note.value("in_reply_to_id");
            if (!parent.isUndefined() && !parent.isNull()) {
                const auto parentId = parent.toInteger(-1);
                if (parentId <= 0)
                    return fail(ErrorCode::Corrupt, "Invalid provider reply parent");
                comment.replyParent = static_cast<std::uint64_t>(parentId);
            }
            auto current = line(note, "line");
            auto original = line(note, "original_line");
            if (!current || !original)
                return fail(ErrorCode::Corrupt, "Invalid GitHub comment position");
            comment.originalLine = *original;
            const auto side = value(note, "side");
            if (*current) {
                if (side == "LEFT")
                    comment.oldLine = **current;
                else if (side == "RIGHT")
                    comment.newLine = **current;
                else
                    return fail(ErrorCode::Corrupt, "Unknown GitHub comment side");
            }
        } else {
            auto old = line(position, "old_line"), next = line(position, "new_line");
            if (!old || !next)
                return fail(ErrorCode::Corrupt, "Invalid GitLab comment position");
            comment.oldLine = *old;
            comment.newLine = *next;
        }
        result.comments.push_back(std::move(comment));
        if (result.comments.size() > 1000)
            return fail(ErrorCode::OutOfRange, "Provider thread page exceeds1000 notes");
        return {};
    };
    for (const auto& element : doc->array()) {
        if (!element.isObject())
            return fail(ErrorCode::Corrupt, "Invalid provider discussion");
        const auto row = element.toObject();
        if (request.provider == HostingProvider::GitHub) {
            if (auto added = append(row, {}); !added)
                return std::unexpected(added.error());
        } else {
            const auto id = value(row, "id");
            const auto notes = row.value("notes");
            if (id.empty() || id.size() > 128 ||
                id.find_first_not_of("0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_-") != id.npos ||
                !notes.isArray())
                return fail(ErrorCode::Corrupt, "Malformed GitLab discussion");
            for (const auto& note : notes.toArray()) {
                if (!note.isObject())
                    return fail(ErrorCode::Corrupt, "Malformed GitLab note");
                if (auto added = append(note.toObject(), id); !added)
                    return std::unexpected(added.error());
            }
        }
    }
    return result;
}
Result<void> prepareHostedReviewMutation(const HostingRequest& request, HostingCommand& command) {
    if (request.operation != HostingOperation::InlineComment && request.operation != HostingOperation::Reply)
        return fail(ErrorCode::InvalidArgument, "Not an anchored-comment operation");
    if (request.body.size() > 65536 || request.body.find('\0') != request.body.npos)
        return fail(ErrorCode::InvalidArgument, "Invalid comment body");
    if (request.body.empty())
        return fail(ErrorCode::InvalidArgument, "Enter a comment body");
    const bool github = request.provider == HostingProvider::GitHub;
    const auto project =
        github ? "repos/" + request.repository
               : "projects/" + QUrl::toPercentEncoding(QString::fromStdString(request.repository)).toStdString();
    const auto review = project + (github ? "/pulls/" : "/merge_requests/") + std::to_string(request.number);
    std::string endpoint;
    QJsonObject body;
    body.insert("body", QString::fromStdString(request.body));
    if (request.operation == HostingOperation::InlineComment) {
        if (!request.diff || !sameContext(request, *request.diff) || request.head != request.diff->identity.head ||
            request.fileIndex >= request.diff->files.size())
            return fail(ErrorCode::InvalidArgument, "Load the selected review's actual file diff before commenting");
        if (!sha(request.diff->identity.head) || !sha(request.diff->identity.base) ||
            !sha(request.diff->identity.start))
            return fail(ErrorCode::InvalidArgument, "Incomplete reviewed diff identity");
        const auto& file = request.diff->files[request.fileIndex];
        if (request.lineIndex >= file.lines.size() || !file.unavailable.empty())
            return fail(ErrorCode::InvalidArgument, "Select a complete provider diff line");
        const auto parsed = patchLines(file.patch);
        if (!path(file.oldPath) || !path(file.newPath) || !parsed || *parsed != file.lines)
            return fail(ErrorCode::InvalidArgument, "Inline rows differ from the reviewed provider patch");
        const auto& row = file.lines[request.lineIndex];
        const auto selected = request.right ? row.newLine : row.oldLine;
        if (!selected)
            return fail(ErrorCode::InvalidArgument, "This provider diff line has no selected-side anchor");
        if (github) {
            endpoint = review + "/comments";
            body.insert("commit_id", QString::fromStdString(request.diff->identity.head));
            body.insert("path", QString::fromStdString(file.newPath));
            body.insert("line", static_cast<qint64>(*selected));
            body.insert("side", request.right ? "RIGHT" : "LEFT");
        } else {
            endpoint = review + "/discussions";
            QJsonObject position{{"position_type", "text"},
                                 {"base_sha", QString::fromStdString(request.diff->identity.base)},
                                 {"start_sha", QString::fromStdString(request.diff->identity.start)},
                                 {"head_sha", QString::fromStdString(request.diff->identity.head)},
                                 {"old_path", QString::fromStdString(file.oldPath)},
                                 {"new_path", QString::fromStdString(file.newPath)}};
            if (row.oldLine)
                position.insert("old_line", static_cast<qint64>(*row.oldLine));
            if (row.newLine)
                position.insert("new_line", static_cast<qint64>(*row.newLine));
            body.insert("position", position);
        }
    } else {
        if (!request.comments || !sameContext(request, *request.comments) ||
            request.commentIndex >= request.comments->comments.size())
            return fail(ErrorCode::InvalidArgument,
                        "Refresh existing provider comments and select a thread before replying");
        const auto& comment = request.comments->comments[request.commentIndex];
        if (github)
            endpoint = review + "/comments/" + std::to_string(comment.replyParent ? comment.replyParent : comment.id) +
                       "/replies";
        else {
            if (comment.discussion.empty())
                return fail(ErrorCode::InvalidArgument, "Selected GitLab note has no discussion id");
            endpoint = review + "/discussions/" +
                       QUrl::toPercentEncoding(QString::fromStdString(comment.discussion)).toStdString() + "/notes";
        }
    }
    command.process.arguments = {"api", endpoint, "--hostname", request.host, "--method", "POST", "--input", "-"};
    command.process.stdinMode = io::Stdio::Pipe;
    command.input = QJsonDocument(body).toJson(QJsonDocument::Compact).toStdString();
    return {};
}
Result<void> validateHostedCommentMutation(const HostingRequest& request, std::string_view bytes) {
    HostingCommand command;
    if (auto valid = prepareHostedReviewMutation(request, command); !valid)
        return valid;
    auto parsed = document(bytes, false);
    if (!parsed)
        return std::unexpected(parsed.error());
    const auto row = parsed->object();
    const auto sameBody = [&](std::string actual) {
        std::string expected = request.body;
        const auto normalize = [](std::string& text) {
            std::size_t at = 0;
            while ((at = text.find("\r\n", at)) != text.npos)
                text.erase(at, 1);
        };
        normalize(actual);
        normalize(expected);
        return actual == expected;
    };
    if (request.provider == HostingProvider::GitHub) {
        if (row.value("id").toInteger(-1) <= 0 || !row.value("body").isString() || !sameBody(value(row, "body")))
            return fail(ErrorCode::Corrupt, "Provider did not confirm a comment identity");
        if (request.operation == HostingOperation::Reply) {
            const auto& source = request.comments->comments[request.commentIndex];
            const auto expected = source.replyParent ? source.replyParent : source.id;
            if (row.value("in_reply_to_id").toInteger(-1) != static_cast<qint64>(expected))
                return fail(ErrorCode::Corrupt, "Provider reply belongs to another thread");
        } else {
            const auto& file = request.diff->files[request.fileIndex];
            const auto& source = file.lines[request.lineIndex];
            const auto expected = request.right ? source.newLine : source.oldLine;
            auto actual = line(row, "original_line");
            if (!actual)
                return std::unexpected(actual.error());
            if (!*actual)
                actual = line(row, "line");
            if (!actual || *actual != expected || value(row, "path") != file.newPath ||
                (value(row, "original_commit_id").empty()
                     ? value(row, "commit_id")
                     : value(row, "original_commit_id")) != request.diff->identity.head ||
                value(row, "side") != (request.right ? "RIGHT" : "LEFT"))
                return fail(ErrorCode::Corrupt, "Provider did not confirm the reviewed line/commit anchor");
        }
    } else if (request.operation == HostingOperation::InlineComment) {
        const auto notes = row.value("notes");
        if (value(row, "id").empty() || !notes.isArray() || notes.toArray().size() != 1)
            return fail(ErrorCode::Corrupt, "Provider did not confirm a single anchored discussion");
        auto comments = parseHostedComments(request, "[" + std::string(bytes) + "]");
        if (!comments || comments->comments.size() != 1)
            return fail(ErrorCode::Corrupt, "Provider returned an invalid anchored note");
        const auto& actual = comments->comments.front();
        const auto& file = request.diff->files[request.fileIndex];
        const auto& source = file.lines[request.lineIndex];
        const auto position = notes.toArray().first().toObject().value("position").toObject();
        if (!sameBody(actual.body) || actual.path != file.newPath || actual.head != request.diff->identity.head ||
            actual.oldLine != source.oldLine || actual.newLine != source.newLine ||
            value(position, "base_sha") != request.diff->identity.base ||
            value(position, "start_sha") != request.diff->identity.start || value(position, "old_path") != file.oldPath)
            return fail(ErrorCode::Corrupt, "Provider discussion anchor differs from the reviewed snapshot");
    } else {
        if (row.value("id").toInteger(-1) <= 0 || !row.value("body").isString() || !sameBody(value(row, "body")))
            return fail(ErrorCode::Corrupt, "Provider did not confirm a reply identity");
        if (row.contains("noteable_iid") && row.value("noteable_iid").toInteger(-1) != request.number)
            return fail(ErrorCode::Corrupt, "Provider reply names another merge request");
    }
    return {};
}
Result<HostedDiffSnapshot> loadHostedDiff(io::IProcessLauncher& launcher, const HostingRequest& request,
                                          const std::function<bool()>& admitted, std::stop_token stop) {
    auto details = request;
    details.operation = HostingOperation::Details;
    auto before = runHosting(launcher, details, admitted, stop);
    if (!before)
        return std::unexpected(before.error());
    auto rows = parseHostedReviews(request.provider, "[" + *before + "]");
    if (!rows || rows->size() != 1 || rows->front().number != request.number)
        return fail(ErrorCode::Corrupt, "Provider returned another review identity");
    auto identity = parseHostedDiffIdentity(request.provider, *before);
    if (!identity)
        return std::unexpected(identity.error());
    if (request.head != identity->head)
        return fail(ErrorCode::Busy, "Review head changed; reload the provider page before reviewing lines");
    auto files = request;
    files.operation = HostingOperation::Files;
    auto bytes = runHosting(launcher, files, admitted, stop);
    if (!bytes)
        return std::unexpected(bytes.error());
    auto after = runHosting(launcher, details, admitted, stop);
    if (!after)
        return std::unexpected(after.error());
    auto afterRows = parseHostedReviews(request.provider, "[" + *after + "]");
    if (!afterRows || afterRows->size() != 1 || afterRows->front().number != request.number)
        return fail(ErrorCode::Corrupt, "Provider returned another review identity");
    auto current = parseHostedDiffIdentity(request.provider, *after);
    if (!current)
        return std::unexpected(current.error());
    if (*current != *identity)
        return fail(ErrorCode::Busy, "Review head/base changed while loading provider patches; refresh the file page");
    return parseHostedDiffFiles(request, *identity, *bytes);
}
Result<void> verifyHostedDiff(io::IProcessLauncher& launcher, const HostingRequest& request,
                              const std::function<bool()>& admitted, std::stop_token stop) {
    if (!request.diff)
        return fail(ErrorCode::InvalidArgument, "No reviewed diff evidence");
    auto details = request;
    details.operation = HostingOperation::Details;
    auto bytes = runHosting(launcher, details, admitted, stop);
    if (!bytes)
        return std::unexpected(bytes.error());
    auto rows = parseHostedReviews(request.provider, "[" + *bytes + "]");
    if (!rows || rows->size() != 1 || rows->front().number != request.number)
        return fail(ErrorCode::Corrupt, "Provider returned another review identity");
    auto current = parseHostedDiffIdentity(request.provider, *bytes);
    if (!current)
        return std::unexpected(current.error());
    if (*current != request.diff->identity)
        return fail(ErrorCode::Busy,
                    "Review head/base changed; no line comment sent. Refresh and review the new diff before retrying.");
    return {};
}
} // namespace frost::app
