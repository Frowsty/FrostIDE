// SPDX-License-Identifier: Apache-2.0
//
// The .frost/ layout, its .gitignore, and the application settings file's location
// (project_layout.hpp).

#include "frost/settings/project_layout.hpp"

#include "frost/io/utf.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#else
#include <cstdlib>
#endif

namespace frost::settings {

namespace {

[[nodiscard]] Result<std::string> join(std::string_view base, std::string_view name, io::PathStyle style) {
    return io::joinPath(base, name, style);
}

// True when `text` has a line that is exactly "workspace.json" or "/workspace.json" (blanks and a
// trailing CR ignored).
[[nodiscard]] bool listsWorkspace(std::string_view text) {
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = std::min(text.find('\n', start), text.size());
        std::string_view line = text.substr(start, end - start);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
            line.remove_suffix(1);
        }
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
            line.remove_prefix(1);
        }
        if (line == "workspace.json" || line == "/workspace.json") {
            return true;
        }
        start = end + 1;
    }
    return false;
}

} // namespace

Result<ProjectLayout> ProjectLayout::forRoot(std::string_view root, io::PathStyle style) {
    auto normalized = io::normalizePath(root, style);
    if (!normalized) {
        return std::unexpected(normalized.error());
    }
    ProjectLayout layout;
    layout.root = std::move(*normalized);
    layout.style = style;
    auto frost = join(layout.root, ".frost", style);
    if (!frost) {
        return std::unexpected(frost.error());
    }
    layout.frostDirectory = std::move(*frost);
    const auto inFrost = [&](std::string_view name, std::string& out) -> Result<void> {
        auto path = join(layout.frostDirectory, name, style);
        if (!path) {
            return std::unexpected(path.error());
        }
        out = std::move(*path);
        return {};
    };
    for (auto [name, out] :
         {std::pair{"settings.json", &layout.sharedSettings}, std::pair{"workspace.json", &layout.personalSettings},
          std::pair{".gitignore", &layout.gitignore}, std::pair{"runConfigurations", &layout.runConfigurations},
          std::pair{"inspectionProfiles", &layout.inspectionProfiles}}) {
        if (auto ok = inFrost(name, *out); !ok) {
            return std::unexpected(ok.error());
        }
    }
    return layout;
}

Result<void> ensureProjectLayout(io::IFileSystem& fs, const ProjectLayout& layout) {
    if (auto made = fs.createDirectories(layout.frostDirectory); !made) {
        return made;
    }
    auto existing = fs.readFile(layout.gitignore);
    if (!existing) {
        if (existing.error().code() != ErrorCode::NotFound) {
            return std::unexpected(existing.error());
        }
        return fs.writeFileAtomic(layout.gitignore, kFrostGitignoreText);
    }
    if (listsWorkspace(*existing)) {
        return {};
    }
    std::string text = std::move(*existing);
    const bool crlf = text.find("\r\n") != std::string::npos;
    if (!text.empty() && text.back() != '\n') {
        text += crlf ? "\r\n" : "\n";
    }
    text += crlf ? "workspace.json\r\n" : "workspace.json\n";
    return fs.writeFileAtomic(layout.gitignore, text);
}

Result<std::string> applicationSettingsFileFor(const EnvironmentLookup& environment, io::PathStyle style) {
    const auto absolute = [&](std::string_view name) -> std::optional<std::string> {
        auto value = environment(name);
        if (!value || value->empty() || !io::isAbsolutePath(*value, style)) {
            return std::nullopt;
        }
        auto normalized = io::normalizePath(*value, style);
        return normalized ? std::optional(std::move(*normalized)) : std::nullopt;
    };
    if (style == io::PathStyle::Windows) {
        if (auto appData = absolute("APPDATA")) {
            return join(*appData, "FrostIDE\\settings.json", style);
        }
        return fail(ErrorCode::NotFound, "APPDATA is not set to an absolute path");
    }
    if (auto xdg = absolute("XDG_CONFIG_HOME")) {
        return join(*xdg, "frostide/settings.json", style);
    }
    if (auto home = absolute("HOME")) {
        return join(*home, ".config/frostide/settings.json", style);
    }
    return fail(ErrorCode::NotFound, "neither XDG_CONFIG_HOME nor HOME is set to an absolute path");
}

Result<std::string> applicationSettingsFile() {
    const EnvironmentLookup environment = [](std::string_view name) -> std::optional<std::string> {
#ifdef _WIN32
        auto wideName = io::toWide(name);
        if (!wideName) {
            return std::nullopt;
        }
        std::wstring value(32'768, L'\0');
        const DWORD length = GetEnvironmentVariableW(wideName->c_str(), value.data(), static_cast<DWORD>(value.size()));
        if (length == 0 || length >= value.size()) {
            return std::nullopt;
        }
        value.resize(length);
        auto utf8 = io::fromWide(value);
        return utf8 ? std::optional(std::move(*utf8)) : std::nullopt;
#else
        // Read once at use; FrostIDE never calls setenv after start-up.
        const char* value = std::getenv(std::string(name).c_str()); // NOLINT(concurrency-mt-unsafe)
        return value != nullptr ? std::optional<std::string>(value) : std::nullopt;
#endif
    };
    return applicationSettingsFileFor(environment, io::kNativePathStyle);
}

} // namespace frost::settings
