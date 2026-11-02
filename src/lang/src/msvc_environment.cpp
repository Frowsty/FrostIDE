// SPDX-License-Identifier: Apache-2.0
//
// MSVC and Windows SDK include directories (msvc_environment.hpp): vswhere, the toolset version
// file and the registry's Installed Roots.

#include "frost/lang/msvc_environment.hpp"

#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/io/process.hpp"
#include "frost/io/utf.hpp"
#include "frost/lang/json.hpp"

#include <algorithm>
#include <chrono>
#include <format>
#include <optional>
#include <string_view>

#ifdef _WIN32
#include <windows.h>

#include <shlobj.h>
#endif

namespace frost::lang {

#ifdef _WIN32
namespace {

[[nodiscard]] std::string_view trimmed(std::string_view text) noexcept {
    const std::size_t begin = text.find_first_not_of(" \t\r\n\xEF\xBB\xBF");
    if (begin == std::string_view::npos) {
        return {};
    }
    const std::size_t end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

[[nodiscard]] Result<std::string> programFilesX86() {
    // SHGetFolderPathW, not SHGetKnownFolderPath, whose result needs CoTaskMemFree (ole32.dll),
    // an import frost_app.dll may not have (FrostProcess.AppDllImportsOnlyAllowedDlls).
    wchar_t folder[MAX_PATH] = {};
    const HRESULT hr = SHGetFolderPathW(nullptr, CSIDL_PROGRAM_FILESX86, nullptr, SHGFP_TYPE_CURRENT, folder);
    if (FAILED(hr) || folder[0] == L'\0') {
        return fail(ErrorCode::NotFound, "the Program Files (x86) folder is unknown");
    }
    return io::fromWide(folder);
}

[[nodiscard]] std::optional<std::string> kitsRoot10() {
    constexpr const wchar_t* kKey = L"SOFTWARE\\Microsoft\\Windows Kits\\Installed Roots";
    DWORD bytes = 0;
    const DWORD flags = RRF_RT_REG_SZ | RRF_SUBKEY_WOW6432KEY;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, kKey, L"KitsRoot10", flags, nullptr, nullptr, &bytes) != ERROR_SUCCESS ||
        bytes == 0) {
        return std::nullopt;
    }
    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    if (RegGetValueW(HKEY_LOCAL_MACHINE, kKey, L"KitsRoot10", flags, nullptr, value.data(), &bytes) != ERROR_SUCCESS) {
        return std::nullopt;
    }
    value.resize(bytes / sizeof(wchar_t));
    while (!value.empty() && value.back() == L'\0') {
        value.pop_back();
    }
    auto utf8 = io::fromWide(value);
    if (!utf8) {
        return std::nullopt;
    }
    auto normalized = io::normalizePath(*utf8);
    return normalized ? std::optional<std::string>(std::move(*normalized)) : std::nullopt;
}

// "10.0.26100.0" as numbers for ordering; non-numeric parts count as 0.
[[nodiscard]] std::vector<unsigned> versionParts(std::string_view version) {
    std::vector<unsigned> parts;
    unsigned current = 0;
    for (char c : version) {
        if (c >= '0' && c <= '9') {
            current = current * 10 + static_cast<unsigned>(c - '0');
        } else if (c == '.') {
            parts.push_back(current);
            current = 0;
        }
    }
    parts.push_back(current);
    return parts;
}

[[nodiscard]] bool isDirectory(io::IFileSystem& fs, std::string_view path) {
    auto status = fs.status(path);
    return status && status->type == io::FileType::Directory;
}

[[nodiscard]] std::string join(std::string_view base, std::string_view name) {
    auto joined = io::joinPath(base, name);
    return joined ? std::move(*joined) : std::string(base);
}

} // namespace

Result<MsvcEnvironment> findMsvcEnvironment(io::IProcessLauncher& launcher, io::IFileSystem& fs) {
    auto programFiles = programFilesX86();
    if (!programFiles) {
        return std::unexpected(std::move(programFiles.error()));
    }
    const std::string vswhere = join(*programFiles, "Microsoft Visual Studio\\Installer\\vswhere.exe");
    if (auto exists = fs.exists(vswhere); !exists || !*exists) {
        return fail(ErrorCode::NotFound, std::format("vswhere.exe not found at {}", vswhere));
    }
    io::ProcessSpec spec;
    spec.executable = vswhere;
    spec.arguments = {"-latest", "-products", "*",    "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                      "-format", "json",      "-utf8"};
    auto run = io::runToCompletion(launcher, spec, {}, std::chrono::seconds(30));
    if (!run) {
        return std::unexpected(std::move(run.error()));
    }
    if (run->exitCode != 0) {
        return fail(ErrorCode::IoError, std::format("vswhere exited with {}", run->exitCode));
    }
    auto parsed = parseJson(run->stdoutBytes);
    if (!parsed || !parsed->isArray() || parsed->items().empty()) {
        return fail(ErrorCode::NotFound, "no Visual Studio installation with the C++ tools");
    }
    MsvcEnvironment out;
    out.installationPath = std::string(parsed->items().front().stringAt("installationPath"));
    if (out.installationPath.empty()) {
        return fail(ErrorCode::NotFound, "vswhere named no installation path");
    }
    auto versionFile =
        fs.readFile(join(out.installationPath, "VC\\Auxiliary\\Build\\Microsoft.VCToolsVersion.default.txt"));
    if (!versionFile) {
        return std::unexpected(std::move(versionFile.error().withContext("the MSVC toolset version")));
    }
    out.toolsetVersion = std::string(trimmed(*versionFile));
    const std::string toolset = join(out.installationPath, "VC\\Tools\\MSVC\\" + out.toolsetVersion);
    if (!isDirectory(fs, join(toolset, "include"))) {
        return fail(ErrorCode::NotFound,
                    std::format("the MSVC toolset {} has no include directory", out.toolsetVersion));
    }
    out.includeDirectories.push_back(join(toolset, "include"));
    if (isDirectory(fs, join(toolset, "ATLMFC\\include"))) {
        out.includeDirectories.push_back(join(toolset, "ATLMFC\\include"));
    }

    if (const std::optional<std::string> root = kitsRoot10()) {
        const std::string includeRoot = join(*root, "Include");
        if (auto entries = fs.listDirectory(includeRoot)) {
            std::vector<std::string> versions;
            for (const io::DirEntry& entry : *entries) {
                if (entry.type == io::FileType::Directory && entry.name.starts_with("10.")) {
                    versions.push_back(entry.name);
                }
            }
            std::ranges::sort(
                versions, [](const std::string& a, const std::string& b) { return versionParts(a) > versionParts(b); });
            for (const std::string& version : versions) {
                const std::string base = join(includeRoot, version);
                if (auto has = fs.exists(join(base, "um\\windows.h")); has && *has) {
                    out.sdkVersion = version;
                    for (std::string_view part : {"ucrt", "shared", "um", "winrt", "cppwinrt"}) {
                        if (isDirectory(fs, join(base, part))) {
                            out.includeDirectories.push_back(join(base, part));
                        }
                    }
                    break;
                }
            }
        }
    }
    return out;
}

#else

Result<MsvcEnvironment> findMsvcEnvironment(io::IProcessLauncher& /*launcher*/, io::IFileSystem& /*fs*/) {
    return fail(ErrorCode::Unsupported, "MSVC exists only on Windows");
}

#endif

} // namespace frost::lang
