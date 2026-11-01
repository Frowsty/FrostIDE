// SPDX-License-Identifier: Apache-2.0
//
// Corpus replay for a fuzz harness (cmake/FrostFuzz.cmake): in normal test trees, where libFuzzer
// is not linked, <name>_replay runs runInput() over every file of the committed corpus, so the
// corpus is a regression test in every -Quick (CTest fuzz_replay.<name>, label unit).
//
// Usage: <name>_replay <corpus directory>. Reports each failing file by name and continues; exits
// non-zero if any input failed, the directory is missing or it holds no files.

#include "fuzz/fuzz_harness.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>
#include <vector>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <corpus directory>\n", argc > 0 ? argv[0] : "replay");
        return 2;
    }
    frost::fuzz::initialize();
    const std::filesystem::path corpus(argv[1]);
    std::error_code error;
    std::vector<std::filesystem::path> files;
    for (std::filesystem::directory_iterator it(corpus, error), end; !error && it != end; it.increment(error)) {
        if (it->is_regular_file(error)) {
            files.push_back(it->path());
        }
    }
    if (error) {
        std::fprintf(stderr, "cannot read corpus directory %s: %s\n", argv[1], error.message().c_str());
        return 2;
    }
    if (files.empty()) {
        std::fprintf(stderr, "corpus directory %s holds no inputs\n", argv[1]);
        return 2;
    }
    std::sort(files.begin(), files.end()); // A stable order for reports.
    int failed = 0;
    for (const std::filesystem::path& file : files) {
        std::ifstream in(file, std::ios::binary);
        if (!in) {
            std::fprintf(stderr, "FAILED %s: cannot open the file\n", file.filename().string().c_str());
            ++failed;
            continue;
        }
        const std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (const frost::Result<void> result = frost::fuzz::runInput(data); !result) {
            std::fprintf(stderr, "FAILED %s: %s\n", file.filename().string().c_str(),
                         result.error().describe().c_str());
            ++failed;
        }
    }
    std::printf("%zu inputs replayed, %d failed\n", files.size(), failed);
    return failed == 0 ? 0 : 1;
}
