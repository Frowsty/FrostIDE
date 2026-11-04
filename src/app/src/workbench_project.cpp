// SPDX-License-Identifier: Apache-2.0
//
// Workbench (frost/app/workbench.hpp), the project side: opening solutions, folders and files,
// closing the project, the last project at start-up, the recent-projects list, and clangd (start
// on solutionCompileCommandsChain, restart on a configuration switch, document sync, live
// diagnostics in Problems and the editors, the "clangd: indexing" background task).

#include "frost/app/workbench.hpp"
#include "frost/build/msbuild_locator.hpp"
#include "frost/core/log.hpp"
#include "frost/io/path.hpp"
#include "frost/lang/compile_commands.hpp"
#include "frost/lang/msbuild_compile_commands.hpp"
#include "frost/lang/msvc_environment.hpp"
#include "frost/lang/uri.hpp"
#include "frost/project/cmake_syntax.hpp"
#include "frost/sched/lane.hpp"
#include "frost/settings/keys.hpp"
#include "frost/ui/lang/editor_insight_binding.hpp"
#include "frost/ui/lang/language_controller.hpp"
#include "frost/ui/run_controller.hpp"
#include "frost/ui/shell/editor_tabs.hpp"
#include "frost/ui/shell/shell_actions.hpp"
#include "frost/ui/shell/tool_window_manager.hpp"
#include "frost/ui/toolwindows/problems.hpp"
#include "frost/ui/toolwindows/solution_explorer.hpp"
#include "frost/ui/tr.hpp"
#include "frost/ui/workbench/file_system_view.hpp"

#include "cmake_integration.hpp"
#include "engine_integration.hpp"
#include "git_integration.hpp"
#include "language_bridge.hpp"
#include "project_edit_process.hpp"
#include "unity_control.hpp"
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

#include <algorithm>
#include <array>
#include <cctype>
#include <format>
#include <functional>
#include <limits>
#include <utility>

namespace frost::app {

namespace {

constexpr std::string_view kLog = "workbench";

// Retain the service until its supervisor and process have finished stopping.
sched::Task<void> stopOwnedClangd(std::shared_ptr<lang::ClangdService> service, sched::Task<void> stopped) {
    (void)service;
    co_await std::move(stopped);
}

sched::Task<void> stopOwnedProjectEvaluation(std::shared_ptr<lang::IndependentProjectEvaluation> service) {
    service->stopAndJoin();
    co_return;
}

QString text(const char* source) { return ui::qtr("frost::app::Workbench", source); }

QString qs(std::string_view utf8) { return QString::fromUtf8(utf8.data(), static_cast<qsizetype>(utf8.size())); }

// The lower-cased extension of the last component, with its dot ("" for none).
std::string extensionOf(std::string_view path) {
    const std::size_t slash = path.find_last_of("/\\");
    const std::string_view name = slash == std::string_view::npos ? path : path.substr(slash + 1);
    const std::size_t dot = name.find_last_of('.');
    if (dot == std::string_view::npos || dot == 0) {
        return {};
    }
    std::string out(name.substr(dot));
    std::ranges::transform(out, out.begin(),
                           [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; });
    return out;
}

// A database directory name for `root`: its last component and a hash of its document key.
std::string cacheNameFor(std::string_view root) {
    const std::size_t slash = root.find_last_of("/\\");
    std::string name(slash == std::string_view::npos ? root : root.substr(slash + 1));
    std::ranges::replace_if(name, [](char c) { return !std::isalnum(static_cast<unsigned char>(c)); }, '_');
    return std::format("{}-{:016x}", name.empty() ? std::string("root") : name,
                       std::hash<std::string>{}(lang::documentKey(root)));
}

bool isWindowsReservedDeviceName(std::string_view component) {
    const std::size_t dot = component.find('.');
    const std::string_view stem = dot == std::string_view::npos ? component : component.substr(0, dot);
    if (stem.empty())
        return false;
    std::string upper(stem);
    std::ranges::transform(upper, upper.begin(),
                           [](char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 32) : c; });
    static constexpr std::array<std::string_view, 22> kReserved{
        "CON",  "PRN",  "AUX",  "NUL",  "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7",
        "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};
    return std::ranges::find(kReserved, std::string_view(upper)) != kReserved.end();
}

Result<void> validateProjectNameComponent(std::string_view name) {
    if (name.empty() || name == "." || name == "..") {
        return fail(ErrorCode::InvalidArgument, "enter a valid project name");
    }
    if (name.back() == ' ' || name.back() == '.') {
        return fail(ErrorCode::InvalidArgument, "a project name cannot end with a space or period");
    }
    for (const char raw : name) {
        const auto ch = static_cast<unsigned char>(raw);
        if (ch < 0x20U || ch == '<' || ch == '>' || ch == ':' || ch == '"' || ch == '/' || ch == '\\' || ch == '|' ||
            ch == '?' || ch == '*') {
            return fail(ErrorCode::InvalidArgument, "the project name contains invalid path characters");
        }
    }
    if (isWindowsReservedDeviceName(name)) {
        return fail(ErrorCode::InvalidArgument, "the project name is a reserved device name");
    }
    return {};
}

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

std::string escapeXml(std::string_view input) {
    std::string out;
    out.reserve(input.size());
    for (const char ch : input) {
        switch (ch) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        case '\'': out += "&apos;"; break;
        default: out.push_back(ch); break;
        }
    }
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

Result<std::string> cmakeRelativeSourcePath(std::string_view projectRoot, std::string_view sourcePath) {
    if (!sourcePath.starts_with(projectRoot) || sourcePath.size() <= projectRoot.size() ||
        (sourcePath[projectRoot.size()] != '/' && sourcePath[projectRoot.size()] != '\\')) {
        return fail(ErrorCode::InvalidArgument, "the source file must be inside the CMake project directory");
    }
    std::string out(sourcePath.substr(projectRoot.size() + 1));
    std::ranges::replace(out, '\\', '/');
    while (out.starts_with('/')) {
        out.erase(out.begin());
    }
    if (out.empty() || out == "." || out == ".." || out.starts_with("../") || out.find("/../") != std::string::npos) {
        return fail(ErrorCode::InvalidArgument, "the source file must be inside the CMake project directory");
    }
    for (const char raw : out) {
        const auto ch = static_cast<unsigned char>(raw);
        if (ch <= 0x20U || ch == '"' || ch == '#' || ch == '$' || ch == '(' || ch == ')' || ch == ';') {
            return fail(ErrorCode::Unsupported,
                        "the source path contains characters that require manual CMakeLists.txt quoting");
        }
    }
    return out;
}

} // namespace

bool isCFamilySource(std::string_view path) noexcept {
    static constexpr std::array<std::string_view, 18> kExtensions{".c",   ".cc",  ".cpp", ".cxx", ".c++", ".cppm",
                                                                  ".ixx", ".h",   ".hh",  ".hpp", ".hxx", ".h++",
                                                                  ".inl", ".ipp", ".tpp", ".m",   ".mm",  ".cu"};
    const std::string extension = extensionOf(path);
    return std::ranges::find(kExtensions, extension) != kExtensions.end();
}

bool isSolutionFile(std::string_view path) noexcept {
    const std::string extension = extensionOf(path);
    return extension == ".sln" || extension == ".slnx";
}

// --- Opening ---

Result<void> Workbench::openCMakeProject(std::string_view path) {
    const std::string canonical = m_documentRegistry->canonicalize(path);
    const auto status = m_services.fs.status(canonical);
    if (!status)
        return std::unexpected(status.error());
    if (status->type != io::FileType::Regular || io::fileName(canonical) != "CMakeLists.txt")
        return fail(ErrorCode::InvalidArgument, "a CMake project must name an existing CMakeLists.txt");
    return openFolder(std::string(io::parentPath(canonical)), false, canonical);
}

Result<void> Workbench::openPath(std::string_view path, FileLocation at) {
    // The registry's canonical spelling (one document per file), the lexical form for a missing one.
    const std::string canonical = m_documentRegistry->canonicalize(path);
    Result<io::FileStatus> status = m_services.fs.status(canonical);
    if (!status) {
        notify(text("Cannot open %1").arg(qs(canonical)), qs(status.error().message()), ui::NotificationKind::Error);
        return std::unexpected(std::move(status.error()));
    }
    if (status->type == io::FileType::Directory) {
        return openFolder(canonical);
    }
    if (extensionOf(canonical) == ".frostproj")
        return openFolder(std::string(io::parentPath(canonical)), false, canonical);
    if (isSolutionFile(canonical)) {
        return openSolution(canonical);
    }
    if (!m_nav) {
        return fail(ErrorCode::InvalidArgument, "the workbench has no window");
    }
    std::optional<ui::LinePosition> position;
    if (at.line > 0) {
        position = ui::LinePosition{at.line, std::max(at.column, 1)};
    }
    // The shell notifies a refusal (the large-file guard, a read error) itself.
    Result<int> opened = m_nav->openFile(canonical, position);
    if (!opened) {
        return std::unexpected(std::move(opened.error()));
    }
    return {};
}

Result<void> Workbench::openExternalProject(std::string_view path) {
    // Resolve the explicit project argument itself: on POSIX DocumentRegistry normally keeps
    // symlink spellings, while project IPC and FrostEngine identify the target directory.
    const QString resolved = QFileInfo(qs(path)).canonicalFilePath();
    const std::string canonical =
        resolved.isEmpty() ? m_documentRegistry->canonicalize(path) : resolved.toUtf8().toStdString();
    auto status = m_services.fs.status(canonical);
    if (!status)
        return std::unexpected(std::move(status.error()));
    if (status->type == io::FileType::Regular && extensionOf(canonical) == ".frostproj")
        return openPath(canonical);
    if (status->type != io::FileType::Directory)
        return fail(ErrorCode::InvalidArgument, "--project requires a directory or .frostproj file");
    auto entries = m_services.fs.listDirectory(canonical);
    if (!entries)
        return std::unexpected(std::move(entries.error()));
    std::vector<std::string> projects;
    for (const auto& entry : *entries) {
        if (entry.type == io::FileType::Regular && extensionOf(entry.name) == ".frostproj")
            projects.push_back(entry.name);
    }
    if (projects.size() > 1)
        return fail(ErrorCode::InvalidArgument, "project directory has multiple .frostproj files");
    if (!projects.empty()) {
        auto file = io::joinPath(canonical, projects.front());
        if (!file)
            return std::unexpected(std::move(file.error()));
        return openPath(*file);
    }
    return openFolder(canonical, false);
}

Result<void> Workbench::openSolution(const std::string& path) {
    if (m_solution && lang::documentKey(m_solution->path()) == lang::documentKey(path)) {
        return {}; // Already the project.
    }
    Result<project::SolutionModel> loaded = project::SolutionModel::load(m_services.fs, path);
    if (!loaded) {
        notify(text("Cannot open %1").arg(qs(path)), qs(loaded.error().message()), ui::NotificationKind::Error);
        return std::unexpected(std::move(loaded.error()));
    }
    if (!closeForOpen()) {
        return fail(ErrorCode::Cancelled, "the current project could not be closed");
    }
    auto model = std::make_shared<const project::SolutionModel>(std::move(*loaded));
    m_solution = model;
    if (auto opened = m_build->openSolution(model); !opened) {
        // The explorer still shows the solution; building is unavailable.
        notify(text("The build service cannot open %1").arg(qs(model->name())), qs(opened.error().message()),
               ui::NotificationKind::Warning);
        if (m_explorer) {
            m_explorer->setSolution(model);
        }
    }
    if (auto journal = io::joinPath(model->directory(), ".frost-project-edit-journal"); journal) {
        const auto exists = m_services.fs.exists(*journal);
        if (!exists || *exists) {
            if (auto blocked = m_buildService->beginProjectEdit(); blocked) {
                notify(text("Project recovery required"),
                       text("A project edit journal is present or cannot be checked. Builds are blocked until it is "
                            "resolved."),
                       ui::NotificationKind::Warning);
            }
        }
    }
    if (auto opened = m_runService->openSolution(model); !opened) {
        log::warn(kLog, "run service: {}", opened.error().describe());
    }
    m_runController->refreshStartupCombo();
    // The navigation workbench: Go to File over the solution's files, the session's tabs.
    if (m_nav) {
        if (auto opened = m_nav->openSolution(*model); !opened) {
            log::warn(kLog, "navigation: {}", opened.error().describe());
        }
    }
    setProject(model->directory(), qs(model->name()), path);
    if (m_window) {
        m_window->toolWindows().show(ui::tool_windows::kProject, false);
    }
    startProjectEvaluation(model->directory());
    startClangd(model->directory());
    startLua(model->directory());
    startManaged(model->directory());
    refreshAllMarkers();
    return {};
}

Result<void> Workbench::openFolder(const std::string& path, bool discoverSolution, std::string recentPath) {
    // A folder with one solution opens that solution (.sln before .slnx of the same name).
    if (Result<std::vector<io::DirEntry>> entries = m_services.fs.listDirectory(path); discoverSolution && entries) {
        std::vector<std::string> solutions;
        for (const io::DirEntry& entry : *entries) {
            if (entry.type == io::FileType::Regular && isSolutionFile(entry.name)) {
                solutions.push_back(entry.name);
            }
        }
        std::ranges::sort(solutions);
        const auto stem = [](const std::string& name) { return name.substr(0, name.find_last_of('.')); };
        const bool oneSolution = !solutions.empty() && std::ranges::all_of(solutions, [&](const std::string& name) {
            return stem(name) == stem(solutions.front());
        });
        if (oneSolution) {
            if (Result<std::string> solution = io::joinPath(path, solutions.front()); solution) {
                return openSolution(*solution);
            }
        }
    }
    if (recentPath.empty())
        recentPath = path;
    if (m_projectRoot == path && !m_solution) {
        // The same directory may have been opened by a folder or a .frostproj. Preserve the
        // caller's explicit choice for Open Recent and the next process's session restore.
        if (auto added = addRecentProject(m_services.settings, recentPath); !added) {
            log::warn(kLog, "recent projects: {}", added.error().describe());
        }
        refreshRecent();
        m_projectIdentity = recentPath;
        if (m_sessions) {
            if (auto saved = m_sessions->setLastProject(m_projectIdentity); !saved)
                log::warn(kLog, "last project: {}", saved.error().describe());
        }
        return {};
    }
    if (!closeForOpen()) {
        return fail(ErrorCode::Cancelled, "the current project could not be closed");
    }
    const auto lists = io::joinPath(path, "CMakeLists.txt");
    if (m_window && lists && m_services.fs.exists(*lists).value_or(false)) {
        m_cmake = std::make_unique<CMakeIntegration>(
            m_services.fs, m_services.launcher, m_services.trust, m_services.settings, *m_build, *m_buildService,
            *m_window, *m_runConsole, m_options.cmakeStatePath, [this] { m_runController->alternateStateChanged(); },
            [this](const std::string& file) { (void)openPath(file); },
            [this] {
                if (m_cmake && m_clangd)
                    restartClangd();
            });
        if (auto opened = m_cmake->open(path); !opened) {
            m_cmake.reset();
            return std::unexpected(std::move(opened.error()));
        }
    }
    if (m_nav) {
        if (auto opened = m_nav->openProject(path); !opened) {
            log::warn(kLog, "navigation: {}", opened.error().describe());
        }
        if (m_cmake) {
            m_cmake->setTargetPreferences(m_nav->cmakeTargets(), [this](const auto& targets) {
                if (m_nav)
                    m_nav->setCMakeTargets(targets);
            });
            m_cmake->setArgumentPreferences(m_nav->cmakeArguments(), [this](const auto& arguments) {
                if (m_nav)
                    m_nav->setCMakeArguments(arguments);
            });
        }
    }
    const std::string_view name = io::fileName(path);
    setProject(path, qs(name.empty() ? std::string_view(path) : name), std::move(recentPath));
    if (m_window) {
        // A folder has no solution to show: the File System view takes the Solution Explorer's place.
        m_window->toolWindows().hide(ui::tool_windows::kProject);
        if (!m_cmake)
            m_window->toolWindows().show(ui::kFileSystemToolWindow, false);
    }
    startClangd(path);
    startLua(path);
    startManaged(path);
    return {};
}

void Workbench::setProject(std::string root, QString name, std::string recentPath) {
    invalidateManagedDebug();
    cancelFind();
    if (m_debug)
        m_debug->cancelRerun();
    m_projectRoot = std::move(root);
    if (m_git)
        m_git->openProject(m_projectRoot);
    if (m_debug) {
        if (auto loaded = m_debug->setWorkspaceRoot(m_projectRoot); !loaded)
            log::warn(kLog, "debug breakpoints: {}", loaded.error().describe());
    }
    m_projectIdentity = recentPath;
    if (m_sessions) {
        if (auto saved = m_sessions->setLastProject(m_projectIdentity); !saved)
            log::warn(kLog, "last project: {}", saved.error().describe());
    }
    if (auto added = addRecentProject(m_services.settings, recentPath); !added) {
        log::warn(kLog, "recent projects: {}", added.error().describe());
    }
    refreshRecent();
    if (m_window) {
        m_window->setProject(name);
        m_window->tabs().setEmptyStateWidget(nullptr);
        m_window->refreshActions();
    }
    emit projectChanged(name);
}

bool Workbench::closeForOpen() {
    if (m_projectEdit && m_projectEdit->busy()) {
        notify(text("The project stays open"), text("Wait for the project edit to finish."),
               ui::NotificationKind::Warning);
        return false;
    }
    m_language->structureChanged();
    invalidateManagedDebug();
    if (m_engine)
        m_engine->cancel();
    if (m_unity)
        m_unity->cancel();
    if (!hasProject()) {
        return true;
    }
    // Check the services themselves: the controller's queued state can lag a newly started build.
    // Keep the current project and its tabs intact until the user stops the build or run.
    if ((m_debug && m_debug->debugging()) ||
        (m_solution && (m_buildService->state() != build::BuildState::Idle || !m_runService->activeRuns().empty())) ||
        (m_cmake && (m_cmake->service().state() != build::BuildState::Idle || m_cmake->running()))) {
        notify(text("The project stays open"), text("Stop the build and running programs before closing the project."),
               ui::NotificationKind::Warning);
        return false;
    }
    // A save-failure prompt can run a nested event loop, so cancel a queued rerun before
    // closing any tab. A cancelled close also leaves that rerun cancelled.
    if (m_debug)
        m_debug->cancelRerun();
    // Stop background operations before closing project documents.
    cancelFind();
    if (m_git)
        m_git->closeProject();
    // The session is saved and every tab saved and closed; a refusal keeps everything open.
    if (m_nav) {
        if (!m_nav->closeProject()) {
            if (m_git)
                m_git->openProject(m_projectRoot);
            return false;
        }
    } else if (m_window) {
        m_window->tabs().closeAll();
        if (m_window->tabs().totalCount() > 0) {
            if (m_git)
                m_git->openProject(m_projectRoot);
            return false;
        }
    }
    m_closingSolution = true;
    stopLua();
    stopManaged();
    stopClangd();
    stopProjectEvaluation();
    if (m_cmake) {
        m_build->setBackend(m_buildBackend.get());
        m_cmake.reset();
    }
    if (m_solution) {
        if (m_buildService->projectEditInProgress())
            m_buildService->cancelProjectEdit(); // The unresolved on-disk journal remains untouched.
        if (auto closed = m_build->closeSolution(); !closed) {
            log::warn(kLog, "closing the solution: {}", closed.error().describe());
        }
        m_runService->stopAll();
        if (auto closed = m_runService->closeSolution(); !closed) {
            log::warn(kLog, "closing the solution's runs: {}", closed.error().describe());
        }
        m_runController->refreshStartupCombo();
        if (m_explorer) {
            m_explorer->setSolution(nullptr);
        }
    }
    m_solution.reset();
    m_closingSolution = false;
    if (m_debug)
        (void)m_debug->setWorkspaceRoot({});
    m_projectRoot.clear();
    m_projectIdentity.clear();
    if (m_window) {
        m_window->setProject(QString());
        m_window->tabs().setEmptyStateWidget(m_welcome);
        m_window->refreshActions();
    }
    refreshRecent();
    emit projectChanged(QString());
    return true;
}

bool Workbench::closeProject() {
    if (!hasProject()) {
        return true;
    }
    if (!closeForOpen()) {
        return false;
    }
    // Closed on purpose: the next start shows the Welcome screen (Rider), not this project.
    if (m_sessions) {
        if (auto cleared = m_sessions->clearLastProject(); !cleared) {
            log::warn(kLog, "last project: {}", cleared.error().describe());
        }
    }
    return true;
}

bool Workbench::restoreLastProject() {
    if (!m_nav || !m_services.settings.snapshot()->get(settings::key::generalReopenLastProject)) {
        return false;
    }
    return m_nav->restoreLastProject();
}

Result<void> Workbench::openProjectIdentity(const std::string& path) {
    return io::fileName(path) == "CMakeLists.txt" ? openCMakeProject(path) : openPath(path);
}

Result<void> Workbench::reopenRoot(const std::string& root) {
    if (io::fileName(root) == "CMakeLists.txt")
        return openCMakeProject(root);
    if (isSolutionFile(root) || extensionOf(root) == ".frostproj")
        return openPath(root);
    // Older session stores keep roots; the recent projects say which solution in it the user opened.
    const std::string key = lang::documentKey(root);
    for (const std::string& recent : recentProjects(*m_services.settings.snapshot())) {
        const std::string recentRoot =
            isSolutionFile(recent) || extensionOf(recent) == ".frostproj" || io::fileName(recent) == "CMakeLists.txt"
                ? std::string(io::parentPath(recent))
                : m_documentRegistry->canonicalize(recent);
        if (lang::documentKey(m_documentRegistry->canonicalize(recentRoot)) == key) {
            return openProjectIdentity(recent);
        }
    }
    return openPath(root);
}

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

    const std::string canonicalDir = m_documentRegistry->canonicalize(*targetDir);
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
        if (auto loaded = project::SolutionModel::load(m_services.fs, m_solution->path())) {
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
        if (auto loaded = project::SolutionModel::load(m_services.fs, m_solution->path())) {
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

Result<std::string> Workbench::createNewProjectSource(std::string_view projectPath, std::string_view relativeName) {
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
    removeProjectFile(qs(canonicalProject), qs(*created), true);
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

Result<void> Workbench::addSourceToCMakeLists(std::string_view sourcePath, std::string_view targetName) {
    if (m_projectRoot.empty()) {
        return fail(ErrorCode::InvalidArgument, "no project is open");
    }
    Result<std::string> listsPath = io::joinPath(m_projectRoot, "CMakeLists.txt");
    if (!listsPath) {
        return std::unexpected(std::move(listsPath.error()));
    }
    const std::string canonicalLists = m_documentRegistry->canonicalize(*listsPath);
    if (!m_services.fs.exists(canonicalLists).value_or(false)) {
        return fail(ErrorCode::NotFound, "CMakeLists.txt was not found in the project root");
    }
    if (const document::FileDocument* openLists = m_documentRegistry->find(canonicalLists);
        openLists != nullptr && openLists->dirty()) {
        Error err(ErrorCode::Busy, "save or revert unsaved changes in CMakeLists.txt before adding a source file");
        notify(text("Cannot update CMakeLists.txt"), qs(err.message()), ui::NotificationKind::Warning);
        return std::unexpected(std::move(err));
    }

    const std::string canonicalRoot = m_documentRegistry->canonicalize(m_projectRoot);
    const std::string canonicalSource = m_documentRegistry->canonicalize(sourcePath);
    if (!m_services.fs.exists(canonicalSource).value_or(false)) {
        return fail(ErrorCode::NotFound, "source file does not exist: " + canonicalSource);
    }
    Result<std::string> relSource = cmakeRelativeSourcePath(canonicalRoot, canonicalSource);
    if (!relSource) {
        notify(text("Manual CMakeLists.txt edit required"), qs(relSource.error().message()),
               ui::NotificationKind::Warning);
        return std::unexpected(std::move(relSource.error()));
    }

    Result<std::string> content = m_services.fs.readFile(canonicalLists);
    if (!content) {
        notify(text("Cannot read CMakeLists.txt"), qs(content.error().message()), ui::NotificationKind::Error);
        return std::unexpected(std::move(content.error()));
    }

    Result<std::vector<project::CMakeSyntaxCommand>> commands = project::scanCMakeSyntax(*content);
    if (!commands) {
        notify(text("Manual CMakeLists.txt edit required"), qs(commands.error().message()),
               ui::NotificationKind::Warning);
        return std::unexpected(std::move(commands.error()));
    }

    std::vector<const project::CMakeSyntaxCommand*> candidates;
    for (const project::CMakeSyntaxCommand& cmd : *commands) {
        if (cmd.scopeDepth != 0 || cmd.nestedArguments || cmd.name == "add_subdirectory" || cmd.name == "include") {
            Error err(ErrorCode::Unsupported,
                      "CMakeLists.txt uses conditional or multi-directory commands; update it manually");
            notify(text("Manual CMakeLists.txt edit required"), qs(err.message()), ui::NotificationKind::Warning);
            return std::unexpected(std::move(err));
        }
        if (cmd.name != "add_executable" && cmd.name != "add_library") {
            continue;
        }
        if (cmd.arguments.empty() || !std::ranges::all_of(cmd.arguments, &project::CMakeSyntaxArgument::isLiteral)) {
            Error err(ErrorCode::Unsupported,
                      "CMake target declaration uses non-literal arguments; update CMakeLists.txt manually");
            notify(text("Manual CMakeLists.txt edit required"), qs(err.message()), ui::NotificationKind::Warning);
            return std::unexpected(std::move(err));
        }
        const bool isImportedOrAlias = std::ranges::any_of(cmd.arguments, [](const project::CMakeSyntaxArgument& arg) {
            return arg.literal == "IMPORTED" || arg.literal == "ALIAS" || arg.literal == "INTERFACE";
        });
        if (isImportedOrAlias) {
            continue;
        }
        if (!targetName.empty() && cmd.arguments.front().literal != targetName) {
            continue;
        }
        candidates.push_back(&cmd);
    }

    if (candidates.size() != 1) {
        Error err(ErrorCode::Unsupported, "CMakeLists.txt does not have a single simple top-level C/C++ target");
        notify(text("Manual CMakeLists.txt edit required"), qs(err.message()), ui::NotificationKind::Warning);
        return std::unexpected(std::move(err));
    }

    const project::CMakeSyntaxCommand* selected = candidates.front();
    for (std::size_t i = 1; i < selected->arguments.size(); ++i) {
        if (selected->arguments[i].literal == *relSource) {
            return {};
        }
    }

    const std::string_view newline = content->find("\r\n") != std::string::npos ? "\r\n" : "\n";
    const std::size_t firstArgBegin = selected->arguments.front().begin;
    const std::size_t closePos = selected->close;
    const std::string_view spanView = std::string_view(*content).substr(firstArgBegin, closePos - firstArgBegin);
    const bool multiline = spanView.find('\n') != std::string_view::npos;

    std::string updated = *content;
    if (multiline) {
        std::size_t lineStart = closePos;
        while (lineStart > 0 && (updated[lineStart - 1] == ' ' || updated[lineStart - 1] == '\t')) {
            --lineStart;
        }
        if (lineStart > 0 && updated[lineStart - 1] == '\n') {
            updated.insert(lineStart, std::format("    {}{}", *relSource, newline));
        } else {
            updated.insert(closePos, std::format("{}    {}{}", newline, *relSource, newline));
        }
    } else {
        updated.insert(closePos, std::format(" {}", *relSource));
    }

    if (Result<std::vector<project::CMakeSyntaxCommand>> verified = project::scanCMakeSyntax(updated); !verified) {
        return std::unexpected(std::move(verified.error()));
    }
    if (Result<io::SafeWriteResult> written = m_services.fs.safeWrite(canonicalLists, updated, {}); !written) {
        notify(text("Cannot update CMakeLists.txt"), qs(written.error().message()), ui::NotificationKind::Error);
        return std::unexpected(std::move(written.error()));
    }
    if (document::FileDocument* openLists = m_documents->find(canonicalLists); openLists != nullptr) {
        (void)m_documents->checkDisk(*openLists);
        m_documentRegistry->notifyChanged(canonicalLists);
    }
    notify(text("CMakeLists.txt updated"),
           text("Added %1 to target %2.").arg(qs(*relSource), qs(selected->arguments.front().literal)),
           ui::NotificationKind::Info);
    return {};
}

std::string Workbench::defaultTargetPathForModify() const {
    const bool filesVisible = m_window && m_window->toolWindows().isVisible(ui::kFileSystemToolWindow);
    const bool projectVisible = m_window && m_window->toolWindows().isVisible(ui::tool_windows::kProject);
    if (m_solution && m_explorer && (projectVisible || !filesVisible)) {
        if (const auto nodeId = m_explorer->currentNode()) {
            const auto& node = m_solution->node(*nodeId);
            if ((node.kind == project::NodeKind::File || node.kind == project::NodeKind::Filter) &&
                !node.path.empty()) {
                return node.path;
            }
        }
    }
    if (m_nav && m_nav->fileSystemView()) {
        const std::string selected = m_nav->fileSystemView()->selectedPath();
        if (!selected.empty() && selected != m_projectRoot) {
            return selected;
        }
    }
    if (m_solution && m_explorer) {
        if (const auto nodeId = m_explorer->currentNode()) {
            const auto& node = m_solution->node(*nodeId);
            if ((node.kind == project::NodeKind::File || node.kind == project::NodeKind::Filter) &&
                !node.path.empty()) {
                return node.path;
            }
        }
    }
    if (const DocumentEditorView* active = currentView(); active != nullptr && !active->path().empty()) {
        return active->path();
    }
    return {};
}

void Workbench::refreshSolutionExplorerForFsChange(std::string_view selectPath) {
    if (!m_solution || m_buildService->state() != build::BuildState::Idle || m_buildService->projectEditInProgress()) {
        return;
    }
    if (auto loaded = project::SolutionModel::load(m_services.fs, m_solution->path())) {
        auto refreshed = std::make_shared<const project::SolutionModel>(std::move(*loaded));
        if (m_buildService->beginProjectEdit()) {
            (void)m_buildService->finishProjectEdit(refreshed);
        }
        m_solution = refreshed;
        if (m_build) {
            m_build->projectEditPublished();
        }
        if (m_runService) {
            (void)m_runService->openSolution(refreshed);
        }
        if (m_explorer) {
            m_explorer->setSolution(refreshed, true);
            if (!selectPath.empty()) {
                (void)m_explorer->selectPath(selectPath);
            }
        }
        if (m_nav) {
            (void)m_nav->refreshSolution(*refreshed);
        }
    }
}

void Workbench::renameSourceInCMakeLists(const std::string& oldPath, const std::string& newPath, bool directory) {
    if (m_projectRoot.empty()) {
        return;
    }
    const auto listsPath = io::joinPath(m_projectRoot, "CMakeLists.txt");
    if (!listsPath) {
        return;
    }
    const std::string canonicalLists = m_documentRegistry->canonicalize(*listsPath);
    if (!m_services.fs.exists(canonicalLists).value_or(false)) {
        return;
    }
    if (const document::FileDocument* openLists = m_documentRegistry->find(canonicalLists);
        openLists != nullptr && openLists->dirty()) {
        return;
    }
    const std::string canonicalRoot = m_documentRegistry->canonicalize(m_projectRoot);
    const auto relOld = cmakeRelativeSourcePath(canonicalRoot, oldPath);
    const auto relNew = cmakeRelativeSourcePath(canonicalRoot, newPath);
    if (!relOld || !relNew) {
        return;
    }
    const auto content = m_services.fs.readFile(canonicalLists);
    if (!content) {
        return;
    }
    const auto commands = project::scanCMakeSyntax(*content);
    if (!commands) {
        return;
    }
    struct Replacement {
        std::size_t begin;
        std::size_t end;
        std::string text;
    };
    std::vector<Replacement> edits;
    const std::string oldPrefix = *relOld + "/";
    const std::string newPrefix = *relNew + "/";
    for (const project::CMakeSyntaxCommand& cmd : *commands) {
        if (cmd.name != "add_executable" && cmd.name != "add_library" && cmd.name != "target_sources") {
            continue;
        }
        for (std::size_t i = 1; i < cmd.arguments.size(); ++i) {
            const auto& arg = cmd.arguments[i];
            if (!arg.isLiteral) {
                continue;
            }
            if (!directory && arg.literal == *relOld) {
                const bool quoted = (*content)[arg.begin] == '"';
                edits.push_back(Replacement{arg.begin, arg.end, quoted ? "\"" + *relNew + "\"" : *relNew});
            } else if (directory && arg.literal.starts_with(oldPrefix)) {
                const std::string replaced = newPrefix + arg.literal.substr(oldPrefix.size());
                const bool quoted = (*content)[arg.begin] == '"';
                edits.push_back(Replacement{arg.begin, arg.end, quoted ? "\"" + replaced + "\"" : replaced});
            }
        }
    }
    if (edits.empty()) {
        return;
    }
    std::string updated = *content;
    for (auto it = edits.rbegin(); it != edits.rend(); ++it) {
        updated.replace(it->begin, it->end - it->begin, it->text);
    }
    if (!project::scanCMakeSyntax(updated)) {
        return;
    }
    if (m_services.fs.safeWrite(canonicalLists, updated, {})) {
        if (document::FileDocument* openLists = m_documents->find(canonicalLists); openLists != nullptr) {
            (void)m_documents->checkDisk(*openLists);
            m_documentRegistry->notifyChanged(canonicalLists);
        }
    }
}

void Workbench::removeSourceFromCMakeLists(const std::string& deletedPath, bool directory) {
    if (m_projectRoot.empty()) {
        return;
    }
    const auto listsPath = io::joinPath(m_projectRoot, "CMakeLists.txt");
    if (!listsPath) {
        return;
    }
    const std::string canonicalLists = m_documentRegistry->canonicalize(*listsPath);
    if (!m_services.fs.exists(canonicalLists).value_or(false)) {
        return;
    }
    if (const document::FileDocument* openLists = m_documentRegistry->find(canonicalLists);
        openLists != nullptr && openLists->dirty()) {
        return;
    }
    const std::string canonicalRoot = m_documentRegistry->canonicalize(m_projectRoot);
    const auto relDeleted = cmakeRelativeSourcePath(canonicalRoot, deletedPath);
    if (!relDeleted) {
        return;
    }
    const auto content = m_services.fs.readFile(canonicalLists);
    if (!content) {
        return;
    }
    const auto commands = project::scanCMakeSyntax(*content);
    if (!commands) {
        return;
    }
    std::vector<std::pair<std::size_t, std::size_t>> spans;
    const std::string prefix = *relDeleted + "/";
    for (const project::CMakeSyntaxCommand& cmd : *commands) {
        if (cmd.name != "add_executable" && cmd.name != "add_library" && cmd.name != "target_sources") {
            continue;
        }
        for (std::size_t i = 1; i < cmd.arguments.size(); ++i) {
            const auto& arg = cmd.arguments[i];
            if (!arg.isLiteral) {
                continue;
            }
            if ((!directory && arg.literal == *relDeleted) || (directory && arg.literal.starts_with(prefix))) {
                std::size_t b = arg.begin;
                std::size_t e = arg.end;
                while (b > 0 && ((*content)[b - 1] == ' ' || (*content)[b - 1] == '\t')) {
                    --b;
                }
                if (b > 0 && (*content)[b - 1] == '\n' && e < content->size() &&
                    ((*content)[e] == '\n' ||
                     ((*content)[e] == '\r' && e + 1 < content->size() && (*content)[e + 1] == '\n'))) {
                    e += ((*content)[e] == '\r') ? 2 : 1;
                }
                spans.emplace_back(b, e);
            }
        }
    }
    if (spans.empty()) {
        return;
    }
    std::string updated = *content;
    for (auto it = spans.rbegin(); it != spans.rend(); ++it) {
        updated.erase(it->first, it->second - it->first);
    }
    if (!project::scanCMakeSyntax(updated)) {
        return;
    }
    if (m_services.fs.safeWrite(canonicalLists, updated, {})) {
        if (document::FileDocument* openLists = m_documents->find(canonicalLists); openLists != nullptr) {
            (void)m_documents->checkDisk(*openLists);
            m_documentRegistry->notifyChanged(canonicalLists);
        }
    }
}

void Workbench::updatePathInVcxproj(const std::string& oldPath, const std::string& newPath, bool directory,
                                    bool deleted) {
    if (!m_solution) {
        return;
    }
    const auto updateXmlFile = [&](const std::string& xmlFile, const std::string& projDir) {
        if (!m_services.fs.exists(xmlFile).value_or(false)) {
            return;
        }
        if (const document::FileDocument* openXml = m_documentRegistry->find(xmlFile);
            openXml != nullptr && openXml->dirty()) {
            return;
        }
        auto content = m_services.fs.readFile(xmlFile);
        if (!content) {
            return;
        }
        const auto relToProj = [&](const std::string& p) -> std::optional<std::string> {
            const std::string projKey = lang::documentKey(projDir);
            const std::string pKey = lang::documentKey(p);
            if (!pKey.starts_with(projKey) || pKey.size() <= projKey.size()) {
                return std::nullopt;
            }
            const char sep = p[projDir.size()];
            if (sep != '/' && sep != '\\') {
                return std::nullopt;
            }
            std::string rel = p.substr(projDir.size() + 1);
            while (!rel.empty() && (rel.front() == '/' || rel.front() == '\\')) {
                rel.erase(rel.begin());
            }
            return rel;
        };
        const auto relOld = relToProj(oldPath);
        if (!relOld || relOld->empty()) {
            return;
        }
        std::string relOldBack = *relOld;
        std::ranges::replace(relOldBack, '/', '\\');
        std::string relOldFwd = *relOld;
        std::ranges::replace(relOldFwd, '\\', '/');

        std::string updated = *content;
        bool changed = false;
        if (!deleted) {
            const auto relNew = relToProj(newPath);
            if (!relNew || relNew->empty()) {
                return;
            }
            std::string relNewBack = *relNew;
            std::ranges::replace(relNewBack, '/', '\\');
            std::string relNewFwd = *relNew;
            std::ranges::replace(relNewFwd, '\\', '/');
            const auto replaceAll = [&](const std::string& from, const std::string& to) {
                std::size_t pos = 0;
                while ((pos = updated.find(from, pos)) != std::string::npos) {
                    updated.replace(pos, from.size(), to);
                    pos += to.size();
                    changed = true;
                }
            };
            if (!directory) {
                replaceAll("Include=\"" + escapeXml(relOldBack) + "\"", "Include=\"" + escapeXml(relNewBack) + "\"");
                replaceAll("Include=\"" + escapeXml(relOldFwd) + "\"", "Include=\"" + escapeXml(relNewFwd) + "\"");
            } else {
                replaceAll("Include=\"" + escapeXml(relOldBack) + "\\", "Include=\"" + escapeXml(relNewBack) + "\\");
                replaceAll("Include=\"" + escapeXml(relOldFwd) + "/", "Include=\"" + escapeXml(relNewFwd) + "/");
            }
        } else {
            const auto removeMatchingAttr = [&](const std::string& attrPattern, bool prefixMatch) {
                std::size_t pos = 0;
                while ((pos = updated.find(attrPattern, pos)) != std::string::npos) {
                    if (!prefixMatch) {
                        const std::size_t after = pos + attrPattern.size();
                        if (after >= updated.size() || updated[after] != '"') {
                            pos += attrPattern.size();
                            continue;
                        }
                    }
                    std::size_t tagStart = updated.rfind('<', pos);
                    if (tagStart == std::string::npos) {
                        break;
                    }
                    const std::size_t tagClose = updated.find('>', pos);
                    if (tagClose == std::string::npos) {
                        break;
                    }
                    std::size_t eraseEnd = tagClose + 1;
                    if (tagClose > 0 && updated[tagClose - 1] != '/') {
                        const std::size_t nameEnd = updated.find_first_of(" \t\r\n>", tagStart + 1);
                        if (nameEnd != std::string::npos) {
                            const std::string closeTag =
                                "</" + updated.substr(tagStart + 1, nameEnd - (tagStart + 1)) + ">";
                            if (const std::size_t closePos = updated.find(closeTag, tagClose + 1);
                                closePos != std::string::npos) {
                                eraseEnd = closePos + closeTag.size();
                            }
                        }
                    }
                    while (tagStart > 0 && (updated[tagStart - 1] == ' ' || updated[tagStart - 1] == '\t')) {
                        --tagStart;
                    }
                    if (eraseEnd < updated.size() && updated[eraseEnd] == '\r') {
                        ++eraseEnd;
                    }
                    if (eraseEnd < updated.size() && updated[eraseEnd] == '\n') {
                        ++eraseEnd;
                    }
                    updated.erase(tagStart, eraseEnd - tagStart);
                    pos = tagStart;
                    changed = true;
                }
            };
            if (!directory) {
                removeMatchingAttr("Include=\"" + escapeXml(relOldBack), false);
                removeMatchingAttr("Include=\"" + escapeXml(relOldFwd), false);
            } else {
                removeMatchingAttr("Include=\"" + escapeXml(relOldBack) + "\\", true);
                removeMatchingAttr("Include=\"" + escapeXml(relOldFwd) + "/", true);
            }
        }
        if (changed) {
            if (m_services.fs.safeWrite(xmlFile, updated, {})) {
                if (document::FileDocument* openXml = m_documents->find(xmlFile); openXml != nullptr) {
                    (void)m_documents->checkDisk(*openXml);
                    m_documentRegistry->notifyChanged(xmlFile);
                }
            }
        }
    };

    for (const project::ProjectInfo& proj : m_solution->projects()) {
        if ((proj.kind != project::ProjectKind::Cpp && proj.kind != project::ProjectKind::CSharp) ||
            proj.nodeKind != project::NodeKind::Project || proj.path.empty()) {
            continue;
        }
        const std::string projDir(io::parentPath(proj.path));
        updateXmlFile(proj.path, projDir);
        if (proj.kind == project::ProjectKind::Cpp) {
            updateXmlFile(proj.path + ".filters", projDir);
        }
    }
}

Result<std::string> Workbench::renameItem(std::string_view oldPath, std::string_view newRelativeName) {
    if (oldPath.empty()) {
        return fail(ErrorCode::InvalidArgument, "select a file or folder to rename");
    }
    if (Result<void> validName = validateProjectNameComponent(newRelativeName); !validName) {
        notify(text("Cannot rename"), qs(validName.error().message()), ui::NotificationKind::Error);
        return std::unexpected(std::move(validName.error()));
    }
    const std::string canonicalOld = m_documentRegistry->canonicalize(oldPath);
    if (!m_projectRoot.empty() && lang::documentKey(canonicalOld) == lang::documentKey(m_projectRoot)) {
        Error err(ErrorCode::InvalidArgument, "cannot rename the project root folder");
        notify(text("Cannot rename"), qs(err.message()), ui::NotificationKind::Error);
        return std::unexpected(std::move(err));
    }
    auto status = m_services.fs.status(canonicalOld);
    if (!status) {
        notify(text("Cannot rename %1").arg(qs(io::fileName(canonicalOld))), qs(status.error().message()),
               ui::NotificationKind::Error);
        return std::unexpected(std::move(status.error()));
    }
    const bool isDir = status->type == io::FileType::Directory;
    if (io::fileName(canonicalOld) == newRelativeName) {
        return canonicalOld;
    }
    const std::string parentDir(io::parentPath(canonicalOld));
    auto targetJoined = io::joinPath(parentDir, newRelativeName);
    if (!targetJoined) {
        notify(text("Cannot rename %1").arg(qs(io::fileName(canonicalOld))), qs(targetJoined.error().message()),
               ui::NotificationKind::Error);
        return std::unexpected(std::move(targetJoined.error()));
    }
    if (m_services.fs.exists(*targetJoined).value_or(false)) {
        Error err(ErrorCode::AlreadyExists, "'" + std::string(newRelativeName) + "' already exists");
        notify(text("Cannot rename %1").arg(qs(io::fileName(canonicalOld))), qs(err.message()),
               ui::NotificationKind::Error);
        return std::unexpected(std::move(err));
    }
    if (auto renamed = m_services.fs.rename(canonicalOld, *targetJoined); !renamed) {
        notify(text("Cannot rename %1").arg(qs(io::fileName(canonicalOld))), qs(renamed.error().message()),
               ui::NotificationKind::Error);
        return std::unexpected(std::move(renamed.error()));
    }
    const std::string canonicalNew = m_documentRegistry->canonicalize(*targetJoined);

    if (!isDir) {
        rebindRenamedDocument(canonicalOld, canonicalNew);
    } else {
        const std::string oldDirKey = lang::documentKey(canonicalOld);
        std::vector<std::pair<std::string, std::string>> docRenames;
        for (const auto& doc : m_documents->documents()) {
            if (doc->untitled()) {
                continue;
            }
            const std::string docKey = lang::documentKey(doc->path());
            if (docKey.starts_with(oldDirKey) && docKey.size() > oldDirKey.size() &&
                (doc->path()[canonicalOld.size()] == '/' || doc->path()[canonicalOld.size()] == '\\')) {
                const std::string suffix = doc->path().substr(canonicalOld.size() + 1);
                if (auto joined = io::joinPath(canonicalNew, suffix); joined) {
                    docRenames.emplace_back(doc->path(), m_documentRegistry->canonicalize(*joined));
                }
            }
        }
        for (const auto& [oldDoc, newDoc] : docRenames) {
            rebindRenamedDocument(oldDoc, newDoc);
        }
        if (m_nav) {
            m_nav->notifyPathRenamed(canonicalOld, canonicalNew);
        }
    }

    renameSourceInCMakeLists(canonicalOld, canonicalNew, isDir);
    updatePathInVcxproj(canonicalOld, canonicalNew, isDir, false);
    refreshSolutionExplorerForFsChange(isDir ? std::string_view{} : std::string_view(canonicalNew));
    return canonicalNew;
}

Result<void> Workbench::deleteItem(std::string_view path) {
    if (path.empty()) {
        return fail(ErrorCode::InvalidArgument, "select a file or folder to delete");
    }
    const std::string canonical = m_documentRegistry->canonicalize(path);
    if (!m_projectRoot.empty() && lang::documentKey(canonical) == lang::documentKey(m_projectRoot)) {
        Error err(ErrorCode::InvalidArgument, "cannot delete the project root folder");
        notify(text("Cannot delete"), qs(err.message()), ui::NotificationKind::Error);
        return std::unexpected(std::move(err));
    }
    if (m_solution && lang::documentKey(canonical) == lang::documentKey(m_solution->path())) {
        Error err(ErrorCode::InvalidArgument, "cannot delete the open solution file");
        notify(text("Cannot delete"), qs(err.message()), ui::NotificationKind::Error);
        return std::unexpected(std::move(err));
    }
    auto status = m_services.fs.status(canonical);
    if (!status) {
        notify(text("Cannot delete %1").arg(qs(io::fileName(canonical))), qs(status.error().message()),
               ui::NotificationKind::Error);
        return std::unexpected(std::move(status.error()));
    }
    const bool isDir = status->type == io::FileType::Directory;
    if (isDir) {
        if (auto removed = m_services.fs.removeAll(canonical); !removed) {
            notify(text("Cannot delete %1").arg(qs(io::fileName(canonical))), qs(removed.error().message()),
                   ui::NotificationKind::Error);
            return std::unexpected(std::move(removed.error()));
        }
    } else {
        if (auto removed = m_services.fs.remove(canonical); !removed) {
            notify(text("Cannot delete %1").arg(qs(io::fileName(canonical))), qs(removed.error().message()),
                   ui::NotificationKind::Error);
            return std::unexpected(std::move(removed.error()));
        }
    }

    closeDeletedDocuments(canonical, isDir);
    removeSourceFromCMakeLists(canonical, isDir);
    updatePathInVcxproj(canonical, {}, isDir, true);
    refreshSolutionExplorerForFsChange();
    return {};
}

void Workbench::promptAndRenameItem(std::string_view path) {
    const std::string target = path.empty() ? defaultTargetPathForModify() : std::string(path);
    if (target.empty()) {
        return;
    }
    const std::string canonical = m_documentRegistry->canonicalize(target);
    auto status = m_services.fs.status(canonical);
    if (!status) {
        notify(text("Cannot rename %1").arg(qs(io::fileName(canonical))), qs(status.error().message()),
               ui::NotificationKind::Error);
        return;
    }
    const bool isDir = status->type == io::FileType::Directory;
    const std::string newName = m_services.prompts.chooseRenameName(m_window, canonical, isDir);
    if (newName.empty() || newName == io::fileName(canonical)) {
        return;
    }
    (void)renameItem(canonical, newName);
}

void Workbench::promptAndDeleteItem(std::string_view path) {
    const std::string target = path.empty() ? defaultTargetPathForModify() : std::string(path);
    if (target.empty()) {
        return;
    }
    const std::string canonical = m_documentRegistry->canonicalize(target);
    auto status = m_services.fs.status(canonical);
    if (!status) {
        notify(text("Cannot delete %1").arg(qs(io::fileName(canonical))), qs(status.error().message()),
               ui::NotificationKind::Error);
        return;
    }
    const bool isDir = status->type == io::FileType::Directory;
    if (!m_services.prompts.confirmDelete(m_window, qs(io::fileName(canonical)), isDir)) {
        return;
    }
    (void)deleteItem(canonical);
}

// --- clangd ---

void Workbench::stopProjectEvaluation() {
    if (m_streamedProjects) {
        m_streamedProjects->invalidate();
        m_streamedProjects.reset();
    }
    std::erase_if(m_retiredProjectEvaluations, [](auto& stopped) {
        if (!stopped.ready())
            return false;
        stopped.join();
        return true;
    });
    if (!m_projectEvaluation)
        return;
    auto old = std::move(m_projectEvaluation);
    old->requestStop();
    m_retiredProjectEvaluations.push_back(
        m_pool->spawn(sched::Lane::Background, stopOwnedProjectEvaluation(std::move(old))));
}

void Workbench::startProjectEvaluation(const std::string& root) {
    if (m_explorer) {
        m_explorer->endSpeedSearch();
        if (m_solution)
            m_explorer->setSolution(m_solution);
    }
    stopProjectEvaluation();
    const auto admission = m_services.trust.snapshot(root);
    m_projectTrustRevision = admission.revision;
    if (!m_solution || !admission.trusted || m_options.compileCommandsCache.empty())
        return;
    auto cache = io::joinPath(m_options.compileCommandsCache, cacheNameFor(root));
    if (!cache)
        return;
    m_streamedProjects = std::make_shared<lang::StreamedProjectContexts>(m_commandsRetirement, m_solution);
    lang::MsBuildProviderOptions options;
    options.retirement = m_commandsRetirement;
    options.streamedProjects = m_streamedProjects;
    if (const auto active = m_build->activeConfigurationIndex(); active && *active < m_build->configurations().size())
        options.configuration = m_build->configurations()[*active].name();
    const auto settings = m_services.settings.snapshot();
    options.msbuildPath =
        !m_options.msbuildPath.empty() ? m_options.msbuildPath : settings->get(settings::key::toolsMsbuildPath);
    options.locateMsBuild = [launcher = &m_services.launcher, fs = &m_services.fs]() -> Result<std::string> {
        auto instance = build::findMsBuild(*launcher, *fs);
        if (!instance)
            return std::unexpected(std::move(instance.error()));
        if (instance->msbuildPath.empty())
            return fail(ErrorCode::NotFound, instance->displayName + " has no MSBuild.exe");
        return instance->msbuildPath;
    };
    options.trusted = [trust = &m_services.trust, root, revision = admission.revision] {
        const auto current = trust->snapshot(root);
        return current.trusted && current.revision == revision;
    };
    options.fallback.configuration = options.configuration;
    m_projectEvaluation = std::make_shared<lang::IndependentProjectEvaluation>(m_services.launcher, m_services.fs,
                                                                               m_solution, *cache, std::move(options));
}

void Workbench::startClangd(const std::string& root) {
    stopClangd();
    if (!m_options.clangd) {
        return;
    }
    const auto snapshot = m_services.settings.snapshot();
    Result<lang::ClangdLocation> found =
        lang::findClangd(m_services.fs, lang::defaultClangdSearch(lang::configuredClangdPath(*snapshot)));
    if (!found) {
        log::info(kLog, "no clangd, code insight is off: {}", found.error().describe());
        return;
    }
    if (!m_reactor) {
        Result<std::unique_ptr<io::Reactor>> reactor = io::Reactor::create(io::ReactorOptions{1, "frost-clangd-io"});
        if (!reactor) {
            notify(text("clangd cannot start"), qs(reactor.error().message()), ui::NotificationKind::Warning);
            return;
        }
        m_reactor = std::move(*reactor);
    }

    // ADR 0041's order: an existing compile_commands.json, then MSBuild's evaluation (a trusted
    // solution, Windows), then the commands synthesized from the projects.
    std::optional<std::string> cache;
    if (!m_options.compileCommandsCache.empty()) {
        if (Result<std::string> joined = io::joinPath(m_options.compileCommandsCache, cacheNameFor(root)); joined) {
            cache = std::move(*joined);
        }
    }
    if (m_cmake) {
        std::weak_ptr<build::CMakeService> service = m_cmake->sharedService();
        m_compileCommands = std::make_shared<lang::BuildDirectoryCompileCommandsProvider>(m_services.fs, [service] {
            const auto current = service.lock();
            return current ? current->activeBuildDirectory() : std::string();
        });
    } else if (m_solution && cache) {
        lang::MsBuildProviderOptions options;
        options.retirement = m_commandsRetirement;
        if (const std::optional<std::size_t> active = m_build->activeConfigurationIndex();
            active && *active < m_build->configurations().size()) {
            options.configuration = m_build->configurations()[*active].name();
        }
        options.msbuildPath =
            !m_options.msbuildPath.empty() ? m_options.msbuildPath : snapshot->get(settings::key::toolsMsbuildPath);
        options.locateMsBuild = [launcher = &m_services.launcher, fs = &m_services.fs]() -> Result<std::string> {
            Result<build::VsInstance> instance = build::findMsBuild(*launcher, *fs);
            if (!instance) {
                return std::unexpected(std::move(instance.error()));
            }
            if (instance->msbuildPath.empty()) {
                return fail(ErrorCode::NotFound, instance->displayName + " has no MSBuild.exe");
            }
            return instance->msbuildPath;
        };
        const auto admission = m_services.trust.snapshot(root);
        options.trusted = [trust = &m_services.trust, root, revision = admission.revision] {
            const auto current = trust->snapshot(root);
            return current.trusted && current.revision == revision;
        };
        options.fallback.configuration = options.configuration;
        if constexpr (io::kNativePathStyle == io::PathStyle::Windows) {
            options.msvc = [launcher = &m_services.launcher, fs = &m_services.fs] {
                return lang::findMsvcEnvironment(*launcher, *fs);
            };
        }
        m_compileCommands = lang::solutionCompileCommandsChain(m_services.launcher, m_services.fs, m_solution, root,
                                                               *cache, std::move(options), m_projectEvaluation);
    } else {
        m_compileCommands = std::make_shared<lang::ExistingCompileCommandsProvider>(m_services.fs, root);
    }

    lang::ClangdServiceOptions options;
    options.clangdPath = found->path;
    options.workspaceRoot = root;
    options.launch.jobs = m_options.clangdJobs;
    options.compileCommands = m_compileCommands;
    const auto generation = m_clangdGeneration;
    // The owned server supervisors are joined before Workbench's QObject base is destroyed;
    // this worker callback only posts, and the queued GUI callback rechecks the controller.
    options.onLifecycle = [self = QPointer<Workbench>(this), generation] {
        if (self)
            QMetaObject::invokeMethod(
                self.data(),
                [self, generation] {
                    if (self && self->m_clangdGeneration == generation && self->m_language)
                        self->m_language->structureChanged();
                },
                Qt::QueuedConnection);
    };
    // Called on clangd's threads; the queued calls die with this object.
    options.onProgress = [this, generation](const lang::ClangdProgress& progress) {
        QMetaObject::invokeMethod(
            this,
            [this, generation, progress] {
                if (generation == m_clangdGeneration)
                    onClangdProgress(progress);
            },
            Qt::QueuedConnection);
    };
    m_clangd = std::make_shared<lang::ClangdService>(m_services.launcher, io::AsyncContext{*m_reactor, *m_pool},
                                                     std::move(options));
    m_clangdListener =
        m_clangd->diagnostics().addListener([this, generation](const lang::FileDiagnostics& diagnostics) {
            QMetaObject::invokeMethod(
                this,
                [this, generation, file = diagnostics.file] {
                    if (generation == m_clangdGeneration)
                        onLiveDiagnostics(file);
                },
                Qt::QueuedConnection);
        });
    if (auto started = m_clangd->start(); !started) {
        notify(text("clangd cannot start"), qs(started.error().message()), ui::NotificationKind::Warning);
        stopClangd();
        return;
    }
    m_bridge->setServer(m_clangd);
    for (DocumentEditorView* view : m_editors->uniqueDocumentViews())
        syncClangdDocument(*view);
    for (DocumentEditorView* view : m_editors->views()) {
        if (isCFamilySource(view->path()) && view == currentView()) {
            auto* binding = new ui::lang::EditorInsightBinding(view->editor(), {*m_bridge, *m_pool});
            binding->attach(*m_clangd);
        }
    }
    m_language->structureChanged();
}

void Workbench::restartClangd() {
    // A configuration switch: other flags, so another database (the chain regenerates it).
    if (hasProject()) {
        log::info(kLog, "the solution configuration changed: clangd restarts");
        startClangd(m_projectRoot);
    }
}

void Workbench::stopClangd() {
    ++m_clangdGeneration;
    std::erase_if(m_retiredClangd, [](auto& stopped) {
        if (!stopped.ready())
            return false;
        stopped.join();
        return true;
    });
    if (!m_clangd) {
        return;
    }
    m_language->structureChanged();
    for (DocumentEditorView* view : m_editors->views()) {
        if (isCFamilySource(view->path()))
            delete view->editor().findChild<ui::lang::EditorInsightBinding*>();
    }
    m_bridge->setServer(nullptr);
    m_clangd->diagnostics().removeListener(m_clangdListener);
    m_clangdListener = 0;
    auto old = std::move(m_clangd);
    auto stopped = old->stop(); // Signal even when the only pool worker is inside evaluation.
    m_retiredClangd.push_back(
        m_pool->spawn(sched::Lane::Background, stopOwnedClangd(std::move(old), std::move(stopped))));
    m_compileCommands.reset();
    if (m_indexing) {
        m_indexing->finish();
        m_indexing.reset();
    }
    m_indexToken.clear();
    for (DocumentEditorView* view : m_editors->views()) {
        if (!isCFamilySource(view->path()))
            continue;
        m_liveMarkers.erase(lang::documentKey(view->path()));
        // Without clangd the editors show no live analysis: no squiggles, usages or inspection widget.
        view->editor().setDiagnostics(ui::EditorDiagnosticSource::Live, {});
        view->editor().setUsageHighlights({});
        const auto version = view->editor().editor().text().version();
        (void)view->editor().setSemanticTokens(version, {});
        (void)view->editor().setInactiveRanges(version, {});
        view->editor().setInsightEnabled(false);
        refreshMarkers(*view);
    }
    refreshLiveProblems();
}

void Workbench::syncClangdDocument(DocumentEditorView& view) {
    if (!m_clangd || !isCFamilySource(view.path())) {
        return;
    }
    const text::Document& document = view.document().document();
    if (auto opened = m_clangd->openDocument(view.path(), document.text()); opened) {
        return;
    }
    // Already open: a ChangeSet that continues from no known version sends the whole text.
    text::ChangeSet whole;
    whole.baseVersion = std::numeric_limits<std::uint64_t>::max();
    whole.version = document.version();
    (void)m_clangd->changeDocument(view.path(), whole, document.text());
}

void Workbench::onLiveDiagnostics(const std::string& file) {
    if (!m_clangd) {
        return;
    }
    const std::optional<lang::FileDiagnostics> set = m_clangd->diagnostics().forFile(file);
    if (!set || !set->stale) {
        // A stale publish (for a text the user has typed past) waits for the next one.
        const std::string key = lang::documentKey(file);
        std::vector<ui::LineMarker> markers;
        if (set) {
            for (const lang::LiveDiagnostic& d : set->diagnostics) {
                if (d.severity != lang::DiagnosticSeverity::Error && d.severity != lang::DiagnosticSeverity::Warning) {
                    continue;
                }
                const ui::MarkerSeverity severity = d.severity == lang::DiagnosticSeverity::Error
                                                        ? ui::MarkerSeverity::Error
                                                        : ui::MarkerSeverity::Warning;
                if (d.line > 0) {
                    markers.push_back(
                        ui::LineMarker{LineIndex{static_cast<std::uint32_t>(d.line - 1)}, severity, d.message});
                }
            }
        }
        if (markers.empty()) {
            m_liveMarkers.erase(key);
        } else {
            m_liveMarkers[key] = std::move(markers);
        }
        for (DocumentEditorView* view : m_editors->views()) {
            if (lang::documentKey(view->path()) != key)
                continue;
            // The squiggles: every live diagnostic (editor-insight's translation; ranges of the
            // published text version, mapped by the view through the edits made since).
            view->editor().setDiagnostics(ui::EditorDiagnosticSource::Live, set ? ui::lang::toEditorDiagnostics(*set)
                                                                                : std::vector<ui::EditorDiagnostic>{});
            refreshMarkers(*view);
        }
    }
    refreshLiveProblems();
}

void Workbench::refreshLiveProblems() {
    if (!m_problems) {
        return;
    }
    std::vector<build::BuildDiagnostic> rows;
    std::vector<lang::LiveDiagnostic> live;
    if (m_clangd)
        live = m_clangd->diagnostics().problems();
    if (m_lua) {
        auto lua = m_lua->diagnostics().problems();
        live.insert(live.end(), lua.begin(), lua.end());
    }
    if (m_managed) {
        auto managed = m_managed->diagnostics().problems();
        live.insert(live.end(), managed.begin(), managed.end());
    }
    for (const lang::LiveDiagnostic& d : live) {
        build::BuildDiagnostic row;
        row.diagnostic.severity = d.severity == lang::DiagnosticSeverity::Error     ? build::Severity::Error
                                  : d.severity == lang::DiagnosticSeverity::Warning ? build::Severity::Warning
                                                                                    : build::Severity::Message;
        row.diagnostic.code = d.code;
        row.diagnostic.message = d.message;
        row.diagnostic.file = d.file;
        row.diagnostic.line = d.line;
        row.diagnostic.column = d.column;
        row.path = d.file;
        row.jumpable = !d.file.empty() && d.line > 0;
        rows.push_back(std::move(row));
    }
    m_problems->model()->setProblems(ui::DiagnosticSource::Live, rows);
}

void Workbench::onClangdProgress(const lang::ClangdProgress& progress) {
    if (!m_clangd || !m_window) {
        return;
    }
    // clangd's background index reports as the work-done progress titled "indexing".
    const bool indexing = progress.token == m_indexToken || (progress.kind == lang::ClangdProgress::Kind::Begin &&
                                                             progress.title.find("index") != std::string::npos);
    if (!indexing) {
        return;
    }
    switch (progress.kind) {
    case lang::ClangdProgress::Kind::Begin:
        if (m_indexing) {
            m_indexing->finish();
        }
        m_indexToken = progress.token;
        m_indexing = m_window->backgroundTasks().start(text("clangd: indexing"), false);
        [[fallthrough]];
    case lang::ClangdProgress::Kind::Report:
        if (m_indexing) {
            if (progress.percentage) {
                m_indexing->setProgress(static_cast<double>(*progress.percentage) / 100.0);
            }
            if (!progress.message.empty()) {
                m_indexing->setDetail(qs(progress.message));
            }
        }
        return;
    case lang::ClangdProgress::Kind::End:
        if (m_indexing) {
            m_indexing->finish();
            m_indexing.reset();
        }
        m_indexToken.clear();
        return;
    }
}

} // namespace frost::app
