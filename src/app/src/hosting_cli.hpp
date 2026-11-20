// SPDX-License-Identifier: Apache-2.0
// GitHub/GitLab CLI command contracts; credentials remain owned by the installed provider CLI.
#pragma once
#include "frost/io/process.hpp"
#include <functional>
#include <string>
#include <vector>
namespace frost::app {
enum class HostingProvider { GitHub, GitLab };
enum class HostingOperation { Account, List, Details, Diff, Checkout, Comment, Approve, RequestChanges };
struct HostingRequest {
    HostingProvider provider = HostingProvider::GitHub;
    HostingOperation operation = HostingOperation::List;
    std::string executable, root, host, repository;
    std::uint32_t number = 0;
    std::uint32_t page = 1;
    std::string head, body;
};
struct HostingCommand { io::ProcessSpec process; std::string input; };
struct HostedReview {
    std::uint32_t number = 0;
    std::string title, state, author, source, target, head, url;
};
// Pure command preparation; accepts only explicit host/repository and absolute executable/root.
// Review bodies travel on stdin as JSON, never shell syntax or temporary files.
[[nodiscard]] Result<HostingCommand> hostingCommand(const HostingRequest& request);
// Parses a bounded genuine provider list; malformed rows fail the complete result, never synthesize defaults.
[[nodiscard]] Result<std::vector<HostedReview>> parseHostedReviews(HostingProvider provider, std::string_view bytes);
// Executes only through the supplied launcher; checks live project/trust immediately before launch.
// Caller runs off the GUI thread and owns launcher and admission callback until completion.
[[nodiscard]] Result<std::string> runHosting(io::IProcessLauncher& launcher, const HostingRequest& request,
    const std::function<bool()>& admitted, std::stop_token stop = {});
}
