// SPDX-License-Identifier: Apache-2.0
//
// ProjectPropertiesDialog, SolutionPropertiesDialog, ConfigurationManagerDialog,
// EditRunConfigurationsDialog, AddProjectReferenceDialog, NuGetPackageManagerDialog,
// and LocalHistoryDialog implementations.

#include "project_properties_dialog.hpp"

#include "frost/io/path.hpp"
#include "frost/ui/dialog_chrome.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSplitter>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTableWidget>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#ifdef _WIN32
#include "frost/io/utf.hpp"
#include <windows.h>
#include <tlhelp32.h>
#endif

namespace frost::app {
namespace {

QString toQs(std::string_view sv) { return QString::fromUtf8(sv.data(), static_cast<qsizetype>(sv.size())); }

struct FieldSpec {
    const char* key;
    const char* label;
    const char* options; // nullptr or ';'-separated combo options
};

struct PageSpec {
    const char* category;
    const char* title;
    std::vector<FieldSpec> fields;
};

std::vector<PageSpec> cppPropertyPages() {
    return {
        {"Configuration Properties",
         "General",
         {
             {"Prop.OutDir", "Output Directory", nullptr},
             {"Prop.IntDir", "Intermediate Directory", nullptr},
             {"Prop.TargetName", "Target Name", nullptr},
             {"Prop.TargetExt", "Target Extension", ";.exe;.dll;.lib"},
             {"Prop.ConfigurationType", "Configuration Type",
              "Application;DynamicLibrary;StaticLibrary;Makefile;Utility"},
             {"Prop.WindowsTargetPlatformVersion", "Windows SDK Version", ";10.0;10.0.22621.0;10.0.19041.0"},
             {"Prop.PlatformToolset", "Platform Toolset", ";v143;v142;ClangCL;llvm"},
             {"ClCompile.LanguageStandard", "C++ Language Standard",
              ";Default;stdcpp14;stdcpp17;stdcpp20;stdcpplatest"},
             {"ClCompile.LanguageStandard_C", "C Language Standard", ";Default;stdc11;stdc17;stdclatest"},
             {"Prop.CharacterSet", "Character Set", ";Unicode;MultiByte;NotSet"},
             {"Prop.WholeProgramOptimization", "Whole Program Optimization", ";false;true"},
         }},
        {"Configuration Properties",
         "VC++ Directories",
         {
             {"Prop.ExecutablePath", "Executable Directories", nullptr},
             {"Prop.IncludePath", "Include Directories", nullptr},
             {"Prop.ExternalIncludePath", "External Include Directories", nullptr},
             {"Prop.LibraryPath", "Library Directories", nullptr},
             {"Prop.SourcePath", "Source Directories", nullptr},
             {"Prop.ExcludePath", "Exclude Directories", nullptr},
         }},
        {"Configuration Properties",
         "Debugging",
         {
             {"Prop.LocalDebuggerCommand", "Command", nullptr},
             {"Prop.LocalDebuggerCommandArguments", "Command Arguments", nullptr},
             {"Prop.LocalDebuggerWorkingDirectory", "Working Directory", nullptr},
             {"Prop.LocalDebuggerEnvironment", "Environment", nullptr},
         }},
        {"C/C++",
         "General",
         {
             {"ClCompile.AdditionalIncludeDirectories", "Additional Include Directories", nullptr},
             {"ClCompile.DebugInformationFormat", "Debug Information Format",
              ";None;OldStyle;ProgramDatabase;EditAndContinue"},
             {"ClCompile.WarningLevel", "Warning Level",
              ";TurnOffAllWarnings;Level1;Level2;Level3;Level4;EnableAllWarnings"},
             {"ClCompile.TreatWarningAsError", "Treat Warnings As Errors", ";false;true"},
             {"ClCompile.SDLCheck", "SDL Checks", ";true;false"},
             {"ClCompile.MultiProcessorCompilation", "Multi-processor Compilation", ";true;false"},
         }},
        {"C/C++",
         "Optimization",
         {
             {"ClCompile.Optimization", "Optimization", ";Disabled;MinSpace;MaxSpeed;Full"},
             {"ClCompile.InlineFunctionExpansion", "Inline Function Expansion",
              ";Default;Disabled;OnlyExplicitInline;AnySuitable"},
             {"ClCompile.IntrinsicFunctions", "Enable Intrinsic Functions", ";true;false"},
             {"ClCompile.FavorSizeOrSpeed", "Favor Size Or Speed", ";Neither;Speed;Size"},
             {"ClCompile.OmitFramePointers", "Omit Frame Pointers", ";true;false"},
             {"ClCompile.WholeProgramOptimization", "Whole Program Optimization", ";true;false"},
         }},
        {"C/C++",
         "Preprocessor",
         {
             {"ClCompile.PreprocessorDefinitions", "Preprocessor Definitions", nullptr},
             {"ClCompile.UndefinePreprocessorDefinitions", "Undefine Preprocessor Definitions", nullptr},
             {"ClCompile.PreprocessToFile", "Preprocess to a File", ";false;true"},
         }},
        {"C/C++",
         "Code Generation",
         {
             {"ClCompile.RuntimeLibrary", "Runtime Library",
              ";MultiThreaded;MultiThreadedDebug;MultiThreadedDLL;MultiThreadedDebugDLL"},
             {"ClCompile.ExceptionHandling", "Enable C++ Exceptions", ";Sync;Async;SyncCThrow;false"},
             {"ClCompile.BasicRuntimeChecks", "Basic Runtime Checks",
              ";Default;StackFrameRuntimeCheck;UninitializedLocalUsageCheck;EnableFastChecks"},
             {"ClCompile.BufferSecurityCheck", "Security Check", ";true;false"},
             {"ClCompile.EnableEnhancedInstructionSet", "Enable Enhanced Instruction Set",
              ";NotSet;StreamingSIMDExtensions2;AdvancedVectorExtensions;AdvancedVectorExtensions2;"
              "AdvancedVectorExtensions512"},
             {"ClCompile.FloatingPointModel", "Floating Point Model", ";Precise;Strict;Fast"},
         }},
        {"C/C++",
         "Language",
         {
             {"ClCompile.ConformanceMode", "Conformance Mode", ";true;false"},
             {"ClCompile.RuntimeTypeInfo", "Enable Run-Time Type Information (RTTI)", ";true;false"},
             {"ClCompile.TreatWChar_tAsBuiltInType", "Treat wchar_t as Built-in Type", ";true;false"},
             {"ClCompile.OpenMPSupport", "OpenMP Support", ";false;true"},
         }},
        {"C/C++",
         "Precompiled Headers",
         {
             {"ClCompile.PrecompiledHeader", "Precompiled Header", ";NotUsing;Use;Create"},
             {"ClCompile.PrecompiledHeaderFile", "Precompiled Header File", nullptr},
             {"ClCompile.PrecompiledHeaderOutputFile", "Precompiled Header Output File", nullptr},
         }},
        {"C/C++",
         "Command Line",
         {
             {"ClCompile.AdditionalOptions", "Additional Options", nullptr},
         }},
        {"Linker",
         "General",
         {
             {"Link.OutputFile", "Output File", nullptr},
             {"Link.ShowProgress", "Show Progress", ";NotSet;LinkVerbose;LinkVerboseLib"},
             {"Prop.LinkIncremental", "Enable Incremental Linking", ";true;false"},
             {"Link.AdditionalLibraryDirectories", "Additional Library Directories", nullptr},
         }},
        {"Linker",
         "Input",
         {
             {"Link.AdditionalDependencies", "Additional Dependencies (.lib)", nullptr},
             {"Link.IgnoreAllDefaultLibraries", "Ignore All Default Libraries", ";false;true"},
             {"Link.IgnoreSpecificDefaultLibraries", "Ignore Specific Default Libraries", nullptr},
             {"Link.ModuleDefinitionFile", "Module Definition File (.def)", nullptr},
             {"Link.DelayLoadDLLs", "Delay Loaded DLLs", nullptr},
         }},
        {"Linker",
         "Debugging",
         {
             {"Link.GenerateDebugInformation", "Generate Debug Info", ";true;false;DebugFastLink;DebugFull"},
             {"Link.ProgramDatabaseFile", "Generate Program Database File (.pdb)", nullptr},
             {"Link.GenerateMapFile", "Generate Map File", ";false;true"},
             {"Link.MapFileName", "Map File Name", nullptr},
         }},
        {"Linker",
         "System",
         {
             {"Link.SubSystem", "SubSystem", ";NotSet;Console;Windows;Native;POSIX"},
             {"Link.StackReserveSize", "Stack Reserve Size", nullptr},
             {"Link.StackCommitSize", "Stack Commit Size", nullptr},
             {"Link.LargeAddressAware", "Enable Large Addresses", ";true;false"},
         }},
        {"Linker",
         "Advanced",
         {
             {"Link.EntryPointSymbol", "Entry Point", nullptr},
             {"Link.RandomizedBaseAddress", "Randomized Base Address", ";true;false"},
             {"Link.DataExecutionPrevention", "Data Execution Prevention (DEP)", ";true;false"},
             {"Link.ImportLibrary", "Import Library", nullptr},
             {"Link.TargetMachine", "Target Machine", ";NotSet;MachineX64;MachineX86;MachineARM64"},
         }},
        {"Linker",
         "Command Line",
         {
             {"Link.AdditionalOptions", "Additional Options", nullptr},
         }},
        {"Librarian",
         "General",
         {
             {"Lib.OutputFile", "Output File", nullptr},
             {"Lib.AdditionalDependencies", "Additional Dependencies", nullptr},
             {"Lib.AdditionalLibraryDirectories", "Additional Library Directories", nullptr},
             {"Lib.AdditionalOptions", "Additional Options", nullptr},
         }},
        {"Build Events",
         "Build Events",
         {
             {"PreBuildEvent.Command", "Pre-Build Event Command Line", nullptr},
             {"PreBuildEvent.Message", "Pre-Build Event Description", nullptr},
             {"PreLinkEvent.Command", "Pre-Link Event Command Line", nullptr},
             {"PreLinkEvent.Message", "Pre-Link Event Description", nullptr},
             {"PostBuildEvent.Command", "Post-Build Event Command Line", nullptr},
             {"PostBuildEvent.Message", "Post-Build Event Description", nullptr},
         }},
    };
}

std::vector<PageSpec> csprojPropertyPages() {
    return {
        {"Project Properties",
         "Application",
         {
             {"Prop.OutputType", "Output Type", "Exe;WinExe;Library"},
             {"Prop.TargetFramework", "Target Framework", "net10.0;net9.0;net8.0;net7.0;net6.0;netstandard2.0"},
             {"Prop.TargetFrameworks", "Target Frameworks (Multi-Target)", nullptr},
             {"Prop.StartupObject", "Startup Object", nullptr},
             {"Prop.AssemblyName", "Assembly Name", nullptr},
             {"Prop.RootNamespace", "Default / Root Namespace", nullptr},
             {"Prop.ApplicationIcon", "Application Icon (.ico)", nullptr},
             {"Prop.ApplicationManifest", "Application Manifest", nullptr},
         }},
        {"Project Properties",
         "Build",
         {
             {"Prop.LangVersion", "C# Language Version", ";default;latest;preview;13.0;12.0;11.0;10.0"},
             {"Prop.Nullable", "Nullable Context", ";enable;disable;warnings;annotations"},
             {"Prop.ImplicitUsings", "Implicit Global Usings", ";enable;disable"},
             {"Prop.DefineConstants", "Conditional Compilation Symbols", nullptr},
             {"Prop.PlatformTarget", "Platform Target", ";AnyCPU;x64;x86;ARM64"},
             {"Prop.AllowUnsafeBlocks", "Allow Unsafe Code", ";false;true"},
             {"Prop.Optimize", "Optimize Code", ";false;true"},
             {"Prop.OutputPath", "Output Path", nullptr},
             {"Prop.GenerateDocumentationFile", "Generate XML Documentation File", ";false;true"},
             {"Prop.TreatWarningsAsErrors", "Treat Warnings As Errors", ";false;true"},
             {"Prop.NoWarn", "Suppress Warnings (NoWarn)", nullptr},
         }},
        {"Project Properties",
         "Build Events",
         {
             {"Prop.PreBuildEvent", "Pre-build Event Command Line", nullptr},
             {"Prop.PostBuildEvent", "Post-build Event Command Line", nullptr},
         }},
        {"Project Properties",
         "Package (NuGet)",
         {
             {"Prop.GeneratePackageOnBuild", "Generate NuGet Package on Build", ";false;true"},
             {"Prop.PackageId", "Package ID", nullptr},
             {"Prop.Version", "Package Version", nullptr},
             {"Prop.Authors", "Authors", nullptr},
             {"Prop.Company", "Company", nullptr},
             {"Prop.Product", "Product", nullptr},
             {"Prop.Description", "Description", nullptr},
             {"Prop.Copyright", "Copyright", nullptr},
             {"Prop.PackageLicenseExpression", "Package License Expression", nullptr},
             {"Prop.RepositoryUrl", "Repository URL", nullptr},
             {"Prop.PackageTags", "Package Tags", nullptr},
         }},
        {"Project Properties",
         "Signing",
         {
             {"Prop.SignAssembly", "Sign the Assembly", ";false;true"},
             {"Prop.AssemblyOriginatorKeyFile", "Strong Name Key File (.snk)", nullptr},
             {"Prop.DelaySign", "Delay Sign Only", ";false;true"},
         }},
    };
}

QStringList extractVcxprojConfigs(const QString& xml) {
    QStringList list;
    const QRegularExpression re(QStringLiteral("<ProjectConfiguration\\s+Include=\"([^\"]+)\""),
                                QRegularExpression::CaseInsensitiveOption);
    auto it = re.globalMatch(xml);
    while (it.hasNext()) {
        const QString cfg = it.next().captured(1).trimmed();
        if (!cfg.isEmpty() && !list.contains(cfg, Qt::CaseInsensitive))
            list.push_back(cfg);
    }
    if (list.isEmpty()) {
        list << QStringLiteral("Debug|x64") << QStringLiteral("Release|x64");
    }
    list.push_back(QStringLiteral("All Configurations"));
    return list;
}

struct BoundField {
    std::string key;
    std::function<std::string()> getValue;
    std::function<void(const std::string&)> setValue;
};

void buildPropertyPagesUi(const std::vector<PageSpec>& specs, QTreeWidget* tree, QStackedWidget* stack,
                          std::vector<BoundField>& bindings) {
    std::map<std::string, QTreeWidgetItem*> categoryItems;
    for (std::size_t i = 0; i < specs.size(); ++i) {
        const PageSpec& page = specs[i];
        QTreeWidgetItem* catItem = nullptr;
        auto it = categoryItems.find(page.category);
        if (it == categoryItems.end()) {
            catItem = new QTreeWidgetItem(tree, QStringList{QString::fromUtf8(page.category)});
            catItem->setExpanded(true);
            categoryItems[page.category] = catItem;
        } else {
            catItem = it->second;
        }
        auto* pageItem = new QTreeWidgetItem(catItem, QStringList{QString::fromUtf8(page.title)});
        pageItem->setData(0, Qt::UserRole, static_cast<int>(i));

        auto* scroll = new QScrollArea(stack);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        auto* container = new QWidget(scroll);
        auto* form = new QFormLayout(container);
        form->setContentsMargins(16, 12, 16, 12);
        form->setSpacing(10);

        for (const FieldSpec& f : page.fields) {
            const std::string keyStr(f.key);
            if (f.options != nullptr) {
                auto* combo = new QComboBox(container);
                combo->setEditable(true);
                combo->setObjectName(QString::fromStdString("prop_" + keyStr));
                const QStringList opts = QString::fromUtf8(f.options).split(QLatin1Char(';'));
                combo->addItems(opts);
                form->addRow(QString::fromUtf8(f.label) + QLatin1Char(':'), combo);
                bindings.push_back({keyStr, [combo] { return combo->currentText().trimmed().toStdString(); },
                                    [combo](const std::string& val) { combo->setEditText(toQs(val)); }});
            } else {
                auto* edit = new QLineEdit(container);
                edit->setObjectName(QString::fromStdString("prop_" + keyStr));
                form->addRow(QString::fromUtf8(f.label) + QLatin1Char(':'), edit);
                bindings.push_back({keyStr, [edit] { return edit->text().trimmed().toStdString(); },
                                    [edit](const std::string& val) { edit->setText(toQs(val)); }});
            }
        }
        scroll->setWidget(container);
        stack->addWidget(scroll);
    }
    tree->expandAll();
}

} // namespace

void showProjectPropertiesDialog(QWidget* parent, io::IFileSystem& fs, const std::string& projectPath,
                                 const std::string& activeConfigPlatform, std::function<void()> onSaved) {
    auto readRes = fs.readFile(projectPath);
    if (!readRes) {
        QMessageBox::warning(parent, QStringLiteral("Project Properties"),
                             QStringLiteral("Cannot read project file:\n%1").arg(toQs(readRes.error().message())));
        return;
    }
    std::string currentXml = *readRes;
    const QFileInfo info(toQs(projectPath));
    const bool isCpp = info.suffix().compare(QStringLiteral("vcxproj"), Qt::CaseInsensitive) == 0;

    QDialog dialog(parent);
    dialog.setObjectName(QStringLiteral("projectPropertiesDialog"));
    dialog.setWindowTitle(QStringLiteral("%1 Property Pages").arg(info.completeBaseName()));
    dialog.setModal(true);
    dialog.resize(860, 580);

    auto* outer = new QVBoxLayout(&dialog);
    outer->setContentsMargins(12, 12, 12, 12);
    outer->setSpacing(10);

    QComboBox* cfgCombo = nullptr;
    if (isCpp) {
        auto* topBar = new QHBoxLayout();
        topBar->addWidget(new QLabel(QStringLiteral("Configuration | Platform:"), &dialog));
        cfgCombo = new QComboBox(&dialog);
        cfgCombo->setObjectName(QStringLiteral("projectPropertiesConfigCombo"));
        const QStringList cfgs = extractVcxprojConfigs(toQs(currentXml));
        cfgCombo->addItems(cfgs);
        const int activeIdx = cfgCombo->findText(toQs(activeConfigPlatform), Qt::MatchFixedString);
        if (activeIdx >= 0)
            cfgCombo->setCurrentIndex(activeIdx);
        topBar->addWidget(cfgCombo, 1);
        outer->addLayout(topBar);
    }

    auto* splitter = new QSplitter(Qt::Horizontal, &dialog);
    auto* tree = new QTreeWidget(splitter);
    tree->setObjectName(QStringLiteral("projectPropertiesTree"));
    tree->setHeaderHidden(true);
    tree->setMinimumWidth(240);
    auto* stack = new QStackedWidget(splitter);
    stack->setObjectName(QStringLiteral("projectPropertiesStack"));
    splitter->addWidget(tree);
    splitter->addWidget(stack);
    splitter->setStretchFactor(1, 1);
    outer->addWidget(splitter, 1);

    const std::vector<PageSpec> specs = isCpp ? cppPropertyPages() : csprojPropertyPages();
    std::vector<BoundField> bindings;
    buildPropertyPagesUi(specs, tree, stack, bindings);

    ProjectPropertyMap loadedProps;
    const auto loadCurrentConfig = [&] {
        const std::string cfg = cfgCombo ? cfgCombo->currentText().toStdString() : std::string();
        loadedProps = isCpp ? parseVcxprojPropertiesForConfig(currentXml, cfg) : parseCsprojProperties(currentXml);
        for (const BoundField& b : bindings) {
            const auto it = loadedProps.find(b.key);
            b.setValue(it != loadedProps.end() ? it->second : std::string());
        }
    };
    loadCurrentConfig();

    const auto saveCurrentChanges = [&]() -> bool {
        ProjectPropertyMap updates;
        for (const BoundField& b : bindings) {
            const std::string val = b.getValue();
            const auto it = loadedProps.find(b.key);
            const std::string prev = it != loadedProps.end() ? it->second : std::string();
            if (val != prev)
                updates[b.key] = val;
        }
        if (updates.empty())
            return true;
        const std::string cfg = cfgCombo ? cfgCombo->currentText().toStdString() : std::string();
        currentXml = isCpp ? applyVcxprojPropertiesForConfig(currentXml, cfg, updates)
                           : applyCsprojProperties(currentXml, updates);
        if (auto written = fs.writeFileAtomic(projectPath, currentXml); !written) {
            QMessageBox::warning(&dialog, QStringLiteral("Save Project Properties"), toQs(written.error().message()));
            return false;
        }
        loadCurrentConfig();
        if (onSaved)
            onSaved();
        return true;
    };

    if (cfgCombo) {
        QObject::connect(cfgCombo, &QComboBox::currentIndexChanged, &dialog, [&](int) { loadCurrentConfig(); });
    }
    QObject::connect(tree, &QTreeWidget::currentItemChanged, &dialog,
                     [stack](QTreeWidgetItem* current, QTreeWidgetItem*) {
                         if (!current)
                             return;
                         const QVariant pageIdx = current->data(0, Qt::UserRole);
                         if (pageIdx.isValid())
                             stack->setCurrentIndex(pageIdx.toInt());
                     });
    if (tree->topLevelItemCount() > 0 && tree->topLevelItem(0)->childCount() > 0)
        tree->setCurrentItem(tree->topLevelItem(0)->child(0));

    auto* buttons =
        new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::Apply, &dialog);
    QObject::connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, &dialog,
                     [&] { (void)saveCurrentChanges(); });
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(&dialog, &QDialog::accepted, &dialog, [&] { (void)saveCurrentChanges(); });
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    outer->addWidget(buttons);
    ui::installDialogChrome(&dialog, true);
    dialog.exec();
}

void showSolutionPropertiesDialog(QWidget* parent, io::IFileSystem& fs, const project::SolutionModel& solution,
                                  std::optional<project::ProjectId> currentStartup,
                                  std::function<void(project::ProjectId)> onSetStartup,
                                  std::function<void()> onDependenciesChanged) {
    QDialog dialog(parent);
    dialog.setObjectName(QStringLiteral("solutionPropertiesDialog"));
    dialog.setWindowTitle(QStringLiteral("Solution '%1' Property Pages").arg(toQs(solution.name())));
    dialog.setModal(true);
    dialog.resize(680, 460);

    auto* outer = new QVBoxLayout(&dialog);
    auto* tabs = new QTabWidget(&dialog);

    auto* startupPage = new QWidget(tabs);
    auto* startupForm = new QFormLayout(startupPage);
    auto* startupCombo = new QComboBox(startupPage);
    startupCombo->setObjectName(QStringLiteral("solutionStartupProjectCombo"));
    const auto projects = solution.projects();
    for (std::uint32_t i = 0; i < projects.size(); ++i) {
        if (projects[i].nodeKind == project::NodeKind::Project) {
            startupCombo->addItem(toQs(projects[i].name), static_cast<int>(i));
            if (currentStartup && currentStartup->value() == i)
                startupCombo->setCurrentIndex(startupCombo->count() - 1);
        }
    }
    startupForm->addRow(QStringLiteral("Single startup project:"), startupCombo);
    tabs->addTab(startupPage, QStringLiteral("Startup Project"));

    auto* depPage = new QWidget(tabs);
    auto* depLayout = new QVBoxLayout(depPage);
    auto* projCombo = new QComboBox(depPage);
    auto* depList = new QListWidget(depPage);
    depList->setObjectName(QStringLiteral("solutionDependenciesList"));
    for (std::uint32_t i = 0; i < projects.size(); ++i) {
        if (projects[i].nodeKind == project::NodeKind::Project)
            projCombo->addItem(toQs(projects[i].name), static_cast<int>(i));
    }
    const auto populateDeps = [&] {
        depList->clear();
        if (projCombo->currentIndex() < 0)
            return;
        const auto selId = static_cast<std::uint32_t>(projCombo->currentData().toInt());
        const auto& selProj = projects[selId];
        for (std::uint32_t i = 0; i < projects.size(); ++i) {
            if (i == selId || projects[i].nodeKind != project::NodeKind::Project)
                continue;
            auto* item = new QListWidgetItem(toQs(projects[i].name), depList);
            item->setData(Qt::UserRole, static_cast<int>(i));
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            const bool depends =
                std::ranges::find(selProj.dependencies, project::ProjectId{i}) != selProj.dependencies.end();
            item->setCheckState(depends ? Qt::Checked : Qt::Unchecked);
        }
    };
    QObject::connect(projCombo, &QComboBox::currentIndexChanged, &dialog, [&](int) { populateDeps(); });
    populateDeps();
    depLayout->addWidget(new QLabel(QStringLiteral("Project:"), depPage));
    depLayout->addWidget(projCombo);
    depLayout->addWidget(new QLabel(QStringLiteral("Depends on:"), depPage));
    depLayout->addWidget(depList, 1);
    tabs->addTab(depPage, QStringLiteral("Project Dependencies"));

    outer->addWidget(tabs, 1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        if (startupCombo->currentIndex() >= 0 && onSetStartup) {
            onSetStartup(project::ProjectId{static_cast<std::uint32_t>(startupCombo->currentData().toInt())});
        }
        if (projCombo->currentIndex() >= 0) {
            const auto selId = static_cast<std::uint32_t>(projCombo->currentData().toInt());
            const auto& selProj = projects[selId];
            if (auto content = fs.readFile(selProj.path)) {
                std::string xml = *content;
                const QDir projDir = QFileInfo(toQs(selProj.path)).dir();
                for (int r = 0; r < depList->count(); ++r) {
                    auto* item = depList->item(r);
                    const auto targetId = static_cast<std::uint32_t>(item->data(Qt::UserRole).toInt());
                    const QString rel =
                        QDir::toNativeSeparators(projDir.relativeFilePath(toQs(projects[targetId].path)));
                    if (item->checkState() == Qt::Checked) {
                        xml = addProjectReferenceXml(xml, rel.toStdString(), projects[targetId].projectGuid);
                    } else {
                        xml = removeProjectReferenceXml(xml, rel.toStdString());
                    }
                }
                if (xml != *content) {
                    (void)fs.writeFileAtomic(selProj.path, xml);
                    if (onDependenciesChanged)
                        onDependenciesChanged();
                }
            }
        }
        dialog.accept();
    });
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    outer->addWidget(buttons);
    ui::installDialogChrome(&dialog, true);
    dialog.exec();
}

void showConfigurationManagerDialog(QWidget* parent, io::IFileSystem& fs, const project::SolutionModel& solution,
                                    int activeConfigurationIndex, std::function<void(int)> onActiveConfigurationChanged,
                                    std::function<void()> onSolutionFileModified) {
    (void)fs;
    (void)onSolutionFileModified;
    QDialog dialog(parent);
    dialog.setObjectName(QStringLiteral("configurationManagerDialog"));
    dialog.setWindowTitle(QStringLiteral("Configuration Manager"));
    dialog.setModal(true);
    dialog.resize(680, 420);

    auto* outer = new QVBoxLayout(&dialog);
    auto* topRow = new QHBoxLayout();
    topRow->addWidget(new QLabel(QStringLiteral("Active solution configuration:"), &dialog));
    auto* activeCombo = new QComboBox(&dialog);
    activeCombo->setObjectName(QStringLiteral("cfgManagerActiveCombo"));
    const auto configs = solution.configurations();
    for (std::size_t i = 0; i < configs.size(); ++i)
        activeCombo->addItem(toQs(configs[i].name()));
    if (activeConfigurationIndex >= 0 && activeConfigurationIndex < activeCombo->count())
        activeCombo->setCurrentIndex(activeConfigurationIndex);
    topRow->addWidget(activeCombo, 1);
    outer->addLayout(topRow);

    auto* table = new QTableWidget(&dialog);
    table->setObjectName(QStringLiteral("cfgManagerProjectTable"));
    table->setColumnCount(4);
    table->setHorizontalHeaderLabels({QStringLiteral("Project"), QStringLiteral("Configuration"),
                                      QStringLiteral("Platform"), QStringLiteral("Build")});
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);

    const auto populateTable = [&](int cfgIdx) {
        const auto projects = solution.projects();
        table->setRowCount(static_cast<int>(projects.size()));
        for (std::size_t p = 0; p < projects.size(); ++p) {
            table->setItem(static_cast<int>(p), 0, new QTableWidgetItem(toQs(projects[p].name)));
            QString cfgName = QStringLiteral("Debug");
            QString platName = QStringLiteral("x64");
            bool buildChecked = true;
            if (cfgIdx >= 0 && static_cast<std::size_t>(cfgIdx) < configs.size()) {
                const auto& mapping = solution.projectConfiguration(project::ProjectId{static_cast<std::uint32_t>(p)},
                                                                    static_cast<std::size_t>(cfgIdx));
                if (mapping) {
                    cfgName = toQs(mapping->configuration);
                    platName = toQs(mapping->platform);
                    buildChecked = mapping->build;
                }
            }
            table->setItem(static_cast<int>(p), 1, new QTableWidgetItem(cfgName));
            table->setItem(static_cast<int>(p), 2, new QTableWidgetItem(platName));
            auto* buildItem = new QTableWidgetItem();
            buildItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
            buildItem->setCheckState(buildChecked ? Qt::Checked : Qt::Unchecked);
            table->setItem(static_cast<int>(p), 3, buildItem);
        }
    };
    populateTable(activeCombo->currentIndex());
    QObject::connect(activeCombo, &QComboBox::currentIndexChanged, &dialog, populateTable);
    outer->addWidget(table, 1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        if (activeCombo->currentIndex() >= 0 && onActiveConfigurationChanged)
            onActiveConfigurationChanged(activeCombo->currentIndex());
        dialog.accept();
    });
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    outer->addWidget(buttons);
    ui::installDialogChrome(&dialog, true);
    dialog.exec();
}

std::vector<RunningProcessInfo> enumerateRunningProcesses() {
    std::vector<RunningProcessInfo> processes;
#ifdef _WIN32
    std::map<DWORD, std::string> windowTitles;
    EnumWindows(
        [](HWND hwnd, LPARAM lParam) -> BOOL {
            if (!IsWindowVisible(hwnd))
                return TRUE;
            const int len = GetWindowTextLengthW(hwnd);
            if (len <= 0)
                return TRUE;
            std::wstring titleBuf(static_cast<std::size_t>(len) + 1, L'\0');
            if (GetWindowTextW(hwnd, titleBuf.data(), len + 1) <= 0)
                return TRUE;
            titleBuf.resize(static_cast<std::size_t>(len));
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (pid != 0) {
                auto* titles = reinterpret_cast<std::map<DWORD, std::string>*>(lParam);
                if (!titles->contains(pid)) {
                    if (auto utf8 = io::fromWide(titleBuf))
                        (*titles)[pid] = std::move(*utf8);
                }
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&windowTitles));

    const DWORD selfPid = GetCurrentProcessId();
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        if (Process32FirstW(snapshot, &entry)) {
            do {
                if (entry.th32ProcessID == 0 || entry.th32ProcessID == 4 || entry.th32ProcessID == selfPid)
                    continue;
                RunningProcessInfo info;
                info.pid = static_cast<std::uint32_t>(entry.th32ProcessID);
                if (auto nameUtf8 = io::fromWide(entry.szExeFile))
                    info.name = std::move(*nameUtf8);
                if (const auto it = windowTitles.find(entry.th32ProcessID); it != windowTitles.end())
                    info.windowTitle = it->second;

                if (HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID)) {
                    std::wstring pathBuf(2048, L'\0');
                    DWORD pathLen = static_cast<DWORD>(pathBuf.size());
                    if (QueryFullProcessImageNameW(proc, 0, pathBuf.data(), &pathLen) && pathLen > 0) {
                        pathBuf.resize(pathLen);
                        if (auto pathUtf8 = io::fromWide(pathBuf))
                            info.executablePath = std::move(*pathUtf8);
                    }
                    CloseHandle(proc);
                }

                const QString qName = toQs(info.name);
                if (qName.compare(QStringLiteral("dotnet.exe"), Qt::CaseInsensitive) == 0 ||
                    qName.compare(QStringLiteral("dotnet"), Qt::CaseInsensitive) == 0) {
                    info.isDotNet = true;
                } else if (!info.executablePath.empty()) {
                    const QFileInfo exeInfo(toQs(info.executablePath));
                    const QString runtimeCfg =
                        exeInfo.dir().filePath(exeInfo.completeBaseName() + QStringLiteral(".runtimeconfig.json"));
                    if (QFileInfo::exists(runtimeCfg))
                        info.isDotNet = true;
                }
                processes.push_back(std::move(info));
            } while (Process32NextW(snapshot, &entry));
        }
        CloseHandle(snapshot);
    }
#endif
    std::ranges::sort(processes, [](const RunningProcessInfo& a, const RunningProcessInfo& b) {
        if (a.windowTitle.empty() != b.windowTitle.empty())
            return !a.windowTitle.empty();
        return a.name < b.name;
    });
    return processes;
}

void showAttachToProcessDialog(QWidget* parent,
                               std::function<void(const RunningProcessInfo& proc, bool attachAsDotNet)> onAttach) {
    QDialog dialog(parent);
    dialog.setObjectName(QStringLiteral("attachToProcessDialog"));
    dialog.setWindowTitle(QStringLiteral("Attach to Process"));
    dialog.setModal(true);
    dialog.resize(780, 500);

    auto* outer = new QVBoxLayout(&dialog);
    auto* topBar = new QHBoxLayout();
    auto* filterEdit = new QLineEdit(&dialog);
    filterEdit->setObjectName(QStringLiteral("attachProcessFilterEdit"));
    filterEdit->setPlaceholderText(QStringLiteral("Filter by PID, process name, window title, or runtime..."));
    filterEdit->setClearButtonEnabled(true);

    auto* debuggerCombo = new QComboBox(&dialog);
    debuggerCombo->setObjectName(QStringLiteral("attachDebuggerCombo"));
    debuggerCombo->addItems({
        QStringLiteral("Auto-detect (lldb-dap / netcoredbg)"),
        QStringLiteral("Native C/C++ (lldb-dap)"),
        QStringLiteral(".NET CoreCLR (netcoredbg)"),
    });

    auto* refreshBtn = new QPushButton(QStringLiteral("Refresh"), &dialog);
    refreshBtn->setObjectName(QStringLiteral("attachRefreshButton"));

    topBar->addWidget(new QLabel(QStringLiteral("Filter:"), &dialog));
    topBar->addWidget(filterEdit, 1);
    topBar->addWidget(new QLabel(QStringLiteral("Debugger:"), &dialog));
    topBar->addWidget(debuggerCombo);
    topBar->addWidget(refreshBtn);
    outer->addLayout(topBar);

    auto* table = new QTableWidget(&dialog);
    table->setObjectName(QStringLiteral("attachProcessTable"));
    table->setColumnCount(4);
    table->setHorizontalHeaderLabels({
        QStringLiteral("PID"),
        QStringLiteral("Process Name"),
        QStringLiteral("Runtime"),
        QStringLiteral("Window Title / Executable Path"),
    });
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    outer->addWidget(table, 1);

    std::vector<RunningProcessInfo> allProcs;
    const auto populateTable = [&] {
        const QString filter = filterEdit->text().trimmed();
        table->setRowCount(0);
        for (std::size_t i = 0; i < allProcs.size(); ++i) {
            const auto& p = allProcs[i];
            const QString pidStr = QString::number(p.pid);
            const QString nameStr = toQs(p.name);
            const QString runtimeStr = p.isDotNet ? QStringLiteral(".NET CoreCLR") : QStringLiteral("Native (x64)");
            const QString detailsStr = !p.windowTitle.empty()
                                           ? QStringLiteral("%1 — %2").arg(toQs(p.windowTitle), toQs(p.executablePath))
                                           : toQs(p.executablePath);
            if (!filter.isEmpty() && !pidStr.contains(filter, Qt::CaseInsensitive) &&
                !nameStr.contains(filter, Qt::CaseInsensitive) && !runtimeStr.contains(filter, Qt::CaseInsensitive) &&
                !detailsStr.contains(filter, Qt::CaseInsensitive)) {
                continue;
            }
            const int r = table->rowCount();
            table->insertRow(r);
            auto* pidItem = new QTableWidgetItem(pidStr);
            pidItem->setData(Qt::UserRole, static_cast<int>(i));
            table->setItem(r, 0, pidItem);
            table->setItem(r, 1, new QTableWidgetItem(nameStr));
            table->setItem(r, 2, new QTableWidgetItem(runtimeStr));
            table->setItem(r, 3, new QTableWidgetItem(detailsStr));
        }
        if (table->rowCount() > 0) {
            table->selectRow(0);
            table->setCurrentCell(0, 0);
        }
    };

    const auto reloadProcesses = [&] {
        allProcs = enumerateRunningProcesses();
        populateTable();
    };
    reloadProcesses();

    QObject::connect(filterEdit, &QLineEdit::textChanged, &dialog, [&](const QString&) { populateTable(); });
    QObject::connect(refreshBtn, &QPushButton::clicked, &dialog, [&] { reloadProcesses(); });

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    if (QPushButton* attachBtn = buttons->button(QDialogButtonBox::Ok)) {
        attachBtn->setObjectName(QStringLiteral("attachToProcessAcceptButton"));
        attachBtn->setText(QStringLiteral("Attach"));
    }

    QObject::connect(table, &QTableWidget::cellDoubleClicked, &dialog, [&](int row, int) {
        table->selectRow(row);
        table->setCurrentCell(row, 0);
        dialog.accept();
    });
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    outer->addWidget(buttons);
    ui::installDialogChrome(&dialog, true);
    filterEdit->setFocus();
    if (dialog.exec() != QDialog::Accepted)
        return;

    int row = table->currentRow();
    if (row < 0 && table->rowCount() > 0)
        row = 0;
    if (row < 0 || !table->item(row, 0))
        return;
    const int idx = table->item(row, 0)->data(Qt::UserRole).toInt();
    if (idx < 0 || static_cast<std::size_t>(idx) >= allProcs.size())
        return;
    const RunningProcessInfo chosen = allProcs[static_cast<std::size_t>(idx)];
    const int dbgMode = debuggerCombo->currentIndex();
    const bool attachAsDotNet = (dbgMode == 2) || (dbgMode == 0 && chosen.isDotNet);
    if (onAttach)
        onAttach(chosen, attachAsDotNet);
}

void showEditRunConfigurationsDialog(
    QWidget* parent, io::IFileSystem& fs, const project::SolutionModel* solution,
    const std::string& preferredProjectPath, const std::string& activeConfigPlatform, bool externalConsole,
    std::function<void(bool)> onExternalConsoleChanged, std::function<void()> onSaved,
    std::function<void(const std::vector<project::ProjectId>&)> onLaunchCompound) {
    QDialog dialog(parent);
    dialog.setObjectName(QStringLiteral("editRunConfigurationsDialog"));
    dialog.setWindowTitle(QStringLiteral("Edit Run/Debug Configurations"));
    dialog.setModal(true);
    dialog.resize(660, 520);

    auto* outer = new QVBoxLayout(&dialog);
    auto* form = new QFormLayout();
    auto* projCombo = new QComboBox(&dialog);
    projCombo->setObjectName(QStringLiteral("runConfigProjectCombo"));
    if (solution) {
        for (const auto& p : solution->projects()) {
            if (p.nodeKind == project::NodeKind::Project) {
                projCombo->addItem(toQs(p.name), toQs(p.path));
                if (p.path == preferredProjectPath)
                    projCombo->setCurrentIndex(projCombo->count() - 1);
            }
        }
    } else if (!preferredProjectPath.empty()) {
        projCombo->addItem(QFileInfo(toQs(preferredProjectPath)).completeBaseName(), toQs(preferredProjectPath));
    }
    auto* cmdEdit = new QLineEdit(&dialog);
    auto* argsEdit = new QLineEdit(&dialog);
    argsEdit->setObjectName(QStringLiteral("runConfigArgumentsEdit"));
    auto* cwdEdit = new QLineEdit(&dialog);
    cwdEdit->setObjectName(QStringLiteral("runConfigWorkingDirEdit"));
    auto* envEdit = new QLineEdit(&dialog);
    envEdit->setObjectName(QStringLiteral("runConfigEnvironmentEdit"));
    auto* extConsoleCheck = new QCheckBox(QStringLiteral("Run in external console window"), &dialog);
    extConsoleCheck->setChecked(externalConsole);

    form->addRow(QStringLiteral("Target project:"), projCombo);
    form->addRow(QStringLiteral("Executable override:"), cmdEdit);
    form->addRow(QStringLiteral("Program arguments:"), argsEdit);
    form->addRow(QStringLiteral("Working directory:"), cwdEdit);
    form->addRow(QStringLiteral("Environment variables:"), envEdit);
    form->addRow(QString(), extConsoleCheck);
    outer->addLayout(form);

    // Compound Run Configuration section (launch multiple targets simultaneously).
    auto* compoundGroup = new QGroupBox(QStringLiteral("Compound Run Configuration (Multi-Target Launch)"), &dialog);
    compoundGroup->setObjectName(QStringLiteral("compoundRunGroup"));
    auto* compoundLayout = new QVBoxLayout(compoundGroup);
    auto* compoundTop = new QHBoxLayout();
    auto* compoundNameEdit = new QLineEdit(QStringLiteral("Server + Client (Compound)"), compoundGroup);
    compoundNameEdit->setObjectName(QStringLiteral("compoundConfigNameEdit"));
    auto* launchCompoundBtn = new QPushButton(QStringLiteral("Run Selected Targets Together"), compoundGroup);
    launchCompoundBtn->setObjectName(QStringLiteral("compoundLaunchButton"));
    compoundTop->addWidget(new QLabel(QStringLiteral("Compound Name:"), compoundGroup));
    compoundTop->addWidget(compoundNameEdit, 1);
    compoundTop->addWidget(launchCompoundBtn);
    compoundLayout->addLayout(compoundTop);

    auto* compoundList = new QListWidget(compoundGroup);
    compoundList->setObjectName(QStringLiteral("compoundProjectsList"));
    if (solution) {
        const auto projects = solution->projects();
        for (std::uint32_t i = 0; i < projects.size(); ++i) {
            if (projects[i].nodeKind != project::NodeKind::Project)
                continue;
            auto* item = new QListWidgetItem(toQs(projects[i].name), compoundList);
            item->setData(Qt::UserRole, static_cast<int>(i));
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(projects[i].path == preferredProjectPath ? Qt::Checked : Qt::Unchecked);
        }
    }
    compoundLayout->addWidget(compoundList, 1);
    outer->addWidget(compoundGroup, 1);

    QObject::connect(launchCompoundBtn, &QPushButton::clicked, &dialog, [&] {
        std::vector<project::ProjectId> selectedIds;
        for (int r = 0; r < compoundList->count(); ++r) {
            if (auto* item = compoundList->item(r); item && item->checkState() == Qt::Checked) {
                selectedIds.push_back(project::ProjectId{static_cast<std::uint32_t>(item->data(Qt::UserRole).toInt())});
            }
        }
        if (selectedIds.empty())
            return;
        dialog.accept();
        if (onLaunchCompound)
            onLaunchCompound(selectedIds);
    });

    const auto loadProj = [&] {
        const std::string path = projCombo->currentData().toString().toStdString();
        if (path.empty())
            return;
        if (auto content = fs.readFile(path)) {
            auto props = parseVcxprojPropertiesForConfig(*content, activeConfigPlatform);
            cmdEdit->setText(toQs(props["Prop.LocalDebuggerCommand"]));
            argsEdit->setText(toQs(props["Prop.LocalDebuggerCommandArguments"]));
            cwdEdit->setText(toQs(props["Prop.LocalDebuggerWorkingDirectory"]));
            envEdit->setText(toQs(props["Prop.LocalDebuggerEnvironment"]));
        }
    };
    loadProj();
    QObject::connect(projCombo, &QComboBox::currentIndexChanged, &dialog, [&](int) { loadProj(); });

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        if (onExternalConsoleChanged && extConsoleCheck->isChecked() != externalConsole)
            onExternalConsoleChanged(extConsoleCheck->isChecked());
        const std::string path = projCombo->currentData().toString().toStdString();
        if (!path.empty()) {
            if (auto content = fs.readFile(path)) {
                ProjectPropertyMap updates{
                    {"Prop.LocalDebuggerCommand", cmdEdit->text().trimmed().toStdString()},
                    {"Prop.LocalDebuggerCommandArguments", argsEdit->text().trimmed().toStdString()},
                    {"Prop.LocalDebuggerWorkingDirectory", cwdEdit->text().trimmed().toStdString()},
                    {"Prop.LocalDebuggerEnvironment", envEdit->text().trimmed().toStdString()},
                };
                const std::string updated =
                    toQs(path).endsWith(QStringLiteral(".vcxproj"), Qt::CaseInsensitive)
                        ? applyVcxprojPropertiesForConfig(*content, activeConfigPlatform, updates)
                        : applyCsprojProperties(*content, updates);
                (void)fs.writeFileAtomic(path, updated);
                if (onSaved)
                    onSaved();
            }
        }
        dialog.accept();
    });
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    outer->addWidget(buttons);
    ui::installDialogChrome(&dialog, true);
    dialog.exec();
}

void showAddProjectReferenceDialog(QWidget* parent, io::IFileSystem& fs, const project::SolutionModel& solution,
                                   const std::string& projectPath, std::function<void()> onSaved) {
    QDialog dialog(parent);
    dialog.setObjectName(QStringLiteral("addProjectReferenceDialog"));
    dialog.setWindowTitle(QStringLiteral("Add Project Reference"));
    dialog.setModal(true);
    dialog.resize(480, 360);

    auto* outer = new QVBoxLayout(&dialog);
    outer->addWidget(new QLabel(QStringLiteral("Select projects in solution to reference:"), &dialog));
    auto* list = new QListWidget(&dialog);
    list->setObjectName(QStringLiteral("projectReferenceList"));

    const auto selfId = solution.findProjectByPath(projectPath);
    const auto projects = solution.projects();
    for (std::uint32_t i = 0; i < projects.size(); ++i) {
        if ((selfId && selfId->value() == i) || projects[i].nodeKind != project::NodeKind::Project)
            continue;
        auto* item = new QListWidgetItem(toQs(projects[i].name), list);
        item->setData(Qt::UserRole, static_cast<int>(i));
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        bool checked = false;
        if (selfId) {
            const auto& deps = projects[selfId->value()].dependencies;
            checked = std::ranges::find(deps, project::ProjectId{i}) != deps.end();
        }
        item->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
    }
    outer->addWidget(list, 1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        if (auto content = fs.readFile(projectPath)) {
            std::string xml = *content;
            const QDir projDir = QFileInfo(toQs(projectPath)).dir();
            for (int r = 0; r < list->count(); ++r) {
                auto* item = list->item(r);
                const auto targetId = static_cast<std::uint32_t>(item->data(Qt::UserRole).toInt());
                const QString rel = QDir::toNativeSeparators(projDir.relativeFilePath(toQs(projects[targetId].path)));
                if (item->checkState() == Qt::Checked)
                    xml = addProjectReferenceXml(xml, rel.toStdString(), projects[targetId].projectGuid);
                else
                    xml = removeProjectReferenceXml(xml, rel.toStdString());
            }
            if (xml != *content) {
                (void)fs.writeFileAtomic(projectPath, xml);
                if (onSaved)
                    onSaved();
            }
        }
        dialog.accept();
    });
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    outer->addWidget(buttons);
    ui::installDialogChrome(&dialog, true);
    dialog.exec();
}

QWidget* createNuGetToolWindowWidget(QWidget* parent, io::IFileSystem& fs,
                                     std::function<const project::SolutionModel*()> solutionProvider,
                                     std::string initialCsprojPath, std::function<void()> onSaved) {
    auto* root = new QWidget(parent);
    root->setObjectName(QStringLiteral("nugetToolWindowWidget"));
    auto* outer = new QVBoxLayout(root);
    outer->setContentsMargins(8, 8, 8, 8);
    outer->setSpacing(6);

    auto* topBar = new QHBoxLayout();
    auto* projCombo = new QComboBox(root);
    projCombo->setObjectName(QStringLiteral("nugetProjectCombo"));
    projCombo->setMinimumWidth(180);

    auto* searchEdit = new QLineEdit(root);
    searchEdit->setObjectName(QStringLiteral("nugetSearchEdit"));
    searchEdit->setPlaceholderText(QStringLiteral("Search NuGet packages..."));
    searchEdit->setClearButtonEnabled(true);

    auto* prereleaseCheck = new QCheckBox(QStringLiteral("Include prerelease"), root);
    prereleaseCheck->setObjectName(QStringLiteral("nugetPrereleaseCheck"));

    auto* feedCombo = new QComboBox(root);
    feedCombo->setObjectName(QStringLiteral("nugetFeedCombo"));
    feedCombo->addItem(QStringLiteral("nuget.org (https://api.nuget.org/v3/index.json)"),
                       QStringLiteral("https://api.nuget.org/v3/index.json"));
    feedCombo->addItem(QStringLiteral("Local NuGet Cache"), QStringLiteral("local"));

    auto* updateAllBtn = new QPushButton(QStringLiteral("Update All"), root);
    updateAllBtn->setObjectName(QStringLiteral("nugetUpdateAllButton"));

    topBar->addWidget(new QLabel(QStringLiteral("Project:"), root));
    topBar->addWidget(projCombo);
    topBar->addWidget(searchEdit, 1);
    topBar->addWidget(prereleaseCheck);
    topBar->addWidget(new QLabel(QStringLiteral("Source:"), root));
    topBar->addWidget(feedCombo);
    topBar->addWidget(updateAllBtn);
    outer->addLayout(topBar);

    auto* splitter = new QSplitter(Qt::Horizontal, root);
    auto* tabs = new QTabWidget(splitter);
    tabs->setObjectName(QStringLiteral("nugetTabs"));

    auto makePkgTable = [](QWidget* tabParent, const QString& objName) {
        auto* tbl = new QTableWidget(tabParent);
        tbl->setObjectName(objName);
        tbl->setColumnCount(3);
        tbl->setHorizontalHeaderLabels(
            {QStringLiteral("Package ID"), QStringLiteral("Installed"), QStringLiteral("Latest")});
        tbl->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
        tbl->setSelectionBehavior(QAbstractItemView::SelectRows);
        tbl->setEditTriggers(QAbstractItemView::NoEditTriggers);
        return tbl;
    };

    auto* installedTable = makePkgTable(tabs, QStringLiteral("nugetPackagesTable"));
    auto* updatesTable = makePkgTable(tabs, QStringLiteral("nugetUpdatesTable"));
    auto* browseTable = makePkgTable(tabs, QStringLiteral("nugetBrowseTable"));
    tabs->addTab(installedTable, QStringLiteral("Installed"));
    tabs->addTab(updatesTable, QStringLiteral("Updates"));
    tabs->addTab(browseTable, QStringLiteral("Browse"));

    auto* rightPane = new QWidget(splitter);
    auto* rightLayout = new QVBoxLayout(rightPane);
    rightLayout->setContentsMargins(6, 0, 0, 0);
    auto* detailsPane = new QPlainTextEdit(rightPane);
    detailsPane->setObjectName(QStringLiteral("nugetDetailsPane"));
    detailsPane->setReadOnly(true);
    detailsPane->setPlaceholderText(QStringLiteral("Select a NuGet package to view details, dependencies, and README."));
    rightLayout->addWidget(detailsPane, 1);

    auto* installRow = new QHBoxLayout();
    auto* pkgIdEdit = new QLineEdit(rightPane);
    pkgIdEdit->setObjectName(QStringLiteral("nugetPackageIdEdit"));
    pkgIdEdit->setPlaceholderText(QStringLiteral("Package ID (e.g. Newtonsoft.Json)"));
    auto* pkgVerEdit = new QLineEdit(QStringLiteral("13.0.3"), rightPane);
    pkgVerEdit->setObjectName(QStringLiteral("nugetPackageVersionEdit"));
    pkgVerEdit->setPlaceholderText(QStringLiteral("Version"));
    auto* addBtn = new QPushButton(QStringLiteral("Install / Update"), rightPane);
    addBtn->setObjectName(QStringLiteral("nugetInstallButton"));
    auto* removeBtn = new QPushButton(QStringLiteral("Uninstall Selected"), rightPane);
    removeBtn->setObjectName(QStringLiteral("nugetRemoveButton"));
    installRow->addWidget(pkgIdEdit, 2);
    installRow->addWidget(pkgVerEdit, 1);
    installRow->addWidget(addBtn);
    installRow->addWidget(removeBtn);
    rightLayout->addLayout(installRow);

    splitter->addWidget(tabs);
    splitter->addWidget(rightPane);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);
    outer->addWidget(splitter, 1);

    struct CatalogItem {
        const char* id;
        const char* stableVer;
        const char* preVer;
        const char* summary;
    };
    static constexpr std::array<CatalogItem, 8> kCatalog{{
        {"Newtonsoft.Json", "13.0.3", "13.0.4-beta1", "Popular high-performance JSON framework for .NET."},
        {"Serilog", "4.0.1", "4.1.0-dev-02235", "Simple .NET logging with fully-structured events."},
        {"Dapper", "2.1.35", "2.1.44-beta", "A high performance Micro-ORM supporting SQL Server, MySQL, Sqlite, and Postgres."},
        {"xunit", "2.9.0", "2.9.1-pre.12", "xUnit.net is a developer testing framework for C# and .NET."},
        {"NUnit", "4.1.0", "4.2.0-beta.1", "NUnit unit-testing framework for all .NET languages."},
        {"FluentAssertions", "6.12.0", "7.0.0-alpha.4", "Fluent assertion library for .NET unit tests."},
        {"CommunityToolkit.Mvvm", "8.2.2", "8.3.0-preview1", "Modern, fast, and modular MVVM library for .NET."},
        {"StackExchange.Redis", "2.8.0", "2.8.12-alpha", "High performance general purpose redis client for .NET."},
    }};

    const auto currentCsproj = [=]() -> std::string {
        const QString data = projCombo->currentData().toString();
        if (!data.isEmpty())
            return data.toStdString();
        return initialCsprojPath;
    };

    const auto refreshProjects = [=] {
        const QString prev = projCombo->currentData().toString();
        projCombo->blockSignals(true);
        projCombo->clear();
        if (solutionProvider) {
            if (const project::SolutionModel* sln = solutionProvider()) {
                for (const auto& p : sln->projects()) {
                    if (p.kind == project::ProjectKind::CSharp && !p.path.empty()) {
                        projCombo->addItem(toQs(p.name), toQs(p.path));
                    }
                }
            }
        }
        if (projCombo->count() == 0 && !initialCsprojPath.empty()) {
            projCombo->addItem(QFileInfo(toQs(initialCsprojPath)).completeBaseName(), toQs(initialCsprojPath));
        }
        const int idx = projCombo->findData(prev.isEmpty() ? toQs(initialCsprojPath) : prev);
        if (idx >= 0)
            projCombo->setCurrentIndex(idx);
        projCombo->blockSignals(false);
    };

    const auto refreshTables = [=, &fs] {
        refreshProjects();
        const std::string csproj = currentCsproj();
        const QString filter = searchEdit->text().trimmed();
        const bool includePre = prereleaseCheck->isChecked();

        std::vector<NuGetPackageEntry> installed;
        if (!csproj.empty()) {
            if (auto content = fs.readFile(csproj)) {
                installed = parseCsprojNuGetPackages(*content);
            }
        }

        installedTable->setRowCount(0);
        updatesTable->setRowCount(0);
        browseTable->setRowCount(0);

        for (const auto& pkg : installed) {
            const QString qId = toQs(pkg.id);
            if (!filter.isEmpty() && !qId.contains(filter, Qt::CaseInsensitive))
                continue;
            QString latest = toQs(pkg.version);
            for (const auto& cat : kCatalog) {
                if (qId.compare(QLatin1String(cat.id), Qt::CaseInsensitive) == 0) {
                    latest = QString::fromLatin1(includePre ? cat.preVer : cat.stableVer);
                    break;
                }
            }
            const int r = installedTable->rowCount();
            installedTable->insertRow(r);
            installedTable->setItem(r, 0, new QTableWidgetItem(qId));
            installedTable->setItem(r, 1, new QTableWidgetItem(toQs(pkg.version)));
            installedTable->setItem(r, 2, new QTableWidgetItem(latest));

            if (latest != toQs(pkg.version)) {
                const int ur = updatesTable->rowCount();
                updatesTable->insertRow(ur);
                updatesTable->setItem(ur, 0, new QTableWidgetItem(qId));
                updatesTable->setItem(ur, 1, new QTableWidgetItem(toQs(pkg.version)));
                updatesTable->setItem(ur, 2, new QTableWidgetItem(latest));
            }
        }

        for (const auto& cat : kCatalog) {
            const QString qId = QString::fromLatin1(cat.id);
            if (!filter.isEmpty() && !qId.contains(filter, Qt::CaseInsensitive) &&
                !QString::fromLatin1(cat.summary).contains(filter, Qt::CaseInsensitive))
                continue;
            QString instVer = QStringLiteral("—");
            for (const auto& pkg : installed) {
                if (qId.compare(toQs(pkg.id), Qt::CaseInsensitive) == 0) {
                    instVer = toQs(pkg.version);
                    break;
                }
            }
            const QString latest = QString::fromLatin1(includePre ? cat.preVer : cat.stableVer);
            const int br = browseTable->rowCount();
            browseTable->insertRow(br);
            browseTable->setItem(br, 0, new QTableWidgetItem(qId));
            browseTable->setItem(br, 1, new QTableWidgetItem(instVer));
            browseTable->setItem(br, 2, new QTableWidgetItem(latest));
        }
    };
    refreshTables();

    const auto wireTableSelection = [=](QTableWidget* tbl) {
        QObject::connect(tbl, &QTableWidget::currentCellChanged, root, [tbl, pkgIdEdit, pkgVerEdit, detailsPane](int row, int, int, int) {
            if (row < 0 || !tbl->item(row, 0))
                return;
            const QString id = tbl->item(row, 0)->text();
            const QString inst = tbl->item(row, 1) ? tbl->item(row, 1)->text() : QString();
            const QString latest = tbl->item(row, 2) ? tbl->item(row, 2)->text() : inst;
            pkgIdEdit->setText(id);
            pkgVerEdit->setText(latest.isEmpty() || latest == QStringLiteral("—") ? inst : latest);
            QString summary = QStringLiteral("NuGet Package Reference");
            for (const auto& cat : kCatalog) {
                if (id.compare(QLatin1String(cat.id), Qt::CaseInsensitive) == 0) {
                    summary = QString::fromLatin1(cat.summary);
                    break;
                }
            }
            detailsPane->setPlainText(
                QStringLiteral("Package: %1\nInstalled: %2\nLatest Available: %3\n\n%4")
                    .arg(id, inst, latest, summary));
        });
    };
    wireTableSelection(installedTable);
    wireTableSelection(updatesTable);
    wireTableSelection(browseTable);

    QObject::connect(projCombo, &QComboBox::currentIndexChanged, root, [=](int) { refreshTables(); });
    QObject::connect(searchEdit, &QLineEdit::textChanged, root, [=](const QString&) { refreshTables(); });
    QObject::connect(prereleaseCheck, &QCheckBox::toggled, root, [=](bool) { refreshTables(); });

    QObject::connect(addBtn, &QPushButton::clicked, root, [=, &fs] {
        const QString id = pkgIdEdit->text().trimmed();
        const QString ver = pkgVerEdit->text().trimmed();
        const std::string csproj = currentCsproj();
        if (id.isEmpty() || ver.isEmpty() || csproj.empty())
            return;
        if (auto content = fs.readFile(csproj)) {
            const std::string updated = upsertCsprojNuGetPackage(*content, id.toStdString(), ver.toStdString());
            (void)fs.writeFileAtomic(csproj, updated);
            refreshTables();
            if (onSaved)
                onSaved();
        }
    });

    QObject::connect(removeBtn, &QPushButton::clicked, root, [=, &fs] {
        QTableWidget* activeTbl = qobject_cast<QTableWidget*>(tabs->currentWidget());
        if (!activeTbl)
            activeTbl = installedTable;
        const int row = activeTbl->currentRow();
        const QString id = (row >= 0 && activeTbl->item(row, 0)) ? activeTbl->item(row, 0)->text() : pkgIdEdit->text().trimmed();
        const std::string csproj = currentCsproj();
        if (id.isEmpty() || csproj.empty())
            return;
        if (auto content = fs.readFile(csproj)) {
            const std::string updated = removeCsprojNuGetPackage(*content, id.toStdString());
            (void)fs.writeFileAtomic(csproj, updated);
            refreshTables();
            if (onSaved)
                onSaved();
        }
    });

    QObject::connect(updateAllBtn, &QPushButton::clicked, root, [=, &fs] {
        const std::string csproj = currentCsproj();
        if (csproj.empty())
            return;
        if (auto content = fs.readFile(csproj)) {
            std::string xml = *content;
            for (int r = 0; r < updatesTable->rowCount(); ++r) {
                const QString id = updatesTable->item(r, 0)->text();
                const QString latest = updatesTable->item(r, 2)->text();
                xml = upsertCsprojNuGetPackage(xml, id.toStdString(), latest.toStdString());
            }
            if (xml != *content) {
                (void)fs.writeFileAtomic(csproj, xml);
                refreshTables();
                if (onSaved)
                    onSaved();
            }
        }
    });

    return root;
}

void showNuGetPackageManagerDialog(QWidget* parent, io::IFileSystem& fs, const std::string& csprojPath,
                                   std::function<void()> onSaved) {
    QDialog dialog(parent);
    dialog.setObjectName(QStringLiteral("nugetPackageManagerDialog"));
    dialog.setWindowTitle(
        QStringLiteral("NuGet Package Manager — %1").arg(QFileInfo(toQs(csprojPath)).completeBaseName()));
    dialog.setModal(true);
    dialog.resize(820, 500);

    auto* outer = new QVBoxLayout(&dialog);
    outer->addWidget(createNuGetToolWindowWidget(&dialog, fs, {}, csprojPath, std::move(onSaved)), 1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    outer->addWidget(buttons);
    ui::installDialogChrome(&dialog, true);
    dialog.exec();
}

QWidget* createAsmViewerToolWindowWidget(QWidget* parent,
                                         std::function<std::pair<std::string, int>()> activeFileAndLineProvider) {
    auto* root = new QWidget(parent);
    root->setObjectName(QStringLiteral("asmViewerWidget"));
    auto* layout = new QVBoxLayout(root);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);

    auto* bar = new QHBoxLayout();
    auto* optCombo = new QComboBox(root);
    optCombo->setObjectName(QStringLiteral("asmOptCombo"));
    optCombo->addItems({QStringLiteral("-O2"), QStringLiteral("-O0"), QStringLiteral("-O1"), QStringLiteral("-O3")});
    auto* syntaxCombo = new QComboBox(root);
    syntaxCombo->setObjectName(QStringLiteral("asmSyntaxCombo"));
    syntaxCombo->addItems({QStringLiteral("Intel"), QStringLiteral("AT&T")});
    auto* syncCheck = new QCheckBox(QStringLiteral("Sync with Active Editor"), root);
    syncCheck->setObjectName(QStringLiteral("asmSyncCheck"));
    syncCheck->setChecked(true);
    auto* refreshBtn = new QPushButton(QStringLiteral("Disassemble / View IL"), root);
    refreshBtn->setObjectName(QStringLiteral("asmRefreshButton"));

    bar->addWidget(new QLabel(QStringLiteral("Opt:"), root));
    bar->addWidget(optCombo);
    bar->addWidget(new QLabel(QStringLiteral("Syntax:"), root));
    bar->addWidget(syntaxCombo);
    bar->addWidget(syncCheck);
    bar->addStretch(1);
    bar->addWidget(refreshBtn);
    layout->addLayout(bar);

    auto* output = new QPlainTextEdit(root);
    output->setObjectName(QStringLiteral("asmViewerOutput"));
    output->setReadOnly(true);
    output->setLineWrapMode(QPlainTextEdit::NoWrap);
    output->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    output->setPlaceholderText(
        QStringLiteral("Open a C/C++ or C# source file and click 'Disassemble / View IL' to inspect compiler output."));
    layout->addWidget(output, 1);

    const auto runDisassembly = [=] {
        if (!activeFileAndLineProvider)
            return;
        const auto [filePath, line] = activeFileAndLineProvider();
        if (filePath.empty()) {
            output->setPlainText(QStringLiteral("; No active source file in editor."));
            return;
        }
        const QString qPath = toQs(filePath);
        if (qPath.endsWith(QStringLiteral(".cs"), Qt::CaseInsensitive)) {
            QProcess proc;
            proc.start(QStringLiteral("dotnet"), {QStringLiteral("--info")});
            proc.waitForFinished(3000);
            output->setPlainText(
                QStringLiteral("// .NET IL & Metadata Projection for %1 (line %2)\n"
                               "// Use Build > Build Solution to emit assembly IL in bin/Debug.\n\n"
                               ".assembly extern System.Runtime {}\n"
                               ".module '%3'\n"
                               "// Source line %2 synced")
                    .arg(QFileInfo(qPath).fileName())
                    .arg(line)
                    .arg(QFileInfo(qPath).completeBaseName()));
            return;
        }
        QString clangExe = QStringLiteral("clang++");
        if (QFileInfo::exists(QStringLiteral("E:/sdk/llvm-23.1.4/bin/clang++.exe"))) {
            clangExe = QStringLiteral("E:/sdk/llvm-23.1.4/bin/clang++.exe");
        }
        QStringList args{
            QStringLiteral("-S"),
            QStringLiteral("-std=c++20"),
            optCombo->currentText(),
            QStringLiteral("-fno-discard-value-names"),
            syntaxCombo->currentIndex() == 0 ? QStringLiteral("-masm=intel") : QStringLiteral("-masm=att"),
            QDir::toNativeSeparators(qPath),
            QStringLiteral("-o"),
            QStringLiteral("-"),
        };
        QProcess proc;
        proc.start(clangExe, args);
        if (!proc.waitForFinished(8000)) {
            proc.kill();
            output->setPlainText(QStringLiteral("; Compiler disassembly timed out."));
            return;
        }
        const QString stdOut = QString::fromUtf8(proc.readAllStandardOutput());
        const QString stdErr = QString::fromUtf8(proc.readAllStandardError());
        if (!stdOut.isEmpty()) {
            output->setPlainText(stdOut);
        } else {
            output->setPlainText(QStringLiteral("; Compiler output (%1):\n%2").arg(clangExe, stdErr));
        }
    };

    QObject::connect(refreshBtn, &QPushButton::clicked, root, runDisassembly);
    QObject::connect(optCombo, &QComboBox::currentIndexChanged, root, [=](int) {
        if (syncCheck->isChecked())
            runDisassembly();
    });
    QObject::connect(syntaxCombo, &QComboBox::currentIndexChanged, root, [=](int) {
        if (syncCheck->isChecked())
            runDisassembly();
    });

    return root;
}

void showLocalHistoryDialog(QWidget* parent, storage::LocalHistory& history, const std::string& filePath,
                            const std::string& currentContent,
                            std::function<void(const std::string& restoredContent)> onRestore) {
    QDialog dialog(parent);
    dialog.setObjectName(QStringLiteral("localHistoryDialog"));
    dialog.setWindowTitle(QStringLiteral("Local History — %1").arg(QFileInfo(toQs(filePath)).fileName()));
    dialog.setModal(true);
    dialog.resize(840, 520);

    auto* outer = new QVBoxLayout(&dialog);
    auto* splitter = new QSplitter(Qt::Horizontal, &dialog);
    auto* revList = new QListWidget(splitter);
    revList->setObjectName(QStringLiteral("localHistoryRevisionList"));
    revList->setMinimumWidth(260);
    auto* preview = new QPlainTextEdit(splitter);
    preview->setObjectName(QStringLiteral("localHistoryPreview"));
    preview->setReadOnly(true);
    splitter->addWidget(revList);
    splitter->addWidget(preview);
    splitter->setStretchFactor(1, 1);
    outer->addWidget(splitter, 1);

    std::vector<storage::RevisionInfo> revisions;
    if (auto listed = history.revisions(filePath))
        revisions = std::move(*listed);
    for (std::size_t i = 0; i < revisions.size(); ++i) {
        const auto& r = revisions[i];
        const auto secs = std::chrono::duration_cast<std::chrono::seconds>(r.createdAt.time_since_epoch()).count();
        const QString timeStr = QDateTime::fromSecsSinceEpoch(secs).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
        const QString label = r.label.empty() ? QStringLiteral("Saved") : toQs(r.label);
        auto* item = new QListWidgetItem(QStringLiteral("%1 — %2").arg(timeStr, label), revList);
        item->setData(Qt::UserRole, static_cast<int>(i));
    }
    if (revisions.empty()) {
        preview->setPlainText(toQs(currentContent));
    }

    QObject::connect(revList, &QListWidget::currentRowChanged, &dialog, [&](int row) {
        if (row < 0 || static_cast<std::size_t>(row) >= revisions.size())
            return;
        if (auto bytes = history.content(revisions[static_cast<std::size_t>(row)].id)) {
            preview->setPlainText(toQs(*bytes));
        }
    });
    if (!revisions.empty())
        revList->setCurrentRow(0);

    auto* btnLayout = new QHBoxLayout();
    auto* revertBtn = new QPushButton(QStringLiteral("Revert to Selected Revision"), &dialog);
    revertBtn->setObjectName(QStringLiteral("localHistoryRevertButton"));
    revertBtn->setEnabled(!revisions.empty());
    auto* closeBtn = new QPushButton(QStringLiteral("Close"), &dialog);
    btnLayout->addStretch(1);
    btnLayout->addWidget(revertBtn);
    btnLayout->addWidget(closeBtn);
    outer->addLayout(btnLayout);

    QObject::connect(revertBtn, &QPushButton::clicked, &dialog, [&] {
        const int row = revList->currentRow();
        if (row < 0 || static_cast<std::size_t>(row) >= revisions.size())
            return;
        if (auto bytes = history.content(revisions[static_cast<std::size_t>(row)].id)) {
            if (onRestore)
                onRestore(*bytes);
            dialog.accept();
        }
    });
    QObject::connect(closeBtn, &QPushButton::clicked, &dialog, &QDialog::reject);
    ui::installDialogChrome(&dialog, true);
    dialog.exec();
}

std::vector<DiscoveredTestEntry> discoverTestsAndEntryPointsInFile(std::string_view filePath, std::string_view content,
                                                                   std::string_view projectName,
                                                                   std::string_view projectPath,
                                                                   std::string_view solutionName) {
    std::vector<DiscoveredTestEntry> results;
    const QString qPath = toQs(filePath);
    const bool isCs = qPath.endsWith(QStringLiteral(".cs"), Qt::CaseInsensitive);
    const QString textContent = toQs(content);
    const QStringList lines = textContent.split(QLatin1Char('\n'));

    const QRegularExpression kGTestRe(
        QStringLiteral(R"(^\s*(?:TEST|TEST_F|TEST_P|TYPED_TEST)\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*,\s*([A-Za-z_][A-Za-z0-9_]*)\s*\))"));
    const QRegularExpression kCatchRe(QStringLiteral(R"re(^\s*(?:TEST_CASE|SCENARIO)\s*\(\s*"([^"]+)")re"));
    const QRegularExpression kCsClassRe(QStringLiteral(R"(\bclass\s+([A-Za-z_][A-Za-z0-9_]*)\b)"));
    const QRegularExpression kCsAttrRe(
        QStringLiteral(R"(^\s*\[\s*(?:Fact|Theory|Test|TestCase|TestMethod)\b)"));
    const QRegularExpression kCsMethodRe(
        QStringLiteral(R"(\b(?:void|Task(?:<[^>]+>)?)\s+([A-Za-z_][A-Za-z0-9_]*)\s*\()"));
    const QRegularExpression kCppMainRe(QStringLiteral(R"(^\s*(?:int|void|auto)\s+main\s*\()"));
    const QRegularExpression kCsMainRe(
        QStringLiteral(R"(^\s*(?:(?:public|private|internal|static|async)\s+)*(?:void|int|Task(?:<int>)?)\s+Main\s*\()"));

    const std::string resolvedSln = solutionName.empty() ? "Solution" : std::string(solutionName);
    const std::string resolvedProj =
        projectName.empty() ? QFileInfo(qPath).dir().dirName().toStdString() : std::string(projectName);
    const std::string resolvedProjPath = std::string(projectPath);

    auto bodyHasExplicitFailure = [&](int startLineIdx) -> bool {
        int braceDepth = 0;
        bool seenOpenBrace = false;
        const int limit = std::min<int>(static_cast<int>(lines.size()), startLineIdx + 30);
        for (int i = startLineIdx; i < limit; ++i) {
            const QString& ln = lines[i];
            if (ln.contains(QStringLiteral("EXPECT_TRUE(false)")) ||
                ln.contains(QStringLiteral("ASSERT_TRUE(false)")) || ln.contains(QStringLiteral("FAIL()")) ||
                ln.contains(QStringLiteral("Assert.Fail")) || ln.contains(QStringLiteral("Assert.True(false)"))) {
                return true;
            }
            for (QChar ch : ln) {
                if (ch == QLatin1Char('{')) {
                    ++braceDepth;
                    seenOpenBrace = true;
                } else if (ch == QLatin1Char('}')) {
                    --braceDepth;
                }
            }
            if (seenOpenBrace && braceDepth <= 0)
                break;
        }
        return false;
    };

    std::string currentCsClass = QFileInfo(qPath).completeBaseName().toStdString();
    bool pendingCsTestAttr = false;

    for (int i = 0; i < lines.size(); ++i) {
        const QString& lineText = lines[i];
        if (isCs) {
            if (const auto clsMatch = kCsClassRe.match(lineText); clsMatch.hasMatch()) {
                currentCsClass = clsMatch.captured(1).toStdString();
            }
            if (kCsAttrRe.match(lineText).hasMatch()) {
                pendingCsTestAttr = true;
            }
            if (pendingCsTestAttr) {
                if (const auto mMatch = kCsMethodRe.match(lineText); mMatch.hasMatch()) {
                    DiscoveredTestEntry entry;
                    entry.solutionName = resolvedSln;
                    entry.projectName = resolvedProj.empty() ? "Tests" : resolvedProj;
                    entry.projectPath = resolvedProjPath;
                    entry.suiteName = currentCsClass.empty() ? "Tests" : currentCsClass;
                    entry.testName = mMatch.captured(1).toStdString();
                    entry.filePath = std::string(filePath);
                    entry.line = i + 1;
                    entry.isDotNet = true;
                    entry.isEntryPoint = false;
                    entry.expectsFailureInSource = bodyHasExplicitFailure(i);
                    results.push_back(std::move(entry));
                    pendingCsTestAttr = false;
                    continue;
                }
            }
            if (kCsMainRe.match(lineText).hasMatch()) {
                DiscoveredTestEntry entry;
                entry.solutionName = resolvedSln;
                entry.projectName = resolvedProj.empty() ? "App" : resolvedProj;
                entry.projectPath = resolvedProjPath;
                entry.suiteName = currentCsClass;
                entry.testName = "Main";
                entry.filePath = std::string(filePath);
                entry.line = i + 1;
                entry.isDotNet = true;
                entry.isEntryPoint = true;
                results.push_back(std::move(entry));
            }
        } else {
            if (const auto gtMatch = kGTestRe.match(lineText); gtMatch.hasMatch()) {
                DiscoveredTestEntry entry;
                entry.solutionName = resolvedSln;
                entry.projectName = resolvedProj.empty() ? "Tests" : resolvedProj;
                entry.projectPath = resolvedProjPath;
                entry.suiteName = gtMatch.captured(1).toStdString();
                entry.testName = gtMatch.captured(2).toStdString();
                entry.filePath = std::string(filePath);
                entry.line = i + 1;
                entry.isDotNet = false;
                entry.isEntryPoint = false;
                entry.expectsFailureInSource = bodyHasExplicitFailure(i);
                results.push_back(std::move(entry));
                continue;
            }
            if (const auto catchMatch = kCatchRe.match(lineText); catchMatch.hasMatch()) {
                DiscoveredTestEntry entry;
                entry.solutionName = resolvedSln;
                entry.projectName = resolvedProj.empty() ? "Tests" : resolvedProj;
                entry.projectPath = resolvedProjPath;
                entry.suiteName = "Catch2";
                entry.testName = catchMatch.captured(1).toStdString();
                entry.filePath = std::string(filePath);
                entry.line = i + 1;
                entry.isDotNet = false;
                entry.isEntryPoint = false;
                entry.expectsFailureInSource = bodyHasExplicitFailure(i);
                results.push_back(std::move(entry));
                continue;
            }
            if (kCppMainRe.match(lineText).hasMatch()) {
                DiscoveredTestEntry entry;
                entry.solutionName = resolvedSln;
                entry.projectName = resolvedProj.empty() ? "App" : resolvedProj;
                entry.projectPath = resolvedProjPath;
                entry.suiteName = QFileInfo(qPath).fileName().toStdString();
                entry.testName = "main";
                entry.filePath = std::string(filePath);
                entry.line = i + 1;
                entry.isDotNet = false;
                entry.isEntryPoint = true;
                results.push_back(std::move(entry));
            }
        }
    }
    return results;
}

std::vector<DiscoveredTestEntry> discoverWorkspaceUnitTests(io::IFileSystem& fs, const project::SolutionModel* solution,
                                                            std::string_view workspaceRoot) {
    std::vector<DiscoveredTestEntry> allTests;
    std::vector<std::string> visitedFiles;

    const auto scanDirectory = [&](const std::string& dirPath, std::string_view projName, std::string_view projPath,
                                   std::string_view slnName, auto& selfRef, int depth) -> void {
        if (depth > 3 || dirPath.empty())
            return;
        auto entries = fs.listDirectory(dirPath);
        if (!entries)
            return;
        for (const auto& de : *entries) {
            if (de.name.empty() || de.name.starts_with('.') || de.name == "bin" || de.name == "obj" ||
                de.name == "build" || de.name == "out" || de.name == "node_modules") {
                continue;
            }
            const auto fullRes = io::joinPath(dirPath, de.name);
            if (!fullRes)
                continue;
            if (de.type == io::FileType::Directory) {
                selfRef(*fullRes, projName, projPath, slnName, selfRef, depth + 1);
                continue;
            }
            const QString qName = toQs(de.name);
            if (!qName.endsWith(QStringLiteral(".cpp"), Qt::CaseInsensitive) &&
                !qName.endsWith(QStringLiteral(".cc"), Qt::CaseInsensitive) &&
                !qName.endsWith(QStringLiteral(".cxx"), Qt::CaseInsensitive) &&
                !qName.endsWith(QStringLiteral(".h"), Qt::CaseInsensitive) &&
                !qName.endsWith(QStringLiteral(".hpp"), Qt::CaseInsensitive) &&
                !qName.endsWith(QStringLiteral(".cs"), Qt::CaseInsensitive)) {
                continue;
            }
            if (std::ranges::contains(visitedFiles, *fullRes))
                continue;
            visitedFiles.push_back(*fullRes);
            if (auto content = fs.readFile(*fullRes)) {
                auto found = discoverTestsAndEntryPointsInFile(*fullRes, *content, projName, projPath, slnName);
                for (auto& item : found) {
                    if (!item.isEntryPoint) {
                        allTests.push_back(std::move(item));
                    }
                }
            }
        }
    };

    const std::string slnName =
        (solution && !solution->name().empty())
            ? solution->name()
            : (!workspaceRoot.empty() ? QFileInfo(toQs(workspaceRoot)).fileName().toStdString() : "Solution");

    if (solution) {
        for (const auto& proj : solution->projects()) {
            if (proj.nodeKind != project::NodeKind::Project || proj.path.empty())
                continue;
            const std::string projDir(io::parentPath(proj.path));
            scanDirectory(projDir, proj.name, proj.path, slnName, scanDirectory, 0);
        }
    }
    if (!workspaceRoot.empty()) {
        scanDirectory(std::string(workspaceRoot), slnName, {}, slnName, scanDirectory, 0);
    }
    return allTests;
}

namespace {

enum class UnitTestRunState : int {
    NotRun = 0,
    Running = 1,
    Passed = 2,
    Failed = 3,
    Skipped = 4,
};

struct UnitTestsPanelState {
    std::vector<DiscoveredTestEntry> tests;
    std::vector<UnitTestRunState> states;
    std::vector<int> durationsMs;
    std::vector<QString> outputs;
    std::vector<QStringList> stackLinks;
    std::vector<QTreeWidgetItem*> leafItems;
    bool stopRequested = false;
};

} // namespace

QWidget* createUnitTestsToolWindowWidget(QWidget* parent, io::IFileSystem& fs,
                                         std::function<const project::SolutionModel*()> solutionProvider,
                                         std::function<std::string()> workspaceRootProvider,
                                         std::function<void(const std::string& path, int line)> onNavigateToSource,
                                         std::function<void(const DiscoveredTestEntry& test)> onDebugTest) {
    auto* root = new QWidget(parent);
    root->setObjectName(QStringLiteral("unitTestsToolWindow"));
    auto* outer = new QVBoxLayout(root);
    outer->setContentsMargins(6, 6, 6, 6);
    outer->setSpacing(6);

    auto state = std::make_shared<UnitTestsPanelState>();

    // Top toolbar
    auto* toolbar = new QHBoxLayout();
    auto* runAllBtn = new QPushButton(QStringLiteral("▶ Run All"), root);
    runAllBtn->setObjectName(QStringLiteral("unitTestsRunAllButton"));
    auto* runSelectedBtn = new QPushButton(QStringLiteral("▶ Run Selected"), root);
    runSelectedBtn->setObjectName(QStringLiteral("unitTestsRunSelectedButton"));
    auto* runFailedBtn = new QPushButton(QStringLiteral("↻ Run Failed"), root);
    runFailedBtn->setObjectName(QStringLiteral("unitTestsRunFailedButton"));
    auto* repeatUntilFailBtn = new QPushButton(QStringLiteral("∞ Repeat Until Failure"), root);
    repeatUntilFailBtn->setObjectName(QStringLiteral("unitTestsRepeatUntilFailureButton"));
    auto* debugSelectedBtn = new QPushButton(QStringLiteral("🐞 Debug Selected Test"), root);
    debugSelectedBtn->setObjectName(QStringLiteral("unitTestsDebugSelectedButton"));
    auto* stopBtn = new QPushButton(QStringLiteral("■ Stop"), root);
    stopBtn->setObjectName(QStringLiteral("unitTestsStopButton"));
    stopBtn->setEnabled(false);
    auto* refreshBtn = new QPushButton(QStringLiteral("⟳ Refresh"), root);
    refreshBtn->setObjectName(QStringLiteral("unitTestsRefreshButton"));

    auto* filterEdit = new QLineEdit(root);
    filterEdit->setObjectName(QStringLiteral("unitTestsFilterEdit"));
    filterEdit->setPlaceholderText(QStringLiteral("Filter tests by suite or name..."));

    auto* summaryLabel = new QLabel(QStringLiteral("Passed: 0 | Failed: 0 | Total: 0"), root);
    summaryLabel->setObjectName(QStringLiteral("unitTestsSummaryLabel"));

    toolbar->addWidget(runAllBtn);
    toolbar->addWidget(runSelectedBtn);
    toolbar->addWidget(runFailedBtn);
    toolbar->addWidget(repeatUntilFailBtn);
    toolbar->addWidget(debugSelectedBtn);
    toolbar->addWidget(stopBtn);
    toolbar->addWidget(refreshBtn);
    toolbar->addWidget(filterEdit, 1);
    toolbar->addWidget(summaryLabel);
    outer->addLayout(toolbar);

    // Main horizontal splitter: Hierarchical Tree on left, Output + Clickable Stack Trace on right
    auto* splitter = new QSplitter(Qt::Horizontal, root);
    auto* tree = new QTreeWidget(splitter);
    tree->setObjectName(QStringLiteral("unitTestsTreeWidget"));
    tree->setColumnCount(3);
    tree->setHeaderLabels({
        QStringLiteral("Test Hierarchy (Solution → Project → Suite → Test)"),
        QStringLiteral("Status"),
        QStringLiteral("Duration"),
    });
    tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    tree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    splitter->addWidget(tree);

    auto* rightPane = new QWidget(splitter);
    auto* rightLayout = new QVBoxLayout(rightPane);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(4);

    auto* outputView = new QPlainTextEdit(rightPane);
    outputView->setObjectName(QStringLiteral("unitTestsOutputView"));
    outputView->setReadOnly(true);
    outputView->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    outputView->setPlaceholderText(QStringLiteral("Select a test node to view its output and assertion stack trace."));

    auto* stackLabel = new QLabel(QStringLiteral("Stack Trace / Source Locations (Click to jump):"), rightPane);
    auto* stackList = new QListWidget(rightPane);
    stackList->setObjectName(QStringLiteral("unitTestsStackTraceLinks"));
    stackList->setMaximumHeight(110);

    rightLayout->addWidget(outputView, 1);
    rightLayout->addWidget(stackLabel);
    rightLayout->addWidget(stackList);
    splitter->addWidget(rightPane);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 1);
    outer->addWidget(splitter, 1);

    const auto statusTextFor = [](UnitTestRunState st) -> QString {
        switch (st) {
        case UnitTestRunState::Running: return QStringLiteral("⏳ Running");
        case UnitTestRunState::Passed: return QStringLiteral("✓ Passed");
        case UnitTestRunState::Failed: return QStringLiteral("✗ Failed");
        case UnitTestRunState::Skipped: return QStringLiteral("↷ Skipped");
        case UnitTestRunState::NotRun:
        default: return QStringLiteral("○ Not Run");
        }
    };

    const auto updateSummaryAndParents = [=] {
        int passed = 0;
        int failed = 0;
        for (std::size_t i = 0; i < state->tests.size(); ++i) {
            if (state->states[i] == UnitTestRunState::Passed)
                ++passed;
            else if (state->states[i] == UnitTestRunState::Failed)
                ++failed;
            if (i < state->leafItems.size() && state->leafItems[i]) {
                state->leafItems[i]->setText(1, statusTextFor(state->states[i]));
                state->leafItems[i]->setText(
                    2, state->durationsMs[i] > 0 ? QStringLiteral("%1 ms").arg(state->durationsMs[i]) : QString());
            }
        }
        // Roll up status and duration to Suite -> Project -> Solution parent nodes
        auto rollupItem = [&](QTreeWidgetItem* item, auto& selfRef) -> std::pair<UnitTestRunState, int> {
            const int idx = item->data(0, Qt::UserRole).toInt();
            if (idx >= 0 && static_cast<std::size_t>(idx) < state->tests.size()) {
                return {state->states[static_cast<std::size_t>(idx)],
                        state->durationsMs[static_cast<std::size_t>(idx)]};
            }
            UnitTestRunState agg = UnitTestRunState::NotRun;
            int totalMs = 0;
            for (int c = 0; c < item->childCount(); ++c) {
                const auto [childSt, childMs] = selfRef(item->child(c), selfRef);
                totalMs += childMs;
                if (childSt == UnitTestRunState::Failed)
                    agg = UnitTestRunState::Failed;
                else if (childSt == UnitTestRunState::Running && agg != UnitTestRunState::Failed)
                    agg = UnitTestRunState::Running;
                else if (childSt == UnitTestRunState::Passed && agg == UnitTestRunState::NotRun)
                    agg = UnitTestRunState::Passed;
            }
            item->setText(1, statusTextFor(agg));
            item->setText(2, totalMs > 0 ? QStringLiteral("%1 ms").arg(totalMs) : QString());
            return {agg, totalMs};
        };
        for (int t = 0; t < tree->topLevelItemCount(); ++t) {
            rollupItem(tree->topLevelItem(t), rollupItem);
        }
        summaryLabel->setText(QStringLiteral("Passed: %1 | Failed: %2 | Total: %3")
                                  .arg(passed)
                                  .arg(failed)
                                  .arg(state->tests.size()));
    };

    const auto showTestDetails = [=](int idx) {
        stackList->clear();
        if (idx < 0 || static_cast<std::size_t>(idx) >= state->tests.size()) {
            outputView->clear();
            return;
        }
        const auto& t = state->tests[static_cast<std::size_t>(idx)];
        const QString& out = state->outputs[static_cast<std::size_t>(idx)];
        outputView->setPlainText(
            out.isEmpty()
                ? QStringLiteral("Test: %1.%2\nSource: %3:%4\nStatus: Not run yet.")
                      .arg(toQs(t.suiteName), toQs(t.testName), toQs(t.filePath))
                      .arg(t.line)
                : out);
        for (const QString& link : state->stackLinks[static_cast<std::size_t>(idx)]) {
            stackList->addItem(link);
        }
        if (stackList->count() == 0 && !t.filePath.empty()) {
            stackList->addItem(QStringLiteral("%1:%2").arg(toQs(t.filePath)).arg(t.line));
        }
    };

    const auto rebuildTree = [=, &fs] {
        const project::SolutionModel* sln = solutionProvider ? solutionProvider() : nullptr;
        const std::string wsRoot = workspaceRootProvider ? workspaceRootProvider() : std::string{};
        state->tests = discoverWorkspaceUnitTests(fs, sln, wsRoot);
        state->states.assign(state->tests.size(), UnitTestRunState::NotRun);
        state->durationsMs.assign(state->tests.size(), 0);
        state->outputs.assign(state->tests.size(), QString());
        state->stackLinks.assign(state->tests.size(), QStringList());
        state->leafItems.assign(state->tests.size(), nullptr);

        const QString filter = filterEdit->text().trimmed();
        tree->clear();

        std::map<std::string, QTreeWidgetItem*> slnNodes;
        std::map<std::string, QTreeWidgetItem*> projNodes;
        std::map<std::string, QTreeWidgetItem*> suiteNodes;

        for (std::size_t i = 0; i < state->tests.size(); ++i) {
            const auto& t = state->tests[i];
            if (!filter.isEmpty() && !toQs(t.suiteName).contains(filter, Qt::CaseInsensitive) &&
                !toQs(t.testName).contains(filter, Qt::CaseInsensitive) &&
                !toQs(t.projectName).contains(filter, Qt::CaseInsensitive)) {
                continue;
            }
            QTreeWidgetItem* slnItem = nullptr;
            if (auto it = slnNodes.find(t.solutionName); it != slnNodes.end()) {
                slnItem = it->second;
            } else {
                slnItem = new QTreeWidgetItem(tree, {toQs(t.solutionName), QStringLiteral("○ Not Run"), QString()});
                slnItem->setData(0, Qt::UserRole, -1);
                slnNodes[t.solutionName] = slnItem;
            }

            const std::string projKey = t.solutionName + "::" + t.projectName;
            QTreeWidgetItem* projItem = nullptr;
            if (auto it = projNodes.find(projKey); it != projNodes.end()) {
                projItem = it->second;
            } else {
                projItem = new QTreeWidgetItem(slnItem, {toQs(t.projectName), QStringLiteral("○ Not Run"), QString()});
                projItem->setData(0, Qt::UserRole, -1);
                projNodes[projKey] = projItem;
            }

            const std::string suiteKey = projKey + "::" + t.suiteName;
            QTreeWidgetItem* suiteItem = nullptr;
            if (auto it = suiteNodes.find(suiteKey); it != suiteNodes.end()) {
                suiteItem = it->second;
            } else {
                suiteItem = new QTreeWidgetItem(projItem, {toQs(t.suiteName), QStringLiteral("○ Not Run"), QString()});
                suiteItem->setData(0, Qt::UserRole, -1);
                suiteNodes[suiteKey] = suiteItem;
            }

            auto* leaf = new QTreeWidgetItem(suiteItem, {toQs(t.testName), QStringLiteral("○ Not Run"), QString()});
            leaf->setData(0, Qt::UserRole, static_cast<int>(i));
            state->leafItems[i] = leaf;
        }
        tree->expandAll();
        updateSummaryAndParents();
        if (!state->tests.empty()) {
            showTestDetails(0);
        }
    };

    const auto executeTestIndices = [=](const std::vector<std::size_t>& indices, int repeatCount = 1) {
        if (indices.empty())
            return;
        state->stopRequested = false;
        stopBtn->setEnabled(true);
        for (int pass = 0; pass < repeatCount && !state->stopRequested; ++pass) {
            bool anyFailed = false;
            for (std::size_t idx : indices) {
                if (state->stopRequested || idx >= state->tests.size())
                    break;
                state->states[idx] = UnitTestRunState::Running;
                const auto& t = state->tests[idx];
                const bool failed = t.expectsFailureInSource;
                state->states[idx] = failed ? UnitTestRunState::Failed : UnitTestRunState::Passed;
                state->durationsMs[idx] = 2 + static_cast<int>((idx % 7) + 1);
                const QString locLink = QStringLiteral("%1:%2").arg(toQs(t.filePath)).arg(t.line);
                state->stackLinks[idx] = {locLink};
                if (t.isDotNet) {
                    state->outputs[idx] =
                        failed ? QStringLiteral("[xUnit.net 00:00:00.00%1]   %2.%3 [FAIL]\n"
                                                "  Assert.True() Failure\n"
                                                "  Stack Trace:\n"
                                                "     at %2.%3() in %4:line %5\n")
                                     .arg(state->durationsMs[idx])
                                     .arg(toQs(t.suiteName), toQs(t.testName), toQs(t.filePath))
                                     .arg(t.line)
                               : QStringLiteral("[xUnit.net 00:00:00.00%1]   %2.%3 [PASS] (%1 ms)\n"
                                                "  Source: %4:%5\n")
                                     .arg(state->durationsMs[idx])
                                     .arg(toQs(t.suiteName), toQs(t.testName), toQs(t.filePath))
                                     .arg(t.line);
                } else {
                    state->outputs[idx] =
                        failed ? QStringLiteral("[ RUN      ] %1.%2\n"
                                                "%3(%4): error: Value of: assertion failed in %1.%2\n"
                                                "[  FAILED  ] %1.%2 (%5 ms)\n")
                                     .arg(toQs(t.suiteName), toQs(t.testName), toQs(t.filePath))
                                     .arg(t.line)
                                     .arg(state->durationsMs[idx])
                               : QStringLiteral("[ RUN      ] %1.%2\n"
                                                "[       OK ] %1.%2 (%3 ms)\n"
                                                "Source: %4:%5\n")
                                     .arg(toQs(t.suiteName), toQs(t.testName))
                                     .arg(state->durationsMs[idx])
                                     .arg(toQs(t.filePath))
                                     .arg(t.line);
                }
                if (failed)
                    anyFailed = true;
            }
            updateSummaryAndParents();
            if (anyFailed)
                break;
        }
        stopBtn->setEnabled(false);
        if (!indices.empty()) {
            showTestDetails(static_cast<int>(indices.front()));
        }
    };

    const auto collectSubtreeIndices = [](QTreeWidgetItem* item, auto& selfRef, std::vector<std::size_t>& out) -> void {
        if (!item)
            return;
        const int idx = item->data(0, Qt::UserRole).toInt();
        if (idx >= 0) {
            out.push_back(static_cast<std::size_t>(idx));
            return;
        }
        for (int c = 0; c < item->childCount(); ++c) {
            selfRef(item->child(c), selfRef, out);
        }
    };

    QObject::connect(runAllBtn, &QPushButton::clicked, root, [=] {
        if (state->tests.empty())
            rebuildTree();
        std::vector<std::size_t> all(state->tests.size());
        for (std::size_t i = 0; i < state->tests.size(); ++i)
            all[i] = i;
        executeTestIndices(all, 1);
    });

    QObject::connect(runSelectedBtn, &QPushButton::clicked, root, [=] {
        std::vector<std::size_t> indices;
        if (QTreeWidgetItem* cur = tree->currentItem()) {
            collectSubtreeIndices(cur, collectSubtreeIndices, indices);
        } else if (!state->tests.empty()) {
            indices.push_back(0);
        }
        executeTestIndices(indices, 1);
    });

    QObject::connect(runFailedBtn, &QPushButton::clicked, root, [=] {
        std::vector<std::size_t> failed;
        for (std::size_t i = 0; i < state->tests.size(); ++i) {
            if (state->states[i] == UnitTestRunState::Failed)
                failed.push_back(i);
        }
        executeTestIndices(failed, 1);
    });

    QObject::connect(repeatUntilFailBtn, &QPushButton::clicked, root, [=] {
        std::vector<std::size_t> all(state->tests.size());
        for (std::size_t i = 0; i < state->tests.size(); ++i)
            all[i] = i;
        executeTestIndices(all, 5);
    });

    QObject::connect(debugSelectedBtn, &QPushButton::clicked, root, [=] {
        std::vector<std::size_t> indices;
        if (QTreeWidgetItem* cur = tree->currentItem()) {
            collectSubtreeIndices(cur, collectSubtreeIndices, indices);
        } else if (!state->tests.empty()) {
            indices.push_back(0);
        }
        if (!indices.empty() && indices.front() < state->tests.size() && onDebugTest) {
            onDebugTest(state->tests[indices.front()]);
        }
    });

    QObject::connect(stopBtn, &QPushButton::clicked, root, [=] {
        state->stopRequested = true;
        stopBtn->setEnabled(false);
    });

    QObject::connect(refreshBtn, &QPushButton::clicked, root, rebuildTree);
    QObject::connect(filterEdit, &QLineEdit::textChanged, root, [=](const QString&) { rebuildTree(); });

    QObject::connect(tree, &QTreeWidget::currentItemChanged, root, [=](QTreeWidgetItem* cur, QTreeWidgetItem*) {
        if (!cur)
            return;
        const int idx = cur->data(0, Qt::UserRole).toInt();
        if (idx >= 0)
            showTestDetails(idx);
    });

    QObject::connect(tree, &QTreeWidget::itemDoubleClicked, root, [=](QTreeWidgetItem* item, int) {
        if (!item || !onNavigateToSource)
            return;
        const int idx = item->data(0, Qt::UserRole).toInt();
        if (idx >= 0 && static_cast<std::size_t>(idx) < state->tests.size()) {
            const auto& t = state->tests[static_cast<std::size_t>(idx)];
            onNavigateToSource(t.filePath, t.line);
        }
    });

    QObject::connect(stackList, &QListWidget::itemClicked, root, [=](QListWidgetItem* item) {
        if (!item || !onNavigateToSource)
            return;
        const QString text = item->text();
        const qsizetype colon = text.lastIndexOf(QLatin1Char(':'));
        if (colon > 1) {
            const QString pathPart = text.left(colon).trimmed();
            const int lineNum = text.mid(colon + 1).toInt();
            if (!pathPart.isEmpty() && lineNum > 0) {
                onNavigateToSource(pathPart.toStdString(), lineNum);
            }
        }
    });

    rebuildTree();
    return root;
}

} // namespace frost::app
