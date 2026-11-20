// SPDX-License-Identifier: Apache-2.0
#include "dxc_profile.hpp"

#include "frost/build/diagnostic.hpp"
#include "frost/io/path.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <regex>
#include <thread>
namespace frost::app {
Result<io::ProcessSpec> dxcSpec(const DxcProfile& p) {
    const auto identifier = [](std::string_view s) {
        return !s.empty() && s.size() <= 128 &&
               ((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= 'a' && s[0] <= 'z') || s[0] == '_') &&
               std::ranges::all_of(s, [](char c) {
                   return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
               });
    };
    if (!io::isAbsolutePath(p.executable) || !io::isAbsolutePath(p.root) || !io::isAbsolutePath(p.source) ||
        p.executable.find_first_of("\r\n") != std::string::npos ||
        p.source.find_first_of("\r\n") != std::string::npos || p.source.find('\0') != std::string::npos ||
        p.executable.find('\0') != std::string::npos)
        return fail(ErrorCode::InvalidArgument,
                    "DXC needs explicit absolute installed compiler, project and saved source paths");
    if (!p.source.ends_with(".hlsl"))
        return fail(ErrorCode::Unsupported,
                    "Select a standalone .hlsl translation unit; ShaderLab generated variants are unsupported");
    static const std::regex target("(vs|ps|cs|gs|hs|ds|ms|as)_6_[0-9]");
    if (!identifier(p.entry) || !std::regex_match(p.target, target))
        return fail(ErrorCode::InvalidArgument,
                    "Use an HLSL entry identifier and explicit Shader Model 6 stage target");
    io::ProcessSpec spec;
    spec.executable = p.executable;
    spec.workingDirectory = p.root;
    spec.arguments = {
        "-E",    p.entry, "-T", p.target, "-HV", "2021", "-fdiagnostics-format=msvc", "-fdiagnostics-color=never",
        "-Fo",
#ifdef _WIN32
        "NUL",
#else
        "/dev/null",
#endif
        p.source};
    spec.stdinMode = io::Stdio::Null;
    spec.stdoutMode = io::Stdio::Pipe;
    spec.stderrMode = io::Stdio::MergeIntoStdout;
    spec.killOnClose = true;
    return spec;
}
Result<std::vector<build::BuildDiagnostic>> parseDxcDiagnostics(std::string_view output, std::string_view root) {
    if (output.size() > 1024 * 1024)
        return fail(ErrorCode::Corrupt, "DXC output exceeds 1 MiB");
    std::vector<build::BuildDiagnostic> rows;
    while (!output.empty()) {
        const auto end = output.find('\n');
        auto line = output.substr(0, end);
        if (auto diagnostic = build::parseDiagnosticLine(line)) {
            build::BuildDiagnostic row;
            row.diagnostic = std::move(*diagnostic);
            auto path = io::isAbsolutePath(row.diagnostic.file) ? io::normalizePath(row.diagnostic.file)
                                                                : io::joinPath(root, row.diagnostic.file);
            if (path && row.diagnostic.line > 0) {
                row.path = *path;
                row.diagnostic.file = *path;
                row.jumpable = true;
            }
            row.diagnostic.code = row.diagnostic.code.empty() ? "DXC" : row.diagnostic.code;
            rows.push_back(std::move(row));
            if (rows.size() > 4096)
                return fail(ErrorCode::Corrupt, "DXC diagnostic count exceeds 4096");
        }
        if (end == std::string_view::npos)
            break;
        output.remove_prefix(end + 1);
    }
    return rows;
}
Result<DxcReport> runDxc(io::IProcessLauncher& launcher, const DxcProfile& profile, std::stop_token stop) {
    auto spec = dxcSpec(profile);
    if (!spec)
        return std::unexpected(spec.error());
    if (stop.stop_requested())
        return fail(ErrorCode::Cancelled, "DXC cancelled before launch");
    auto child = launcher.launch(*spec);
    if (!child)
        return std::unexpected(child.error());
    std::stop_callback cancel(stop, [&] { (void)(*child)->terminate(); });
    std::mutex mutex;
    std::condition_variable_any wake;
    std::atomic_bool timedOut = false;
    std::jthread deadline([&](std::stop_token token) {
        std::unique_lock lock(mutex);
        wake.wait_for(lock, token, std::chrono::seconds(30), [] { return false; });
        if (!token.stop_requested()) {
            timedOut = true;
            (void)(*child)->terminate();
        }
    });
    std::array<char, 4096> bytes;
    DxcReport report;
    bool oversized = false;
    std::optional<Error> readError;
    for (;;) {
        auto count = (*child)->readStdout(bytes);
        if (!count) {
            readError = count.error();
            break;
        }
        if (!*count)
            break;
        if (report.output.size() + *count > 1024 * 1024) {
            oversized = true;
            (void)(*child)->terminate();
            break;
        }
        report.output.append(bytes.data(), *count);
    }
    if (readError)
        (void)(*child)->terminate();
    auto status = (*child)->wait(std::chrono::seconds(5));
    deadline.request_stop();
    wake.notify_all();
    deadline.join();
    if (stop.stop_requested())
        return fail(ErrorCode::Cancelled, "DXC cancelled");
    if (timedOut)
        return fail(ErrorCode::Timeout, "DXC exceeded its 30 second diagnostic deadline");
    if (oversized)
        return fail(ErrorCode::Corrupt, "DXC output exceeded 1 MiB; no partial diagnostics published");
    if (readError)
        return std::unexpected(*readError);
    if (!status)
        return std::unexpected(status.error());
    report.exitCode = status->exitCode;
    auto rows = parseDxcDiagnostics(report.output, profile.root);
    if (!rows)
        return std::unexpected(rows.error());
    report.diagnostics = std::move(*rows);
    return report;
}
} // namespace frost::app
