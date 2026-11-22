// SPDX-License-Identifier: Apache-2.0
// Owned pre-exec policy helper. Never continue to the extension after any enforcement failure.
#include "process_sandbox_internal.hpp"

#include <charconv>
#include <cstdio>

#include <unistd.h>
extern char** environ; // NOLINT(readability-redundant-declaration): POSIX requires the declaration
int main(int argc, char** argv) {
    using namespace frost::io;
    auto refuse = [](const char* message) {
        std::fprintf(stderr, "FROST_SANDBOX_UNAVAILABLE: %s\n", message);
        return 125;
    };
    if (argc < 6 || std::string_view(argv[1]) != "--profile-v1")
        return refuse("invalid owned helper protocol");
    ProcessSandboxProfile profile;
    if (std::string_view(argv[2]) == "deny-network")
        profile.network = SandboxNetwork::Deny;
    else if (std::string_view(argv[2]) == "internet-client")
        profile.network = SandboxNetwork::InternetClient;
    else
        return refuse("unknown network policy");
    int target = 3;
    while (target < argc && std::string_view(argv[target]) != "--") {
        if (target + 2 >= argc || profile.paths.size() >= 256)
            return refuse("invalid grant bounds");
        unsigned rights = 0;
        const std::string_view encoded(argv[target]);
        const auto parsed = std::from_chars(encoded.data(), encoded.data() + encoded.size(), rights);
        if (parsed.ec != std::errc{} || parsed.ptr != encoded.data() + encoded.size() || rights == 0 || rights > 7)
            return refuse("unknown grant rights");
        const std::string_view scope(argv[target + 1]);
        if (scope != "tree" && scope != "exact")
            return refuse("unknown grant scope");
        profile.paths.push_back({argv[target + 2], static_cast<SandboxAccess>(rights), scope == "tree"});
        target += 3;
    }
    if (++target >= argc)
        return refuse("missing extension executable");
    ProcessSpec spec;
    spec.executable = argv[target];
    spec.cleanEnvironment = true;
    spec.sandbox = profile;
    if (auto valid = validateProcessSandbox(spec, false); !valid)
        return refuse(valid.error().message().c_str());
    if (auto enforced = frost::io::detail::enforceLinuxSandbox(profile); !enforced)
        return refuse(enforced.error().message().c_str());
    execve(argv[target], argv + target, environ);
    return refuse("sandboxed extension exec failed; content/runtime dependencies must be explicitly granted");
}
