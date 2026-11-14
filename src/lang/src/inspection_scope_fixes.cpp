// SPDX-License-Identifier: Apache-2.0
#include "frost/lang/inspection_scope_fixes.hpp"

#include "frost/io/path.hpp"
#include "frost/lang/uri.hpp"

#include <algorithm>
#include <array>
#include <functional>
#include <map>
#include <set>
#include <tuple>
namespace frost::lang {
namespace {
bool within(std::string_view root, std::string_view path) {
    const auto r = documentKey(root), p = documentKey(path);
    return p == r || (p.starts_with(r) && p.size() > r.size() && (p[r.size()] == '/' || p[r.size()] == '\\'));
}
bool inputFile(std::string_view path) {
    const auto name = io::fileName(path);
    if (name == ".clang-tidy" || name == ".clang-format" || name == ".clangd")
        return true;
    const auto dot = name.rfind('.');
    if (dot == name.npos)
        return false;
    std::string suffix(name.substr(dot));
    std::ranges::transform(suffix, suffix.begin(),
                           [](unsigned char c) { return static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c); });
    constexpr std::array extensions{".c",   ".cc",  ".cpp",  ".cxx", ".h",  ".hh",  ".hpp", ".hxx", ".inl", ".inc",
                                    ".ipp", ".tpp", ".cppm", ".ixx", ".cu", ".cuh", ".m",   ".mm",  ".rsp"};
    return std::ranges::find(extensions, suffix) != extensions.end();
}
Result<std::vector<std::string>> directoryImage(io::IFileSystem& fs, const std::string& path) {
    auto entries = fs.listDirectory(path);
    if (!entries)
        return std::unexpected(entries.error());
    std::vector<std::string> result;
    if (entries->size() > 32768)
        return fail(ErrorCode::OutOfRange, "Inspection namespace budget exceeded");
    for (const auto& entry : *entries)
        result.push_back(entry.name + "|" + std::to_string(static_cast<unsigned>(entry.type)));
    std::ranges::sort(result);
    return result;
}
} // namespace
bool InspectionScopeFixProposal::ready() const {
    return !files.empty() && std::ranges::none_of(groups, [](const auto& group) { return !group.refusal.empty(); });
}
Result<InspectionScopeCapture>
captureInspectionScope(io::IFileSystem& fs, const InspectionRequest& request, std::string projectRoot,
                       std::span<const std::pair<std::string, std::string>> openBuffers) {
    InspectionScopeCapture result;
    auto root = io::normalizePath(projectRoot);
    if (!root || !io::isAbsolutePath(projectRoot))
        return fail(ErrorCode::InvalidArgument, "An absolute project root is required");
    result.root = *root;
    auto physicalRoot = fs.physicalPath(result.root);
    if (!physicalRoot)
        return std::unexpected(physicalRoot.error());
    result.physicalRoot = *physicalRoot;
    result.databasePath = request.compileCommandsDir.ends_with(".json")
                              ? request.compileCommandsDir
                              : io::joinPath(request.compileCommandsDir, "compile_commands.json").value_or("");
    std::map<std::string, std::string> buffers;
    for (const auto& [path, text] : openBuffers) {
        if (!within(result.root, path))
            continue;
        if (!buffers.emplace(documentKey(path), text).second)
            return fail(ErrorCode::Unsupported, "Several buffers alias one input spelling");
    }
    std::set<std::string> captured, physical;
    std::size_t retained = 0;
    auto add = [&](std::string path, bool source = true) -> Result<void> {
        const auto key = documentKey(path);
        if (captured.contains(key))
            return {};
        auto identity = fs.physicalPath(path);
        if (!identity)
            return std::unexpected(identity.error());
        if (source && path != result.databasePath && !within(result.physicalRoot, *identity))
            return fail(ErrorCode::Unsupported, "Source input leaves the physical project: " + path);
        if (!physical.insert(documentKey(*identity)).second)
            return fail(ErrorCode::Unsupported, "Physical source aliases need one unambiguous capture: " + path);
        auto bytes = fs.readFileSnapshot(path, 64U * 1024U * 1024U);
        if (!bytes)
            return std::unexpected(bytes.error());
        const auto open = buffers.find(key);
        InspectionInputImage image{path,
                                   *bytes,
                                   open == buffers.end() ? *bytes : open->second,
                                   *identity,
                                   open != buffers.end(),
                                   source && path != result.databasePath && io::fileName(path) != ".clang-tidy" &&
                                       io::fileName(path) != ".clang-format" && io::fileName(path) != ".clangd" &&
                                       !path.ends_with(".rsp")};
        retained += image.diskBytes.size() + image.contents.size();
        if (retained > 512U * 1024U * 1024U || result.images.size() >= 32768)
            return fail(ErrorCode::OutOfRange, "Inspection capture exceeds its 512 MiB /32768 input budget");
        result.images.push_back(std::move(image));
        captured.insert(key);
        return {};
    };
    std::function<Result<void>(const std::string&, bool)> walk = [&](const std::string& path,
                                                                     bool compilerInputs) -> Result<void> {
        if (request.stopToken.stop_requested())
            return fail(ErrorCode::Cancelled, "Inspection capture cancelled");
        auto identity = fs.physicalPath(path);
        if (!identity || !within(result.physicalRoot, *identity))
            return fail(ErrorCode::Unsupported, "Compiler include directory leaves the physical project: " + path);
        auto entries = fs.listDirectory(path);
        if (!entries)
            return std::unexpected(entries.error());
        if (entries->size() > 32768)
            return fail(ErrorCode::OutOfRange, "Inspection namespace budget exceeded");
        std::vector<std::string> names;
        for (const auto& entry : *entries)
            names.push_back(entry.name + "|" + std::to_string(static_cast<unsigned>(entry.type)));
        std::ranges::sort(names);
        const auto previous = result.directories.find(path);
        if (previous != result.directories.end() && previous->second != names)
            return fail(ErrorCode::Busy, "Source namespace changed during capture");
        result.directories.emplace(path, std::move(names));
        if (result.directories.size() > 32768)
            return fail(ErrorCode::OutOfRange, "Inspection directory capture budget exceeded");
        for (const auto& entry : *entries) {
            if (entry.name == ".git" || entry.name == ".frost" ||
                (!compilerInputs && (entry.name == "build" || entry.name == "out")))
                continue;
            auto child = io::joinPath(path, entry.name);
            if (!child)
                return std::unexpected(child.error());
            if (entry.type == io::FileType::Symlink)
                return fail(ErrorCode::Unsupported,
                            "Linked project trees require a resolved inspection scope: " + *child);
            if (entry.type == io::FileType::Directory) {
                if (auto value = walk(*child, compilerInputs); !value)
                    return value;
            } else if (entry.type == io::FileType::Regular && (inputFile(*child) || compilerInputs)) {
                if (auto value = add(*child, !compilerInputs); !value)
                    return value;
            }
        }
        return {};
    };
    if (auto value = walk(result.root, false); !value)
        return std::unexpected(value.error());
    for (auto parent = std::string(io::parentPath(result.root)); !parent.empty();) {
        const auto path = io::joinPath(parent, ".clang-tidy");
        if (!path)
            return std::unexpected(path.error());
        auto exists = fs.exists(*path);
        if (!exists)
            return std::unexpected(exists.error());
        if (*exists) {
            if (auto value = add(*path, false); !value)
                return std::unexpected(value.error());
        }
        if (!*exists)
            result.absentInputs.push_back(*path);
        const auto next = std::string(io::parentPath(parent));
        if (next == parent)
            break;
        parent = next;
    }
    if (auto value = add(result.databasePath, false); !value)
        return std::unexpected(value.error());
    const auto database = std::ranges::find(result.images, result.databasePath, &InspectionInputImage::path);
    auto commands = parseInspectionCompileCommands(database->diskBytes);
    if (!commands)
        return std::unexpected(commands.error());
    result.translationUnits = resolveInspectionScopeFiles(*commands, request.scope);
    if (result.translationUnits.empty())
        return fail(ErrorCode::NotFound,
                    "Scope has no actual translation units; select a directory/target/project for headers");
    for (const auto& path : result.translationUnits) {
        if (!within(result.root, path))
            return fail(ErrorCode::Unsupported, "Scope translation unit is outside the project");
        if (!captured.contains(documentKey(path)))
            return fail(ErrorCode::Unsupported,
                        "Selected scope includes generated/excluded or uncaptured translation unit: " + path);
    }
    const auto resolve = [](const std::string& directory, std::string_view value) -> Result<std::string> {
        return io::isAbsolutePath(value) ? io::normalizePath(value) : io::joinPath(directory, value);
    };
    for (const auto& command : *commands) {
        if (!io::isAbsolutePath(command.directory))
            return fail(ErrorCode::Unsupported, "Copied compilation databases require absolute working directories");
        auto source = resolve(command.directory, command.file);
        if (!source || std::ranges::none_of(result.translationUnits, [&](const auto& path) {
                return documentKey(path) == documentKey(*source);
            }))
            continue;
        if (std::ranges::any_of(command.arguments, [](const auto& argument) { return argument.starts_with('@'); }))
            return fail(ErrorCode::Unsupported,
                        "Expand response-file arguments in the compilation database before scope fixes");
        for (std::size_t i = 1; i < command.arguments.size(); ++i) {
            const auto& argument = command.arguments[i];
            if (argument == "-include-pch" || argument.starts_with("-fmodule-file") ||
                argument.starts_with("-fmodule-map-file") || argument.starts_with("--config"))
                return fail(ErrorCode::Unsupported,
                            "PCH/module/driver-config inputs require an expanded immutable command profile");
            bool directory = false, input = false;
            std::string value;
            for (const auto prefix :
                 {"-I", "-isystem", "-iquote", "/I", "/external:I", "-include", "-imacros", "/FI"}) {
                const std::string_view flag(prefix);
                if (argument == flag) {
                    if (++i == command.arguments.size())
                        return fail(ErrorCode::Corrupt, "Compiler input flag has no path");
                    value = command.arguments[i];
                    input = true;
                } else if (argument.starts_with(flag) && argument.size() > flag.size()) {
                    value = argument.substr(flag.size());
                    input = true;
                    if (value.starts_with('='))
                        value.erase(0, 1);
                }
                if (input) {
                    directory = flag == "-I" || flag == "-isystem" || flag == "-iquote" || flag == "/I" ||
                                flag == "/external:I";
                    break;
                }
            }
            if (!input || value.empty())
                continue;
            auto path = resolve(command.directory, value);
            if (!path)
                return std::unexpected(path.error());
            if (!within(result.root, *path))
                continue; // System SDK/library trees are outside the editable project profile.
            auto exists = fs.exists(*path);
            if (!exists)
                return std::unexpected(exists.error());
            if (!*exists) {
                result.absentInputs.push_back(*path);
                continue;
            }
            if (directory) {
                if (auto captured = walk(*path, true); !captured)
                    return std::unexpected(captured.error());
            } else {
                auto identity = fs.physicalPath(*path);
                if (!identity || !within(result.physicalRoot, *identity))
                    return fail(ErrorCode::Unsupported, "Compiler input leaves the physical project: " + *path);
                if (auto captured = add(*path, false); !captured)
                    return std::unexpected(captured.error());
            }
        }
    }
    for (const auto& [path, contents] : buffers)
        if (!captured.contains(path) && inputFile(path))
            return fail(ErrorCode::Unsupported, "An open source input was not captured: " + path);
    if (auto current = validateInspectionCapture(fs, result); !current)
        return std::unexpected(current.error());
    return result;
}
Result<void> validateInspectionCapture(io::IFileSystem& fs, const InspectionScopeCapture& capture) {
    for (const auto& image : capture.images) {
        auto identity = fs.physicalPath(image.path);
        if (!identity || documentKey(*identity) != documentKey(image.physicalPath))
            return fail(ErrorCode::Busy, "Inspection input identity changed: " + image.path);
        auto current = fs.readFileSnapshot(image.path, 64U * 1024U * 1024U);
        if (!current || *current != image.diskBytes)
            return fail(ErrorCode::Busy, "Inspection input changed: " + image.path);
    }
    for (const auto& path : capture.absentInputs) {
        auto current = fs.exists(path);
        if (!current || *current)
            return fail(ErrorCode::Busy, "Inspection input appeared: " + path);
    }
    for (const auto& [path, names] : capture.directories) {
        auto current = directoryImage(fs, path);
        if (!current || *current != names)
            return fail(ErrorCode::Busy, "Inspection source directory changed: " + path);
    }
    return {};
}
Result<InspectionScopeFixProposal> planInspectionScopeFixes(const Json& decoded, const InspectionScopeCapture& capture,
                                                            const InspectionProfile& profile) {
    if (decoded.stringAt("kind") != "clang-tidy-fixes" || decoded.integerAt("schemaVersion") != 1)
        return fail(ErrorCode::Corrupt, "Unexpected LLVM YAML decoder response");
    const auto* diagnostics = decoded.find("diagnostics");
    if (!diagnostics || !diagnostics->isArray() || diagnostics->size() > 8192)
        return fail(ErrorCode::Corrupt, "Invalid exported diagnostic groups");
    struct Edit {
        std::size_t offset, length;
        std::string text;
    };
    InspectionScopeFixProposal result;
    std::map<std::string, std::vector<Edit>> edits;
    std::map<std::string, const InspectionInputImage*> images;
    for (const auto& image : capture.images)
        images.emplace(documentKey(image.path), &image);
    for (const auto& diagnostic : diagnostics->items()) {
        InspectionFixGroup group{std::string(diagnostic.stringAt("name")), {}, {}, 0};
        if (profile.effectiveSeverity(group.rule) == InspectionSeverity::Disabled)
            continue;
        std::vector<const Json*> variants;
        auto collect = [&](const Json* message) -> bool {
            if (!message || !message->isObject())
                return false;
            const auto* replacements = message->find("replacements");
            if (!replacements || !replacements->isArray())
                return false;
            if (!replacements->items().empty())
                variants.push_back(message);
            return true;
        };
        const auto* primary = diagnostic.find("primary");
        if (!collect(primary))
            return fail(ErrorCode::Corrupt, "Invalid primary replacement group");
        group.message = std::string(primary->stringAt("message"));
        if (const auto* notes = diagnostic.find("notes")) {
            if (!notes->isArray())
                return fail(ErrorCode::Corrupt, "Invalid alternative replacement notes");
            for (const auto& note : notes->items())
                if (!collect(&note))
                    return fail(ErrorCode::Corrupt, "Invalid note replacement group");
        }
        if (variants.empty()) {
            ++result.withoutFix;
            continue;
        }
        if (variants.size() != 1)
            group.refusal = "Several alternative exported fixes require an explicit diagnostic choice";
        for (const auto* variant : variants)
            for (const auto& replacement : variant->find("replacements")->items()) {
                ++group.replacements;
                const auto offset = replacement.unsignedAt("offset"), length = replacement.unsignedAt("length");
                const auto* text = replacement.find("text");
                std::string path(replacement.stringAt("path"));
                if (!io::isAbsolutePath(path))
                    path = io::joinPath(diagnostic.stringAt("directory"), path).value_or("");
                const auto found = images.find(documentKey(path));
                if (!offset || !length || !text || !text->asString())
                    return fail(ErrorCode::Corrupt, "Invalid actual replacement DTO");
                if (found == images.end()) {
                    group.refusal = "Replacement targets uncaptured source/header: " + path;
                    continue;
                }
                const auto& image = *found->second;
                if (!image.editable) {
                    group.refusal = "Replacement targets inspection configuration";
                    continue;
                }
                if (*offset > image.contents.size() || *length > image.contents.size() - *offset) {
                    group.refusal = "Replacement lies outside its request-start image";
                    continue;
                }
                if (group.refusal.empty())
                    edits[found->first].push_back(
                        Edit{static_cast<std::size_t>(*offset), static_cast<std::size_t>(*length), *text->asString()});
            }
        result.groups.push_back(std::move(group));
    }
    for (auto& [key, values] : edits) {
        std::ranges::sort(values, [](const auto& a, const auto& b) {
            return std::tie(a.offset, a.length, a.text) < std::tie(b.offset, b.length, b.text);
        });
        values.erase(std::unique(values.begin(), values.end(),
                                 [](const auto& a, const auto& b) {
                                     return a.offset == b.offset && a.length == b.length && a.text == b.text;
                                 }),
                     values.end());
        for (std::size_t i = 1; i < values.size(); ++i)
            if (values[i].offset == values[i - 1].offset ||
                values[i].offset < values[i - 1].offset + values[i - 1].length) {
                result.groups.push_back({{},
                                         "Conflicting exported replacements",
                                         "Overlapping or differently ordered replacements refuse the whole scope",
                                         values.size()});
                break;
            }
        const auto& image = *images.at(key);
        auto after = image.contents;
        for (auto it = values.rbegin(); it != values.rend(); ++it)
            after.replace(it->offset, it->length, it->text);
        if (after != image.contents)
            result.files.push_back({image.path, image.contents, std::move(after), image.buffer});
    }
    if (std::ranges::any_of(result.groups, [](const auto& group) { return !group.refusal.empty(); }))
        result.files.clear();
    return result;
}
} // namespace frost::lang
