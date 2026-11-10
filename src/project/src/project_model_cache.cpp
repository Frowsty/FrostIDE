// SPDX-License-Identifier: Apache-2.0
//
// ProjectModelCache (frost/project/project_model_cache.hpp): the on-disk entries, their file
// stamps and glob listings, and the MSBuild identity and environment fingerprints (SLN-4).

#include "frost/project/project_model_cache.hpp"

#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/io/utf.hpp"

#include "json.hpp"
#include "paths.hpp"
#include "text_util.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <map>

#ifdef _WIN32
#include <windows.h>
#else
extern char** environ; // NOLINT(readability-redundant-declaration): POSIX declares it nowhere else.
#endif

namespace frost::project {
namespace {

using detail::JsonNode;

// The entry format; another value reads as a miss.
constexpr std::int64_t kFormat = 1;

// FNV-1a, 64 bits (public domain, Fowler/Noll/Vo): stable across builds and platforms, unlike std::hash.
std::uint64_t fnv1a(std::string_view text, std::uint64_t hash = 0xcbf29ce484222325ULL) {
    for (const char c : text) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

void appendString(std::string& out, std::string_view text) {
    out += '"';
    for (const char c : text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                out += std::format("\\u{:04x}", static_cast<unsigned>(static_cast<unsigned char>(c)));
            } else {
                out += c;
            }
        }
    }
    out += '"';
}

void appendStrings(std::string& out, const std::vector<std::string>& values) {
    out += '[';
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i != 0) {
            out += ',';
        }
        appendString(out, values[i]);
    }
    out += ']';
}

std::vector<std::string> readStrings(const JsonNode* node) {
    std::vector<std::string> out;
    if (node != nullptr && node->isArray()) {
        for (const JsonNode& item : node->items) {
            out.push_back(item.text);
        }
    }
    return out;
}

// "<size>|<mtime ticks>" of a file, or "missing".
Result<std::string> stampOf(io::IFileSystem& fs, std::string_view path) {
    auto status = fs.status(path);
    if (!status) {
        if (status.error().code() == ErrorCode::NotFound) {
            return std::string("missing");
        }
        return std::unexpected(std::move(status.error()));
    }
    return std::format("{}|{}", status->size, status->mtime.time_since_epoch().count());
}

// Where a glob reads: its fixed directory and whether the walk is recursive.
struct GlobScope {
    std::string directory;
    bool recursive = false;
};

std::optional<GlobScope> scopeOf(std::string_view projectDirectory, std::string_view glob) {
    if (glob.empty()) {
        return std::nullopt;
    }
    // Wildcards are not path characters, so only the fixed part is normalised (below).
    const std::string resolved =
        io::isAbsolutePath(glob) ? std::string(glob) : std::string(projectDirectory) + "/" + std::string(glob);
    // Split at either separator; the components before the first wildcard form the fixed part.
    std::vector<std::string> components;
    std::string current;
    for (const char c : resolved) {
        if (c == '\\' || c == '/') {
            components.push_back(std::move(current));
            current.clear();
        } else {
            current += c;
        }
    }
    components.push_back(std::move(current));
    std::size_t firstWild = components.size();
    for (std::size_t i = 0; i < components.size(); ++i) {
        if (detail::hasWildcard(components[i])) {
            firstWild = i;
            break;
        }
    }
    GlobScope scope;
    // Without a wildcard the "glob" names one file: its directory's names decide whether it exists.
    const std::size_t fixedCount = firstWild == components.size() ? components.size() - 1 : firstWild;
    scope.recursive = firstWild + 1 < components.size(); // A wildcard in a directory part, or "**".
    std::string directory;
    for (std::size_t i = 0; i < fixedCount; ++i) {
        if (i != 0) {
            directory += '/';
        }
        directory += components[i];
    }
    if (directory.empty() || (directory.size() == 2 && directory[1] == ':')) {
        directory += '/';
    }
    auto normalized = detail::normalizeAbsolute(directory);
    if (!normalized) {
        return std::nullopt;
    }
    scope.directory = std::move(*normalized);
    return scope;
}

// A hash of the names (and kinds) below `scope`, sorted, or "missing" for an absent directory.
Result<std::string> listingOf(io::IFileSystem& fs, const GlobScope& scope) {
    std::vector<std::string> names;
    std::vector<std::pair<std::string, std::string>> pending{{scope.directory, std::string()}};
    bool first = true;
    while (!pending.empty()) {
        auto [directory, relative] = std::move(pending.back());
        pending.pop_back();
        auto entries = fs.listDirectory(directory);
        if (!entries) {
            if (first && entries.error().code() == ErrorCode::NotFound) {
                return std::string("missing");
            }
            if (entries.error().code() == ErrorCode::NotFound) {
                continue; // Removed while walking: the names differ anyway.
            }
            return std::unexpected(std::move(entries.error()));
        }
        first = false;
        for (const io::DirEntry& entry : *entries) {
            const std::string name = relative.empty() ? entry.name : relative + "/" + entry.name;
            const bool isDirectory = entry.type == io::FileType::Directory;
            names.push_back(std::format("{}{}", isDirectory ? "d:" : "f:", detail::pathKey(name)));
            if (isDirectory && scope.recursive) {
                if (auto child = io::joinPath(directory, entry.name)) {
                    pending.emplace_back(std::move(*child), name);
                }
            }
        }
    }
    std::ranges::sort(names);
    std::uint64_t hash = fnv1a(scope.recursive ? "recursive" : "flat");
    for (const std::string& name : names) {
        hash = fnv1a(name, fnv1a("\n", hash));
    }
    return std::format("{}:{:016x}", names.size(), hash);
}

std::string keyText(const ProjectModelCacheKey& key) {
    return std::format("{}\n{}\n{}\n{}", detail::pathKey(key.solutionPath), detail::pathKey(key.projectPath),
                       key.configuration, key.platform);
}

std::string inputsText(const EvaluationInputs& inputs) {
    std::string out = inputs.msbuildIdentity + "\n" + inputs.environment;
    for (const auto& [name, value] : inputs.globalProperties) {
        out += "\n" + name + "=" + value;
    }
    return out;
}

void appendItems(std::string& out, const EvaluatedProjectItems& items) {
    out += "{\"projectPath\":";
    appendString(out, items.projectPath);
    out += ",\"configuration\":";
    appendString(out, items.configuration);
    out += ",\"platform\":";
    appendString(out, items.platform);
    out += ",\"msbuildVersion\":";
    appendString(out, items.msbuildVersion);
    out += ",\"items\":[";
    for (std::size_t i = 0; i < items.items.size(); ++i) {
        const ProjectItem& item = items.items[i];
        out += i == 0 ? "[" : ",[";
        appendString(out, item.itemName);
        out += ',';
        appendString(out, item.include);
        out += ',';
        appendString(out, item.path);
        out += item.fromWildcard ? ",true]" : ",false]";
    }
    out += "],\"imports\":";
    appendStrings(out, items.imports);
    out += ",\"warnings\":";
    appendStrings(out, items.warnings);
    out += '}';
}

std::optional<EvaluatedProjectItems> readItems(const JsonNode* node) {
    if (node == nullptr || !node->isObject()) {
        return std::nullopt;
    }
    EvaluatedProjectItems out{.projectPath = node->stringAt("projectPath"),
                              .configuration = node->stringAt("configuration"),
                              .platform = node->stringAt("platform"),
                              .source = EvaluationSource::ProjectHost,
                              .msbuildVersion = node->stringAt("msbuildVersion"),
                              .items = {},
                              .imports = readStrings(node->find("imports")),
                              .warnings = readStrings(node->find("warnings"))};
    const JsonNode* items = node->find("items");
    if (items == nullptr || !items->isArray()) {
        return std::nullopt;
    }
    for (const JsonNode& item : items->items) {
        if (!item.isArray() || item.items.size() != 4) {
            return std::nullopt;
        }
        const std::optional<ItemType> type = fileItemTypeOf(item.items[0].text);
        if (!type) {
            return std::nullopt;
        }
        out.items.push_back(ProjectItem{.type = *type,
                                        .itemName = item.items[0].text,
                                        .include = item.items[1].text,
                                        .path = item.items[2].text,
                                        .fromWildcard = item.items[3].boolean});
    }
    return out;
}

void appendCommandLines(std::string& out, const ProjectClCommandLines& lines) {
    out += "{\"projectPath\":";
    appendString(out, lines.projectPath);
    out += ",\"configuration\":";
    appendString(out, lines.configuration);
    out += ",\"platform\":";
    appendString(out, lines.platform);
    out += ",\"includePath\":";
    appendStrings(out, lines.includePath);
    out += ",\"externalIncludePath\":";
    appendStrings(out, lines.externalIncludePath);
    out += ",\"commandLines\":[";
    for (std::size_t i = 0; i < lines.commandLines.size(); ++i) {
        const ClCommandLine& line = lines.commandLines[i];
        out += i == 0 ? "{\"commandLine\":" : ",{\"commandLine\":";
        appendString(out, line.commandLine);
        out += ",\"workingDirectory\":";
        appendString(out, line.workingDirectory);
        out += line.configurationOptions ? ",\"configurationOptions\":true" : ",\"configurationOptions\":false";
        out += ",\"files\":";
        appendStrings(out, line.files);
        out += '}';
    }
    out += "]}";
}

std::optional<ProjectClCommandLines> readCommandLines(const JsonNode* node) {
    if (node == nullptr || !node->isObject()) {
        return std::nullopt;
    }
    ProjectClCommandLines out{.projectPath = node->stringAt("projectPath"),
                              .configuration = node->stringAt("configuration"),
                              .platform = node->stringAt("platform"),
                              .commandLines = {},
                              .includePath = readStrings(node->find("includePath")),
                              .externalIncludePath = readStrings(node->find("externalIncludePath"))};
    const JsonNode* lines = node->find("commandLines");
    if (lines == nullptr || !lines->isArray()) {
        return std::nullopt;
    }
    for (const JsonNode& line : lines->items) {
        out.commandLines.push_back(ClCommandLine{.commandLine = line.stringAt("commandLine"),
                                                 .files = readStrings(line.find("files")),
                                                 .workingDirectory = line.stringAt("workingDirectory"),
                                                 .configurationOptions = line.boolAt("configurationOptions")});
    }
    return out;
}

} // namespace

Result<std::string> msbuildIdentity(io::IFileSystem& fs, std::string_view msbuildPath) {
    if (!io::isAbsolutePath(msbuildPath)) {
        return fail(ErrorCode::InvalidArgument, std::format("{} is not an absolute MSBuild path", msbuildPath));
    }
    auto exe = stampOf(fs, msbuildPath);
    if (!exe) {
        return std::unexpected(std::move(exe.error()));
    }
    auto assemblyPath = io::joinPath(io::parentPath(msbuildPath), "Microsoft.Build.dll");
    if (!assemblyPath) {
        return std::unexpected(std::move(assemblyPath.error()));
    }
    auto assembly = stampOf(fs, *assemblyPath);
    if (!assembly) {
        return std::unexpected(std::move(assembly.error()));
    }
    return std::format("{}|{}|{}", detail::pathKey(msbuildPath), *exe, *assembly);
}

std::string environmentFingerprint(std::span<const std::pair<std::string, std::optional<std::string>>> extra) {
    // Name (case folded on Windows, where names are case-insensitive) -> value.
    const auto fold = [](std::string name) {
#ifdef _WIN32
        std::ranges::transform(name, name.begin(),
                               [](char c) { return static_cast<char>(std::toupper(static_cast<unsigned char>(c))); });
#endif
        return name;
    };
    std::map<std::string, std::string> variables;
    const auto add = [&](std::string_view entry) {
        // Windows lists per-drive directories as "=C:=C:\dir"; the name then starts after the first '='.
        const std::size_t equals = entry.find('=', 1);
        if (equals == std::string_view::npos) {
            return;
        }
        variables[fold(std::string(entry.substr(0, equals)))] = std::string(entry.substr(equals + 1));
    };
#ifdef _WIN32
    if (wchar_t* block = GetEnvironmentStringsW(); block != nullptr) {
        for (const wchar_t* entry = block; *entry != L'\0'; entry += std::wcslen(entry) + 1) {
            if (auto utf8 = io::fromWide(std::wstring_view(entry))) {
                add(*utf8);
            }
        }
        FreeEnvironmentStringsW(block);
    }
#else
    for (char** entry = environ; entry != nullptr && *entry != nullptr; ++entry) {
        add(*entry);
    }
#endif
    for (const auto& [name, value] : extra) {
        if (value) {
            variables[fold(name)] = *value;
        } else {
            variables.erase(fold(name));
        }
    }
    std::uint64_t hash = fnv1a("environment");
    for (const auto& [name, value] : variables) {
        hash = fnv1a(value, fnv1a("=", fnv1a(name, fnv1a("\n", hash))));
    }
    return std::format("{}:{:016x}", variables.size(), hash);
}

ProjectModelCache::ProjectModelCache(io::IFileSystem& fs, std::string directory)
    : m_fs(fs), m_directory(std::move(directory)) {}

std::string ProjectModelCache::entryPath(const ProjectModelCacheKey& key) const {
    return io::joinPath(m_directory, std::format("{:016x}.json", fnv1a(keyText(key)))).value_or(std::string());
}

std::optional<CachedProjectModel> ProjectModelCache::lookup(const ProjectModelCacheKey& key,
                                                            const EvaluationInputs& inputs, std::string* whyNot) {
    const auto miss = [whyNot](std::string why) -> std::optional<CachedProjectModel> {
        if (whyNot != nullptr) {
            *whyNot = std::move(why);
        }
        return std::nullopt;
    };
    const std::string path = entryPath(key);
    if (path.empty()) {
        return miss("the cache directory is not usable");
    }
    auto text = m_fs.readFile(path);
    if (!text) {
        return miss(text.error().code() == ErrorCode::NotFound
                        ? std::string("no entry")
                        : std::format("unreadable ({})", text.error().message()));
    }
    auto json = detail::parseJson(*text);
    if (!json || !json->isObject() || json->integerAt("format").value_or(0) != kFormat) {
        return miss("the entry is damaged or of another format");
    }
    if (json->stringAt("key") != keyText(key)) {
        return miss("the entry belongs to another project configuration");
    }
    if (json->stringAt("inputs") != inputsText(inputs)) {
        return miss("MSBuild, the environment or the global properties changed");
    }
    const JsonNode* files = json->find("files");
    const JsonNode* globs = json->find("globs");
    if (files == nullptr || !files->isArray() || globs == nullptr || !globs->isArray()) {
        return miss("the entry is damaged");
    }
    for (const JsonNode& file : files->items) {
        const std::string filePath = file.stringAt("path");
        auto stamp = stampOf(m_fs, filePath);
        if (!stamp || *stamp != file.stringAt("stamp")) {
            return miss(std::format("{} changed", io::fileName(filePath)));
        }
    }
    for (const JsonNode& glob : globs->items) {
        const GlobScope scope{.directory = glob.stringAt("directory"), .recursive = glob.boolAt("recursive")};
        auto listing = listingOf(m_fs, scope);
        if (!listing || *listing != glob.stringAt("listing")) {
            return miss(std::format("the files below {} changed", scope.directory));
        }
    }
    auto items = readItems(json->find("items"));
    if (!items) {
        return miss("the entry is damaged");
    }
    CachedProjectModel out{.items = std::move(*items), .clCommandLines = std::nullopt};
    if (const JsonNode* lines = json->find("clCommandLines"); lines != nullptr && !lines->isNull()) {
        out.clCommandLines = readCommandLines(lines);
        if (!out.clCommandLines) {
            return miss("the entry is damaged");
        }
    }
    return out;
}

Result<void> ProjectModelCache::store(const ProjectModelCacheKey& key, const EvaluationInputs& inputs,
                                      const CachedProjectModel& model, std::span<const std::string> globs) {
    if (!io::isAbsolutePath(m_directory) || !io::isAbsolutePath(key.projectPath)) {
        return fail(ErrorCode::InvalidArgument, "the cache directory and the project need absolute paths");
    }
    std::string out = std::format("{{\"format\":{},\"key\":", kFormat);
    appendString(out, keyText(key));
    out += ",\"inputs\":";
    appendString(out, inputsText(inputs));
    out += ",\"files\":[";
    std::vector<std::string> files{key.projectPath};
    files.insert(files.end(), model.items.imports.begin(), model.items.imports.end());
    for (std::size_t i = 0; i < files.size(); ++i) {
        auto stamp = stampOf(m_fs, files[i]);
        if (!stamp) {
            return std::unexpected(std::move(stamp.error()));
        }
        out += i == 0 ? "{\"path\":" : ",{\"path\":";
        appendString(out, files[i]);
        out += ",\"stamp\":";
        appendString(out, *stamp);
        out += '}';
    }
    out += "],\"globs\":[";
    const std::string projectDirectory(io::parentPath(key.projectPath));
    bool firstGlob = true;
    for (const std::string& glob : globs) {
        const std::optional<GlobScope> scope = scopeOf(projectDirectory, glob);
        if (!scope) {
            return fail(ErrorCode::InvalidArgument, std::format("the glob {} names no directory", glob));
        }
        auto listing = listingOf(m_fs, *scope);
        if (!listing) {
            return std::unexpected(std::move(listing.error()));
        }
        out += firstGlob ? "{\"directory\":" : ",{\"directory\":";
        firstGlob = false;
        appendString(out, scope->directory);
        out += scope->recursive ? ",\"recursive\":true" : ",\"recursive\":false";
        out += ",\"listing\":";
        appendString(out, *listing);
        out += '}';
    }
    out += "],\"items\":";
    appendItems(out, model.items);
    out += ",\"clCommandLines\":";
    if (model.clCommandLines) {
        appendCommandLines(out, *model.clCommandLines);
    } else {
        out += "null";
    }
    out += "}\n";
    if (auto created = m_fs.createDirectories(m_directory); !created) {
        return std::unexpected(std::move(created.error()));
    }
    return m_fs.writeFileAtomic(entryPath(key), out);
}

Result<void> ProjectModelCache::remove(const ProjectModelCacheKey& key) {
    auto removed = m_fs.removeFile(entryPath(key));
    if (!removed && removed.error().code() != ErrorCode::NotFound) {
        return removed;
    }
    return {};
}

} // namespace frost::project
