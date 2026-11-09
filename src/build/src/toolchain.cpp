// SPDX-License-Identifier: Apache-2.0
//
// Toolchain discovery core (frost/build/toolchain.hpp): MSVC toolsets, platform toolsets and Windows
// SDKs from the disk and the registry, the inventory over vswhere's instances, and choice checking.

#include "frost/build/toolchain.hpp"

#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/io/utf.hpp"

#include <algorithm>
#include <format>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#endif

namespace frost::build {

namespace {

std::string_view trimmed(std::string_view text) noexcept {
    const std::size_t begin = text.find_first_not_of(" \t\r\n\xEF\xBB\xBF");
    if (begin == std::string_view::npos) {
        return {};
    }
    const std::size_t end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

// `base` joined with each component in turn (native separators); "" when a join fails.
std::string under(std::string_view base, std::initializer_list<std::string_view> components) {
    std::string out(base);
    for (const std::string_view component : components) {
        auto joined = io::joinPath(out, component);
        if (!joined) {
            return {};
        }
        out = std::move(*joined);
    }
    return out;
}

bool isType(io::IFileSystem& fs, std::string_view path, io::FileType type) {
    if (path.empty()) {
        return false;
    }
    auto status = fs.status(path);
    return status && status->type == type;
}

bool isFile(io::IFileSystem& fs, std::string_view path) { return isType(fs, path, io::FileType::Regular); }
bool isDirectory(io::IFileSystem& fs, std::string_view path) { return isType(fs, path, io::FileType::Directory); }

// The names of the sub-directories of `path` (none when it cannot be listed).
std::vector<std::string> subdirectories(io::IFileSystem& fs, std::string_view path) {
    std::vector<std::string> names;
    if (auto entries = fs.listDirectory(path)) {
        for (const io::DirEntry& entry : *entries) {
            if (entry.type == io::FileType::Directory) {
                names.push_back(entry.name);
            }
        }
    }
    return names;
}

std::vector<unsigned long long> versionParts(std::string_view version) {
    std::vector<unsigned long long> parts;
    unsigned long long current = 0;
    for (const char c : version) {
        if (c >= '0' && c <= '9') {
            current = current * 10 + static_cast<unsigned long long>(c - '0');
        } else if (c == '.') {
            parts.push_back(current);
            current = 0;
        }
    }
    parts.push_back(current);
    return parts;
}

// A toolset request matches a version exactly or as a prefix ending at a dot ("14.44").
bool versionMatches(std::string_view requested, std::string_view version) noexcept {
    return version == requested ||
           (version.size() > requested.size() && version.starts_with(requested) && version[requested.size()] == '.');
}

Result<std::string> absolute(std::string_view path, std::string_view what) {
    auto normalized = io::normalizePath(path);
    if (!normalized) {
        return fail(ErrorCode::InvalidArgument, std::format("{} '{}' is not an absolute path", what, path));
    }
    return normalized;
}

} // namespace

bool versionNewer(std::string_view left, std::string_view right) noexcept {
    return versionParts(left) > versionParts(right);
}

const VsToolchain* ToolchainInventory::firstWithCppTools() const noexcept {
    const auto it = std::ranges::find_if(visualStudio, [](const VsToolchain& entry) { return entry.hasCppTools(); });
    return it == visualStudio.end() ? nullptr : &*it;
}

const VsToolchain* ToolchainInventory::find(std::string_view instanceId) const noexcept {
    const auto it = std::ranges::find_if(
        visualStudio, [instanceId](const VsToolchain& entry) { return entry.instance.instanceId == instanceId; });
    return it == visualStudio.end() ? nullptr : &*it;
}

std::string defaultMsvcToolset(io::IFileSystem& fs, std::string_view installationPath) {
    auto base = io::normalizePath(installationPath);
    if (!base) {
        return {};
    }
    auto text = fs.readFile(under(*base, {"VC", "Auxiliary", "Build", "Microsoft.VCToolsVersion.default.txt"}));
    return text ? std::string(trimmed(*text)) : std::string();
}

Result<std::vector<MsvcToolset>> listMsvcToolsets(io::IFileSystem& fs, std::string_view installationPath) {
    auto base = absolute(installationPath, "the Visual Studio installation");
    if (!base) {
        return std::unexpected(std::move(base.error()));
    }
    const std::string root = under(*base, {"VC", "Tools", "MSVC"});
    if (!isDirectory(fs, root)) {
        return fail(ErrorCode::NotFound,
                    std::format("{} has no MSVC toolsets (the C++ build tools are not installed)", *base));
    }
    const std::string fallback = defaultMsvcToolset(fs, *base);
    std::vector<MsvcToolset> toolsets;
    for (std::string& version : subdirectories(fs, root)) {
        MsvcToolset toolset;
        toolset.directory = under(root, {version});
        toolset.compiler = under(toolset.directory, {"bin", "Hostx64", "x64", "cl.exe"});
        if (!isFile(fs, toolset.compiler)) {
            continue; // A partial or other-host-only toolset: nothing x64 can be built with it.
        }
        toolset.isDefault = version == fallback;
        toolset.version = std::move(version);
        toolsets.push_back(std::move(toolset));
    }
    std::ranges::sort(toolsets,
                      [](const MsvcToolset& a, const MsvcToolset& b) { return versionNewer(a.version, b.version); });
    return toolsets;
}

std::vector<std::string> listPlatformToolsets(io::IFileSystem& fs, std::string_view installationPath) {
    std::vector<std::string> names;
    auto base = io::normalizePath(installationPath);
    if (!base) {
        return names;
    }
    const std::string vc = under(*base, {"MSBuild", "Microsoft", "VC"});
    for (const std::string& msbuildVersion : subdirectories(fs, vc)) {
        const std::string toolsets = under(vc, {msbuildVersion, "Platforms", "x64", "PlatformToolsets"});
        for (std::string& name : subdirectories(fs, toolsets)) {
            names.push_back(std::move(name));
        }
    }
    std::ranges::sort(names);
    names.erase(std::ranges::unique(names).begin(), names.end());
    return names;
}

Result<std::vector<WindowsSdk>> listWindowsSdks(io::IFileSystem& fs, std::string_view kitsRoot) {
    auto root = absolute(kitsRoot, "the Windows Kits root");
    if (!root) {
        return std::unexpected(std::move(root.error()));
    }
    const std::string include = under(*root, {"Include"});
    if (!isDirectory(fs, include)) {
        return fail(ErrorCode::NotFound, std::format("no Windows SDK headers under {}", *root));
    }
    std::vector<WindowsSdk> sdks;
    for (std::string& version : subdirectories(fs, include)) {
        if (!version.starts_with("10.")) {
            continue; // wdf and other non-SDK folders.
        }
        WindowsSdk sdk;
        sdk.includeDirectory = under(include, {version});
        sdk.hasDesktopHeaders = isFile(fs, under(sdk.includeDirectory, {"um", "windows.h"}));
        sdk.hasX64Libraries = isFile(fs, under(*root, {"Lib", version, "um", "x64", "kernel32.lib"}));
        sdk.version = std::move(version);
        sdks.push_back(std::move(sdk));
    }
    std::ranges::sort(sdks,
                      [](const WindowsSdk& a, const WindowsSdk& b) { return versionNewer(a.version, b.version); });
    return sdks;
}

Result<std::string> windowsKitsRoot() {
#ifdef _WIN32
    constexpr const wchar_t* kKey = L"SOFTWARE\\Microsoft\\Windows Kits\\Installed Roots";
    const DWORD flags = RRF_RT_REG_SZ | RRF_SUBKEY_WOW6432KEY;
    DWORD bytes = 0;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, kKey, L"KitsRoot10", flags, nullptr, nullptr, &bytes) != ERROR_SUCCESS ||
        bytes == 0) {
        return fail(ErrorCode::NotFound, "no Windows 10/11 SDK is installed (the registry has no KitsRoot10)");
    }
    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    if (RegGetValueW(HKEY_LOCAL_MACHINE, kKey, L"KitsRoot10", flags, nullptr, value.data(), &bytes) != ERROR_SUCCESS) {
        return fail(ErrorCode::IoError, "the registry's KitsRoot10 could not be read");
    }
    value.resize(bytes / sizeof(wchar_t));
    while (!value.empty() && value.back() == L'\0') {
        value.pop_back();
    }
    auto utf8 = io::fromWide(value);
    if (!utf8) {
        return std::unexpected(std::move(utf8.error()));
    }
    return absolute(*utf8, "the registry's KitsRoot10");
#else
    return fail(ErrorCode::Unsupported, "Windows SDKs exist on Windows only");
#endif
}

VsToolchain describeInstance(io::IFileSystem& fs, const VsInstance& instance) {
    VsToolchain toolchain;
    toolchain.instance = instance;
    toolchain.platformToolsets = listPlatformToolsets(fs, instance.installationPath);
    auto toolsets = listMsvcToolsets(fs, instance.installationPath);
    if (!toolsets) {
        return toolchain;
    }
    toolchain.toolsets = std::move(*toolsets);
    toolchain.defaultToolset = defaultMsvcToolset(fs, instance.installationPath);
    if (auto base = io::normalizePath(instance.installationPath)) {
        if (const std::string vcvarsall = under(*base, {"VC", "Auxiliary", "Build", "vcvarsall.bat"});
            isFile(fs, vcvarsall)) {
            toolchain.vcvarsall = vcvarsall;
        }
    }
    return toolchain;
}

ToolchainInventory discoverToolchains(io::IProcessLauncher& launcher, io::IFileSystem& fs,
                                      const ToolchainDiscoveryOptions& options) {
    ToolchainInventory inventory;
    Result<std::vector<VsInstance>> instances = options.instances ? Result<std::vector<VsInstance>>(*options.instances)
                                                                  : listVsInstances(launcher, fs, options.vswhere);
    if (!instances) {
        inventory.visualStudioError = std::move(instances.error());
    } else {
        for (const VsInstance& instance : *instances) {
            inventory.visualStudio.push_back(describeInstance(fs, instance));
        }
        if (inventory.visualStudio.empty()) {
            inventory.visualStudioError = Error(ErrorCode::NotFound, "vswhere lists no Visual Studio instance");
        } else if (inventory.firstWithCppTools() == nullptr) {
            inventory.visualStudioError = Error(
                ErrorCode::NotFound, "no Visual Studio instance has the C++ build tools (the \"Desktop development "
                                     "with C++\" workload)");
        }
    }
    Result<std::string> root =
        !options.windowsKitsRoot.empty() ? Result<std::string>(options.windowsKitsRoot) : windowsKitsRoot();
    if (!root) {
        inventory.windowsSdkError = std::move(root.error());
        return inventory;
    }
    inventory.windowsKitsRoot = *root;
    auto sdks = listWindowsSdks(fs, *root);
    if (!sdks) {
        inventory.windowsSdkError = std::move(sdks.error());
    } else {
        inventory.windowsSdks = std::move(*sdks);
        if (std::ranges::none_of(inventory.windowsSdks, &WindowsSdk::usable)) {
            inventory.windowsSdkError = Error(
                ErrorCode::NotFound, std::format("no Windows SDK under {} has the desktop headers and x64 libraries",
                                                 inventory.windowsKitsRoot));
        }
    }
    return inventory;
}

Result<ResolvedToolchain> resolveToolchain(const ToolchainInventory& inventory, const ToolchainChoice& choice) {
    if (!isVcvarsArchitecture(choice.architecture)) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("'{}' is not an architecture vcvarsall.bat knows", choice.architecture));
    }
    for (const std::string_view text : {std::string_view(choice.toolsetVersion), std::string_view(choice.sdkVersion)}) {
        if (!text.empty() && !isVcvarsVersionText(text)) {
            return fail(ErrorCode::InvalidArgument, std::format("'{}' is not a version", text));
        }
    }
    const VsToolchain* toolchain = nullptr;
    if (choice.instanceId.empty()) {
        toolchain = inventory.firstWithCppTools();
        if (toolchain == nullptr) {
            if (inventory.visualStudioError) {
                return std::unexpected(*inventory.visualStudioError);
            }
            return fail(ErrorCode::NotFound, "no Visual Studio instance has the C++ build tools");
        }
    } else {
        toolchain = inventory.find(choice.instanceId);
        if (toolchain == nullptr) {
            return fail(ErrorCode::NotFound,
                        std::format("the Visual Studio instance '{}' is not installed", choice.instanceId));
        }
        if (!toolchain->hasCppTools()) {
            return fail(ErrorCode::NotFound, std::format("{} has no C++ build tools", toolchain->instance.displayName));
        }
    }
    ResolvedToolchain resolved;
    resolved.toolchain = *toolchain;
    resolved.architecture = choice.architecture;
    // The toolset: the chosen one (the newest that matches a prefix), else the default, else the newest.
    const auto& toolsets = toolchain->toolsets;
    const MsvcToolset* toolset = nullptr;
    if (!choice.toolsetVersion.empty()) {
        const auto it = std::ranges::find_if(
            toolsets, [&](const MsvcToolset& entry) { return versionMatches(choice.toolsetVersion, entry.version); });
        if (it == toolsets.end()) {
            std::string installed;
            for (const MsvcToolset& entry : toolsets) {
                installed += (installed.empty() ? "" : ", ") + entry.version;
            }
            return fail(ErrorCode::NotFound,
                        std::format("the MSVC toolset {} is not installed in {} (installed: {})", choice.toolsetVersion,
                                    toolchain->instance.displayName, installed));
        }
        toolset = &*it;
        resolved.selection.toolsetVersion = choice.toolsetVersion;
    } else {
        const auto it = std::ranges::find_if(toolsets, &MsvcToolset::isDefault);
        toolset = it != toolsets.end() ? &*it : &toolsets.front();
    }
    resolved.toolset = *toolset;
    if (!choice.sdkVersion.empty()) {
        const auto it = std::ranges::find_if(inventory.windowsSdks,
                                             [&](const WindowsSdk& sdk) { return sdk.version == choice.sdkVersion; });
        if (it == inventory.windowsSdks.end()) {
            return fail(ErrorCode::NotFound, std::format("the Windows SDK {} is not installed", choice.sdkVersion));
        }
        if (!it->usable()) {
            return fail(ErrorCode::NotFound,
                        std::format("the Windows SDK {} has no desktop headers or x64 libraries", choice.sdkVersion));
        }
        resolved.selection.sdkVersion = choice.sdkVersion;
    }
    return resolved;
}

std::vector<std::string> describeToolchains(const ToolchainInventory& inventory) {
    std::vector<std::string> lines;
    if (inventory.visualStudio.empty()) {
        lines.push_back(std::format("Visual Studio: unavailable ({})", inventory.visualStudioError
                                                                           ? inventory.visualStudioError->message()
                                                                           : std::string("not searched")));
    }
    for (const VsToolchain& entry : inventory.visualStudio) {
        std::string line = std::format("{} {} ({})", entry.instance.displayName, entry.instance.installationVersion,
                                       entry.instance.installationPath);
        if (!entry.hasCppTools()) {
            line += ": no C++ build tools";
        } else {
            std::string versions;
            for (const MsvcToolset& toolset : entry.toolsets) {
                versions += (versions.empty() ? "" : ", ") + toolset.version + (toolset.isDefault ? " (default)" : "");
            }
            line += ": MSVC " + versions;
        }
        if (!entry.platformToolsets.empty()) {
            std::string names;
            for (const std::string& name : entry.platformToolsets) {
                names += (names.empty() ? "" : ", ") + name;
            }
            line += "; platform toolsets " + names;
        }
        line +=
            entry.instance.msbuildPath.empty() ? "; MSBuild unavailable" : "; MSBuild " + entry.instance.msbuildPath;
        lines.push_back(std::move(line));
    }
    if (inventory.windowsSdks.empty()) {
        lines.push_back(std::format("Windows SDK: unavailable ({})", inventory.windowsSdkError
                                                                         ? inventory.windowsSdkError->message()
                                                                         : std::string("not searched")));
    }
    for (const WindowsSdk& sdk : inventory.windowsSdks) {
        lines.push_back(std::format("Windows SDK {}{}", sdk.version,
                                    sdk.usable()             ? ""
                                    : !sdk.hasDesktopHeaders ? " (no desktop headers)"
                                                             : " (no x64 libraries)"));
    }
    return lines;
}

} // namespace frost::build
