// SPDX-License-Identifier: Apache-2.0
// Build a local LLDB native launch with explicit original player/module/symbol and C++ source paths.
#include "unity_native_profile.hpp"

#include "frost/io/path.hpp"

#include "managed_source_snapshot.hpp"
namespace frost::app {
namespace {
Result<std::string> owned(io::IFileSystem& fs, std::string_view root, std::string_view path, io::FileType kind) {
    auto normal = io::normalizePath(path), physical = fs.physicalPath(path);
    auto normalizedRoot = io::normalizePath(root);
    if (!normal || !physical || !normalizedRoot || *normal != *physical || !managedPathHasNoLinks(fs, path) ||
        normal->size() <= normalizedRoot->size() || !normal->starts_with(*normalizedRoot) ||
        ((*normal)[normalizedRoot->size()] != '/' && (*normal)[normalizedRoot->size()] != '\\'))
        return fail(ErrorCode::InvalidArgument,
                    "IL2CPP native inputs must be physical paths inside the trusted project");
    auto status = fs.status(*normal);
    if (!status || status->type != kind)
        return fail(ErrorCode::NotFound, "IL2CPP native input is unavailable or has the wrong type");
    return normal;
}
Result<std::string> quoteLldbPath(std::string_view path) {
    if (path.empty() || path.size() > 32767)
        return fail(ErrorCode::InvalidArgument, "invalid native symbol path");
    std::string result = "\"";
    for (char c : path) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte < 32 || byte == 127 || c == '`')
            return fail(ErrorCode::InvalidArgument,
                        "native debugger command paths cannot contain control characters or backticks");
        if (c == '\\' || c == '\"')
            result.push_back('\\');
        result.push_back(c);
    }
    result.push_back('\"');
    return result;
}
} // namespace
bool isUnityGeneratedSource(io::IFileSystem& fs, std::string_view root, std::string_view path) {
    if (!(path.ends_with(".cpp") || path.ends_with(".cc") || path.ends_with(".cxx") || path.ends_with(".h") ||
          path.ends_with(".hpp") || path.ends_with(".inl")))
        return false;
    return owned(fs, root, path, io::FileType::Regular).has_value();
}

Result<UnityNativeProfile> prepareUnityNativeProfile(io::IFileSystem& fs, const project::TrustStore& trust,
                                                     std::string_view root,
                                                     const UnityIl2cppNativeSelection& selection) {
    auto physicalRoot = fs.physicalPath(root), normalRoot = io::normalizePath(root);
    if (!physicalRoot || !normalRoot || *physicalRoot != *normalRoot || !managedPathHasNoLinks(fs, root) ||
        !trust.isTrusted(root))
        return fail(ErrorCode::Untrusted, "Trust the physical Unity project before native debugging");
    auto player = owned(fs, root, selection.player, io::FileType::Regular);
    auto module = owned(fs, root, selection.module, io::FileType::Regular);
    auto symbols = owned(fs, root, selection.symbols, io::FileType::Regular);
    auto sources = owned(fs, root, selection.generatedRoot, io::FileType::Directory);
    if (!player)
        return std::unexpected(std::move(player.error()));
    if (!module)
        return std::unexpected(std::move(module.error()));
    if (!symbols)
        return std::unexpected(std::move(symbols.error()));
    if (!sources)
        return std::unexpected(std::move(sources.error()));
    auto moduleArg = quoteLldbPath(*module), symbolArg = quoteLldbPath(*symbols);
    if (!moduleArg)
        return std::unexpected(std::move(moduleArg.error()));
    if (!symbolArg)
        return std::unexpected(std::move(symbolArg.error()));
    UnityNativeProfile profile;
    profile.generatedRoot = *sources;
    profile.target.name = "IL2CPP generated C++ (native)";
    profile.target.program = *player;
    profile.target.workingDirectory = std::string(io::parentPath(*player));
    profile.target.trustRoots = {std::string(root), *sources};
    profile.target.stopOnEntry = true;
    if (!selection.buildSourceRoot.empty()) {
        auto from = io::normalizePath(selection.buildSourceRoot);
        if (!from)
            return std::unexpected(std::move(from.error()));
        if (*from != *sources) {
            auto mapping = debug::makeSourceMap(fs, trust, *from, *sources, profile.target.trustRoots);
            if (!mapping)
                return std::unexpected(std::move(mapping.error()));
            profile.target.sourceMap = std::move(*mapping);
        }
    }
    profile.arguments = debug::lldbDapLaunchArguments(profile.target);
    // LLDB23 DAP README: preRunCommands execute after target creation, before ordinary launch.
    const std::string addModule = "!target modules add --symfile " + *symbolArg + " -- " + *moduleArg;
    profile.arguments.set("preRunCommands", rpc::Json::array({addModule}));
    return profile;
}
} // namespace frost::app
