// SPDX-License-Identifier: Apache-2.0
//
/*
 * ProjectPropertiesDialog, SolutionPropertiesDialog and ConfigurationManagerDialog, with the
 * property page specifications they share (project_properties_dialog.hpp). Under the truth rule
 * (ADR 0078, FND-3) a view without a real tool behind it is read-only or disabled and names the
 * epic that will provide it: project reference and configuration edits (SLN-6). The Debugging
 * page's per-user debugger settings are saved to <project>.vcxproj.user (SLN-11,
 * frost/build/vcxproj_user.hpp), where Visual Studio keeps them. The other dialogs of
 * project_properties_dialog.hpp live beside it (FND-12a):
 * Attach to Process and Run/Debug Configurations in project_run_dialogs.cpp; project references,
 * NuGet (LTR-7), the ASM/IL viewer (CPP-3, LTR-3) and Local History in project_tool_dialogs.cpp;
 * the source-text test and entry-point recognisers in project_unit_tests.cpp; the Unit Tests
 * window (RUN-7) in unit_tests_window.cpp.
 */

#include "project_properties_dialog.hpp"

#include "frost/build/vcxproj_user.hpp"
#include "frost/ui/dialog_chrome.hpp"

#include <QAbstractItemView>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
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

/*
 * Reasons shown where a feature has no real tool behind it yet (ADR 0078 truth rule, FND-3).
 * Each names the epic that will provide it; tests match on the epic identifiers.
 */
QString referenceEditsUnavailable() {
    return QStringLiteral("Read-only: adding or removing project references requires safe solution and project "
                          "edits (SLN-6).");
}

// FieldSpec::options for a multi-line value (a QPlainTextEdit), such as the debugger's NAME=VALUE lines.
constexpr const char* kMultiLineField = "\n";

struct FieldSpec {
    const char* key;
    const char* label;
    const char* options; // nullptr, kMultiLineField, or ';'-separated combo options
};

struct PageSpec {
    const char* category;
    const char* title;
    std::vector<FieldSpec> fields;
    // Non-null: the page is shown read-only with this reason and its fields are never written.
    const char* readOnlyReason = nullptr;
    // Non-null: an editable page's note (the Debugging page says where its values are saved).
    const char* note = nullptr;
};

/*
 * The Debugging page's LocalDebugger* properties are per-user settings: Visual Studio keeps them in
 * <project>.vcxproj.user, and writing them into the shared project file would put one user's command
 * line into every checkout. The page shows them as MSBuild sees them (the .vcxproj.user values over
 * the project file's) and saves a changed value to the .vcxproj.user file only, in Visual Studio's
 * format (frost/build/vcxproj_user.hpp, SLN-11); Run and Debug ask MSBuild for them, so F5 uses
 * what was saved. A cleared field removes the .user value, so the project file's applies again.
 */
constexpr const char* kDebuggingNote =
    "Debugger settings are per user: they are saved to the project's .vcxproj.user file, as Visual Studio "
    "does, and override the project file's. Run and Debug (F5) use them.";
constexpr std::string_view kLocalDebuggerPrefix = "Prop.LocalDebugger";
constexpr std::string_view kPropPrefix = "Prop.";

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
             {"Prop.LocalDebuggerEnvironment", "Environment", kMultiLineField},
         },
         nullptr,
         kDebuggingNote},
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
    bool readOnly = false; // Shown only; saveCurrentChanges never writes it.
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
        const bool readOnly = page.readOnlyReason != nullptr;
        if (readOnly) {
            auto* note = new QLabel(QString::fromUtf8(page.readOnlyReason), container);
            note->setObjectName(QStringLiteral("projectPropertiesReadOnlyNote"));
            note->setWordWrap(true);
            form->addRow(note);
        } else if (page.note != nullptr) {
            auto* note = new QLabel(QString::fromUtf8(page.note), container);
            note->setObjectName(QStringLiteral("projectPropertiesNote"));
            note->setWordWrap(true);
            form->addRow(note);
        }

        for (const FieldSpec& f : page.fields) {
            const std::string keyStr(f.key);
            if (f.options == kMultiLineField) {
                auto* edit = new QPlainTextEdit(container);
                edit->setObjectName(QString::fromStdString("prop_" + keyStr));
                edit->setMaximumHeight(edit->fontMetrics().lineSpacing() * 5);
                edit->setReadOnly(readOnly);
                form->addRow(QString::fromUtf8(f.label) + QLatin1Char(':'), edit);
                // Lines stay as typed; only the whitespace around the whole value is dropped.
                bindings.push_back({keyStr, [edit] { return edit->toPlainText().trimmed().toStdString(); },
                                    [edit](const std::string& val) { edit->setPlainText(toQs(val)); }, readOnly});
            } else if (f.options != nullptr) {
                auto* combo = new QComboBox(container);
                combo->setEditable(true);
                combo->setObjectName(QString::fromStdString("prop_" + keyStr));
                const QStringList opts = QString::fromUtf8(f.options).split(QLatin1Char(';'));
                combo->addItems(opts);
                if (readOnly) {
                    combo->setEnabled(false);
                    combo->setToolTip(QString::fromUtf8(page.readOnlyReason));
                }
                form->addRow(QString::fromUtf8(f.label) + QLatin1Char(':'), combo);
                bindings.push_back({keyStr, [combo] { return combo->currentText().trimmed().toStdString(); },
                                    [combo](const std::string& val) { combo->setEditText(toQs(val)); }, readOnly});
            } else {
                auto* edit = new QLineEdit(container);
                edit->setObjectName(QString::fromStdString("prop_" + keyStr));
                if (readOnly) {
                    edit->setReadOnly(true);
                    edit->setToolTip(QString::fromUtf8(page.readOnlyReason));
                }
                form->addRow(QString::fromUtf8(f.label) + QLatin1Char(':'), edit);
                bindings.push_back({keyStr, [edit] { return edit->text().trimmed().toStdString(); },
                                    [edit](const std::string& val) { edit->setText(toQs(val)); }, readOnly});
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
        // MSBuild imports <project>.user after the project, so its debugger settings win; a changed
        // one is saved back to the .user file (kDebuggingNote).
        if (isCpp) {
            const std::string userPath = projectPath + ".user";
            if (fs.exists(userPath).value_or(false)) {
                if (auto user = fs.readFile(userPath)) {
                    for (auto& [key, value] : parseVcxprojPropertiesForConfig(*user, cfg)) {
                        if (std::string_view(key).starts_with(kLocalDebuggerPrefix))
                            loadedProps[key] = std::move(value);
                    }
                }
            }
        }
        for (const BoundField& b : bindings) {
            const auto it = loadedProps.find(b.key);
            b.setValue(it != loadedProps.end() ? it->second : std::string());
        }
    };
    loadCurrentConfig();

    const auto saveCurrentChanges = [&]() -> bool {
        ProjectPropertyMap updates;
        for (const BoundField& b : bindings) {
            if (b.readOnly)
                continue;
            const std::string val = b.getValue();
            const auto it = loadedProps.find(b.key);
            const std::string prev = it != loadedProps.end() ? it->second : std::string();
            if (val != prev)
                updates[b.key] = val;
        }
        if (updates.empty())
            return true;
        const std::string cfg = cfgCombo ? cfgCombo->currentText().toStdString() : std::string();
        // The debugger settings go to <project>.vcxproj.user (kDebuggingNote), in each configuration
        // the combo names ("All Configurations": every one it lists), never into the project file.
        std::vector<build::UserFileProperty> userUpdates;
        if (isCpp) {
            for (auto it = updates.begin(); it != updates.end();) {
                if (std::string_view(it->first).starts_with(kLocalDebuggerPrefix)) {
                    userUpdates.push_back({it->first.substr(kPropPrefix.size()), it->second});
                    it = updates.erase(it);
                } else {
                    ++it;
                }
            }
        }
        if (!userUpdates.empty()) {
            std::vector<std::string> configurations;
            if (cfgCombo != nullptr && cfgCombo->currentIndex() == cfgCombo->count() - 1) {
                for (int i = 0; i + 1 < cfgCombo->count(); ++i)
                    configurations.push_back(cfgCombo->itemText(i).toStdString());
            } else {
                configurations.push_back(cfg);
            }
            for (const std::string& configuration : configurations) {
                const std::size_t bar = configuration.find('|');
                if (bar == std::string::npos) {
                    QMessageBox::warning(&dialog, QStringLiteral("Save Project Properties"),
                                         QStringLiteral("'%1' is not a Configuration|Platform pair; the debugger "
                                                        "settings were not saved.")
                                             .arg(toQs(configuration)));
                    return false;
                }
                if (auto written = build::writeUserFileProperties(fs, projectPath, configuration.substr(0, bar),
                                                                  configuration.substr(bar + 1), userUpdates);
                    !written) {
                    QMessageBox::warning(&dialog, QStringLiteral("Save Project Properties"),
                                         toQs(written.error().message()));
                    return false;
                }
            }
        }
        if (!updates.empty()) {
            currentXml = isCpp ? applyVcxprojPropertiesForConfig(currentXml, cfg, updates)
                               : applyCsprojProperties(currentXml, updates);
            if (auto written = fs.writeFileAtomic(projectPath, currentXml); !written) {
                QMessageBox::warning(&dialog, QStringLiteral("Save Project Properties"),
                                     toQs(written.error().message()));
                return false;
            }
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
    // saveCurrentChanges reports a failed write itself; OK closes only after the write succeeded.
    QObject::connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, &dialog,
                     [&] { static_cast<void>(saveCurrentChanges()); });
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        if (saveCurrentChanges())
            dialog.accept();
    });
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
            // Shown as loaded, not editable: reference edits are refused until SLN-6.
            item->setFlags(Qt::ItemIsUserCheckable);
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
    auto* depNote = new QLabel(referenceEditsUnavailable(), depPage);
    depNote->setObjectName(QStringLiteral("solutionDependenciesReadOnlyNote"));
    depNote->setWordWrap(true);
    depLayout->addWidget(depNote);
    tabs->addTab(depPage, QStringLiteral("Project Dependencies"));

    // Nothing in this dialog writes a file until SLN-6.
    static_cast<void>(fs);
    static_cast<void>(onDependenciesChanged);
    outer->addWidget(tabs, 1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        if (startupCombo->currentIndex() >= 0 && onSetStartup) {
            onSetStartup(project::ProjectId{static_cast<std::uint32_t>(startupCombo->currentData().toInt())});
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
    // Read-only until SLN-6/SLN-7: nothing here writes the solution or a project file.
    static_cast<void>(fs);
    static_cast<void>(onSolutionFileModified);
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
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);

    // Shows each mapping exactly as the solution file declares it; a project without one says so
    // instead of showing an invented Debug|x64.
    const auto populateTable = [&](int cfgIdx) {
        const auto projects = solution.projects();
        table->setRowCount(static_cast<int>(projects.size()));
        for (std::size_t p = 0; p < projects.size(); ++p) {
            const auto readOnly = [](QTableWidgetItem* item) {
                item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
                return item;
            };
            table->setItem(static_cast<int>(p), 0, readOnly(new QTableWidgetItem(toQs(projects[p].name))));
            const project::ProjectConfigurationMapping* mapping = nullptr;
            if (cfgIdx >= 0 && static_cast<std::size_t>(cfgIdx) < configs.size()) {
                const auto& found = solution.projectConfiguration(project::ProjectId{static_cast<std::uint32_t>(p)},
                                                                  static_cast<std::size_t>(cfgIdx));
                if (found)
                    mapping = &*found;
            }
            const QString notMapped = QStringLiteral("(not mapped)");
            table->setItem(static_cast<int>(p), 1,
                           readOnly(new QTableWidgetItem(mapping ? toQs(mapping->configuration) : notMapped)));
            table->setItem(static_cast<int>(p), 2,
                           readOnly(new QTableWidgetItem(mapping ? toQs(mapping->platform) : notMapped)));
            auto* buildItem = new QTableWidgetItem(mapping ? QString() : notMapped);
            // Checkable for display only: without Qt::ItemIsEnabled the user cannot toggle it.
            buildItem->setFlags(Qt::ItemIsUserCheckable);
            buildItem->setCheckState(mapping && mapping->build ? Qt::Checked : Qt::Unchecked);
            table->setItem(static_cast<int>(p), 3, buildItem);
        }
    };
    populateTable(activeCombo->currentIndex());
    QObject::connect(activeCombo, &QComboBox::currentIndexChanged, &dialog, populateTable);
    outer->addWidget(table, 1);

    auto* readOnlyNote = new QLabel(
        QStringLiteral("Read-only: editing project configuration mappings arrives with SLN-6 (safe solution and "
                       "project edits). Choosing the active solution configuration is applied; nothing is saved to "
                       "the solution."),
        &dialog);
    readOnlyNote->setObjectName(QStringLiteral("cfgManagerReadOnlyNote"));
    readOnlyNote->setWordWrap(true);
    outer->addWidget(readOnlyNote);

    // "Set Active" applies the selection to the IDE only; there is no OK that implies a save.
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    auto* setActive = buttons->addButton(QStringLiteral("Set Active"), QDialogButtonBox::AcceptRole);
    setActive->setObjectName(QStringLiteral("cfgManagerSetActiveButton"));
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

} // namespace frost::app
