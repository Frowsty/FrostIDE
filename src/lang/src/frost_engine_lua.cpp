// SPDX-License-Identifier: Apache-2.0
// FrostEngine's Lua host provider (frost_engine_lua.hpp, FEC-10a on LUA-13's lua_host.hpp): project
// detection (a .frostproj or .feproject file; isFrostEngineProjectRoot, shared with the FrostEngine
// tool window), the engine's generated scripting reference and the contribution built from them.
#include "frost/lang/frost_engine_lua.hpp"

#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"

#include <algorithm>
#include <format>
#include <set>

namespace frost::lang {
namespace {
bool regular(io::IFileSystem& fs, std::string_view path) {
    auto status = fs.status(path);
    return status && status->type == io::FileType::Regular;
}
} // namespace
bool isFrostEngineProjectFileName(std::string_view name) noexcept {
    // ASCII case is ignored on every platform, so a project copied between Windows and Linux is
    // recognised the same way; the stem must not be empty (".frostproj" alone is no project).
    const auto endsWithIgnoringCase = [name](std::string_view suffix) {
        if (name.size() <= suffix.size())
            return false;
        const std::string_view tail = name.substr(name.size() - suffix.size());
        return std::ranges::equal(tail, suffix, [](char a, char b) {
            const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
            return lower(a) == lower(b);
        });
    };
    return endsWithIgnoringCase(".frostproj") || endsWithIgnoringCase(".feproject");
}
bool isFrostEngineProjectRoot(io::IFileSystem& fs, std::string_view root) {
    if (root.empty())
        return false;
    auto entries = fs.listDirectory(root);
    if (!entries)
        return false;
    return std::ranges::any_of(*entries, [](const io::DirEntry& entry) {
        return entry.type == io::FileType::Regular && isFrostEngineProjectFileName(entry.name);
    });
}
bool isFrostEngineLuaWorkspace(io::IFileSystem& fs, std::string_view root) {
    return isFrostEngineProjectRoot(fs, root);
}
namespace {
// The identifier `text` starts with ("Wait" of "Wait(seconds: number)"), or "".
std::string_view leadingIdentifier(std::string_view text) noexcept {
    const auto start = [](char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'; };
    const auto rest = [&](char c) { return start(c) || (c >= '0' && c <= '9'); };
    if (text.empty() || !start(text.front()))
        return {};
    std::size_t length = 1;
    while (length < text.size() && rest(text[length]))
        ++length;
    return text.substr(0, length);
}
bool directory(io::IFileSystem& fs, std::string_view path) {
    auto status = fs.status(path);
    return status && status->type == io::FileType::Directory;
}
// Adds the generated definitions folder of the project at `project` (relative to `root`, "" for the
// root itself), '/'-separated, when it exists.
void addLibrary(io::IFileSystem& fs, std::string_view root, std::string_view project, std::vector<std::string>& out) {
    const std::string relative = project.empty() ? std::string(kFrostEngineLuaApiDirectory)
                                                 : std::format("{}/{}", project, kFrostEngineLuaApiDirectory);
    if (auto path = io::joinPath(root, relative); path && directory(fs, *path))
        out.push_back(relative);
}
} // namespace
Result<FrostEngineScriptApi> parseFrostEngineScriptApi(std::string_view markdown) {
    if (markdown.find(kFrostEngineScriptApiMarker) == std::string_view::npos)
        return fail(ErrorCode::InvalidArgument,
                    std::format("not the engine's generated scripting reference (no \"{}\" line)",
                                kFrostEngineScriptApiMarker));
    // The sections whose entries are Lua globals, by the reference's own introductions (header comment).
    enum class Section : std::uint8_t { Other, Globals, GlobalTables };
    Section section = Section::Other;
    std::set<std::string, std::less<>> globals;
    FrostEngineScriptApi api;
    std::string_view rest = markdown;
    while (!rest.empty()) {
        const std::size_t end = rest.find('\n');
        std::string_view line = rest.substr(0, end);
        rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end + 1);
        if (line.ends_with('\r'))
            line.remove_suffix(1);
        if (line.starts_with("## ")) {
            const std::string_view title = line.substr(3);
            section = title == "Globals" ? Section::Globals
                      : title == "Namespaces" || title == "Types" || title == "Enums" || title == "Lua standard library"
                          ? Section::GlobalTables
                          : Section::Other;
            continue;
        }
        if (section == Section::Other)
            continue;
        if (line.starts_with("### ")) {
            if (const std::string_view name = leadingIdentifier(line.substr(4)); !name.empty())
                globals.emplace(name);
            continue;
        }
        // The Globals table: | `name(...)` | Kind | Summary |.
        if (section == Section::Globals && line.starts_with("| `")) {
            const std::string_view name = leadingIdentifier(line.substr(3));
            if (name.empty())
                continue;
            globals.emplace(name);
            // | `_VERSION: string` | GlobalValue | The Lua version ("Lua 5.4"). |
            if (name == "_VERSION") {
                if (const std::size_t quote = line.find("\"Lua "); quote != std::string_view::npos) {
                    if (const std::size_t close = line.find('"', quote + 1); close != std::string_view::npos)
                        api.runtimeVersion = std::string(line.substr(quote + 1, close - quote - 1));
                }
            }
        }
    }
    if (globals.empty())
        return fail(ErrorCode::InvalidArgument, "the engine's scripting reference declares no global");
    api.globals.assign(globals.begin(), globals.end());
    return api;
}
FrostEngineLuaProfile discoverFrostEngineLua(io::IFileSystem& fs, std::string_view root) {
    FrostEngineLuaProfile profile;
    if (root.empty() || !io::isAbsolutePath(root))
        return profile;
    profile.projectRoot = isFrostEngineProjectRoot(fs, root);
    bool engineCheckout = false;
    std::string problem;
    std::string folder(root);
    for (std::size_t depth = 0; depth <= kMaxReferenceDepth; ++depth) {
        auto candidate = io::joinPath(folder, kFrostEngineScriptApiReference);
        if (candidate && regular(fs, *candidate)) {
            auto text = fs.readFile(*candidate);
            if (!text) {
                problem = std::format("{} could not be read: {}", *candidate, text.error().describe());
                break;
            }
            auto api = parseFrostEngineScriptApi(*text);
            if (!api) {
                problem = std::format("{}: {}", *candidate, api.error().message());
                break;
            }
            // A reference in an ancestor applies to a FrostEngine project only (a sample inside the
            // engine's repository); the root itself holding it makes it an engine checkout.
            engineCheckout = depth == 0;
            if (engineCheckout || profile.projectRoot) {
                profile.reference = std::move(*candidate);
                profile.api = std::move(*api);
            }
            break;
        }
        const std::string parent(io::parentPath(folder));
        if (parent.empty() || parent == folder)
            break;
        folder = parent;
    }
    profile.frostEngine = profile.projectRoot || engineCheckout;
    if (!profile.frostEngine)
        return profile;
    if (profile.reference.empty())
        profile.problem = problem.empty() ? std::format("no generated {} in {} or its {} parent folders",
                                                        kFrostEngineScriptApiReference, root, kMaxReferenceDepth)
                                          : std::move(problem);
    if (profile.projectRoot)
        addLibrary(fs, root, {}, profile.library);
    if (engineCheckout) {
        // The engine's projects (samples, test projects) up to two levels below the checkout.
        const auto subdirectories = [&fs](std::string_view parent) {
            std::vector<std::string> names;
            if (auto entries = fs.listDirectory(parent))
                for (const io::DirEntry& entry : *entries)
                    if (entry.type == io::FileType::Directory && !entry.name.starts_with('.'))
                        names.push_back(entry.name);
            std::ranges::sort(names);
            return names;
        };
        for (const std::string& child : subdirectories(root)) {
            auto childPath = io::joinPath(root, child);
            if (!childPath)
                continue;
            if (isFrostEngineProjectRoot(fs, *childPath)) {
                profile.projects.push_back(child);
                addLibrary(fs, root, child, profile.library);
                continue;
            }
            for (const std::string& grandchild : subdirectories(*childPath)) {
                const std::string relative = child + "/" + grandchild;
                if (auto path = io::joinPath(root, relative); path && isFrostEngineProjectRoot(fs, *path)) {
                    profile.projects.push_back(relative);
                    addLibrary(fs, root, relative, profile.library);
                }
            }
        }
    }
    return profile;
}
std::optional<LuaHostContribution> frostEngineLuaContribution(const FrostEngineLuaProfile& profile) {
    if (!profile.frostEngine)
        return std::nullopt;
    LuaHostContribution contribution;
    contribution.provider = std::string(FrostEngineLuaProvider::kName);
    contribution.runtimeVersion = profile.api.runtimeVersion;
    contribution.runtimePath = {std::string(kFrostEngineScriptPath)};
    // The engine's sandbox removes these libraries (ScriptEngine::InstallSandbox).
    contribution.builtins = {{"io", "disable"}, {"package", "disable"}, {"os", "disable"}, {"debug", "disable"}};
    contribution.library =
        profile.library.empty() ? std::vector<std::string>{std::string(kFrostEngineLuaApiDirectory)} : profile.library;
    contribution.globals = profile.api.globals;
    contribution.note = profile.problem.empty()
                            ? std::format("{} globals and {} from {}", profile.api.globals.size(),
                                          profile.api.runtimeVersion.empty() ? std::string("the user's Lua version")
                                                                             : profile.api.runtimeVersion,
                                          profile.reference)
                            : "no engine globals: " + profile.problem;
    return contribution;
}
Json frostEngineLuaDefaults(std::string runtimeVersion, const FrostEngineLuaProfile& profile) {
    const auto contribution = frostEngineLuaContribution(profile);
    if (!contribution)
        return composeLuaConfiguration(std::move(runtimeVersion), {});
    return composeLuaConfiguration(std::move(runtimeVersion), std::span<const LuaHostContribution>(&*contribution, 1));
}
std::optional<LuaHostContribution> FrostEngineLuaProvider::contribute(io::IFileSystem& fs, std::string_view folder) {
    // Reads the engine's generated scripting reference (about 1 MB) and a few directory listings.
    const FrostEngineLuaProfile profile = discoverFrostEngineLua(fs, folder);
    auto contribution = frostEngineLuaContribution(profile);
    if (!contribution)
        return contribution;
    // What a later change must re-read: the reference, and the project's generated definitions
    // folder, which the engine may write after the project was opened.
    if (!profile.reference.empty())
        contribution->inputs.push_back(profile.reference);
    if (profile.projectRoot) {
        if (auto api = io::joinPath(folder, kFrostEngineLuaApiDirectory))
            contribution->inputs.push_back(std::move(*api));
    }
    // An engine checkout: each FrostEngine project inside it becomes its own LuaLS folder, so its
    // .luarc.json, runtime.path and definitions apply to its scripts (LuaLS gives a file the first
    // folder that holds it, and folders come before the checkout's root).
    for (const std::string& project : profile.projects) {
        if (auto path = io::joinPath(folder, project)) {
            if (auto api = io::joinPath(*path, kFrostEngineLuaApiDirectory))
                contribution->inputs.push_back(std::move(*api));
            contribution->folders.push_back(std::move(*path));
        }
    }
    return contribution;
}
} // namespace frost::lang
