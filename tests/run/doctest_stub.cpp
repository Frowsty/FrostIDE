// SPDX-License-Identifier: Apache-2.0
//
// run_doctest_stub: the stand-in doctest program of the doctest runner tests (RUN-8,
// tests/run/doctest_runner_test.cpp, fixtures/doctest_project). It reads doctest's own --out= and
// --list-test-cases options like doctest (the last occurrence wins) and writes, as its report, a
// copy of a captured real doctest 2.5.3 report named by its own --stub-* options:
//   --stub-list=<file>      copied to --out= for --list-test-cases
//   --stub-run=<file>       copied to --out= otherwise
//   --stub-console=<file>   printed to stdout (doctest's console reporter output)
//   --stub-exit=<n>         the exit code of a run (0 by default); a listing exits 0, or with it
//                           when there is no --stub-list
//   --stub-sleep-ms=<n>     sleeps first (the cancel test)
// It always prints "cwd=", "env=" (FROST_DOCTEST_STUB_ENV) and "args=" lines first, so the tests
// can see where and how the runner started it. The real doctest is FrostEngine's FrostTests
// (tests/app/corpus_doctest_test.cpp). Test code only; the standard library only.

#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <thread>

namespace {

// Missing optional values retain their zero default. Malformed explicit test inputs fail the
// helper instead of silently choosing a different exit code or cancellation delay.
int number(std::string_view text) {
    if (text.empty())
        return 0;
    int value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
        std::fputs("doctest stub: invalid numeric option\n", stderr);
        std::exit(65);
    }
    return value;
}

std::string valueOf(int argc, char** argv, std::string_view option) {
    std::string value;
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument = argv[i];
        if (argument.starts_with(option))
            value = std::string(argument.substr(option.size()));
    }
    return value;
}

bool has(int argc, char** argv, std::string_view flag) {
    for (int i = 1; i < argc; ++i)
        if (std::string_view(argv[i]) == flag)
            return true;
    return false;
}

std::string read(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// The variable's value, or "" when it is not set.
std::string environmentValue(const char* name) {
#ifdef _WIN32
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr)
        return {};
    std::string out(value);
    std::free(value);
    return out;
#else
    const char* value = std::getenv(name); // NOLINT(concurrency-mt-unsafe): one thread.
    return value ? value : "";
#endif
}

} // namespace

int main(int argc, char** argv) {
    std::cout << "cwd=" << std::filesystem::current_path().generic_string() << "\n";
    std::cout << "env=" << environmentValue("FROST_DOCTEST_STUB_ENV") << "\n";
    std::cout << "args=";
    for (int i = 1; i < argc; ++i)
        std::cout << (i > 1 ? " " : "") << argv[i];
    std::cout << "\n" << std::flush;

    if (const std::string sleep = valueOf(argc, argv, "--stub-sleep-ms="); !sleep.empty())
        std::this_thread::sleep_for(std::chrono::milliseconds(number(sleep)));
    const std::string out = valueOf(argc, argv, "--out=");
    const bool listing = has(argc, argv, "--list-test-cases");
    const std::string report = valueOf(argc, argv, listing ? "--stub-list=" : "--stub-run=");
    if (!out.empty() && !report.empty()) {
        std::ofstream file(out, std::ios::binary);
        file << read(report);
    }
    // doctest lists without running anything and exits 0; a program with nothing to list fails.
    if (listing)
        return report.empty() ? number(valueOf(argc, argv, "--stub-exit=")) : 0;
    if (const std::string console = valueOf(argc, argv, "--stub-console="); !console.empty())
        std::cout << read(console) << std::flush;
    const std::string code = valueOf(argc, argv, "--stub-exit=");
    return code.empty() ? 0 : number(code);
}
