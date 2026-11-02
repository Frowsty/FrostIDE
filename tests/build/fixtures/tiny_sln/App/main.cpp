// SPDX-License-Identifier: Apache-2.0
// The tiny fixture's program; it calls into Lib, so the link needs Lib.lib. Without arguments it
// returns 0. The run tests (tests/build/run_msbuild_test.cpp) pass its .vcxproj.user arguments:
//   --print   prints "tiny answer 42", each argument, FROST_TINY_VALUE and the current directory,
//             and one line to stderr;
//   --echo    reads one line from stdin and prints "echo: <line>";
//   --exit N  exits with N instead.
#include "lib.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <direct.h>

int main(int argc, char** argv) {
    int code = tinyAnswer() == 42 ? 0 : 1;
    bool print = false;
    bool echo = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--print") == 0) {
            print = true;
        } else if (std::strcmp(argv[i], "--echo") == 0) {
            echo = true;
        } else if (std::strcmp(argv[i], "--exit") == 0 && i + 1 < argc) {
            code = std::atoi(argv[++i]);
        }
    }
    if (print) {
        std::printf("tiny answer %d\n", tinyAnswer());
        for (int i = 1; i < argc; ++i) {
            std::printf("arg %d: %s\n", i, argv[i]);
        }
        char* value = nullptr;
        std::size_t length = 0;
        if (_dupenv_s(&value, &length, "FROST_TINY_VALUE") == 0 && value != nullptr) {
            std::printf("FROST_TINY_VALUE=%s\n", value);
            std::free(value);
        } else {
            std::printf("FROST_TINY_VALUE is not set\n");
        }
        char directory[4096];
        std::printf("cwd: %s\n", _getcwd(directory, sizeof directory) != nullptr ? directory : "?");
        std::fprintf(stderr, "tiny stderr line\n");
        std::fflush(stdout);
        std::fflush(stderr);
    }
    if (echo) {
        char line[512];
        if (std::fgets(line, sizeof line, stdin) != nullptr) {
            line[std::strcspn(line, "\r\n")] = '\0';
            std::printf("echo: %s\n", line);
            std::fflush(stdout);
        }
    }
    return code;
}
