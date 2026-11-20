// SPDX-License-Identifier: Apache-2.0
// Single-request worker. Source is read from stdin and never opened or executed.
#include "snapshot_validator_protocol.hpp"

#include <array>
#include <cstdio>
#include <exception>
#include <string>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace {
int report(const char* message) {
    std::fprintf(stderr, "frost-snapshot-validator: %s\n", message);
    return 1;
}
} // namespace

int main(int argc, char**) {
    if (argc != 1)
        return report("Expected one versioned JSON request on stdin");
#ifdef _WIN32
    if (_setmode(_fileno(stdin), _O_BINARY) == -1 || _setmode(_fileno(stdout), _O_BINARY) == -1)
        return report("Cannot open binary protocol streams");
#endif
    try {
        std::string request;
        std::array<char, 8192> chunk;
        while (const auto size = std::fread(chunk.data(), 1, chunk.size(), stdin)) {
            if (request.size() + size > frost::lang::kSnapshotValidationMaxInputBytes)
                return report("Snapshot validation request exceeds the protocol budget");
            request.append(chunk.data(), size);
        }
        if (std::ferror(stdin))
            return report("Could not read the snapshot validation request");
        const auto response = frost::lang::validateSnapshotRequest(request);
        if (!response)
            return report(response.error().describe().c_str());
        if (std::fwrite(response->data(), 1, response->size(), stdout) != response->size() || std::fflush(stdout) != 0)
            return report("Could not write the snapshot validation response");
        return 0;
    } catch (const std::exception&) {
        return report("Snapshot validation failed");
    }
}
