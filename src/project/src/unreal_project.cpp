// SPDX-License-Identifier: Apache-2.0
#include "frost/project/unreal_project.hpp"
#include "frost/io/path.hpp"
#include "frost/io/file_system.hpp"
#include "json.hpp"
#include <algorithm>
#include <set>
namespace frost::project {
namespace {
using detail::JsonNode;
bool uniqueKeys(const JsonNode& node) {
    std::set<std::string> keys;
    for (const auto& key : node.keys) if (!keys.insert(key).second) return false;
    return std::ranges::all_of(node.items, uniqueKeys);
}
bool identifier(std::string_view name) {
    if (name.empty() || name.size() > 256) return false;
    return std::ranges::all_of(name, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
    });
}
Result<JsonNode> descriptor(std::string_view text) {
    if (text.size() > 4 * 1024 * 1024) return fail(ErrorCode::InvalidArgument, "Unreal descriptor exceeds 4 MiB");
    auto node = detail::parseJson(text);
    if (!node) return std::unexpected(node.error());
    if (!node->isObject() || !uniqueKeys(*node)) return fail(ErrorCode::Corrupt, "duplicate or invalid Unreal descriptor object");
    const auto version = node->integerAt("FileVersion");
    if (!version || *version < 1) return fail(ErrorCode::Corrupt, "missing Unreal descriptor FileVersion");
    if (*version > 3) return fail(ErrorCode::Unsupported, "newer Unreal descriptor FileVersion");
    return node;
}
bool stringField(const JsonNode& node, std::string_view key) {
    const auto* value = node.find(key);
    return !value || (value->isString() && value->text.find('\0') == std::string::npos);
}
bool boolField(const JsonNode& node, std::string_view key) {
    const auto* value = node.find(key);
    return !value || value->isBool();
}
Result<std::vector<UnrealModule>> modules(const JsonNode& root) {
    std::vector<UnrealModule> result;
    const auto* rows = root.find("Modules");
    if (!rows) return result;
    if (!rows->isArray()) return fail(ErrorCode::Corrupt, "Unreal Modules must be an array");
    std::set<std::string> names;
    for (const auto& row : rows->items) {
        const auto name = row.stringAt("Name");
        const auto type = row.stringAt("Type");
        if (!row.isObject() || !identifier(name) || !identifier(type) || !stringField(row, "LoadingPhase") || !names.insert(name).second)
            return fail(ErrorCode::Corrupt, "invalid or duplicate Unreal module");
        result.push_back({name, type, row.stringAt("LoadingPhase")});
    }
    return result;
}
}
namespace {
Result<std::string> readDescriptor(io::IFileSystem& fs, std::string_view path) {
    auto reader = fs.openRandomAccess(path);
    if (!reader) return std::unexpected(reader.error());
    if ((*reader)->size() > 4 * 1024 * 1024) return fail(ErrorCode::InvalidArgument, "Unreal descriptor exceeds 4 MiB");
    std::string bytes;
    while (bytes.size() < (*reader)->size()) {
        auto chunk = (*reader)->read(bytes.size(), static_cast<std::size_t>(std::min<std::uint64_t>(
            io::IRandomAccessReader::kMaximumReadBytes, (*reader)->size() - bytes.size())));
        if (!chunk) return std::unexpected(chunk.error());
        if (chunk->empty()) return fail(ErrorCode::Busy, "Unreal descriptor changed during read");
        bytes += *chunk;
    }
    return bytes;
}
struct Inventory {
    io::IFileSystem& fs;
    UnrealWorkspace& result;
    std::string physicalRoot;
    std::size_t directories = 0, entries = 0;
    static std::string key(std::string value) {
        std::ranges::replace(value,'\\','/');
        if constexpr(io::kNativePathStyle==io::PathStyle::Windows)
            std::ranges::transform(value,value.begin(),[](unsigned char c){return c>='A'&&c<='Z'?static_cast<char>(c+'a'-'A'):static_cast<char>(c);});
        while(value.size()>1&&value.back()=='/')value.pop_back();return value;
    }
    Result<bool> admit(const std::string& path,io::FileType type) {
        auto parent=fs.listDirectory(io::parentPath(path));
        if(!parent){if(parent.error().code()==ErrorCode::NotFound)return false;return std::unexpected(parent.error());}
        const auto found=std::ranges::find(*parent,io::fileName(path),&io::DirEntry::name);
        if(found==parent->end())return false;
        if(found->type!=type)return fail(ErrorCode::PermissionDenied,"Unreal metadata input is a link or wrong entry type");
        auto physical=fs.physicalPath(path);if(!physical)return std::unexpected(physical.error());
        const auto actual=key(*physical),root=key(physicalRoot);
        if(actual!=root&&!actual.starts_with(root+"/"))return fail(ErrorCode::PermissionDenied,"Unreal metadata input escaped the physical project root");
        return true;
    }
    Result<std::vector<io::DirEntry>> list(const std::string& path, unsigned depth) {
        if (depth > 32 || ++directories > 1024) return fail(ErrorCode::OutOfRange, "Unreal metadata directory limit exceeded");
        const auto admitted=admit(path,io::FileType::Directory);
        if(!admitted)return std::unexpected(admitted.error());
        if(!*admitted)return std::vector<io::DirEntry>{};
        auto rows = fs.listDirectory(path);
        if (!rows && rows.error().code() == ErrorCode::NotFound) return std::vector<io::DirEntry>{};
        if (!rows) return std::unexpected(rows.error());
        entries += rows->size();
        if (entries > 50000) return fail(ErrorCode::OutOfRange, "Unreal metadata entry limit exceeded");
        std::ranges::sort(*rows, {}, &io::DirEntry::name);
        return rows;
    }
    Result<void> source(const std::string& path, const std::string& owner, unsigned depth) {
        auto rows = list(path, depth);
        if (!rows) return std::unexpected(rows.error());
        for (const auto& row : *rows) {
            auto child = io::joinPath(path, row.name);
            if (!child) return std::unexpected(child.error());
            if (row.type == io::FileType::Directory) {
                if (auto scanned = source(*child, owner, depth + 1); !scanned) return scanned;
            } else if (row.type == io::FileType::Regular) {
                const std::string_view name(row.name);
                const bool target = name.ends_with(".Target.cs"), module = name.ends_with(".Build.cs");
                if (!target && !module) continue;
                std::string stem(name.substr(0, name.size() - (target ? 10 : 9)));
                if (!identifier(stem)) return fail(ErrorCode::Corrupt, "invalid Unreal rule-file identity");
                auto& files = target ? result.targetRules : result.moduleRules;
                if (std::ranges::any_of(files, [&](const auto& file) { return file.name == stem && file.owner == owner; }))
                    return fail(ErrorCode::Corrupt, "ambiguous Unreal rule-file identity");
                files.push_back({std::move(stem), *child, owner});
            }
        }
        return {};
    }
    Result<void> plugins(const std::string& path, unsigned depth) {
        auto rows = list(path, depth);
        if (!rows) return std::unexpected(rows.error());
        const io::DirEntry* descriptorEntry = nullptr;
        for (const auto& row : *rows) if (row.type == io::FileType::Regular && std::string_view(row.name).ends_with(".uplugin")) {
            if (descriptorEntry) return fail(ErrorCode::Corrupt, "multiple Unreal plugin descriptors in one directory");
            descriptorEntry = &row;
        }
        if (descriptorEntry) {
            const auto file = io::joinPath(path, descriptorEntry->name);
            if (!file) return std::unexpected(file.error());
            auto admitted=admit(*file,io::FileType::Regular);
            if(!admitted)return std::unexpected(admitted.error());
            if(!*admitted)return fail(ErrorCode::Busy,"Unreal plugin descriptor disappeared");
            const auto bytes = readDescriptor(fs, *file);
            if (!bytes) return std::unexpected(bytes.error());
            auto descriptor = parseUnrealPluginDescriptor(*bytes);
            if (!descriptor) return std::unexpected(descriptor.error());
            const std::string name = descriptorEntry->name.substr(0, descriptorEntry->name.size() - 8);
            if (!identifier(name) || std::ranges::any_of(result.localPlugins, [&](const auto& plugin) { return plugin.name == name; }))
                return fail(ErrorCode::Corrupt, "ambiguous Unreal local plugin identity");
            result.localPlugins.push_back({name, *file, std::move(*descriptor)});
            auto sourcePath = io::joinPath(path, "Source");
            return sourcePath ? source(*sourcePath, "plugin:" + name, depth + 1) : Result<void>(std::unexpected(sourcePath.error()));
        }
        for (const auto& row : *rows) if (row.type == io::FileType::Directory) {
            auto child = io::joinPath(path, row.name);
            if (!child) return std::unexpected(child.error());
            if (auto scanned = plugins(*child, depth + 1); !scanned) return scanned;
        }
        return {};
    }
};
}
Result<UnrealProject> readUnrealProject(io::IFileSystem& fs, std::string_view absoluteProject) {
    auto bytes = readDescriptor(fs, absoluteProject);
    return bytes ? parseUnrealProject(*bytes, absoluteProject) : Result<UnrealProject>(std::unexpected(bytes.error()));
}
Result<UnrealWorkspace> readUnrealWorkspace(io::IFileSystem& fs, std::string_view absoluteProject) {
    const auto root=io::parentPath(absoluteProject);
    auto physicalRoot=fs.physicalPath(root);if(!physicalRoot)return std::unexpected(physicalRoot.error());
    UnrealWorkspace result;
    Inventory inventory{fs,result,*physicalRoot};
    const auto admitted=inventory.admit(std::string(absoluteProject),io::FileType::Regular);
    if(!admitted)return std::unexpected(admitted.error());
    if(!*admitted)return fail(ErrorCode::NotFound,"Unreal project descriptor is missing");
    auto project=readUnrealProject(fs,absoluteProject);if(!project)return std::unexpected(project.error());
    result.project=std::move(*project);
    auto source=io::joinPath(root,"Source"),plugins=io::joinPath(root,"Plugins");
    if(!source||!plugins)return fail(ErrorCode::InvalidArgument,"invalid Unreal project root");
    if(auto scanned=inventory.source(*source,"Project",0);!scanned)return std::unexpected(scanned.error());
    if(auto scanned=inventory.plugins(*plugins,0);!scanned)return std::unexpected(scanned.error());
    return result;
}
Result<UnrealProject> parseUnrealProject(std::string_view json, std::string_view absoluteProject) {
    if (!io::isAbsolutePath(absoluteProject) || !absoluteProject.ends_with(".uproject") || absoluteProject.find('\0') != std::string_view::npos)
        return fail(ErrorCode::InvalidArgument, "expected an absolute .uproject path");
    auto root = descriptor(json);
    if (!root) return std::unexpected(root.error());
    if (!stringField(*root, "EngineAssociation") || !stringField(*root, "Description")) return fail(ErrorCode::Corrupt, "invalid Unreal project string");
    auto moduleRows = modules(*root);
    if (!moduleRows) return std::unexpected(moduleRows.error());
    UnrealProject result{std::string(absoluteProject), root->stringAt("EngineAssociation"), root->stringAt("Description"), std::move(*moduleRows), {}};
    const auto* plugins = root->find("Plugins");
    if (plugins) {
        if (!plugins->isArray()) return fail(ErrorCode::Corrupt, "Unreal Plugins must be an array");
        std::set<std::string> names;
        for (const auto& row : plugins->items) {
            auto name = row.stringAt("Name");
            if (!row.isObject() || !identifier(name) || !row.find("Enabled") || !boolField(row, "Enabled") || !boolField(row, "Optional") || !names.insert(name).second)
                return fail(ErrorCode::Corrupt, "invalid or duplicate Unreal plugin reference");
            result.plugins.push_back({std::move(name), row.boolAt("Enabled"), row.boolAt("Optional")});
        }
    }
    return result;
}
Result<UnrealPluginDescriptor> parseUnrealPluginDescriptor(std::string_view json) {
    auto root = descriptor(json);
    if (!root) return std::unexpected(root.error());
    if (!stringField(*root, "FriendlyName")) return fail(ErrorCode::Corrupt, "invalid Unreal plugin FriendlyName");
    auto rows = modules(*root);
    if (!rows) return std::unexpected(rows.error());
    return UnrealPluginDescriptor{root->stringAt("FriendlyName"), std::move(*rows)};
}
Result<UnrealInstallation> resolveUnrealInstallation(const UnrealProject& project, const std::vector<UnrealInstallation>& installations) {
    if (project.engineAssociation.empty()) return fail(ErrorCode::NotFound, "select an Unreal installation explicitly");
    const UnrealInstallation* result = nullptr;
    for (const auto& installation : installations) {
        if (installation.association != project.engineAssociation) continue;
        if (!io::isAbsolutePath(installation.root) || installation.root.find('\0') != std::string::npos)
            return fail(ErrorCode::InvalidArgument, "invalid selected Unreal installation path");
        if (result) return fail(ErrorCode::InvalidArgument, "ambiguous Unreal engine association");
        result = &installation;
    }
    if (!result) return fail(ErrorCode::NotFound, "Unreal engine association is not installed in supplied metadata");
    return *result;
}
} // namespace frost::project
