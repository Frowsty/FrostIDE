// SPDX-License-Identifier: Apache-2.0
//
// MSBuild discovery through vswhere.exe (see frost/build/msbuild_locator.hpp).

#include "frost/build/msbuild_locator.hpp"

#include "frost/io/path.hpp"

#include "json.hpp"

#include <format>
#include <utility>

#ifdef _WIN32
#include "frost/io/utf.hpp"

#include <windows.h>

#include <shlobj.h>
#endif

namespace frost::build {

namespace {

// The 64-bit MSBuild of VS 2019 and later, under an installation path.
constexpr std::string_view kMsBuildRelative = "MSBuild\\Current\\Bin\\amd64\\MSBuild.exe";

#ifdef _WIN32
// The vswhere location the Visual Studio Installer guarantees, under Program Files (x86).
constexpr std::string_view kVswhereRelative = "Microsoft Visual Studio\\Installer\\vswhere.exe";

// The last `limit` bytes of a tool's error output, without trailing line breaks.
std::string trimmedTail(std::string_view text, std::size_t limit) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) {
        text.remove_suffix(1);
    }
    if (text.size() > limit) {
        text = text.substr(text.size() - limit);
    }
    return std::string(text);
}
#endif

} // namespace

std::vector<std::string> vswhereArguments() {
    return {"-sort",   "-prerelease", "-products", "*", "-requires", "Microsoft.Component.MSBuild",
            "-format", "json",        "-utf8"};
}

Result<std::vector<VsInstance>> parseVswhereJson(std::string_view json) {
    auto document = detail::parseJson(json);
    if (!document) {
        return std::unexpected(std::move(document.error()).withContext("vswhere output"));
    }
    if (document->kind != detail::JsonValue::Kind::Array) {
        return fail(ErrorCode::Corrupt, "vswhere output: expected a JSON array of instances");
    }
    std::vector<VsInstance> instances;
    for (const detail::JsonValue& item : document->items) {
        if (item.kind != detail::JsonValue::Kind::Object) {
            return fail(ErrorCode::Corrupt, "vswhere output: an instance is not a JSON object");
        }
        VsInstance instance;
        instance.installationPath = item.stringMember("installationPath");
        if (instance.installationPath.empty()) {
            continue;
        }
        instance.instanceId = item.stringMember("instanceId");
        instance.displayName = item.stringMember("displayName");
        instance.productId = item.stringMember("productId");
        instance.installationVersion = item.stringMember("installationVersion");
        instance.isPrerelease = item.boolMember("isPrerelease");
        if (const detail::JsonValue* catalog = item.find("catalog")) {
            instance.productDisplayVersion = catalog->stringMember("productDisplayVersion");
        }
        instances.push_back(std::move(instance));
    }
    return instances;
}

Result<std::string> msbuildPathFor(std::string_view installationPath) {
    auto base = io::normalizePath(installationPath, io::PathStyle::Windows);
    if (!base) {
        return std::unexpected(std::move(base.error()).withContext("Visual Studio installation path"));
    }
    return io::joinPath(*base, kMsBuildRelative, io::PathStyle::Windows);
}

Result<std::string> defaultVswherePath() {
#ifdef _WIN32
    // SHGetFolderPathW fills a caller buffer; SHGetKnownFolderPath's result would need
    // CoTaskMemFree, an ole32.dll import that frost_app.dll may not have
    // (FrostProcess.AppDllImportsOnlyAllowedDlls). Both read the same known folder.
    wchar_t folder[MAX_PATH] = {};
    const HRESULT hr = SHGetFolderPathW(nullptr, CSIDL_PROGRAM_FILESX86, nullptr, SHGFP_TYPE_CURRENT, folder);
    if (FAILED(hr) || folder[0] == L'\0') {
        return fail(ErrorCode::NotFound, std::format("the Program Files (x86) folder is unknown (HRESULT 0x{:08X})",
                                                     static_cast<unsigned long>(hr)));
    }
    auto utf8 = io::fromWide(folder);
    if (!utf8) {
        return std::unexpected(std::move(utf8.error()).withContext("Program Files (x86) folder"));
    }
    auto normalized = io::normalizePath(*utf8);
    if (!normalized) {
        return std::unexpected(std::move(normalized.error()).withContext("Program Files (x86) folder"));
    }
    return io::joinPath(*normalized, kVswhereRelative);
#else
    return fail(ErrorCode::Unsupported, "vswhere.exe exists only on Windows");
#endif
}

Result<std::vector<VsInstance>> listVsInstances(io::IProcessLauncher& launcher, io::IFileSystem& fs,
                                                const MsBuildDiscoveryOptions& options) {
#ifdef _WIN32
    std::string vswhere = options.vswherePath;
    if (vswhere.empty()) {
        auto found = defaultVswherePath();
        if (!found) {
            return std::unexpected(std::move(found.error()));
        }
        vswhere = std::move(*found);
    }
    auto present = fs.exists(vswhere);
    if (!present) {
        return std::unexpected(std::move(present.error()).withContext("looking for vswhere.exe"));
    }
    if (!*present) {
        return fail(
            ErrorCode::NotFound,
            std::format("no Visual Studio is installed: the Visual Studio Installer's '{}' does not exist", vswhere));
    }

    io::ProcessSpec spec;
    spec.executable = vswhere;
    spec.arguments = vswhereArguments();
    auto ran = io::runToCompletion(launcher, spec, {}, options.timeout);
    if (!ran) {
        return std::unexpected(std::move(ran.error()).withContext("running vswhere.exe"));
    }
    if (ran->exitCode != 0) {
        return fail(ErrorCode::IoError, std::format("vswhere.exe exited with code {}: {}", ran->exitCode,
                                                    trimmedTail(ran->stderrBytes, 500)));
    }
    auto instances = parseVswhereJson(ran->stdoutBytes);
    if (!instances) {
        return std::unexpected(std::move(instances.error()));
    }
    for (VsInstance& instance : *instances) {
        auto msbuild = msbuildPathFor(instance.installationPath);
        if (!msbuild) {
            continue; // Not an absolute path: nothing to run there.
        }
        if (auto exists = fs.exists(*msbuild); exists && *exists) {
            instance.msbuildPath = std::move(*msbuild);
        }
    }
    return instances;
#else
    static_cast<void>(launcher);
    static_cast<void>(fs);
    static_cast<void>(options);
    return fail(ErrorCode::Unsupported, "MSBuild discovery needs Windows (vswhere.exe)");
#endif
}

Result<VsInstance> selectVsInstance(std::span<const VsInstance> instances, std::string_view instanceId) {
    if (instances.empty()) {
        return fail(ErrorCode::NotFound, "no Visual Studio or Build Tools instance with MSBuild is installed");
    }
    for (const VsInstance& instance : instances) {
        if (!instanceId.empty() && instance.instanceId != instanceId) {
            continue;
        }
        if (!instance.msbuildPath.empty()) {
            return instance;
        }
        if (!instanceId.empty()) {
            return fail(ErrorCode::NotFound, std::format("Visual Studio instance '{}' ({}) has no MSBuild.exe",
                                                         instanceId, instance.installationPath));
        }
    }
    if (!instanceId.empty()) {
        return fail(ErrorCode::NotFound, std::format("no Visual Studio instance '{}' is installed", instanceId));
    }
    return fail(ErrorCode::NotFound, "no installed Visual Studio instance has an MSBuild.exe");
}

Result<VsInstance> findMsBuild(io::IProcessLauncher& launcher, io::IFileSystem& fs,
                               const MsBuildDiscoveryOptions& options) {
    auto instances = listVsInstances(launcher, fs, options);
    if (!instances) {
        return std::unexpected(std::move(instances.error()));
    }
    return selectVsInstance(*instances, options.instanceId);
}

} // namespace frost::build
