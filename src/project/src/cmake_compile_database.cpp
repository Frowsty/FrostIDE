// SPDX-License-Identifier: Apache-2.0
//
// CMK-4 (cmake_compile_database.hpp): compile commands synthesized from the File API codemodel,
// fragment splitting, and the targets that list a file.

#include "frost/project/cmake_compile_database.hpp"

#include "frost/io/path.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace frost::project {

namespace {

constexpr char lowerAscii(char c) noexcept { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

std::string lowered(std::string_view text) {
    std::string out(text);
    std::ranges::transform(out, out.begin(), lowerAscii);
    return out;
}

bool equalsIgnoringCase(std::string_view a, std::string_view b) noexcept {
    return std::ranges::equal(a, b, [](char x, char y) { return lowerAscii(x) == lowerAscii(y); });
}

std::string normalizedOrSelf(std::string_view path) {
    auto normalized = io::normalizePath(path);
    return normalized ? std::move(*normalized) : std::string(path);
}

// Whether a model path (already normalised) names `normalized`; ASCII case ignored on Windows.
bool samePath(std::string_view modelPath, std::string_view normalized) noexcept {
    if constexpr (io::kNativePathStyle == io::PathStyle::Windows)
        return equalsIgnoringCase(modelPath, normalized);
    else
        return modelPath == normalized;
}

bool isCLanguage(std::string_view language) noexcept { return language == "C" || language == "CXX"; }

// Dotted version `a` >= `b` ("19.44.35229.0" >= "19.29.30036.3"); missing parts count as 0.
bool versionAtLeast(std::string_view a, std::string_view b) {
    // The next dotted part's leading number (0 when it has none); consumes the part and its dot.
    const auto next = [](std::string_view& text) {
        const std::size_t dot = text.find('.');
        const std::string_view part = text.substr(0, dot);
        text = dot == std::string_view::npos ? std::string_view() : text.substr(dot + 1);
        unsigned long value = 0;
        (void)std::from_chars(part.data(), part.data() + part.size(), value);
        return value;
    };
    while (!a.empty() || !b.empty()) {
        const unsigned long x = next(a);
        const unsigned long y = next(b);
        if (x != y)
            return x > y;
    }
    return true;
}

// The compiler of `language`: the toolchains object's, else the cache's CMAKE_<LANG>_COMPILER.
std::optional<CMakeToolchain> compilerFor(const CMakeProject& project, std::string_view language) {
    for (const CMakeToolchain& toolchain : project.toolchains) {
        if (toolchain.language == language && !toolchain.compilerPath.empty())
            return toolchain;
    }
    const std::string name = "CMAKE_" + std::string(language) + "_COMPILER";
    if (auto path = project.cacheValue(name); path && !path->empty()) {
        CMakeToolchain toolchain;
        toolchain.language = std::string(language);
        toolchain.compilerPath = *path;
        if (auto id = project.cacheValue(name + "_ID"))
            toolchain.compilerId = *id;
        return toolchain;
    }
    return std::nullopt;
}

std::string withSeparator(std::string_view path, char separator) {
    std::string out(path);
    std::ranges::replace(out, separator == '\\' ? '/' : '\\', separator);
    return out;
}

// The build directory as the generators write a database's "directory": CMake's own spelling,
// with '/' separators (clangd passes it on as -fdebug-compilation-dir, so the spelling counts).
std::string workingDirectory(const CMakeProject& project) { return withSeparator(project.buildDirectory, '/'); }

// The command of `file` compiled with `group` (header comment); `header` forces the language.
std::optional<std::vector<std::string>> commandFor(const CMakeProject& project, const CMakeCompileGroup& group,
                                                   std::string_view file, bool header,
                                                   const CMakeCommandOptions& options) {
    const auto toolchain = compilerFor(project, group.language);
    if (!toolchain)
        return std::nullopt;
    const CMakeCompilerFrontend frontend = cmakeCompilerFrontend(*toolchain);
    const bool cxx = group.language == "CXX";
    std::vector<std::string> args;
    args.push_back(toolchain->compilerPath);
    if (frontend == CMakeCompilerFrontend::Msvc) {
        args.emplace_back("/nologo");
        if (cxx)
            args.emplace_back("/TP");
        else if (header)
            args.emplace_back("/TC");
    } else if (frontend == CMakeCompilerFrontend::ClangCl) {
        args.emplace_back("/nologo");
        if (cxx)
            args.emplace_back("-TP");
        else if (header)
            args.emplace_back("-TC");
    }
    for (const std::string& define : group.defines)
        args.push_back("-D" + define);
    // Multi-config generators define the configuration being built (cmake-generators(7)).
    if (project.multiConfig && !project.configuration.empty())
        args.push_back("-DCMAKE_INTDIR=\"" + project.configuration + "\"");
    const bool external =
        frontend == CMakeCompilerFrontend::Msvc &&
        versionAtLeast(toolchain->compilerVersion.empty() ? "0" : toolchain->compilerVersion, "19.29.30036.3");
    bool anySystem = false;
    for (const CMakeIncludeDirectory& include : group.includeDirectories) {
        const std::string path = withSeparator(include.path, options.separator);
        if (!include.system) {
            args.push_back("-I" + path);
            continue;
        }
        anySystem = true;
        switch (frontend) {
        case CMakeCompilerFrontend::Msvc: args.push_back((external ? "-external:I" : "-I") + path); break;
        case CMakeCompilerFrontend::ClangCl: args.push_back("-imsvc" + path); break;
        case CMakeCompilerFrontend::Gnu:
            args.emplace_back("-isystem");
            args.push_back(path);
            break;
        }
    }
    if (anySystem && external)
        args.emplace_back("-external:W0");
    for (const std::string& fragment : group.flags) {
        for (std::string& argument : splitCompileCommandFragment(fragment, options.shell))
            args.push_back(std::move(argument));
    }
    if (header && frontend == CMakeCompilerFrontend::Gnu) {
        args.emplace_back("-x");
        args.emplace_back(cxx ? "c++-header" : "c-header");
    }
    args.emplace_back("-c");
    if (frontend == CMakeCompilerFrontend::ClangCl)
        args.emplace_back("--");
    args.emplace_back(file);
    return args;
}

} // namespace

CMakeCompilerFrontend cmakeCompilerFrontend(const CMakeToolchain& toolchain) noexcept {
    if (toolchain.compilerId == "MSVC")
        return CMakeCompilerFrontend::Msvc;
    std::string name = lowered(io::fileName(toolchain.compilerPath));
    if (name.ends_with(".exe"))
        name.resize(name.size() - 4);
    if (name == "clang-cl" || name == "icx-cl" || name.ends_with("-clang-cl"))
        return CMakeCompilerFrontend::ClangCl;
    if (toolchain.compilerId.empty() && name == "cl")
        return CMakeCompilerFrontend::Msvc;
    return CMakeCompilerFrontend::Gnu;
}

CMakeShell nativeCMakeShell() noexcept {
    return io::kNativePathStyle == io::PathStyle::Windows ? CMakeShell::Windows : CMakeShell::Posix;
}

std::vector<std::string> splitCompileCommandFragment(std::string_view fragment, CMakeShell shell) {
    std::vector<std::string> words;
    std::string word;
    bool inWord = false;
    const auto isSpace = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
    const auto finish = [&] {
        if (inWord)
            words.push_back(std::move(word));
        word.clear();
        inWord = false;
    };
    if (shell == CMakeShell::Windows) {
        bool quoted = false;
        for (std::size_t i = 0; i < fragment.size();) {
            const char c = fragment[i];
            if (c == '\\') {
                std::size_t slashes = 0;
                while (i < fragment.size() && fragment[i] == '\\') {
                    ++slashes;
                    ++i;
                }
                inWord = true;
                if (i < fragment.size() && fragment[i] == '"') {
                    word.append(slashes / 2, '\\');
                    if (slashes % 2 == 1) {
                        word += '"';
                        ++i;
                    }
                } else {
                    word.append(slashes, '\\');
                }
                continue;
            }
            if (c == '"') {
                inWord = true;
                if (quoted && i + 1 < fragment.size() && fragment[i + 1] == '"') {
                    word += '"'; // "" inside quotes: a literal quote.
                    i += 2;
                    continue;
                }
                quoted = !quoted;
                ++i;
                continue;
            }
            if (!quoted && isSpace(c)) {
                finish();
                ++i;
                continue;
            }
            word += c;
            inWord = true;
            ++i;
        }
        finish();
        return words;
    }
    for (std::size_t i = 0; i < fragment.size(); ++i) {
        const char c = fragment[i];
        if (isSpace(c)) {
            finish();
        } else if (c == '\'') {
            inWord = true;
            for (++i; i < fragment.size() && fragment[i] != '\''; ++i)
                word += fragment[i];
        } else if (c == '"') {
            inWord = true;
            for (++i; i < fragment.size() && fragment[i] != '"'; ++i) {
                if (fragment[i] == '\\' && i + 1 < fragment.size() &&
                    std::string_view("$`\"\\\n").find(fragment[i + 1]) != std::string_view::npos) {
                    ++i;
                    if (fragment[i] == '\n')
                        continue;
                }
                word += fragment[i];
            }
        } else if (c == '\\' && i + 1 < fragment.size()) {
            inWord = true;
            ++i;
            if (fragment[i] != '\n')
                word += fragment[i];
        } else {
            inWord = true;
            word += c;
        }
    }
    finish();
    return words;
}

std::vector<CMakeCompileCommand> synthesizeCMakeCompileCommands(const CMakeProject& project,
                                                                const CMakeCommandOptions& options) {
    std::vector<CMakeCompileCommand> commands;
    for (std::size_t t = 0; t < project.targets.size(); ++t) {
        const CMakeTarget& target = project.targets[t];
        for (const CMakeSource& source : target.sources) {
            if (!source.compileGroup || *source.compileGroup >= target.compileGroups.size())
                continue;
            const CMakeCompileGroup& group = target.compileGroups[*source.compileGroup];
            if (!isCLanguage(group.language))
                continue;
            auto arguments = commandFor(project, group, source.path, false, options);
            if (!arguments)
                continue;
            commands.push_back(CMakeCompileCommand{.directory = workingDirectory(project),
                                                   .file = source.path,
                                                   .arguments = std::move(*arguments),
                                                   .target = t});
        }
    }
    return commands;
}

std::vector<std::size_t> cmakeTargetsListing(const CMakeProject& project, std::string_view file) {
    const std::string path = normalizedOrSelf(file);
    std::vector<std::size_t> owners;
    for (std::size_t t = 0; t < project.targets.size(); ++t) {
        const auto& sources = project.targets[t].sources;
        if (std::ranges::any_of(sources, [&](const CMakeSource& source) { return samePath(source.path, path); }))
            owners.push_back(t);
    }
    return owners;
}

std::optional<CMakeCompileCommand> cmakeCommandInTarget(const CMakeProject& project, std::size_t targetIndex,
                                                        std::string_view file, const CMakeCommandOptions& options) {
    if (targetIndex >= project.targets.size())
        return std::nullopt;
    const CMakeTarget& target = project.targets[targetIndex];
    const std::string path = normalizedOrSelf(file);
    // A source the target compiles keeps its own group.
    for (const CMakeSource& source : target.sources) {
        if (!source.compileGroup || *source.compileGroup >= target.compileGroups.size() || !samePath(source.path, path))
            continue;
        const CMakeCompileGroup& group = target.compileGroups[*source.compileGroup];
        if (!isCLanguage(group.language))
            return std::nullopt;
        auto arguments = commandFor(project, group, path, false, options);
        if (!arguments)
            return std::nullopt;
        return CMakeCompileCommand{.directory = workingDirectory(project),
                                   .file = path,
                                   .arguments = std::move(*arguments),
                                   .target = targetIndex};
    }
    // A header: the group of the first non-generated compiled source, C++ before C.
    const CMakeCompileGroup* chosen = nullptr;
    for (const std::string_view language : {std::string_view("CXX"), std::string_view("C")}) {
        for (const CMakeSource& source : target.sources) {
            if (source.generated || !source.compileGroup || *source.compileGroup >= target.compileGroups.size())
                continue;
            const CMakeCompileGroup& group = target.compileGroups[*source.compileGroup];
            if (equalsIgnoringCase(group.language, language)) {
                chosen = &group;
                break;
            }
        }
        if (chosen)
            break;
    }
    if (!chosen)
        return std::nullopt;
    auto arguments = commandFor(project, *chosen, path, true, options);
    if (!arguments)
        return std::nullopt;
    return CMakeCompileCommand{.directory = workingDirectory(project),
                               .file = path,
                               .arguments = std::move(*arguments),
                               .target = targetIndex};
}

} // namespace frost::project
