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
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
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

void showEditRunConfigurationsDialog(QWidget* parent, io::IFileSystem& fs, const project::SolutionModel* solution,
                                     const std::string& preferredProjectPath, const std::string& activeConfigPlatform,
                                     bool externalConsole, std::function<void(bool)> onExternalConsoleChanged,
                                     std::function<void()> onSaved) {
    QDialog dialog(parent);
    dialog.setObjectName(QStringLiteral("editRunConfigurationsDialog"));
    dialog.setWindowTitle(QStringLiteral("Edit Run/Debug Configurations"));
    dialog.setModal(true);
    dialog.resize(620, 380);

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
    outer->addLayout(form, 1);

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

void showNuGetPackageManagerDialog(QWidget* parent, io::IFileSystem& fs, const std::string& csprojPath,
                                   std::function<void()> onSaved) {
    QDialog dialog(parent);
    dialog.setObjectName(QStringLiteral("nugetPackageManagerDialog"));
    dialog.setWindowTitle(
        QStringLiteral("NuGet Package Manager — %1").arg(QFileInfo(toQs(csprojPath)).completeBaseName()));
    dialog.setModal(true);
    dialog.resize(640, 420);

    auto* outer = new QVBoxLayout(&dialog);
    auto* table = new QTableWidget(&dialog);
    table->setObjectName(QStringLiteral("nugetPackagesTable"));
    table->setColumnCount(2);
    table->setHorizontalHeaderLabels({QStringLiteral("Package ID"), QStringLiteral("Version")});
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    outer->addWidget(table, 1);

    const auto refreshList = [&] {
        table->setRowCount(0);
        if (auto content = fs.readFile(csprojPath)) {
            const auto pkgs = parseCsprojNuGetPackages(*content);
            table->setRowCount(static_cast<int>(pkgs.size()));
            for (std::size_t i = 0; i < pkgs.size(); ++i) {
                table->setItem(static_cast<int>(i), 0, new QTableWidgetItem(toQs(pkgs[i].id)));
                table->setItem(static_cast<int>(i), 1, new QTableWidgetItem(toQs(pkgs[i].version)));
            }
        }
    };
    refreshList();

    auto* installRow = new QHBoxLayout();
    auto* pkgIdEdit = new QLineEdit(&dialog);
    pkgIdEdit->setObjectName(QStringLiteral("nugetPackageIdEdit"));
    pkgIdEdit->setPlaceholderText(QStringLiteral("Package ID (e.g. Newtonsoft.Json)"));
    auto* pkgVerEdit = new QLineEdit(QStringLiteral("13.0.3"), &dialog);
    pkgVerEdit->setObjectName(QStringLiteral("nugetPackageVersionEdit"));
    pkgVerEdit->setPlaceholderText(QStringLiteral("Version"));
    auto* addBtn = new QPushButton(QStringLiteral("Install / Update"), &dialog);
    addBtn->setObjectName(QStringLiteral("nugetInstallButton"));
    auto* removeBtn = new QPushButton(QStringLiteral("Uninstall Selected"), &dialog);
    removeBtn->setObjectName(QStringLiteral("nugetRemoveButton"));
    installRow->addWidget(pkgIdEdit, 2);
    installRow->addWidget(pkgVerEdit, 1);
    installRow->addWidget(addBtn);
    installRow->addWidget(removeBtn);
    outer->addLayout(installRow);

    QObject::connect(addBtn, &QPushButton::clicked, &dialog, [&] {
        const QString id = pkgIdEdit->text().trimmed();
        const QString ver = pkgVerEdit->text().trimmed();
        if (id.isEmpty() || ver.isEmpty())
            return;
        if (auto content = fs.readFile(csprojPath)) {
            const std::string updated = upsertCsprojNuGetPackage(*content, id.toStdString(), ver.toStdString());
            (void)fs.writeFileAtomic(csprojPath, updated);
            refreshList();
            if (onSaved)
                onSaved();
        }
    });
    QObject::connect(removeBtn, &QPushButton::clicked, &dialog, [&] {
        const int row = table->currentRow();
        if (row < 0 || !table->item(row, 0))
            return;
        const QString id = table->item(row, 0)->text();
        if (auto content = fs.readFile(csprojPath)) {
            const std::string updated = removeCsprojNuGetPackage(*content, id.toStdString());
            (void)fs.writeFileAtomic(csprojPath, updated);
            refreshList();
            if (onSaved)
                onSaved();
        }
    });

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    outer->addWidget(buttons);
    ui::installDialogChrome(&dialog, true);
    dialog.exec();
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

} // namespace frost::app
