// SPDX-License-Identifier: Apache-2.0
//
// Test-only helper that runs a child process and captures its standard output.
// Integration tests use it to drive the real frost executable.
// TODO(M1): replace with frost::io's IProcessLauncher once it exists, so tests
// exercise the production launcher instead of a parallel implementation.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace frost::test {

struct ProcessResult {
    int exitCode = -1;      // The child's exit status; -1 if it could not be started.
    std::string output;     // Everything the child wrote to stdout and stderr, interleaved.
    std::string startError; // Why the process could not start, when exitCode == -1.
};

// Runs `program` with UTF-8 `args` (not including the program name), waits for
// it to exit and returns its status and output. Blocking; intended for tests.
ProcessResult runProcess(const std::filesystem::path& program, const std::vector<std::string>& args);

} // namespace frost::test
