// SPDX-License-Identifier: Apache-2.0
//
// Workbench (frost/app/workbench.hpp), the project side: opening solutions, folders and files,
// closing the project, the last project at start-up, the recent-projects list, and clangd (start
// on solutionCompileCommandsChain, one start per project open (FND-9a), restart on a
// configuration switch or a moved CMake database, document sync, live
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
#include "frost/ui/dialog_chrome.hpp"
#include "frost/ui/lang/editor_insight_binding.hpp"
#include "frost/ui/lang/language_controller.hpp"
#include "frost/ui/run_controller.hpp"
#include "frost/ui/shell/editor_tabs.hpp"
#include "frost/ui/shell/shell_actions.hpp"
#include "frost/ui/shell/tool_window_manager.hpp"
#include "frost/ui/toolwindows/build_controller.hpp"
#include "frost/ui/toolwindows/problems.hpp"
#include "frost/ui/toolwindows/solution_explorer.hpp"
#include "frost/ui/toolwindows/trust_dialog.hpp"
#include "frost/ui/tr.hpp"
#include "frost/ui/workbench/file_system_view.hpp"

#include "cmake_integration.hpp"
#include "engine_integration.hpp"
#include "git_integration.hpp"
#include "language_bridge.hpp"
#include "project_edit_process.hpp"
#include "project_properties_dialog.hpp"
#include "unity_control.hpp"
#include <QClipboard>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QInputDialog>
#include <QLabel>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
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

// Suffix of cmakeClangdState when the active build directory holds compile_commands.json.
constexpr std::string_view kCMakeDatabasePresent = "|database";

/*
 * Where a CMake folder's clangd would read its compile commands right now, as
 * BuildDirectoryCompileCommandsProvider resolves them at clangd's start: the active profile's build
 * directory and whether compile_commands.json is there ("" when there is no active profile). Two
 * equal states give clangd the same --compile-commands-dir, so no restart is needed: clangd reloads
 * a database that changes in place. One stat, on the UI thread.
 */
std::string cmakeClangdState(io::IFileSystem& fs, const build::CMakeService& service) {
    std::string directory = service.activeBuildDirectory();
    if (directory.empty())
        return {};
    bool present = false;
    if (auto database = io::joinPath(directory, "compile_commands.json")) {
        auto status = fs.status(*database);
        present = status && status->type == io::FileType::Regular;
    }
    return directory + std::string(present ? kCMakeDatabasePresent : std::string_view("|none"));
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

/*
 * The changed lines of a CMakeLists.txt edit, for the preview the user confirms before anything is
 * written: the lines both texts share at the start and the end are left out, then "Line N:", the
 * removed lines prefixed "- " and the added lines prefixed "+ ". Pure; O(size).
 */
std::string cmakeEditPreview(std::string_view before, std::string_view after) {
    const auto lines = [](std::string_view text) {
        std::vector<std::string_view> out;
        std::size_t start = 0;
        while (start < text.size()) {
            std::size_t end = text.find('\n', start);
            if (end == std::string_view::npos)
                end = text.size();
            std::string_view line = text.substr(start, end - start);
            if (line.ends_with('\r'))
                line.remove_suffix(1);
            out.push_back(line);
            start = end + 1;
        }
        return out;
    };
    const auto a = lines(before);
    const auto b = lines(after);
    std::size_t prefix = 0;
    while (prefix < a.size() && prefix < b.size() && a[prefix] == b[prefix])
        ++prefix;
    std::size_t suffix = 0;
    while (suffix < a.size() - prefix && suffix < b.size() - prefix &&
           a[a.size() - 1 - suffix] == b[b.size() - 1 - suffix])
        ++suffix;
    std::string preview = std::format("Line {}:\n", prefix + 1);
    for (std::size_t i = prefix; i < a.size() - suffix; ++i)
        preview += std::format("- {}\n", a[i]);
    for (std::size_t i = prefix; i < b.size() - suffix; ++i)
        preview += std::format("+ {}\n", b[i]);
    return preview;
}

/*
 * A FrostEngine asset's metadata sidecar is <asset file name>.fmeta beside it (the engine's own
 * layout: Click.wav and Click.wav.fmeta); it carries the asset's handle, so a rename or move that
 * left it behind would orphan the metadata and break every reference to the handle (FEC-9a). This
 * plans the sidecar's move with its asset: nullopt when `asset` has none (folders and .fmeta files
 * themselves have none); AlreadyExists when `target`.fmeta exists, so neither file moves.
 */
using SidecarMove = std::pair<std::string, std::string>; // The .fmeta now, and where it goes.
Result<std::optional<SidecarMove>> planFmetaSidecarMove(io::IFileSystem& fs, const std::string& asset,
                                                        const std::string& target) {
    if (extensionOf(asset) == ".fmeta")
        return std::nullopt;
    SidecarMove move{asset + ".fmeta", target + ".fmeta"};
    const auto status = fs.status(move.first);
    if (!status || status->type != io::FileType::Regular)
        return std::nullopt;
    if (fs.exists(move.second).value_or(true)) {
        return fail(ErrorCode::AlreadyExists, "'" + std::string(io::fileName(move.second)) +
                                                  "' already exists; an asset and its .fmeta file move together, "
                                                  "so neither was moved");
    }
    return move;
}

} // namespace

bool DialogWorkbenchPrompts::confirmCMakeListsEdit(QWidget* parent, const std::string& listsPath,
                                                   const std::string& before, const std::string& after) {
    QDialog dialog(parent);
    dialog.setObjectName(QStringLiteral("cmakeEditPreviewDialog"));
    dialog.setWindowTitle(text("Edit CMakeLists.txt?"));
    dialog.setModal(true);
    dialog.resize(640, 360);
    auto* layout = new QVBoxLayout(&dialog);
    auto* label = new QLabel(text("FrostIDE would change %1 as shown. Nothing is written unless you apply it.")
                                 .arg(QDir::toNativeSeparators(qs(listsPath))),
                             &dialog);
    label->setWordWrap(true);
    layout->addWidget(label);
    auto* preview = new QPlainTextEdit(qs(cmakeEditPreview(before, after)), &dialog);
    preview->setObjectName(QStringLiteral("cmakeEditPreviewText"));
    preview->setReadOnly(true);
    preview->setLineWrapMode(QPlainTextEdit::NoWrap);
    layout->addWidget(preview, 1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Apply | QDialogButtonBox::Cancel, &dialog);
    QObject::connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    ui::installDialogChrome(&dialog, false);
    return dialog.exec() == QDialog::Accepted;
}

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
        transitionLauncherToMainWindow();
        offerProjectTrust();
        return {}; // Already the project.
    }
    Result<project::SolutionModel> loaded = project::SolutionModel::load(
        m_services.fs, path, {.cppHierarchy = m_cppHierarchyMode, .unloadedProjectPaths = m_unloadedProjectPaths});
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
    offerProjectTrust();
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
        transitionLauncherToMainWindow();
        offerProjectTrust();
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
                // FND-9a: the open's own model publication (and a configure that leaves the
                // database where clangd already reads it) must not spawn a second clangd.
                if (m_cmake && !m_clangdDeferredRoot.empty())
                    resumeDeferredClangd();
                else if (m_cmake && m_clangd)
                    restartClangdIfCMakeStateChanged();
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
    /*
     * FND-9a: resolve where clangd's compile commands come from before starting it, so an open
     * spawns one clangd. A trusted CMake folder whose configure-on-open is still producing its first
     * compile_commands.json starts clangd when that configure ends (resumeDeferredClangd), instead
     * of starting now without a database and restarting when it appears. With a database already in
     * the active build directory, clangd starts now and the configure's model does not restart it.
     */
    if (m_options.clangd && m_cmake && m_cmake->service().state() != build::BuildState::Idle &&
        !cmakeClangdState(m_services.fs, m_cmake->service()).ends_with(kCMakeDatabasePresent)) {
        stopClangd();
        m_clangdDeferredRoot = path;
        m_clangdDeferral =
            connect(m_build.get(), &ui::BuildController::stateChanged, this, [this](build::BuildState state) {
                if (state == build::BuildState::Idle)
                    resumeDeferredClangd(true);
            });
        log::info(kLog, "clangd waits for the CMake configure of {}", path);
    } else {
        startClangd(path);
    }
    startLua(path);
    startManaged(path);
    offerProjectTrust();
    return {};
}

void Workbench::offerProjectTrust() {
    if (m_shutDown || m_projectRoot.empty() || !m_window || !m_build) {
        return;
    }
    QStringList untrustedRoots;
    const auto addUntrusted = [&](std::string_view candidate) {
        if (candidate.empty() || m_services.trust.isTrusted(candidate)) {
            return;
        }
        const QString qpath = qs(candidate);
        if (!untrustedRoots.contains(qpath)) {
            untrustedRoots.push_back(qpath);
        }
    };
    addUntrusted(m_projectRoot);
    if (m_solution) {
        for (const project::ProjectInfo& proj : m_solution->projects()) {
            if (proj.nodeKind == project::NodeKind::Project && !proj.path.empty()) {
                addUntrusted(io::parentPath(proj.path));
            }
        }
    }
    if (untrustedRoots.isEmpty()) {
        m_window->setProjectTrusted(true);
        if (m_projectTrustDialog) {
            delete m_projectTrustDialog.data();
            m_projectTrustDialog.clear();
        }
        return;
    }
    m_window->setProjectTrusted(false);
    if (m_projectTrustDialog) {
        m_projectTrustDialog->setRoots(untrustedRoots);
        return;
    }
    if (m_projectTrustPromptedRoot == m_projectRoot) {
        return;
    }
    m_projectTrustPromptedRoot = m_projectRoot;
    auto* dialog = new ui::TrustDialog(*m_build, untrustedRoots, m_window, ui::TrustPurpose::ProjectOpen);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    m_projectTrustDialog = dialog;
    const std::string root = m_projectRoot;
    connect(dialog, &QDialog::accepted, this, [this, root] {
        m_projectTrustDialog.clear();
        if (m_projectRoot != root || !m_services.trust.isTrusted(root)) {
            return;
        }
        if (m_window) {
            m_window->setProjectTrusted(true);
        }
        if (m_solution) {
            startProjectEvaluation(root);
            if (m_clangd) {
                restartClangd();
            }
        } else if (m_cmake) {
            /* While a configure the grant resumed is running, nothing happens here: its model
             * publication restarts clangd once, trusted, on the database it writes, and the trust
             * watch covers an operation that publishes none once the service is idle (FND-9a: no
             * restart in between). */
            if (m_cmake->service().state() == build::BuildState::Idle) {
                if (!m_cmake->service().model()) {
                    // The configure's model publication restarts clangd if the database or trust moved.
                    (void)m_cmake->service().configure();
                } else {
                    // A clangd started restricted on an existing database restarts trusted (.clangd,
                    // --query-driver); without a running clangd this does nothing.
                    restartClangdIfCMakeStateChanged();
                }
            }
        }
        m_luaObservedTrust = true;
        startLua(root);
        startManaged(root);
        if (m_engine) {
            m_engine->trustChanged();
        }
    });
    connect(dialog, &QDialog::finished, this, [this, dialog](int) {
        if (m_projectTrustDialog == dialog) {
            m_projectTrustDialog.clear();
        }
        dialog->setObjectName(QString());
        dialog->deleteLater();
    });
    if (QGuiApplication::platformName() == QStringLiteral("offscreen")) {
        dialog->setAttribute(Qt::WA_ShowWithoutActivating, true);
        dialog->show();
        if (m_window->isVisible()) {
            m_window->activateWindow();
        }
    } else {
        dialog->show();
        const auto centerAndActivate = [dialog = QPointer<ui::TrustDialog>(dialog),
                                        win = QPointer<ui::ShellWindow>(m_window)] {
            if (!dialog) {
                return;
            }
            if (win && win->isVisible()) {
                const QSize hint = dialog->sizeHint().expandedTo(dialog->minimumSize());
                const QRect parentFrame = win->frameGeometry();
                dialog->move(parentFrame.center().x() - hint.width() / 2, parentFrame.center().y() - hint.height() / 2);
            }
            dialog->show();
            dialog->raise();
            dialog->activateWindow();
        };
        if (m_window->isVisible()) {
            centerAndActivate();
        } else {
            QTimer::singleShot(0, dialog, centerAndActivate);
        }
    }
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
    transitionLauncherToMainWindow();
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
    delete m_projectTrustDialog.data();
    m_projectTrustDialog.clear();
    m_projectTrustPromptedRoot.clear();
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
    m_unloadedProjectPaths.clear();
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
    if (m_launcherModeEnabled && m_launcherWindow) {
        showLauncherWindow();
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
    // Membership goes through the ProjectHost helper only (SLN-1, FND-3); its failures, including a
    // missing helper, arrive as notifications while the new file stays on disk.
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
    if (auto written = writeCMakeListsWithPreview(
            canonicalLists, *content, updated,
            text("Adding %1 to target %2").arg(qs(*relSource), qs(selected->arguments.front().literal)));
        !written) {
        return written;
    }
    notify(text("CMakeLists.txt updated"),
           text("Added %1 to target %2.").arg(qs(*relSource), qs(selected->arguments.front().literal)),
           ui::NotificationKind::Info);
    return {};
}

/*
 * CMakeLists.txt is edited as text (add, rename and remove a source); FrostIDE writes such an edit
 * only after the user saw it: the prompt shows the changed lines (cmakeEditPreview) and nothing is
 * written unless the user applies them. Declined, the file stays byte for byte as it was and an
 * Info notification says so (Cancelled). The write is atomic (safeWrite); an open CMakeLists.txt
 * tab reloads from disk.
 */
Result<void> Workbench::writeCMakeListsWithPreview(const std::string& canonicalLists, const std::string& before,
                                                   const std::string& after, const QString& change) {
    if (!m_services.prompts.confirmCMakeListsEdit(m_window, canonicalLists, before, after)) {
        notify(text("CMakeLists.txt not changed"),
               text("%1 was not applied to CMakeLists.txt; update it manually if the target needs it.").arg(change),
               ui::NotificationKind::Info);
        return fail(ErrorCode::Cancelled, "the CMakeLists.txt edit was declined");
    }
    if (Result<io::SafeWriteResult> written = m_services.fs.safeWrite(canonicalLists, after, {}); !written) {
        notify(text("Cannot update CMakeLists.txt"), qs(written.error().message()), ui::NotificationKind::Error);
        return std::unexpected(std::move(written.error()));
    }
    if (document::FileDocument* openLists = m_documents->find(canonicalLists); openLists != nullptr) {
        (void)m_documents->checkDisk(*openLists);
        m_documentRegistry->notifyChanged(canonicalLists);
    }
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
    if (auto loaded = project::SolutionModel::load(
            m_services.fs, m_solution->path(),
            {.cppHierarchy = m_cppHierarchyMode, .unloadedProjectPaths = m_unloadedProjectPaths})) {
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

void Workbench::addPathsToActiveProject(const std::vector<std::string>& paths, std::string_view targetFilterPath) {
    if (paths.empty()) {
        return;
    }
    if (m_solution) {
        std::vector<const project::ProjectInfo*> cppProjects;
        const project::ProjectInfo* bestMatch = nullptr;
        std::size_t bestMatchLen = 0;
        const std::string firstKey = lang::documentKey(paths.front());

        for (const project::ProjectInfo& proj : m_solution->projects()) {
            if (proj.kind == project::ProjectKind::Cpp && proj.nodeKind == project::NodeKind::Project &&
                !proj.path.empty()) {
                cppProjects.push_back(&proj);
                const std::string projDirKey = lang::documentKey(io::parentPath(proj.path));
                if (firstKey.starts_with(projDirKey) && projDirKey.size() > bestMatchLen) {
                    bestMatch = &proj;
                    bestMatchLen = projDirKey.size();
                }
            }
        }
        if (cppProjects.empty()) {
            notify(text("Add to Project"),
                   text("No loaded C++ (.vcxproj) project is open in the solution. C# SDK-style projects include files "
                        "in their directory automatically."),
                   ui::NotificationKind::Info);
            return;
        }
        // One regular file is the edit ProjectHost supports; several files, a folder or a filter
        // are refused (SLN-6) before the user is asked anything.
        const auto single = m_services.fs.status(paths.front());
        if (paths.size() != 1 || !targetFilterPath.empty() || !single || single->type != io::FileType::Regular) {
            notify(text("Add to Project"),
                   text("Adding several files, a folder or a filter to a project requires safe solution and project "
                        "edits (SLN-6). No file was changed."),
                   ui::NotificationKind::Error);
            return;
        }
        std::string targetVcxproj;
        std::string targetProjName;
        if (bestMatch != nullptr) {
            targetVcxproj = bestMatch->path;
            targetProjName = bestMatch->name;
        } else if (cppProjects.size() == 1) {
            targetVcxproj = cppProjects.front()->path;
            targetProjName = cppProjects.front()->name;
        } else if (m_window) {
            QStringList names;
            for (const project::ProjectInfo* p : cppProjects) {
                names.push_back(qs(p->name));
            }
            bool ok = false;
            const QString chosen = QInputDialog::getItem(m_window, text("Add to Project"),
                                                         text("Select target C++ project:"), names, 0, false, &ok);
            if (!ok || chosen.isEmpty()) {
                return;
            }
            const qsizetype idx = names.indexOf(chosen);
            if (idx >= 0 && static_cast<std::size_t>(idx) < cppProjects.size()) {
                targetVcxproj = cppProjects[static_cast<std::size_t>(idx)]->path;
                targetProjName = cppProjects[static_cast<std::size_t>(idx)]->name;
            }
        }
        if (targetVcxproj.empty()) {
            return;
        }
        removeProjectFile(qs(targetVcxproj), qs(paths.front()), true);
        return;
    }

    if (!m_projectRoot.empty()) {
        const auto listsPath = io::joinPath(m_projectRoot, "CMakeLists.txt");
        if (listsPath && m_services.fs.exists(*listsPath).value_or(false)) {
            std::vector<std::string> sourceFiles;
            const auto collectSources = [&](const auto& self, const std::string& p) -> void {
                auto st = m_services.fs.status(p);
                if (!st) {
                    return;
                }
                if (st->type == io::FileType::Regular) {
                    if (isCFamilySource(p)) {
                        sourceFiles.push_back(p);
                    }
                } else if (st->type == io::FileType::Directory) {
                    if (auto entries = m_services.fs.listDirectory(p)) {
                        for (const io::DirEntry& e : *entries) {
                            if (e.name == ".git" || e.name == "build" || e.name == "out") {
                                continue;
                            }
                            if (auto child = io::joinPath(p, e.name)) {
                                self(self, *child);
                            }
                        }
                    }
                }
            };
            for (const std::string& p : paths) {
                collectSources(collectSources, p);
            }
            for (const std::string& src : sourceFiles) {
                (void)addSourceToCMakeLists(src);
            }
            return;
        }
    }
    notify(text("Add to Project"), text("Open a C++ solution (.sln / .slnx) or CMake project first."),
           ui::NotificationKind::Warning);
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
    (void)writeCMakeListsWithPreview(canonicalLists, *content, updated,
                                     text("Renaming %1 to %2").arg(qs(*relOld), qs(*relNew)));
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
    (void)writeCMakeListsWithPreview(canonicalLists, *content, updated, text("Removing %1").arg(qs(*relDeleted)));
}

/*
 * Project files are not edited on a rename, move or delete (SLN-1, FND-3): the text rewrites of
 * .vcxproj, .vcxproj.filters and .csproj that used to run here could corrupt them, and safe project
 * edits arrive with SLN-6. The file-system operation has already happened; this only finds the
 * loaded projects that still list `oldPath` (a C++ item written without a wildcard, or a C#
 * Include attribute naming it or, for a folder, a path under it) and tells the user which project
 * files to update. Nothing is written. Reads each listed .csproj once; O(project items).
 */
void Workbench::updatePathInVcxproj(const std::string& oldPath, const std::string& newPath, bool directory,
                                    bool deleted) {
    if (!m_solution || oldPath.empty()) {
        return;
    }
    const std::string oldKey = lang::documentKey(oldPath);
    const auto names = [&](std::string_view path) {
        const std::string key = lang::documentKey(path);
        if (!directory)
            return key == oldKey;
        return key.size() > oldKey.size() && key.starts_with(oldKey) &&
               (path[oldPath.size()] == '/' || path[oldPath.size()] == '\\');
    };
    QStringList stale;
    for (const project::ProjectInfo& proj : m_solution->projects()) {
        if (proj.nodeKind != project::NodeKind::Project || proj.path.empty()) {
            continue;
        }
        bool listed = false;
        if (proj.kind == project::ProjectKind::Cpp && proj.vcxproj) {
            listed = std::ranges::any_of(proj.vcxproj->items, [&](const project::ProjectItem& item) {
                return !item.fromWildcard && names(item.path);
            });
        } else if (proj.kind == project::ProjectKind::CSharp) {
            // SDK-style projects include their folder's files implicitly; only an explicit Include needs an edit.
            const std::string projDir(io::parentPath(proj.path));
            const std::string projKey = lang::documentKey(projDir);
            if (oldKey.size() > projKey.size() && oldKey.starts_with(projKey)) {
                std::string rel = oldPath.substr(projDir.size() + 1);
                std::ranges::replace(rel, '/', '\\');
                if (auto content = m_services.fs.readFile(proj.path)) {
                    std::string forward = rel;
                    std::ranges::replace(forward, '\\', '/');
                    for (const std::string& spelling : {rel, forward}) {
                        const std::string attribute = "Include=\"" + escapeXml(spelling);
                        const std::string exact = attribute + (directory ? (spelling == rel ? "\\" : "/") : "\"");
                        if (content->find(exact) != std::string::npos)
                            listed = true;
                    }
                }
            }
        }
        if (listed)
            stale.push_back(qs(io::fileName(proj.path)));
    }
    if (stale.isEmpty()) {
        return;
    }
    const QString name = qs(io::fileName(oldPath));
    const QString what =
        deleted ? text("%1 was deleted").arg(name)
                : text("%1 was %2 to %3")
                      .arg(name, io::parentPath(oldPath) == io::parentPath(newPath) ? text("renamed") : text("moved"),
                           QDir::toNativeSeparators(qs(newPath)));
    notify(text("Project files not updated"),
           text("%1, but %2 still list its old path. Updating project files on rename, move or delete requires "
                "safe solution and project edits (SLN-6); no project file was changed. Update the project manually.")
               .arg(what, stale.join(QStringLiteral(", "))),
           ui::NotificationKind::Warning);
}

/*
 * Renames `from` to `to` and, when `sidecar` is set, the asset's .fmeta with it: both or neither.
 * If the sidecar cannot follow, the asset is moved back; the error says what happened (and that the
 * asset stayed at `to` when even that failed). Open tabs of the sidecar follow it.
 */
Result<void> Workbench::moveWithSidecar(const std::string& from, const std::string& to,
                                        const std::optional<std::pair<std::string, std::string>>& sidecar) {
    if (auto renamed = m_services.fs.rename(from, to); !renamed) {
        return renamed;
    }
    if (!sidecar) {
        return {};
    }
    if (auto metaMoved = m_services.fs.rename(sidecar->first, sidecar->second); !metaMoved) {
        if (auto back = m_services.fs.rename(to, from); !back) {
            return fail(ErrorCode::IoError, std::format("its .fmeta file could not follow ({}), and moving the asset "
                                                        "back failed too ({}); the asset is now {}",
                                                        metaMoved.error().message(), back.error().message(), to));
        }
        return fail(metaMoved.error().code(), "its .fmeta file could not follow (" + metaMoved.error().message() +
                                                  "), so the asset was moved back");
    }
    rebindRenamedDocument(m_documentRegistry->canonicalize(sidecar->first),
                          m_documentRegistry->canonicalize(sidecar->second));
    return {};
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
    auto sidecar = isDir ? Result<std::optional<SidecarMove>>(std::nullopt)
                         : planFmetaSidecarMove(m_services.fs, canonicalOld, *targetJoined);
    if (!sidecar) {
        notify(text("Cannot rename %1").arg(qs(io::fileName(canonicalOld))), qs(sidecar.error().message()),
               ui::NotificationKind::Error);
        return std::unexpected(std::move(sidecar.error()));
    }
    if (auto renamed = moveWithSidecar(canonicalOld, *targetJoined, *sidecar); !renamed) {
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

namespace {

Result<void> copyFileOrDirectoryRecursiveFs(io::IFileSystem& fs, const std::string& srcPath,
                                            const std::string& dstPath) {
    auto st = fs.status(srcPath);
    if (!st) {
        return std::unexpected(st.error());
    }
    if (st->type == io::FileType::Regular) {
        auto bytes = fs.readFile(srcPath);
        if (!bytes) {
            return std::unexpected(bytes.error());
        }
        return fs.writeFileAtomic(dstPath, *bytes);
    }
    if (st->type == io::FileType::Directory) {
        if (auto created = fs.createDirectories(dstPath); !created) {
            return std::unexpected(created.error());
        }
        auto entries = fs.listDirectory(srcPath);
        if (!entries) {
            return std::unexpected(entries.error());
        }
        for (const io::DirEntry& entry : *entries) {
            auto childSrc = io::joinPath(srcPath, entry.name);
            auto childDst = io::joinPath(dstPath, entry.name);
            if (!childSrc || !childDst) {
                continue;
            }
            if (auto copied = copyFileOrDirectoryRecursiveFs(fs, *childSrc, *childDst); !copied) {
                return std::unexpected(copied.error());
            }
        }
        return {};
    }
    return {};
}

std::string findNonCollidingCopyPath(io::IFileSystem& fs, const std::string& targetDirectory, std::string_view fileName,
                                     bool isDirectory) {
    if (auto direct = io::joinPath(targetDirectory, fileName); direct && !fs.exists(*direct).value_or(false)) {
        return *direct;
    }
    std::string stem(fileName);
    std::string ext;
    if (!isDirectory) {
        if (const std::size_t dot = stem.find_last_of('.'); dot != std::string::npos && dot > 0) {
            ext = stem.substr(dot);
            stem = stem.substr(0, dot);
        }
    }
    for (int idx = 1; idx < 1000; ++idx) {
        const std::string candidateName =
            idx == 1 ? std::format("{}_copy{}", stem, ext) : std::format("{}_copy{}{}", stem, idx, ext);
        if (auto joined = io::joinPath(targetDirectory, candidateName); joined && !fs.exists(*joined).value_or(false)) {
            return *joined;
        }
    }
    return {};
}

} // namespace

void Workbench::copyPathsToClipboard(const std::vector<std::string>& paths) {
    m_clipboardPaths.clear();
    QList<QUrl> urls;
    QStringList textLines;
    for (const std::string& p : paths) {
        if (p.empty()) {
            continue;
        }
        const std::string canonical = m_documentRegistry->canonicalize(p);
        m_clipboardPaths.push_back(canonical);
        const QString qpath = qs(canonical);
        urls.push_back(QUrl::fromLocalFile(qpath));
        textLines.push_back(QDir::toNativeSeparators(qpath));
    }
    m_clipboardIsCut = false;
    if (!urls.isEmpty() && QGuiApplication::clipboard() != nullptr) {
        auto* mime = new QMimeData();
        mime->setUrls(urls);
        mime->setText(textLines.join(QLatin1Char('\n')));
        QGuiApplication::clipboard()->setMimeData(mime);
    }
}

void Workbench::cutPathsToClipboard(const std::vector<std::string>& paths) {
    copyPathsToClipboard(paths);
    m_clipboardIsCut = !m_clipboardPaths.empty();
}

void Workbench::movePhysicalPathsToDirectory(const std::vector<std::string>& sourcePaths,
                                             const std::string& targetDirectory, const std::string& projectPath) {
    if (sourcePaths.empty() || targetDirectory.empty()) {
        return;
    }
    const std::string canonicalTargetDir = m_documentRegistry->canonicalize(targetDirectory);
    auto dirSt = m_services.fs.status(canonicalTargetDir);
    if (!dirSt || dirSt->type != io::FileType::Directory) {
        return;
    }
    std::string lastMoved;
    std::vector<std::string> movedFiles;
    std::vector<std::string> oldParents;

    for (const std::string& rawSrc : sourcePaths) {
        const std::string canonicalOld = m_documentRegistry->canonicalize(rawSrc);
        if (canonicalOld.empty()) {
            continue;
        }
        if (!m_projectRoot.empty() && lang::documentKey(canonicalOld) == lang::documentKey(m_projectRoot)) {
            continue;
        }
        auto st = m_services.fs.status(canonicalOld);
        if (!st) {
            continue;
        }
        const bool isDir = st->type == io::FileType::Directory;
        const std::string oldParent(io::parentPath(canonicalOld));
        if (lang::documentKey(oldParent) == lang::documentKey(canonicalTargetDir)) {
            continue;
        }
        if (isDir) {
            const std::string oldKey = lang::documentKey(canonicalOld);
            const std::string targetKey = lang::documentKey(canonicalTargetDir);
            if (targetKey == oldKey ||
                (targetKey.starts_with(oldKey) && targetKey.size() > oldKey.size() &&
                 (canonicalTargetDir[canonicalOld.size()] == '/' || canonicalTargetDir[canonicalOld.size()] == '\\'))) {
                notify(text("Cannot move folder"), text("Cannot move a folder into itself."),
                       ui::NotificationKind::Warning);
                continue;
            }
        }
        auto targetJoined = io::joinPath(canonicalTargetDir, io::fileName(canonicalOld));
        if (!targetJoined) {
            continue;
        }
        if (m_services.fs.exists(*targetJoined).value_or(false)) {
            notify(text("Cannot move %1").arg(qs(io::fileName(canonicalOld))),
                   text("'%1' already exists in the destination folder.").arg(qs(io::fileName(canonicalOld))),
                   ui::NotificationKind::Warning);
            continue;
        }
        auto sidecar = isDir ? Result<std::optional<SidecarMove>>(std::nullopt)
                             : planFmetaSidecarMove(m_services.fs, canonicalOld, *targetJoined);
        if (!sidecar) {
            notify(text("Cannot move %1").arg(qs(io::fileName(canonicalOld))), qs(sidecar.error().message()),
                   ui::NotificationKind::Warning);
            continue;
        }
        if (auto renamed = moveWithSidecar(canonicalOld, *targetJoined, *sidecar); !renamed) {
            notify(text("Cannot move %1").arg(qs(io::fileName(canonicalOld))), qs(renamed.error().message()),
                   ui::NotificationKind::Error);
            continue;
        }
        const std::string canonicalNew = m_documentRegistry->canonicalize(*targetJoined);
        oldParents.push_back(oldParent);
        lastMoved = canonicalNew;
        movedFiles.push_back(canonicalNew);

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
    }

    if (movedFiles.empty()) {
        return;
    }
    if (!projectPath.empty() && projectPath.ends_with(".vcxproj")) {
        if (auto added = addExistingFilesToVcxproj(m_services.fs, projectPath, movedFiles); !added) {
            notify(text("Moved files were not added to %1").arg(qs(io::fileName(projectPath))),
                   qs(added.error().message()), ui::NotificationKind::Warning);
        }
    }
    if (m_nav) {
        m_nav->refresh();
        if (m_nav->fileSystemView()) {
            m_nav->fileSystemView()->selectWhenAvailable(lastMoved);
        }
    }
    refreshSolutionExplorerForFsChange(lastMoved);
    if (m_solution) {
        startProjectEvaluation(m_projectRoot);
        if (m_clangd) {
            restartClangd();
        }
    }
}

void Workbench::pasteClipboardIntoDirectory(const std::string& targetDirectory, const std::string& targetProjectPath,
                                            const std::string& targetFilterPath) {
    if (m_clipboardPaths.empty() || targetDirectory.empty()) {
        return;
    }
    if (m_clipboardIsCut) {
        const std::vector<std::string> toMove = m_clipboardPaths;
        m_clipboardPaths.clear();
        m_clipboardIsCut = false;
        movePhysicalPathsToDirectory(toMove, targetDirectory, targetProjectPath);
        return;
    }

    const std::string canonicalTargetDir = m_documentRegistry->canonicalize(targetDirectory);
    std::vector<std::string> createdPaths;
    for (const std::string& src : m_clipboardPaths) {
        auto st = m_services.fs.status(src);
        if (!st) {
            continue;
        }
        const bool isDir = st->type == io::FileType::Directory;
        const std::string dst = findNonCollidingCopyPath(m_services.fs, canonicalTargetDir, io::fileName(src), isDir);
        if (dst.empty()) {
            continue;
        }
        if (auto copied = copyFileOrDirectoryRecursiveFs(m_services.fs, src, dst); !copied) {
            notify(text("Cannot paste %1").arg(qs(io::fileName(src))), qs(copied.error().message()),
                   ui::NotificationKind::Error);
            continue;
        }
        createdPaths.push_back(m_documentRegistry->canonicalize(dst));
    }

    if (createdPaths.empty()) {
        return;
    }
    if (!targetProjectPath.empty() && targetProjectPath.ends_with(".vcxproj")) {
        if (auto added = addExistingFilesToVcxproj(m_services.fs, targetProjectPath, createdPaths, targetFilterPath);
            !added) {
            notify(text("Pasted files were not added to %1").arg(qs(io::fileName(targetProjectPath))),
                   qs(added.error().message()), ui::NotificationKind::Warning);
        }
    }
    if (m_nav) {
        m_nav->refresh();
        if (m_nav->fileSystemView()) {
            m_nav->fileSystemView()->selectWhenAvailable(createdPaths.front());
        }
    }
    refreshSolutionExplorerForFsChange(createdPaths.front());
    if (m_solution) {
        startProjectEvaluation(m_projectRoot);
        if (m_clangd) {
            restartClangd();
        }
    }
}

void Workbench::duplicatePathInExplorer(const std::string& sourcePath, bool isDirectory, const std::string& projectPath,
                                        const std::string& filterPath) {
    if (sourcePath.empty()) {
        return;
    }
    const std::string canonicalSrc = m_documentRegistry->canonicalize(sourcePath);
    auto st = m_services.fs.status(canonicalSrc);
    if (!st) {
        notify(text("Cannot duplicate"), qs(st.error().message()), ui::NotificationKind::Error);
        return;
    }
    const std::string parentDir(io::parentPath(canonicalSrc));
    const std::string suggestedFull =
        findNonCollidingCopyPath(m_services.fs, parentDir, io::fileName(canonicalSrc), isDirectory);
    const std::string suggestedName =
        suggestedFull.empty() ? std::string(io::fileName(canonicalSrc)) : std::string(io::fileName(suggestedFull));

    std::string chosenName;
    if (m_window) {
        bool ok = false;
        const QString entered =
            QInputDialog::getText(m_window, isDirectory ? text("Duplicate Folder") : text("Duplicate File"),
                                  text("New name:"), QLineEdit::Normal, qs(suggestedName), &ok)
                .trimmed();
        if (!ok || entered.isEmpty()) {
            return;
        }
        chosenName = entered.toStdString();
    } else {
        chosenName = suggestedName;
    }

    if (auto valid = validateProjectNameComponent(chosenName); !valid) {
        notify(text("Cannot duplicate"), qs(valid.error().message()), ui::NotificationKind::Error);
        return;
    }
    auto dstJoined = io::joinPath(parentDir, chosenName);
    if (!dstJoined) {
        return;
    }
    if (m_services.fs.exists(*dstJoined).value_or(false)) {
        notify(text("Cannot duplicate"), text("'%1' already exists.").arg(qs(chosenName)), ui::NotificationKind::Error);
        return;
    }
    if (auto copied = copyFileOrDirectoryRecursiveFs(m_services.fs, canonicalSrc, *dstJoined); !copied) {
        notify(text("Cannot duplicate"), qs(copied.error().message()), ui::NotificationKind::Error);
        return;
    }
    const std::string canonicalDst = m_documentRegistry->canonicalize(*dstJoined);
    if (!projectPath.empty() && projectPath.ends_with(".vcxproj")) {
        const std::array<std::string, 1> duplicate{canonicalDst};
        if (auto added = addExistingFilesToVcxproj(m_services.fs, projectPath, duplicate, filterPath); !added) {
            notify(text("The copy was not added to %1").arg(qs(io::fileName(projectPath))), qs(added.error().message()),
                   ui::NotificationKind::Warning);
        }
    }
    if (m_nav) {
        m_nav->refresh();
        if (m_nav->fileSystemView()) {
            m_nav->fileSystemView()->selectWhenAvailable(canonicalDst);
        }
    }
    refreshSolutionExplorerForFsChange(canonicalDst);
    if (!isDirectory) {
        (void)openPath(canonicalDst);
    }
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
        m_clangdCMakeState = cmakeClangdState(m_services.fs, m_cmake->service());
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
    // Restricted mode (no project config, default limits) unless the root is trusted (clangd.hpp).
    options.launch.trusted = m_services.trust.isTrusted(root);
    m_clangdTrusted = options.launch.trusted;
    options.compileCommands = m_compileCommands;
    const auto generation = m_clangdGeneration;
    // The owned server supervisors are joined before Workbench's QObject base is destroyed;
    // this worker callback only posts, and the queued GUI callback rechecks the controller.
    options.onLifecycle = [self = QPointer<Workbench>(this), generation] {
        if (self)
            QMetaObject::invokeMethod(
                self.data(),
                [self, generation] {
                    if (!self || self->m_clangdGeneration != generation)
                        return;
                    if (self->m_language)
                        self->m_language->structureChanged();
                    // Reads the new server's database for languageReadiness before anyone asks.
                    if (self->m_clangd && self->m_clangd->state() == lang::ClangdState::Running)
                        if (const auto location = self->m_clangd->compileCommands())
                            (void)self->compileDatabaseState(location->file);
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
            binding->setCodeVisionEnabled(true);
            binding->attach(*m_clangd);
        }
    }
    m_language->structureChanged();
}

void Workbench::restartClangd() {
    // A configuration switch: other flags, so another database (the chain regenerates it).
    if (hasProject()) {
        log::info(kLog, "the configuration or trust of the project changed: clangd restarts");
        startClangd(m_projectRoot);
    }
}

void Workbench::restartClangdIfCMakeStateChanged() {
    if (!m_cmake || !m_clangd)
        return;
    const std::string state = cmakeClangdState(m_services.fs, m_cmake->service());
    const bool trusted = m_services.trust.isTrusted(m_projectRoot);
    if (state == m_clangdCMakeState && trusted == m_clangdTrusted) {
        // Same build directory, database present or absent as before, same trust: clangd already
        // reads it with the right launch flags.
        log::info(kLog, "the CMake model changed; clangd's compile commands did not, so it keeps running");
        return;
    }
    /* R0 follow-up 10: a profile switch publishes the new profile's model (none yet when its build
     * directory was never configured) and starts its configure at once. Restarting on that model
     * would run clangd without a database and once more when the configure writes it, so clangd
     * keeps the old database until the configure ends: its publication restarts clangd once, and
     * when it writes no database the trust watch restarts it once the service is idle. A trust
     * change does not wait (its launch flags apply at once), and neither does a job held in
     * NeedsTrust: it runs nothing until the user answers, which may be never. */
    const build::BuildState job = m_cmake->service().state();
    if (trusted == m_clangdTrusted && !state.ends_with(kCMakeDatabasePresent) && job != build::BuildState::Idle &&
        job != build::BuildState::NeedsTrust) {
        log::info(kLog, "the CMake compile commands moved ({} -> {}); clangd waits for the running CMake job",
                  m_clangdCMakeState, state);
        return;
    }
    if (trusted != m_clangdTrusted)
        log::info(kLog, "the project's trust changed ({} -> {}): clangd restarts with the new launch flags",
                  m_clangdTrusted, trusted);
    else
        log::info(kLog, "the CMake compile commands moved ({} -> {}): clangd restarts", m_clangdCMakeState, state);
    restartClangd();
}

std::string Workbench::clangdCMakeStateNow() const {
    return m_cmake ? cmakeClangdState(m_services.fs, m_cmake->service()) : std::string();
}

void Workbench::resumeDeferredClangd(bool retryUntilIdle) {
    if (m_clangdDeferredRoot.empty())
        return;
    if (m_shutDown || !m_cmake || m_clangdDeferredRoot != m_projectRoot) {
        m_clangdDeferredRoot.clear();
        QObject::disconnect(m_clangdDeferral);
        return;
    }
    if (m_cmake->service().state() != build::BuildState::Idle) {
        // The controller can report Idle (or the model) just before the service's own state does.
        if (retryUntilIdle)
            QTimer::singleShot(std::chrono::milliseconds(50), this, [this] { resumeDeferredClangd(true); });
        return;
    }
    const std::string root = std::exchange(m_clangdDeferredRoot, {});
    QObject::disconnect(m_clangdDeferral);
    log::info(kLog, "the CMake configure of {} ended: clangd starts", root);
    startClangd(root);
}

void Workbench::stopClangd() {
    m_clangdDeferredRoot.clear();
    QObject::disconnect(m_clangdDeferral);
    m_clangdCMakeState.clear();
    m_clangdTrusted = false;
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
    m_clangdIndexing = false;
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
    if (!m_clangd) {
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
            m_indexing.reset();
        }
        m_indexToken = progress.token;
        m_clangdIndexing = true; // languageReadiness, with or without a window.
        if (m_window)
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
        m_clangdIndexing = false;
        return;
    }
}

} // namespace frost::app
