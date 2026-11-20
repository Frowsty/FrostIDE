// SPDX-License-Identifier: Apache-2.0
// GitHub/GitLab CLI command contracts; credentials remain owned by the installed provider CLI.
#pragma once
#include "frost/io/process.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>
namespace frost::app {
enum class HostingProvider { GitHub, GitLab };
enum class HostingOperation {
    Account,
    List,
    Details,
    Diff,
    Checkout,
    Comment,
    Approve,
    RequestChanges,
    Files,
    Comments,
    InlineComment,
    Reply
};
struct HostedDiffSnapshot;
struct HostedCommentPage;
struct HostingRequest {
    HostingProvider provider = HostingProvider::GitHub;
    HostingOperation operation = HostingOperation::List;
    std::string executable, gitExecutable, root, host, repository;
    std::uint32_t number = 0;
    std::uint32_t page = 1;
    std::string head, body;
    std::shared_ptr<const HostedDiffSnapshot> diff;
    std::size_t fileIndex = 0, lineIndex = 0;
    bool right = true;
    std::shared_ptr<const HostedCommentPage> comments;
    std::size_t commentIndex = 0;
    // Optional owner phase cell: set immediately before attempting the POST process. Retained
    // independently of view generations so stale completion cannot forget a possible submission.
    std::shared_ptr<std::atomic_bool> dispatched;
};
struct HostingCommand {
    io::ProcessSpec process;
    std::string input;
};
struct HostedReview {
    std::uint32_t number = 0;
    std::string title, state, author, source, target, head, url;
};
struct HostedDiffLine {
    std::optional<std::uint32_t> oldLine, newLine;
    std::string text;
    friend bool operator==(const HostedDiffLine&, const HostedDiffLine&) = default;
};
struct HostedDiffFile {
    std::string oldPath, newPath, patch, unavailable;
    std::vector<HostedDiffLine> lines;
};
struct HostedDiffIdentity {
    std::string head, base, start;
    friend bool operator==(const HostedDiffIdentity&, const HostedDiffIdentity&) = default;
};
struct HostedDiffSnapshot {
    HostingProvider provider = HostingProvider::GitHub;
    std::string executable, root, host, repository;
    std::uint32_t number = 0, page = 1;
    HostedDiffIdentity identity;
    std::vector<HostedDiffFile> files;
};
struct HostedComment {
    std::uint64_t id = 0, replyParent = 0;
    std::string discussion, author, body, path, head, originalHead, url;
    std::optional<std::uint32_t> oldLine, newLine, originalLine;
    std::optional<bool> resolved;
};
struct HostedCommentPage {
    HostingProvider provider = HostingProvider::GitHub;
    std::string executable, root, host, repository;
    std::uint32_t number = 0, page = 1;
    std::vector<HostedComment> comments;
};
// Strict bounded provider data. Only actual supplied patch lines become anchors; binary/missing/
// malformed patches remain visible but unavailable for inline submission. No local-file inference.
[[nodiscard]] Result<HostedDiffIdentity> parseHostedDiffIdentity(HostingProvider provider, std::string_view bytes);
[[nodiscard]] Result<HostedDiffSnapshot> parseHostedDiffFiles(const HostingRequest& request,
                                                              HostedDiffIdentity identity, std::string_view bytes);
[[nodiscard]] Result<HostedCommentPage> parseHostedComments(const HostingRequest& request, std::string_view bytes);
// Worker-only actual metadata -> file page -> metadata; any head/base/start change discards it.
[[nodiscard]] Result<HostedDiffSnapshot> loadHostedDiff(io::IProcessLauncher& launcher, const HostingRequest& request,
                                                        const std::function<bool()>& admitted,
                                                        std::stop_token stop = {});
// Current provider identity must still equal the immutable reviewed snapshot before a line POST.
[[nodiscard]] Result<void> verifyHostedDiff(io::IProcessLauncher& launcher, const HostingRequest& request,
                                            const std::function<bool()>& admitted, std::stop_token stop = {});
// Pure internal command join; requires same-context immutable diff/comment evidence and exact row.
[[nodiscard]] Result<void> prepareHostedReviewMutation(const HostingRequest& request, HostingCommand& command);
// A successful CLI exit alone is insufficient to claim an anchored comment was created.
[[nodiscard]] Result<void> validateHostedCommentMutation(const HostingRequest& request, std::string_view bytes);
// Pure command preparation; accepts only explicit host/repository and absolute executable/root.
// Review bodies travel on stdin as JSON, never shell syntax or temporary files.
[[nodiscard]] Result<HostingCommand> hostingCommand(const HostingRequest& request);
// Parses a bounded genuine provider list; malformed rows fail the complete result, never synthesize defaults.
[[nodiscard]] Result<std::vector<HostedReview>> parseHostedReviews(HostingProvider provider, std::string_view bytes);
// Executes only through the supplied launcher; checks live project/trust immediately before launch.
// Caller runs off the GUI thread and owns launcher and admission callback until completion.
[[nodiscard]] Result<std::string> runHosting(io::IProcessLauncher& launcher, const HostingRequest& request,
                                             const std::function<bool()>& admitted, std::stop_token stop = {});
} // namespace frost::app
