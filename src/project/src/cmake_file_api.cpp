// SPDX-License-Identifier: Apache-2.0
//
// The CMake File API query writer and reply reader (frost/project/cmake_file_api.hpp).

#include "frost/project/cmake_file_api.hpp"

#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"

#include "json.hpp"

#include <algorithm>
#include <format>

namespace frost::project {

namespace {

using detail::JsonNode;

constexpr std::string_view kQuery = R"({
  "requests": [
    { "kind": "codemodel", "version": 2 },
    { "kind": "cache", "version": 2 },
    { "kind": "cmakeFiles", "version": 1 },
    { "kind": "toolchains", "version": 1 }
  ]
}
)";

constexpr char lowerAscii(char c) noexcept { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

bool equalsIgnoringCase(std::string_view a, std::string_view b) noexcept {
    return std::ranges::equal(a, b, [](char x, char y) { return lowerAscii(x) == lowerAscii(y); });
}

// A File API path ('/' separators, relative to `base` or absolute) as an absolute native path.
std::string resolvePath(std::string_view base, std::string_view path) {
    if (path.empty()) {
        return std::string(base);
    }
    std::string joined;
    if (io::isAbsolutePath(path)) {
        joined = std::string(path);
    } else {
        auto relative = io::joinPath(base, path);
        if (!relative) {
            return std::string(path);
        }
        joined = std::move(*relative);
    }
    auto normalized = io::normalizePath(joined);
    return normalized ? *normalized : joined;
}

CMakeTargetType targetType(std::string_view type) noexcept {
    if (type == "EXECUTABLE") {
        return CMakeTargetType::Executable;
    }
    if (type == "STATIC_LIBRARY") {
        return CMakeTargetType::StaticLibrary;
    }
    if (type == "SHARED_LIBRARY") {
        return CMakeTargetType::SharedLibrary;
    }
    if (type == "MODULE_LIBRARY") {
        return CMakeTargetType::ModuleLibrary;
    }
    if (type == "OBJECT_LIBRARY") {
        return CMakeTargetType::ObjectLibrary;
    }
    if (type == "INTERFACE_LIBRARY") {
        return CMakeTargetType::InterfaceLibrary;
    }
    return CMakeTargetType::Utility;
}

class ReplyReader {
public:
    ReplyReader(io::IFileSystem& fs, std::string replyDir) : m_fs(fs), m_replyDir(std::move(replyDir)) {}

    // Reads and parses reply object `file` (relative to the reply directory).
    Result<JsonNode> object(std::string_view file) {
        auto path = io::joinPath(m_replyDir, file);
        if (!path) {
            return std::unexpected(std::move(path.error()));
        }
        auto content = m_fs.readFile(*path);
        if (!content) {
            return std::unexpected(std::move(content.error()).withContext(std::format("while reading {}", *path)));
        }
        auto parsed = detail::parseJson(*content);
        if (!parsed) {
            return std::unexpected(std::move(parsed.error()).withContext(std::format("in {}", *path)));
        }
        return parsed;
    }

private:
    io::IFileSystem& m_fs;
    std::string m_replyDir;
};

// The jsonFile of each kind FrostIDE uses: from the client's responses, else from the index's objects.
struct ObjectFiles {
    std::string codemodel;
    std::string cache;
    std::string cmakeFiles;
    std::string toolchains;
};

void collectObjects(const JsonNode* list, ObjectFiles& files) {
    if (list == nullptr || !list->isArray()) {
        return;
    }
    for (const JsonNode& entry : list->items) {
        const std::string kind = entry.stringAt("kind");
        const std::string file = entry.stringAt("jsonFile");
        if (file.empty() || entry.find("error") != nullptr) {
            continue;
        }
        std::string* slot = kind == "codemodel"    ? &files.codemodel
                            : kind == "cache"      ? &files.cache
                            : kind == "cmakeFiles" ? &files.cmakeFiles
                            : kind == "toolchains" ? &files.toolchains
                                                   : nullptr;
        if (slot != nullptr && slot->empty()) {
            *slot = file;
        }
    }
}

void readTarget(const JsonNode& json, const std::string& sourceDir, const std::string& buildDir, CMakeTarget& target) {
    target.name = json.stringAt("name", target.name);
    target.id = json.stringAt("id", target.id);
    target.type = targetType(json.stringAt("type"));
    target.generatorProvided = json.boolAt("isGeneratorProvided");
    if (const JsonNode* folder = json.find("folder")) {
        target.folder = folder->stringAt("name");
    }
    if (const JsonNode* paths = json.find("paths")) {
        target.sourceDirectory = resolvePath(sourceDir, paths->stringAt("source", "."));
    }
    if (const JsonNode* artifacts = json.find("artifacts"); artifacts != nullptr && artifacts->isArray()) {
        for (const JsonNode& artifact : artifacts->items) {
            const std::string path = artifact.stringAt("path");
            if (!path.empty()) {
                target.artifacts.push_back(resolvePath(buildDir, path));
            }
        }
    }
    if (const JsonNode* groups = json.find("compileGroups"); groups != nullptr && groups->isArray()) {
        for (const JsonNode& group : groups->items) {
            CMakeCompileGroup compile;
            compile.language = group.stringAt("language");
            if (const JsonNode* includes = group.find("includes"); includes != nullptr && includes->isArray()) {
                for (const JsonNode& include : includes->items) {
                    std::string path = resolvePath(sourceDir, include.stringAt("path"));
                    if (include.boolAt("isSystem")) {
                        compile.systemIncludes.push_back(path);
                    }
                    compile.includes.push_back(std::move(path));
                }
            }
            if (const JsonNode* defines = group.find("defines"); defines != nullptr && defines->isArray()) {
                for (const JsonNode& define : defines->items) {
                    compile.defines.push_back(define.stringAt("define"));
                }
            }
            if (const JsonNode* fragments = group.find("compileCommandFragments");
                fragments != nullptr && fragments->isArray()) {
                for (const JsonNode& fragment : fragments->items) {
                    compile.flags.push_back(fragment.stringAt("fragment"));
                }
            }
            if (const JsonNode* standard = group.find("languageStandard")) {
                compile.languageStandard = standard->stringAt("standard");
            }
            target.compileGroups.push_back(std::move(compile));
        }
    }
    std::vector<std::string> groupNames;
    if (const JsonNode* groups = json.find("sourceGroups"); groups != nullptr && groups->isArray()) {
        for (const JsonNode& group : groups->items) {
            groupNames.push_back(group.stringAt("name"));
        }
    }
    if (const JsonNode* sources = json.find("sources"); sources != nullptr && sources->isArray()) {
        for (const JsonNode& source : sources->items) {
            CMakeSource entry;
            entry.path = resolvePath(sourceDir, source.stringAt("path"));
            entry.generated = source.boolAt("isGenerated");
            if (const auto group = source.integerAt("sourceGroupIndex");
                group && *group >= 0 && static_cast<std::size_t>(*group) < groupNames.size()) {
                entry.sourceGroup = groupNames[static_cast<std::size_t>(*group)];
            }
            if (const auto compile = source.integerAt("compileGroupIndex");
                compile && *compile >= 0 && static_cast<std::size_t>(*compile) < target.compileGroups.size()) {
                entry.compileGroup = static_cast<std::size_t>(*compile);
            }
            target.sources.push_back(std::move(entry));
        }
    }
    if (const JsonNode* dependencies = json.find("dependencies"); dependencies != nullptr && dependencies->isArray()) {
        for (const JsonNode& dependency : dependencies->items) {
            target.dependencies.push_back(dependency.stringAt("id"));
        }
    }
}

} // namespace

std::string_view toString(CMakeTargetType type) noexcept {
    switch (type) {
    case CMakeTargetType::Executable: return "EXECUTABLE";
    case CMakeTargetType::StaticLibrary: return "STATIC_LIBRARY";
    case CMakeTargetType::SharedLibrary: return "SHARED_LIBRARY";
    case CMakeTargetType::ModuleLibrary: return "MODULE_LIBRARY";
    case CMakeTargetType::ObjectLibrary: return "OBJECT_LIBRARY";
    case CMakeTargetType::InterfaceLibrary: return "INTERFACE_LIBRARY";
    case CMakeTargetType::Utility: return "UTILITY";
    }
    return "UTILITY";
}

const CMakeTarget* CMakeProject::findTarget(std::string_view targetName) const noexcept {
    for (const CMakeTarget& target : targets) {
        if (target.name == targetName) {
            return &target;
        }
    }
    return nullptr;
}

std::optional<std::string> CMakeProject::cacheValue(std::string_view entryName) const {
    for (const CMakeCacheEntry& entry : cache) {
        if (entry.name == entryName) {
            return entry.value;
        }
    }
    return std::nullopt;
}

std::vector<std::string> CMakeProject::projectInputs() const {
    std::vector<std::string> out;
    for (const CMakeInputFile& input : inputs) {
        if (!input.generated && !input.external && !input.cmake) {
            out.push_back(input.path);
        }
    }
    return out;
}

Result<std::string> cmakeFileApiQueryPath(std::string_view buildDirectory) {
    if (!io::isAbsolutePath(buildDirectory)) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("the build directory '{}' is not absolute", buildDirectory));
    }
    constexpr std::string_view kSeparator = io::kNativePathStyle == io::PathStyle::Windows ? "\\" : "/";
    return io::joinPath(buildDirectory,
                        std::format(".cmake{0}api{0}v1{0}query{0}{1}{0}query.json", kSeparator, kCMakeFileApiClient));
}

Result<void> writeCMakeFileApiQuery(io::IFileSystem& fs, std::string_view buildDirectory) {
    auto path = cmakeFileApiQueryPath(buildDirectory);
    if (!path) {
        return std::unexpected(std::move(path.error()));
    }
    if (auto existing = fs.readFile(*path); existing && *existing == kQuery) {
        return {};
    }
    if (auto created = fs.createDirectories(io::parentPath(*path)); !created) {
        return created;
    }
    return fs.writeFileAtomic(*path, kQuery);
}

Result<std::optional<CMakeProject>> readCMakeFileApiReply(io::IFileSystem& fs, std::string_view buildDirectory,
                                                          std::string_view configuration) {
    auto build = io::normalizePath(buildDirectory);
    if (!build) {
        return std::unexpected(std::move(build.error()));
    }
    constexpr std::string_view kSeparator = io::kNativePathStyle == io::PathStyle::Windows ? "\\" : "/";
    auto replyDir = io::joinPath(*build, std::format(".cmake{0}api{0}v1{0}reply", kSeparator));
    if (!replyDir) {
        return std::unexpected(std::move(replyDir.error()));
    }
    auto entries = fs.listDirectory(*replyDir);
    if (!entries) {
        if (entries.error().code() == ErrorCode::NotFound) {
            return std::optional<CMakeProject>{};
        }
        return std::unexpected(std::move(entries.error()));
    }
    std::string indexName;
    for (const io::DirEntry& entry : *entries) {
        if (entry.type == io::FileType::Regular && entry.name.starts_with("index-") && entry.name.ends_with(".json") &&
            entry.name > indexName) {
            indexName = entry.name;
        }
    }
    if (indexName.empty()) {
        return std::optional<CMakeProject>{};
    }
    ReplyReader reader(fs, *replyDir);
    auto index = reader.object(indexName);
    if (!index) {
        return std::unexpected(std::move(index.error()));
    }

    CMakeProject project;
    project.buildDirectory = *build;
    project.replyIndex = io::joinPath(*replyDir, indexName).value_or(std::string());
    if (const JsonNode* cmake = index->find("cmake")) {
        if (const JsonNode* version = cmake->find("version")) {
            project.cmakeVersion = version->stringAt("string");
        }
        if (const JsonNode* generator = cmake->find("generator")) {
            project.generator = generator->stringAt("name");
            project.multiConfig = generator->boolAt("multiConfig");
        }
    }
    ObjectFiles files;
    if (const JsonNode* reply = index->find("reply")) {
        if (const JsonNode* client = reply->find(kCMakeFileApiClient)) {
            if (const JsonNode* query = client->find("query.json")) {
                collectObjects(query->find("responses"), files);
            }
        }
    }
    {
        ObjectFiles fallback;
        collectObjects(index->find("objects"), fallback);
        for (auto [slot, other] :
             {std::pair{&files.codemodel, &fallback.codemodel}, std::pair{&files.cache, &fallback.cache},
              std::pair{&files.cmakeFiles, &fallback.cmakeFiles}, std::pair{&files.toolchains, &fallback.toolchains}}) {
            if (slot->empty()) {
                *slot = *other;
            }
        }
    }
    if (files.codemodel.empty()) {
        return fail(ErrorCode::Corrupt, std::format("{}: the reply has no codemodel", project.replyIndex));
    }
    auto codemodel = reader.object(files.codemodel);
    if (!codemodel) {
        return std::unexpected(std::move(codemodel.error()));
    }
    const JsonNode* paths = codemodel->find("paths");
    if (paths == nullptr || paths->stringAt("source").empty()) {
        return fail(ErrorCode::Corrupt, std::format("{}: the codemodel has no source path", files.codemodel));
    }
    project.sourceDirectory = resolvePath(*build, paths->stringAt("source"));
    const std::string replyBuild = paths->stringAt("build");
    if (!replyBuild.empty()) {
        project.buildDirectory = resolvePath(*build, replyBuild);
    }
    const JsonNode* configurations = codemodel->find("configurations");
    if (configurations == nullptr || !configurations->isArray() || configurations->items.empty()) {
        return fail(ErrorCode::Corrupt, std::format("{}: the codemodel has no configurations", files.codemodel));
    }
    const JsonNode* chosen = nullptr;
    for (const JsonNode& candidate : configurations->items) {
        project.configurations.push_back(candidate.stringAt("name"));
        if (chosen == nullptr &&
            (configuration.empty() || equalsIgnoringCase(candidate.stringAt("name"), configuration))) {
            chosen = &candidate;
        }
    }
    if (chosen == nullptr) {
        return fail(ErrorCode::NotFound, std::format("the reply has no configuration '{}'", configuration));
    }
    project.configuration = chosen->stringAt("name");
    if (const JsonNode* projects = chosen->find("projects");
        projects != nullptr && projects->isArray() && !projects->items.empty()) {
        project.name = projects->items.front().stringAt("name");
    }
    if (const JsonNode* targets = chosen->find("targets"); targets != nullptr && targets->isArray()) {
        for (const JsonNode& entry : targets->items) {
            CMakeTarget target;
            target.name = entry.stringAt("name");
            target.id = entry.stringAt("id");
            const std::string file = entry.stringAt("jsonFile");
            if (!file.empty()) {
                auto json = reader.object(file);
                if (!json) {
                    return std::unexpected(std::move(json.error()));
                }
                readTarget(*json, project.sourceDirectory, project.buildDirectory, target);
            }
            project.targets.push_back(std::move(target));
        }
    }
    // The optional objects: a missing or broken one leaves its part empty (header comment).
    if (!files.cmakeFiles.empty()) {
        if (auto json = reader.object(files.cmakeFiles)) {
            if (const JsonNode* inputs = json->find("inputs"); inputs != nullptr && inputs->isArray()) {
                for (const JsonNode& input : inputs->items) {
                    project.inputs.push_back(
                        CMakeInputFile{.path = resolvePath(project.sourceDirectory, input.stringAt("path")),
                                       .generated = input.boolAt("isGenerated"),
                                       .external = input.boolAt("isExternal"),
                                       .cmake = input.boolAt("isCMake")});
                }
            }
        }
    }
    if (!files.cache.empty()) {
        if (auto json = reader.object(files.cache)) {
            if (const JsonNode* cacheEntries = json->find("entries");
                cacheEntries != nullptr && cacheEntries->isArray()) {
                for (const JsonNode& entry : cacheEntries->items) {
                    project.cache.push_back(CMakeCacheEntry{.name = entry.stringAt("name"),
                                                            .type = entry.stringAt("type"),
                                                            .value = entry.stringAt("value")});
                }
            }
        }
    }
    if (!files.toolchains.empty()) {
        if (auto json = reader.object(files.toolchains)) {
            if (const JsonNode* toolchains = json->find("toolchains"); toolchains != nullptr && toolchains->isArray()) {
                for (const JsonNode& entry : toolchains->items) {
                    CMakeToolchain toolchain;
                    toolchain.language = entry.stringAt("language");
                    if (const JsonNode* compiler = entry.find("compiler")) {
                        const std::string path = compiler->stringAt("path");
                        toolchain.compilerPath =
                            path.empty() ? std::string() : resolvePath(project.buildDirectory, path);
                        toolchain.compilerId = compiler->stringAt("id");
                        toolchain.compilerVersion = compiler->stringAt("version");
                    }
                    project.toolchains.push_back(std::move(toolchain));
                }
            }
        }
    }
    return std::optional<CMakeProject>(std::move(project));
}

} // namespace frost::project
