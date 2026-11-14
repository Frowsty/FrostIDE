// SPDX-License-Identifier: Apache-2.0
//
// Workbench (frost/app/workbench.hpp): project and file creation, the New Project dialog and its
// generated CMake and MSBuild (.sln, .slnx, .vcxproj) projects, new files, folders and project
// sources, and what follows a new file (split from workbench_project.cpp, FND-12a).

#include "frost/app/workbench.hpp"
#include "frost/io/path.hpp"
#include "frost/lang/compile_commands.hpp"
#include "frost/lang/templates.hpp"
#include "frost/lang/msbuild_compile_commands.hpp"
#include "frost/lang/msvc_environment.hpp"
#include "frost/ui/lang/language_controller.hpp"
#include "frost/ui/shell/tool_window_manager.hpp"
#include "frost/ui/toolwindows/solution_explorer.hpp"
#include "frost/ui/tr.hpp"
#include "frost/ui/workbench/file_system_view.hpp"

#include "cmake_integration.hpp"
#include "engine_integration.hpp"
#include "git_integration.hpp"
#include "language_bridge.hpp"
#include "unity_control.hpp"
#include "workbench_project_helpers.hpp"
#include <QDir>
#include <QStandardPaths>

#include <algorithm>
#include <array>
#include <format>
#include <utility>

namespace frost::app {

namespace {

QString text(const char* source) { return ui::qtr("frost::app::Workbench", source); }

QString qs(std::string_view utf8) { return QString::fromUtf8(utf8.data(), static_cast<qsizetype>(utf8.size())); }

std::string sanitizeCMakeTargetName(std::string_view name) {
    std::string out;
    out.reserve(name.size());
    for (const char raw : name) {
        const auto ch = static_cast<unsigned char>(raw);
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_' ||
            ch == '-') {
            out.push_back(static_cast<char>(ch));
        } else {
            out.push_back('_');
        }
    }
    if (out.empty() || out.front() == '-') {
        out.insert(0, "app_");
    }
    return out;
}

std::string sanitizeCodeIdentifier(std::string_view name) {
    std::string out;
    out.reserve(name.size());
    for (const char raw : name) {
        const auto ch = static_cast<unsigned char>(raw);
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_') {
            out.push_back(static_cast<char>(ch));
        } else {
            out.push_back('_');
        }
    }
    if (out.empty() || (out.front() >= '0' && out.front() <= '9')) {
        out.insert(0, "project_");
    }
    return out;
}

std::string sanitizeMacroPrefix(std::string_view name) {
    std::string out = sanitizeCodeIdentifier(name);
    std::ranges::transform(out, out.begin(),
                           [](char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 32) : c; });
    return out;
}

std::string projectGuidForName(std::string_view name) {
    std::uint64_t h1 = 0xcbf29ce484222325ULL;
    std::uint64_t h2 = 0x84222325cbf29ce4ULL;
    for (const char raw : name) {
        const auto ch = static_cast<unsigned char>(raw);
        h1 = (h1 ^ ch) * 0x100000001b3ULL;
        h2 = (h2 ^ (ch + 0x9e3779b97f4a7c15ULL)) * 0x100000001b3ULL;
    }
    const auto d1 = static_cast<std::uint32_t>(h1 >> 32U);
    const auto d2 = static_cast<std::uint16_t>((h1 >> 16U) & 0xFFFFU);
    const auto d3 = static_cast<std::uint16_t>((h1 & 0x0FFFU) | 0x4000U);
    const auto d4 = static_cast<std::uint16_t>(((h2 >> 48U) & 0x3FFFU) | 0x8000U);
    const auto d5 = h2 & 0xFFFFFFFFFFFFULL;
    return std::format("{{{:08X}-{:04X}-{:04X}-{:04X}-{:012X}}}", d1, d2, d3, d4, d5);
}

std::string buildSlnContents(std::string_view projectName, std::string_view vcxprojFile, std::string_view projectGuid) {
    return std::format("Microsoft Visual Studio Solution File, Format Version 12.00\n"
                       "# Visual Studio Version 17\n"
                       "VisualStudioVersion = 17.0.31903.59\n"
                       "MinimumVisualStudioVersion = 10.0.40219.1\n"
                       "Project(\"{{8BC9CEB8-8B4A-11D0-8D11-00A0C91BC942}}\") = \"{0}\", \"{1}\", \"{2}\"\n"
                       "EndProject\n"
                       "Global\n"
                       "\tGlobalSection(SolutionConfigurationPlatforms) = preSolution\n"
                       "\t\tDebug|x64 = Debug|x64\n"
                       "\t\tRelease|x64 = Release|x64\n"
                       "\tEndGlobalSection\n"
                       "\tGlobalSection(ProjectConfigurationPlatforms) = postSolution\n"
                       "\t\t{2}.Debug|x64.ActiveCfg = Debug|x64\n"
                       "\t\t{2}.Debug|x64.Build.0 = Debug|x64\n"
                       "\t\t{2}.Release|x64.ActiveCfg = Release|x64\n"
                       "\t\t{2}.Release|x64.Build.0 = Release|x64\n"
                       "\tEndGlobalSection\n"
                       "EndGlobal\n",
                       projectName, vcxprojFile, projectGuid);
}

std::string buildSlnxContents(std::string_view vcxprojFile, std::string_view projectGuid) {
    return std::format("<Solution>\n"
                       "  <Configurations>\n"
                       "    <BuildType Name=\"Debug\" />\n"
                       "    <BuildType Name=\"Release\" />\n"
                       "    <Platform Name=\"x64\" />\n"
                       "  </Configurations>\n"
                       "  <Project Path=\"{}\" Id=\"{}\" />\n"
                       "</Solution>\n",
                       escapeXml(vcxprojFile), projectGuid);
}

std::string buildVcxprojContents(std::string_view projectGuid, std::string_view rootNamespace,
                                 std::string_view configurationType, std::string_view preprocessorSuffix,
                                 std::string_view subSystem, std::string_view sourceFile, std::string_view headerFile) {
    const std::string headerGroup = headerFile.empty() ? std::string{}
                                                       : std::format("  <ItemGroup>\n"
                                                                     "    <ClInclude Include=\"{}\" />\n"
                                                                     "  </ItemGroup>\n",
                                                                     escapeXml(headerFile));
    return std::format(
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        "<Project DefaultTargets=\"Build\" xmlns=\"http://schemas.microsoft.com/developer/msbuild/2003\">\n"
        "  <ItemGroup Label=\"ProjectConfigurations\">\n"
        "    <ProjectConfiguration Include=\"Debug|x64\">\n"
        "      <Configuration>Debug</Configuration>\n"
        "      <Platform>x64</Platform>\n"
        "    </ProjectConfiguration>\n"
        "    <ProjectConfiguration Include=\"Release|x64\">\n"
        "      <Configuration>Release</Configuration>\n"
        "      <Platform>x64</Platform>\n"
        "    </ProjectConfiguration>\n"
        "  </ItemGroup>\n"
        "  <PropertyGroup Label=\"Globals\">\n"
        "    <VCProjectVersion>17.0</VCProjectVersion>\n"
        "    <Keyword>Win32Proj</Keyword>\n"
        "    <ProjectGuid>{0}</ProjectGuid>\n"
        "    <RootNamespace>{1}</RootNamespace>\n"
        "    <WindowsTargetPlatformVersion>10.0</WindowsTargetPlatformVersion>\n"
        "  </PropertyGroup>\n"
        "  <Import Project=\"$(VCTargetsPath)\\Microsoft.Cpp.Default.props\" />\n"
        "  <PropertyGroup Condition=\"'$(Configuration)|$(Platform)'=='Debug|x64'\" Label=\"Configuration\">\n"
        "    <ConfigurationType>{2}</ConfigurationType>\n"
        "    <UseDebugLibraries>true</UseDebugLibraries>\n"
        "    <PlatformToolset>v143</PlatformToolset>\n"
        "    <CharacterSet>Unicode</CharacterSet>\n"
        "  </PropertyGroup>\n"
        "  <PropertyGroup Condition=\"'$(Configuration)|$(Platform)'=='Release|x64'\" Label=\"Configuration\">\n"
        "    <ConfigurationType>{2}</ConfigurationType>\n"
        "    <UseDebugLibraries>false</UseDebugLibraries>\n"
        "    <WholeProgramOptimization>true</WholeProgramOptimization>\n"
        "    <PlatformToolset>v143</PlatformToolset>\n"
        "    <CharacterSet>Unicode</CharacterSet>\n"
        "  </PropertyGroup>\n"
        "  <Import Project=\"$(VCTargetsPath)\\Microsoft.Cpp.props\" />\n"
        "  <ItemDefinitionGroup Condition=\"'$(Configuration)|$(Platform)'=='Debug|x64'\">\n"
        "    <ClCompile>\n"
        "      <WarningLevel>Level3</WarningLevel>\n"
        "      <SDLCheck>true</SDLCheck>\n"
        "      <PreprocessorDefinitions>_DEBUG;{3};%(PreprocessorDefinitions)</PreprocessorDefinitions>\n"
        "      <ConformanceMode>true</ConformanceMode>\n"
        "      <LanguageStandard>stdcpp20</LanguageStandard>\n"
        "    </ClCompile>\n"
        "    <Link>\n"
        "      <SubSystem>{4}</SubSystem>\n"
        "      <GenerateDebugInformation>true</GenerateDebugInformation>\n"
        "    </Link>\n"
        "  </ItemDefinitionGroup>\n"
        "  <ItemDefinitionGroup Condition=\"'$(Configuration)|$(Platform)'=='Release|x64'\">\n"
        "    <ClCompile>\n"
        "      <WarningLevel>Level3</WarningLevel>\n"
        "      <FunctionLevelLinking>true</FunctionLevelLinking>\n"
        "      <IntrinsicFunctions>true</IntrinsicFunctions>\n"
        "      <SDLCheck>true</SDLCheck>\n"
        "      <PreprocessorDefinitions>NDEBUG;{3};%(PreprocessorDefinitions)</PreprocessorDefinitions>\n"
        "      <ConformanceMode>true</ConformanceMode>\n"
        "      <LanguageStandard>stdcpp20</LanguageStandard>\n"
        "    </ClCompile>\n"
        "    <Link>\n"
        "      <SubSystem>{4}</SubSystem>\n"
        "      <EnableCOMDATFolding>true</EnableCOMDATFolding>\n"
        "      <OptimizeReferences>true</OptimizeReferences>\n"
        "      <GenerateDebugInformation>true</GenerateDebugInformation>\n"
        "    </Link>\n"
        "  </ItemDefinitionGroup>\n"
        "  <ItemGroup>\n"
        "    <ClCompile Include=\"{5}\" />\n"
        "  </ItemGroup>\n"
        "{6}"
        "  <Import Project=\"$(VCTargetsPath)\\Microsoft.Cpp.targets\" />\n"
        "</Project>\n",
        projectGuid, escapeXml(rootNamespace), configurationType, preprocessorSuffix, subSystem, escapeXml(sourceFile),
        headerGroup);
}

std::string buildVcxprojFiltersContents(std::string_view sourceFile, std::string_view headerFile) {
    const std::string headerGroup = headerFile.empty() ? std::string{}
                                                       : std::format("  <ItemGroup>\n"
                                                                     "    <ClInclude Include=\"{}\">\n"
                                                                     "      <Filter>Header Files</Filter>\n"
                                                                     "    </ClInclude>\n"
                                                                     "  </ItemGroup>\n",
                                                                     escapeXml(headerFile));
    return std::format("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                       "<Project ToolsVersion=\"4.0\" xmlns=\"http://schemas.microsoft.com/developer/msbuild/2003\">\n"
                       "  <ItemGroup>\n"
                       "    <Filter Include=\"Source Files\">\n"
                       "      <UniqueIdentifier>{{4FC737F1-C7A5-4376-A066-2A32D752A2FF}}</UniqueIdentifier>\n"
                       "      <Extensions>cpp;c;cc;cxx;c++;cppm;ixx;def;odl;idl;hpj;bat;asm;asmx</Extensions>\n"
                       "    </Filter>\n"
                       "    <Filter Include=\"Header Files\">\n"
                       "      <UniqueIdentifier>{{93995380-89BD-4b04-88EB-625FBE52EBFB}}</UniqueIdentifier>\n"
                       "      <Extensions>h;hh;hpp;hxx;h++;hm;inl;inc;ipp;xsd</Extensions>\n"
                       "    </Filter>\n"
                       "    <Filter Include=\"Resource Files\">\n"
                       "      <UniqueIdentifier>{{67DA6AB6-F800-4c08-8B7A-83BB121AAD01}}</UniqueIdentifier>\n"
                       "      "
                       "<Extensions>rc;ico;cur;bmp;dlg;rc2;rct;bin;rgs;gif;jpg;jpeg;jpe;resx;tiff;tif;png;wav;"
                       "mfcribbon-ms</Extensions>\n"
                       "    </Filter>\n"
                       "  </ItemGroup>\n"
                       "  <ItemGroup>\n"
                       "    <ClCompile Include=\"{0}\">\n"
                       "      <Filter>Source Files</Filter>\n"
                       "    </ClCompile>\n"
                       "  </ItemGroup>\n"
                       "{1}"
                       "</Project>\n",
                       escapeXml(sourceFile), headerGroup);
}

bool isCTranslationUnit(std::string_view path) noexcept {
    static constexpr std::array<std::string_view, 8> kTranslationUnits{".c",   ".cc",   ".cpp", ".cxx",
                                                                       ".c++", ".cppm", ".ixx", ".cu"};
    const std::string ext = extensionOf(path);
    return std::ranges::find(kTranslationUnits, ext) != kTranslationUnits.end();
}

} // namespace

// --- Project and file creation ---

void Workbench::showNewProjectDialog() {
    std::string defaultLocation;
    if (!m_projectRoot.empty()) {
        defaultLocation = std::string(io::parentPath(m_projectRoot));
    } else {
        QString docs = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
        if (docs.isEmpty())
            docs = QDir::homePath();
        defaultLocation = QDir::toNativeSeparators(docs).toStdString();
    }
    const std::optional<NewProjectSpec> chosen = m_services.prompts.chooseNewProject(m_window, defaultLocation);
    if (!chosen)
        return;
    (void)createNewProject(*chosen);
}

Result<std::string> Workbench::createNewProject(const NewProjectSpec& spec) {
    if(spec.author.size()>256 || spec.description.size()>4096 || spec.author.find_first_of("\r\n")!=spec.author.npos ||
        spec.description.find_first_of("\r\n")!=spec.description.npos || spec.author.find("*/")!=spec.author.npos || spec.description.find("*/")!=spec.description.npos ||
        spec.author.find('\0')!=spec.author.npos || spec.description.find('\0')!=spec.description.npos)
        return fail(ErrorCode::InvalidArgument,"Project template author/description must be bounded comment-safe lines");
    if (Result<void> validName = validateProjectNameComponent(spec.name); !validName) {
        notify(text("Cannot create project"), qs(validName.error().message()), ui::NotificationKind::Error);
        return std::unexpected(std::move(validName.error()));
    }
    if (spec.location.empty() || !io::isAbsolutePath(spec.location)) {
        Error err(ErrorCode::InvalidArgument, "select an absolute location for the project");
        notify(text("Cannot create project"), qs(err.message()), ui::NotificationKind::Error);
        return std::unexpected(std::move(err));
    }
    Result<std::string> normalizedLocation = io::normalizePath(spec.location);
    if (!normalizedLocation) {
        notify(text("Cannot create project"), qs(normalizedLocation.error().message()), ui::NotificationKind::Error);
        return std::unexpected(std::move(normalizedLocation.error()));
    }
    Result<std::string> targetDir = io::joinPath(*normalizedLocation, spec.name);
    if (!targetDir) {
        notify(text("Cannot create project"), qs(targetDir.error().message()), ui::NotificationKind::Error);
        return std::unexpected(std::move(targetDir.error()));
    }

    Result<bool> existed = m_services.fs.exists(*targetDir);
    if (!existed) {
        notify(text("Cannot create project"), qs(existed.error().message()), ui::NotificationKind::Error);
        return std::unexpected(std::move(existed.error()));
    }
    const bool dirExisted = *existed;
    if (dirExisted) {
        Result<io::FileStatus> status = m_services.fs.status(*targetDir);
        if (!status) {
            notify(text("Cannot create project"), qs(status.error().message()), ui::NotificationKind::Error);
            return std::unexpected(std::move(status.error()));
        }
        if (status->type != io::FileType::Directory) {
            Error err(ErrorCode::AlreadyExists, "a file already exists at " + *targetDir);
            notify(text("Cannot create project"), qs(err.message()), ui::NotificationKind::Error);
            return std::unexpected(std::move(err));
        }
        Result<std::vector<io::DirEntry>> entries = m_services.fs.listDirectory(*targetDir);
        if (!entries) {
            notify(text("Cannot create project"), qs(entries.error().message()), ui::NotificationKind::Error);
            return std::unexpected(std::move(entries.error()));
        }
        if (!entries->empty()) {
            Error err(ErrorCode::AlreadyExists, "the target folder is not empty: " + *targetDir);
            notify(text("Cannot create project"), qs(err.message()), ui::NotificationKind::Error);
            return std::unexpected(std::move(err));
        }
    } else {
        if (Result<void> created = m_services.fs.createDirectories(*targetDir); !created) {
            notify(text("Cannot create project"), qs(created.error().message()), ui::NotificationKind::Error);
            return std::unexpected(std::move(created.error()));
        }
    }

    std::string canonicalDir = m_documentRegistry->canonicalize(*targetDir);
    if (spec.kind == NewProjectTemplate::EmptyFolder) {
        Result<void> opened = openFolder(canonicalDir, false);
        if (!opened) {
            return std::unexpected(std::move(opened.error()));
        }
        if (m_window) {
            m_window->toolWindows().show(ui::kFileSystemToolWindow, false);
        }
        return canonicalDir;
    }

    const std::string targetName = sanitizeCMakeTargetName(spec.name);
    const std::string codeIdent = sanitizeCodeIdentifier(spec.name);
    const std::string macroPrefix = sanitizeMacroPrefix(spec.name);
    const std::string projectGuid = projectGuidForName(spec.name);

    struct GeneratedFile {
        std::string path;
        std::string contents;
    };
    std::vector<GeneratedFile> files;
    std::string primarySourcePath;
    std::string solutionToOpen;
    std::string cmakeToOpen;

    const auto addFile = [&](std::string_view relativeName, std::string contents,
                             bool isPrimarySource = false) -> bool {
        Result<std::string> fullPath = io::joinPath(canonicalDir, relativeName);
        if (!fullPath) {
            return false;
        }
        if (isPrimarySource) {
            primarySourcePath = *fullPath;
        }
        if(!spec.author.empty() || !spec.description.empty()) {
            const bool cmake=relativeName=="CMakeLists.txt" || relativeName.ends_with(".cmake");
            const bool cpp=relativeName.ends_with(".cpp") || relativeName.ends_with(".h") || relativeName.ends_with(".hpp");
            if(cmake || cpp) {
                const auto* definition=lang::findTemplate(cmake?"file.cmake":"file.cpp.source");
                auto header=lang::expandTemplate(definition->body,{{"AUTHOR",spec.author},{"DESCRIPTION",spec.description}});
                if(!header) return false;
                contents=header->parsed.text+"\n"+contents;
            }
        }
        files.push_back(GeneratedFile{std::move(*fullPath), std::move(contents)});
        return true;
    };

    bool pathsOk = true;
    switch (spec.kind) {
    case NewProjectTemplate::CMakeExecutable: {
        pathsOk = addFile("CMakeLists.txt", std::format("cmake_minimum_required(VERSION 3.20)\n"
                                                        "project({0} LANGUAGES CXX)\n"
                                                        "\n"
                                                        "set(CMAKE_CXX_STANDARD 20)\n"
                                                        "set(CMAKE_CXX_STANDARD_REQUIRED ON)\n"
                                                        "set(CMAKE_CXX_EXTENSIONS OFF)\n"
                                                        "\n"
                                                        "add_executable({0}\n"
                                                        "    main.cpp\n"
                                                        ")\n",
                                                        targetName)) &&
                  addFile("main.cpp",
                          std::format("#include <iostream>\n"
                                      "\n"
                                      "int main() {{\n"
                                      "    std::cout << \"Hello from {}!\\n\";\n"
                                      "    return 0;\n"
                                      "}}\n",
                                      targetName),
                          true);
        if (!files.empty()) {
            cmakeToOpen = files.front().path;
        }
        break;
    }
    case NewProjectTemplate::CMakeStaticLibrary: {
        const std::string headerName = targetName + ".hpp";
        const std::string sourceName = targetName + ".cpp";
        pathsOk = addFile("CMakeLists.txt",
                          std::format("cmake_minimum_required(VERSION 3.20)\n"
                                      "project({0} LANGUAGES CXX)\n"
                                      "\n"
                                      "set(CMAKE_CXX_STANDARD 20)\n"
                                      "set(CMAKE_CXX_STANDARD_REQUIRED ON)\n"
                                      "set(CMAKE_CXX_EXTENSIONS OFF)\n"
                                      "\n"
                                      "add_library({0} STATIC\n"
                                      "    {1}\n"
                                      "    {2}\n"
                                      ")\n"
                                      "target_include_directories({0} PUBLIC ${{CMAKE_CURRENT_SOURCE_DIR}})\n",
                                      targetName, sourceName, headerName)) &&
                  addFile(headerName, std::format("#pragma once\n"
                                                  "\n"
                                                  "namespace {0} {{\n"
                                                  "\n"
                                                  "int add(int a, int b);\n"
                                                  "\n"
                                                  "}} // namespace {0}\n",
                                                  codeIdent)) &&
                  addFile(sourceName,
                          std::format("#include \"{0}\"\n"
                                      "\n"
                                      "namespace {1} {{\n"
                                      "\n"
                                      "int add(int a, int b) {{\n"
                                      "    return a + b;\n"
                                      "}}\n"
                                      "\n"
                                      "}} // namespace {1}\n",
                                      headerName, codeIdent),
                          true);
        if (!files.empty()) {
            cmakeToOpen = files.front().path;
        }
        break;
    }
    case NewProjectTemplate::CMakeSharedLibrary: {
        const std::string headerName = targetName + ".hpp";
        const std::string sourceName = targetName + ".cpp";
        pathsOk = addFile("CMakeLists.txt",
                          std::format("cmake_minimum_required(VERSION 3.20)\n"
                                      "project({0} LANGUAGES CXX)\n"
                                      "\n"
                                      "set(CMAKE_CXX_STANDARD 20)\n"
                                      "set(CMAKE_CXX_STANDARD_REQUIRED ON)\n"
                                      "set(CMAKE_CXX_EXTENSIONS OFF)\n"
                                      "\n"
                                      "add_library({0} SHARED\n"
                                      "    {1}\n"
                                      "    {2}\n"
                                      ")\n"
                                      "target_compile_definitions({0} PRIVATE {3}_EXPORTS)\n"
                                      "target_include_directories({0} PUBLIC ${{CMAKE_CURRENT_SOURCE_DIR}})\n",
                                      targetName, sourceName, headerName, macroPrefix)) &&
                  addFile(headerName, std::format("#pragma once\n"
                                                  "\n"
                                                  "#if defined(_WIN32)\n"
                                                  "#  if defined({0}_EXPORTS)\n"
                                                  "#    define {0}_API __declspec(dllexport)\n"
                                                  "#  else\n"
                                                  "#    define {0}_API __declspec(dllimport)\n"
                                                  "#  endif\n"
                                                  "#else\n"
                                                  "#  define {0}_API\n"
                                                  "#endif\n"
                                                  "\n"
                                                  "namespace {1} {{\n"
                                                  "\n"
                                                  "{0}_API int add(int a, int b);\n"
                                                  "\n"
                                                  "}} // namespace {1}\n",
                                                  macroPrefix, codeIdent)) &&
                  addFile(sourceName,
                          std::format("#include \"{0}\"\n"
                                      "\n"
                                      "namespace {1} {{\n"
                                      "\n"
                                      "int add(int a, int b) {{\n"
                                      "    return a + b;\n"
                                      "}}\n"
                                      "\n"
                                      "}} // namespace {1}\n",
                                      headerName, codeIdent),
                          true);
        if (!files.empty()) {
            cmakeToOpen = files.front().path;
        }
        break;
    }
    case NewProjectTemplate::VsCppConsole:
    case NewProjectTemplate::SlnxCppConsole: {
        const bool isSlnx = spec.kind == NewProjectTemplate::SlnxCppConsole;
        const std::string slnFile = spec.name + (isSlnx ? ".slnx" : ".sln");
        const std::string vcxprojFile = spec.name + ".vcxproj";
        const std::string filtersFile = vcxprojFile + ".filters";
        pathsOk = addFile(slnFile, isSlnx ? buildSlnxContents(vcxprojFile, projectGuid)
                                          : buildSlnContents(spec.name, vcxprojFile, projectGuid)) &&
                  addFile(vcxprojFile, buildVcxprojContents(projectGuid, codeIdent, "Application", "_CONSOLE",
                                                            "Console", "main.cpp", "")) &&
                  addFile(filtersFile, buildVcxprojFiltersContents("main.cpp", "")) &&
                  addFile("main.cpp",
                          std::format("#include <iostream>\n"
                                      "\n"
                                      "int main() {{\n"
                                      "    std::cout << \"Hello from {}!\\n\";\n"
                                      "    return 0;\n"
                                      "}}\n",
                                      targetName),
                          true);
        if (!files.empty()) {
            solutionToOpen = files.front().path;
        }
        break;
    }
    case NewProjectTemplate::VsCppStaticLibrary: {
        const std::string slnFile = spec.name + ".sln";
        const std::string vcxprojFile = spec.name + ".vcxproj";
        const std::string filtersFile = vcxprojFile + ".filters";
        const std::string headerName = targetName + ".h";
        const std::string sourceName = targetName + ".cpp";
        pathsOk = addFile(slnFile, buildSlnContents(spec.name, vcxprojFile, projectGuid)) &&
                  addFile(vcxprojFile, buildVcxprojContents(projectGuid, codeIdent, "StaticLibrary", "_LIB", "Windows",
                                                            sourceName, headerName)) &&
                  addFile(filtersFile, buildVcxprojFiltersContents(sourceName, headerName)) &&
                  addFile(headerName, std::format("#pragma once\n"
                                                  "\n"
                                                  "namespace {0} {{\n"
                                                  "\n"
                                                  "int add(int a, int b);\n"
                                                  "\n"
                                                  "}} // namespace {0}\n",
                                                  codeIdent)) &&
                  addFile(sourceName,
                          std::format("#include \"{0}\"\n"
                                      "\n"
                                      "namespace {1} {{\n"
                                      "\n"
                                      "int add(int a, int b) {{\n"
                                      "    return a + b;\n"
                                      "}}\n"
                                      "\n"
                                      "}} // namespace {1}\n",
                                      headerName, codeIdent),
                          true);
        if (!files.empty()) {
            solutionToOpen = files.front().path;
        }
        break;
    }
    case NewProjectTemplate::VsCppDynamicLibrary: {
        const std::string slnFile = spec.name + ".sln";
        const std::string vcxprojFile = spec.name + ".vcxproj";
        const std::string filtersFile = vcxprojFile + ".filters";
        const std::string headerName = targetName + ".h";
        const std::string sourceName = targetName + ".cpp";
        const std::string defines = macroPrefix + "_EXPORTS;_WINDOWS;_USRDLL";
        pathsOk = addFile(slnFile, buildSlnContents(spec.name, vcxprojFile, projectGuid)) &&
                  addFile(vcxprojFile, buildVcxprojContents(projectGuid, codeIdent, "DynamicLibrary", defines,
                                                            "Windows", sourceName, headerName)) &&
                  addFile(filtersFile, buildVcxprojFiltersContents(sourceName, headerName)) &&
                  addFile(headerName, std::format("#pragma once\n"
                                                  "\n"
                                                  "#if defined(_WIN32)\n"
                                                  "#  if defined({0}_EXPORTS)\n"
                                                  "#    define {0}_API __declspec(dllexport)\n"
                                                  "#  else\n"
                                                  "#    define {0}_API __declspec(dllimport)\n"
                                                  "#  endif\n"
                                                  "#else\n"
                                                  "#  define {0}_API\n"
                                                  "#endif\n"
                                                  "\n"
                                                  "namespace {1} {{\n"
                                                  "\n"
                                                  "{0}_API int add(int a, int b);\n"
                                                  "\n"
                                                  "}} // namespace {1}\n",
                                                  macroPrefix, codeIdent)) &&
                  addFile(sourceName,
                          std::format("#include \"{0}\"\n"
                                      "\n"
                                      "namespace {1} {{\n"
                                      "\n"
                                      "int add(int a, int b) {{\n"
                                      "    return a + b;\n"
                                      "}}\n"
                                      "\n"
                                      "}} // namespace {1}\n",
                                      headerName, codeIdent),
                          true);
        if (!files.empty()) {
            solutionToOpen = files.front().path;
        }
        break;
    }
    case NewProjectTemplate::DotNetConsole: {
        const std::string slnxFile = spec.name + ".slnx";
        const std::string csprojFile = spec.name + ".csproj";
        pathsOk = addFile(slnxFile, std::format("<Solution>\n"
                                                "  <Project Path=\"{}\" />\n"
                                                "</Solution>\n",
                                                csprojFile)) &&
                  addFile(csprojFile, "<Project Sdk=\"Microsoft.NET.Sdk\">\n"
                                      "\n"
                                      "  <PropertyGroup>\n"
                                      "    <OutputType>Exe</OutputType>\n"
                                      "    <TargetFramework>net10.0</TargetFramework>\n"
                                      "    <ImplicitUsings>enable</ImplicitUsings>\n"
                                      "    <Nullable>enable</Nullable>\n"
                                      "  </PropertyGroup>\n"
                                      "\n"
                                      "</Project>\n") &&
                  addFile("Program.cs",
                          std::format("using System;\n"
                                      "\n"
                                      "Console.WriteLine(\"Hello from {}!\");\n",
                                      targetName),
                          true);
        if (!files.empty()) {
            solutionToOpen = files.front().path;
        }
        break;
    }
    case NewProjectTemplate::DotNetClassLibrary: {
        const std::string slnxFile = spec.name + ".slnx";
        const std::string csprojFile = spec.name + ".csproj";
        pathsOk = addFile(slnxFile, std::format("<Solution>\n"
                                                "  <Project Path=\"{}\" />\n"
                                                "</Solution>\n",
                                                csprojFile)) &&
                  addFile(csprojFile, "<Project Sdk=\"Microsoft.NET.Sdk\">\n"
                                      "\n"
                                      "  <PropertyGroup>\n"
                                      "    <TargetFramework>net10.0</TargetFramework>\n"
                                      "    <ImplicitUsings>enable</ImplicitUsings>\n"
                                      "    <Nullable>enable</Nullable>\n"
                                      "  </PropertyGroup>\n"
                                      "\n"
                                      "</Project>\n") &&
                  addFile("Class1.cs",
                          std::format("namespace {};\n"
                                      "\n"
                                      "public class Class1\n"
                                      "{{\n"
                                      "}}\n",
                                      codeIdent),
                          true);
        if (!files.empty()) {
            solutionToOpen = files.front().path;
        }
        break;
    }
    case NewProjectTemplate::EmptyFolder: break;
    }

    if (!pathsOk) {
        if (!dirExisted) {
            (void)m_services.fs.removeDirectory(canonicalDir);
        }
        Error err(ErrorCode::InvalidArgument, "could not construct starter project file paths");
        notify(text("Cannot create project"), qs(err.message()), ui::NotificationKind::Error);
        return std::unexpected(std::move(err));
    }

    const auto cleanupCreated = [&] {
        for (const GeneratedFile& file : files) {
            (void)m_services.fs.removeFile(file.path);
        }
        if (!dirExisted) {
            (void)m_services.fs.removeDirectory(canonicalDir);
        }
    };

    for (const GeneratedFile& file : files) {
        if (Result<void> wrote = m_services.fs.writeFileAtomic(file.path, file.contents); !wrote) {
            cleanupCreated();
            notify(text("Cannot create project"), qs(wrote.error().message()), ui::NotificationKind::Error);
            return std::unexpected(std::move(wrote.error()));
        }
    }

    if (!solutionToOpen.empty()) {
        Result<void> openedSolution = openSolution(solutionToOpen);
        if (!openedSolution) {
            return std::unexpected(std::move(openedSolution.error()));
        }
    } else if (!cmakeToOpen.empty()) {
        Result<void> openedProject = openCMakeProject(cmakeToOpen);
        if (!openedProject) {
            return std::unexpected(std::move(openedProject.error()));
        }
        if (m_window) {
            m_window->toolWindows().show(ui::kFileSystemToolWindow, false);
        }
        if (m_nav && m_nav->fileSystemView() && !primarySourcePath.empty()) {
            m_nav->fileSystemView()->selectWhenAvailable(primarySourcePath);
        }
    } else {
        Result<void> openedFolder = openFolder(canonicalDir, false);
        if (!openedFolder) {
            return std::unexpected(std::move(openedFolder.error()));
        }
        if (m_window) {
            m_window->toolWindows().show(ui::kFileSystemToolWindow, false);
        }
        if (m_nav && m_nav->fileSystemView() && !primarySourcePath.empty()) {
            m_nav->fileSystemView()->selectWhenAvailable(primarySourcePath);
        }
    }

    if (!primarySourcePath.empty()) {
        (void)openPath(primarySourcePath);
    }
    return canonicalDir;
}

Result<std::string> Workbench::createNewFile(std::string_view directory, std::string_view relativeName) {
    if (!m_nav) {
        return fail(ErrorCode::InvalidArgument, "the workbench has no window");
    }
    const std::string baseDir = directory.empty() ? defaultNewItemDirectory() : std::string(directory);
    Result<std::string> created = m_nav->createNewFile(baseDir, relativeName);
    if (created && m_solution && m_buildService->state() == build::BuildState::Idle &&
        !m_buildService->projectEditInProgress() &&
        std::ranges::any_of(m_solution->projects(), [](const project::ProjectInfo& p) {
            return p.kind == project::ProjectKind::CSharp && p.nodeKind == project::NodeKind::Project;
        })) {
        if (auto loaded = project::SolutionModel::load(
                m_services.fs, m_solution->path(),
                {.cppHierarchy = m_cppHierarchyMode, .unloadedProjectPaths = m_unloadedProjectPaths})) {
            auto refreshed = std::make_shared<const project::SolutionModel>(std::move(*loaded));
            m_solution = refreshed;
            if (m_explorer) {
                m_explorer->setSolution(refreshed, true);
                (void)m_explorer->selectPath(*created);
            }
            if (m_nav) {
                (void)m_nav->refreshSolution(*refreshed);
            }
        }
    }
    return created;
}

Result<std::string> Workbench::createNewFolder(std::string_view directory, std::string_view relativeName) {
    if (!m_nav) {
        return fail(ErrorCode::InvalidArgument, "the workbench has no window");
    }
    const std::string baseDir = directory.empty() ? defaultNewItemDirectory() : std::string(directory);
    Result<std::string> created = m_nav->createNewFolder(baseDir, relativeName);
    if (created && m_solution && m_buildService->state() == build::BuildState::Idle &&
        !m_buildService->projectEditInProgress() &&
        std::ranges::any_of(m_solution->projects(), [](const project::ProjectInfo& p) {
            return p.kind == project::ProjectKind::CSharp && p.nodeKind == project::NodeKind::Project;
        })) {
        if (auto loaded = project::SolutionModel::load(
                m_services.fs, m_solution->path(),
                {.cppHierarchy = m_cppHierarchyMode, .unloadedProjectPaths = m_unloadedProjectPaths})) {
            auto refreshed = std::make_shared<const project::SolutionModel>(std::move(*loaded));
            m_solution = refreshed;
            if (m_explorer) {
                m_explorer->setSolution(refreshed, true);
            }
            if (m_nav) {
                (void)m_nav->refreshSolution(*refreshed);
            }
        }
    }
    return created;
}

Result<std::string> Workbench::createNewProjectSource(std::string_view projectPath, std::string_view relativeName,
                                                      std::string_view filter) {
    if (!m_nav) {
        return fail(ErrorCode::InvalidArgument, "the workbench has no window");
    }
    const std::string canonicalProject = m_documentRegistry->canonicalize(projectPath);
    const std::string projectDir(io::parentPath(canonicalProject));
    m_suppressNewFileNotice = true;
    Result<std::string> created = m_nav->createNewFile(projectDir, relativeName);
    m_suppressNewFileNotice = false;
    if (!created) {
        return created;
    }
    // Membership goes through the ProjectHost helper only (SLN-1, FND-3); its failures, including a
    // missing helper, arrive as notifications while the new file stays on disk.
    removeProjectFile(qs(canonicalProject), qs(*created), true, qs(filter));
    return created;
}

void Workbench::onNewFileCreated(const std::string& path) {
    if (m_suppressNewFileNotice || !m_window || !isCTranslationUnit(path)) {
        return;
    }
    if (m_cmake) {
        ui::Notification notice;
        notice.title = text("Created %1").arg(qs(io::fileName(path)));
        notice.text = text("New C/C++ source files are not added to CMake targets automatically.");
        notice.kind = ui::NotificationKind::Info;
        notice.actions.push_back(ui::NotificationAction{
            text("Add to CMakeLists.txt"),
            [self = QPointer<Workbench>(this), path] {
                if (self)
                    (void)self->addSourceToCMakeLists(path);
            },
        });
        notice.actions.push_back(ui::NotificationAction{
            text("Open CMakeLists.txt"),
            [self = QPointer<Workbench>(this)] {
                if (!self || self->m_projectRoot.empty())
                    return;
                if (const auto lists = io::joinPath(self->m_projectRoot, "CMakeLists.txt"))
                    (void)self->openPath(*lists);
            },
        });
        m_window->notifications().notify(std::move(notice));
    }
}

} // namespace frost::app
