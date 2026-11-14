// SPDX-License-Identifier: Apache-2.0
//
// RunConfigurationsDialog (see frost/ui/run_configurations_dialog.hpp): the list, the fields per
// type, Add, Copy and Remove, the before-launch tasks, compound members and attach process (RUN-5),
// and the validation on OK.

#include "frost/ui/run_configurations_dialog.hpp"

#include "frost/build/run_target.hpp"
#include "frost/io/path.hpp"
#include "frost/io/process.hpp"
#include "frost/run/run_types.hpp"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace frost::ui {

namespace {

QString qs(std::string_view text) { return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())); }
std::string utf8(const QString& text) { return text.toUtf8().toStdString(); }

QString typeLabel(run::RunType type) {
    switch (type) {
    case run::RunType::Native: return QObject::tr("Native executable");
    case run::RunType::FrostEditor: return QObject::tr("FrostEngine Editor");
    case run::RunType::FrostPlayer: return QObject::tr("FrostEngine Player");
    case run::RunType::CMakeTarget: return QObject::tr("CMake application");
    case run::RunType::MSBuild: return QObject::tr("MSBuild project");
    case run::RunType::Compound: return QObject::tr("Compound (runs several configurations)");
    case run::RunType::Attach: return QObject::tr("Attach to a running process (debug)");
    }
    return {};
}

// One line per before-launch task: the program, then its arguments, quoted as the arguments are.
QString formatTasks(const std::vector<run::BeforeLaunchTask>& tasks) {
    QStringList lines;
    for (const run::BeforeLaunchTask& task : tasks) {
        std::vector<std::string> words{task.program};
        words.insert(words.end(), task.arguments.begin(), task.arguments.end());
        lines.push_back(formatRunArguments(words));
    }
    return lines.join(QLatin1Char('\n'));
}

// The inverse; a line equal to an existing task's keeps that task (and its working directory).
std::vector<run::BeforeLaunchTask> parseTasks(const QString& text, const std::vector<run::BeforeLaunchTask>& previous) {
    std::vector<run::BeforeLaunchTask> tasks;
    for (const QString& raw : text.split(QLatin1Char('\n'))) {
        std::vector<std::string> words = parseRunArguments(raw.trimmed());
        if (words.empty()) {
            continue;
        }
        run::BeforeLaunchTask task;
        task.program = std::move(words.front());
        task.arguments.assign(std::make_move_iterator(words.begin() + 1), std::make_move_iterator(words.end()));
        const auto kept = std::ranges::find_if(previous, [&](const run::BeforeLaunchTask& old) {
            return old.program == task.program && old.arguments == task.arguments;
        });
        if (kept != previous.end()) {
            task.workingDirectory = kept->workingDirectory;
        }
        tasks.push_back(std::move(task));
    }
    return tasks;
}

} // namespace

QString formatRunArguments(const std::vector<std::string>& arguments) {
    std::string line;
    for (const std::string& argument : arguments) {
        if (!line.empty()) {
            line += ' ';
        }
        io::appendWindowsArgument(line, argument);
    }
    return qs(line);
}

std::vector<std::string> parseRunArguments(const QString& text) { return build::splitWindowsArguments(utf8(text)); }

QString formatRunEnvironment(const std::vector<run::EnvironmentEntry>& environment) {
    QStringList lines;
    for (const run::EnvironmentEntry& entry : environment) {
        lines.push_back(entry.value ? qs(entry.name) + QLatin1Char('=') + qs(*entry.value) : qs(entry.name));
    }
    return lines.join(QLatin1Char('\n'));
}

std::vector<run::EnvironmentEntry> parseRunEnvironment(const QString& text) {
    std::vector<run::EnvironmentEntry> out;
    for (const QString& raw : text.split(QLatin1Char('\n'))) {
        const QString line = raw.trimmed();
        if (line.isEmpty()) {
            continue;
        }
        const qsizetype equals = line.indexOf(QLatin1Char('='));
        if (equals < 0) {
            out.push_back({.name = utf8(line), .value = std::nullopt});
        } else {
            out.push_back({.name = utf8(line.left(equals)), .value = utf8(line.mid(equals + 1))});
        }
    }
    return out;
}

RunConfigurationsDialog::RunConfigurationsDialog(const run::RunConfigurationSet& set, const std::string& selectedId,
                                                 bool canAddNative, QWidget* parent)
    : QDialog(parent), m_set(set), m_configurations(set.all()), m_canAddNative(canAddNative),
      m_add(new QPushButton(tr("Add"), this)), m_copy(new QPushButton(tr("Copy"), this)),
      m_remove(new QPushButton(tr("Remove"), this)), m_list(new QListWidget(this)), m_name(new QLineEdit(this)),
      m_type(new QLabel(this)), m_target(new QLabel(this)), m_program(new QLineEdit(this)),
      m_engineProject(new QLineEdit(this)), m_arguments(new QLineEdit(this)), m_workingDirectory(new QLineEdit(this)),
      m_environment(new QPlainTextEdit(this)), m_beforeLaunch(new QLabel(this)), m_tasks(new QPlainTextEdit(this)),
      m_members(new QListWidget(this)), m_process(new QLineEdit(this)), m_note(new QLabel(this)),
      m_error(new QLabel(this)) {
    setObjectName(QStringLiteral("editRunConfigurationsDialog"));
    setWindowTitle(tr("Run/Debug Configurations"));
    setModal(true);
    resize(820, 560);

    auto* outer = new QVBoxLayout(this);
    auto* body = new QHBoxLayout();
    auto* left = new QVBoxLayout();
    auto* buttons = new QHBoxLayout();
    m_add->setObjectName(QStringLiteral("runConfigurationAdd"));
    m_add->setToolTip(tr("Add a native executable configuration"));
    m_copy->setObjectName(QStringLiteral("runConfigurationCopy"));
    m_remove->setObjectName(QStringLiteral("runConfigurationRemove"));
    buttons->addWidget(m_add);
    buttons->addWidget(m_copy);
    buttons->addWidget(m_remove);
    left->addLayout(buttons);
    m_list->setObjectName(QStringLiteral("runConfigurationList"));
    left->addWidget(m_list, 1);
    body->addLayout(left, 2);

    auto* form = new QFormLayout();
    m_form = form;
    m_name->setObjectName(QStringLiteral("runConfigurationName"));
    m_type->setObjectName(QStringLiteral("runConfigurationType"));
    m_target->setObjectName(QStringLiteral("runConfigurationTarget"));
    m_target->setWordWrap(true);
    m_program->setObjectName(QStringLiteral("runConfigurationProgram"));
    m_engineProject->setObjectName(QStringLiteral("runConfigurationEngineProject"));
    m_engineProject->setPlaceholderText(tr("Absolute project directory or .frostproj file"));
    m_arguments->setObjectName(QStringLiteral("runConfigurationArguments"));
    m_arguments->setToolTip(tr("Program arguments with Windows command-line quoting; no shell runs them."));
    m_workingDirectory->setObjectName(QStringLiteral("runConfigurationWorkingDirectory"));
    m_workingDirectory->setPlaceholderText(tr("The program's folder"));
    m_environment->setObjectName(QStringLiteral("runConfigurationEnvironment"));
    m_environment->setToolTip(tr("One NAME=VALUE per line; a line with just NAME removes the variable."));
    m_environment->setMaximumHeight(110);
    m_beforeLaunch->setObjectName(QStringLiteral("runConfigurationBeforeLaunch"));
    m_tasks->setObjectName(QStringLiteral("runConfigurationTasks"));
    m_tasks->setToolTip(tr("Programs to run before launch, one per line: the absolute program, then its arguments "
                           "(Windows quoting). A task that exits with a code other than 0 stops the launch."));
    m_tasks->setPlaceholderText(tr("None"));
    m_tasks->setMaximumHeight(80);
    m_members->setObjectName(QStringLiteral("runConfigurationMembers"));
    m_members->setToolTip(tr("The configurations this compound starts together, in this order."));
    m_members->setMaximumHeight(140);
    m_process->setObjectName(QStringLiteral("runConfigurationProcess"));
    m_process->setPlaceholderText(tr("Executable name (FrostEditor.exe) or absolute path"));
    m_process->setToolTip(tr("Debug attaches to the one running process whose executable this names."));
    m_note->setObjectName(QStringLiteral("runConfigurationNote"));
    m_note->setWordWrap(true);
    form->addRow(tr("Name:"), m_name);
    form->addRow(tr("Type:"), m_type);
    form->addRow(tr("Target:"), m_target);
    form->addRow(tr("Program:"), m_program);
    form->addRow(tr("Engine project:"), m_engineProject);
    m_engineDebugMode = new QComboBox(this);
    m_engineDebugMode->setObjectName(QStringLiteral("runConfigurationEngineDebugMode"));
    m_engineDebugMode->addItem(tr("Lua"), static_cast<int>(run::EngineDebugMode::Lua));
    m_engineDebugMode->addItem(tr("Native + Lua"), static_cast<int>(run::EngineDebugMode::NativeAndLua));
    m_engineDebugMode->setToolTip(tr("Native + Lua launches one engine host under the native debugger, then attaches "
                                     "Lua to that same authenticated process."));
    form->addRow(tr("Engine debugger:"), m_engineDebugMode);
    form->addRow(tr("Program arguments:"), m_arguments);
    form->addRow(tr("Working directory:"), m_workingDirectory);
    form->addRow(tr("Environment variables:"), m_environment);
    form->addRow(tr("Before launch:"), m_beforeLaunch);
    form->addRow(tr("Before-launch tasks:"), m_tasks);
    form->addRow(tr("Configurations:"), m_members);
    form->addRow(tr("Process:"), m_process);
    form->addRow(QString(), m_note);
    auto* fields = new QWidget(this);
    fields->setLayout(form);
    body->addWidget(fields, 5);
    outer->addLayout(body, 1);

    // RUN-5: compound and attach configurations.
    auto* compound = new QGroupBox(tr("Compound and attach configurations"), this);
    compound->setObjectName(QStringLiteral("compoundRunGroup"));
    auto* compoundLayout = new QHBoxLayout(compound);
    m_addCompound = new QPushButton(tr("Add Compound..."), compound);
    m_addCompound->setObjectName(QStringLiteral("compoundLaunchButton"));
    m_addCompound->setToolTip(tr("A configuration that runs several configurations together (Run only; compound "
                                 "debugging is DBG-10's)"));
    m_addAttach = new QPushButton(tr("Add Attach..."), compound);
    m_addAttach->setObjectName(QStringLiteral("attachConfigurationButton"));
    m_addAttach->setToolTip(tr("A configuration that attaches the debugger to a running process by its executable"));
    compoundLayout->addWidget(m_addCompound);
    compoundLayout->addWidget(m_addAttach);
    m_addFrostEditor = new QPushButton(tr("Add FrostEditor..."), compound);
    m_addFrostEditor->setObjectName(QStringLiteral("frostEditorConfigurationButton"));
    m_addFrostPlayer = new QPushButton(tr("Add FrostEngine Player..."), compound);
    m_addFrostPlayer->setObjectName(QStringLiteral("frostPlayerConfigurationButton"));
    compoundLayout->addWidget(m_addFrostEditor);
    compoundLayout->addWidget(m_addFrostPlayer);
    compoundLayout->addStretch(1);
    outer->addWidget(compound);
    m_error->setObjectName(QStringLiteral("runConfigurationError"));
    m_error->setWordWrap(true);
    m_error->setStyleSheet(QStringLiteral("color: #d9534f;"));
    m_error->hide();
    outer->addWidget(m_error);
    auto* box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    outer->addWidget(box);
    connect(box, &QDialogButtonBox::accepted, this, &RunConfigurationsDialog::accept);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);

    connect(m_list, &QListWidget::currentRowChanged, this, [this](int row) {
        storeFields();
        showConfiguration(row);
    });
    connect(m_add, &QPushButton::clicked, this, [this] { addNative(); });
    connect(m_copy, &QPushButton::clicked, this, [this] { copySelected(); });
    connect(m_remove, &QPushButton::clicked, this, [this] { removeSelected(); });
    connect(m_addCompound, &QPushButton::clicked, this, [this] { addCompound(); });
    connect(m_addAttach, &QPushButton::clicked, this, [this] { addAttach(); });
    connect(m_addFrostEditor, &QPushButton::clicked, this, [this] { addFrostEditor(); });
    connect(m_addFrostPlayer, &QPushButton::clicked, this, [this] { addFrostPlayer(); });
    // The list follows the name as it is typed.
    connect(m_name, &QLineEdit::textEdited, this, [this](const QString& name) {
        if (m_row >= 0 && m_row < m_list->count()) {
            m_list->item(m_row)->setText(name);
        }
    });

    refreshList();
    const auto selected = std::ranges::find(m_configurations, selectedId, &run::RunConfiguration::id);
    m_list->setCurrentRow(selected == m_configurations.end() ? (m_configurations.empty() ? -1 : 0)
                                                             : static_cast<int>(selected - m_configurations.begin()));
    if (m_list->currentRow() < 0) {
        showConfiguration(-1);
    }
}

RunConfigurationsDialog::~RunConfigurationsDialog() = default;

void RunConfigurationsDialog::refreshList() {
    m_loading = true;
    m_list->clear();
    for (const run::RunConfiguration& configuration : m_configurations) {
        auto* item = new QListWidgetItem(qs(configuration.name), m_list);
        item->setToolTip(m_set.isGenerated(configuration.id)
                             ? tr("%1 (generated from the project)").arg(typeLabel(configuration.type))
                             : typeLabel(configuration.type));
    }
    m_loading = false;
}

void RunConfigurationsDialog::showConfiguration(int row) {
    m_loading = true;
    m_row = row;
    const bool valid = row >= 0 && static_cast<std::size_t>(row) < m_configurations.size();
    for (QWidget* field : std::initializer_list<QWidget*>{m_name, m_program, m_arguments, m_workingDirectory,
                                                          m_environment, m_tasks, m_members, m_process}) {
        field->setEnabled(valid);
    }
    m_copy->setEnabled(valid && m_canAddNative);
    m_add->setEnabled(m_canAddNative);
    m_addCompound->setEnabled(m_canAddNative);
    m_addAttach->setEnabled(m_canAddNative);
    m_addFrostEditor->setEnabled(m_canAddNative);
    m_addFrostPlayer->setEnabled(m_canAddNative);
    const run::RunType shownType = valid ? m_configurations[static_cast<std::size_t>(row)].type : run::RunType::Native;
    const bool compoundType = shownType == run::RunType::Compound;
    const bool attachType = shownType == run::RunType::Attach;
    const bool launches = !compoundType && !attachType;
    for (QWidget* field : std::initializer_list<QWidget*>{m_target, m_program, m_arguments, m_workingDirectory,
                                                          m_environment, m_beforeLaunch, m_tasks}) {
        m_form->setRowVisible(field, launches);
    }
    m_form->setRowVisible(m_engineProject, run::isFrostEngineRunType(shownType));
    m_engineProject->setEnabled(valid);
    m_form->setRowVisible(m_engineDebugMode, run::isFrostEngineRunType(shownType));
    m_engineDebugMode->setEnabled(valid);
    m_form->setRowVisible(m_members, compoundType);
    m_form->setRowVisible(m_process, attachType);
    if (!valid) {
        m_name->clear();
        m_type->clear();
        m_target->clear();
        m_program->clear();
        m_engineProject->clear();
        m_arguments->clear();
        m_workingDirectory->clear();
        m_environment->clear();
        m_beforeLaunch->clear();
        m_tasks->clear();
        m_members->clear();
        m_process->clear();
        m_note->setText(m_canAddNative ? tr("No run configurations yet. Add a native executable, or open a CMake "
                                            "project or a solution to get one per executable target.")
                                       : tr("Open a project to add run configurations."));
        m_remove->setEnabled(false);
        m_loading = false;
        return;
    }
    const run::RunConfiguration& configuration = m_configurations[static_cast<std::size_t>(row)];
    const bool generated = m_set.isGenerated(configuration.id);
    m_name->setText(qs(configuration.name));
    m_type->setText(generated ? tr("%1 (generated)").arg(typeLabel(configuration.type))
                              : typeLabel(configuration.type));
    m_target->setText(configuration.target.empty() ? tr("(none)") : qs(configuration.target));
    m_program->setText(qs(configuration.program));
    m_engineProject->setText(qs(configuration.engineProjectPath));
    m_engineDebugMode->setCurrentIndex(m_engineDebugMode->findData(static_cast<int>(configuration.engineDebugMode)));
    m_arguments->setText(formatRunArguments(configuration.arguments));
    m_workingDirectory->setText(qs(configuration.workingDirectory));
    m_environment->setPlainText(formatRunEnvironment(configuration.environment));
    m_remove->setEnabled(!generated);
    m_remove->setToolTip(generated ? tr("Generated from the project; it comes back with the next generation.")
                                   : tr("Remove this configuration"));
    const bool native = configuration.type == run::RunType::Native || run::isFrostEngineRunType(configuration.type);
    const bool msbuild = configuration.type == run::RunType::MSBuild;
    m_program->setReadOnly(!native);
    m_program->setPlaceholderText(native ? tr("Absolute path of the program")
                                         : tr("The target's build output, resolved after the build"));
    for (QLineEdit* edit : {m_arguments, m_workingDirectory}) {
        edit->setReadOnly(msbuild);
    }
    m_environment->setReadOnly(msbuild);
    m_tasks->setPlainText(formatTasks(configuration.beforeLaunchTasks));
    m_tasks->setReadOnly(msbuild);
    m_tasks->setPlaceholderText(msbuild ? tr("MSBuild projects take no before-launch tasks yet (the RunService "
                                             "runs them)")
                                        : tr("None"));
    m_process->setText(attachType ? qs(configuration.target) : QString());
    fillMembers(configuration);
    m_beforeLaunch->setText(
        configuration.beforeLaunch == run::BeforeLaunch::Build
            ? tr("Build %1")
                  .arg(qs(configuration.type == run::RunType::MSBuild ? std::string(io::fileName(configuration.target))
                                                                      : configuration.target))
            : tr("Nothing"));
    if (msbuild) {
        m_note->setText(tr("MSBuild projects run with the debugger settings MSBuild evaluates for them (the .vcxproj "
                           "and .vcxproj.user LocalDebugger* properties). Edit them on the project's Properties, "
                           "Debugging page, which saves them to its .vcxproj.user file as Visual Studio does "
                           "(SLN-11)."));
    } else if (run::isFrostEngineRunType(configuration.type)) {
        m_note->setText(tr("Choose the executable from the engine build configuration to run. The project is passed "
                           "with --project. These runs establish an authenticated live session; Stop ends only the "
                           "process started by this configuration."));
    } else if (configuration.type == run::RunType::CMakeTarget) {
        m_note->setText(tr("The target is built first, then its executable runs. When the arguments here are empty, "
                           "the CMake Targets window's Run/Debug arguments for the active profile apply."));
    } else if (compoundType) {
        m_note->setText(tr("Run starts every checked configuration, each in its own tab. Debugging several "
                           "configurations together is not available yet (DBG-10)."));
    } else if (attachType) {
        m_note->setText(tr("Debug attaches the native debugger to the running process whose executable this names. "
                           "Run does nothing for an attach configuration."));
    } else {
        m_note->clear();
    }
    m_loading = false;
}

void RunConfigurationsDialog::storeFields() {
    if (m_loading || m_row < 0 || static_cast<std::size_t>(m_row) >= m_configurations.size()) {
        return;
    }
    run::RunConfiguration& configuration = m_configurations[static_cast<std::size_t>(m_row)];
    configuration.name = utf8(m_name->text().trimmed());
    if (configuration.type == run::RunType::Native || run::isFrostEngineRunType(configuration.type)) {
        configuration.program = utf8(m_program->text().trimmed());
    }
    if (run::isFrostEngineRunType(configuration.type)) {
        configuration.engineProjectPath = utf8(m_engineProject->text().trimmed());
        configuration.engineDebugMode = static_cast<run::EngineDebugMode>(m_engineDebugMode->currentData().toInt());
    }
    if (configuration.type == run::RunType::Compound) {
        configuration.configurations.clear();
        for (int i = 0; i < m_members->count(); ++i) {
            if (m_members->item(i)->checkState() == Qt::Checked) {
                configuration.configurations.push_back(utf8(m_members->item(i)->data(Qt::UserRole).toString()));
            }
        }
        return;
    }
    if (configuration.type == run::RunType::Attach) {
        configuration.target = utf8(m_process->text().trimmed());
        return;
    }
    if (configuration.type != run::RunType::MSBuild) {
        configuration.arguments = parseRunArguments(m_arguments->text());
        configuration.workingDirectory = utf8(m_workingDirectory->text().trimmed());
        configuration.environment = parseRunEnvironment(m_environment->toPlainText());
        configuration.beforeLaunchTasks = parseTasks(m_tasks->toPlainText(), configuration.beforeLaunchTasks);
    }
}

void RunConfigurationsDialog::fillMembers(const run::RunConfiguration& configuration) {
    m_members->clear();
    if (configuration.type != run::RunType::Compound) {
        return;
    }
    // The members first, in their order, then every other configuration a compound can start.
    std::vector<const run::RunConfiguration*> entries;
    for (const std::string& id : configuration.configurations) {
        const auto found = std::ranges::find(m_configurations, id, &run::RunConfiguration::id);
        if (found != m_configurations.end()) {
            entries.push_back(&*found);
        }
    }
    for (const run::RunConfiguration& candidate : m_configurations) {
        const bool startable = candidate.type != run::RunType::Compound && candidate.type != run::RunType::Attach;
        if (startable && std::ranges::find(entries, &candidate) == entries.end()) {
            entries.push_back(&candidate);
        }
    }
    for (const run::RunConfiguration* entry : entries) {
        auto* item = new QListWidgetItem(qs(entry->name), m_members);
        item->setData(Qt::UserRole, qs(entry->id));
        item->setToolTip(typeLabel(entry->type));
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(std::ranges::find(configuration.configurations, entry->id) !=
                                    configuration.configurations.end()
                                ? Qt::Checked
                                : Qt::Unchecked);
    }
}

std::vector<run::RunConfiguration> RunConfigurationsDialog::edited() {
    storeFields();
    return m_configurations;
}

std::string RunConfigurationsDialog::selectedId() const {
    const int row = m_list->currentRow();
    return row >= 0 && static_cast<std::size_t>(row) < m_configurations.size()
               ? m_configurations[static_cast<std::size_t>(row)].id
               : std::string();
}

std::string RunConfigurationsDialog::addNative() {
    if (!m_canAddNative) {
        return {};
    }
    storeFields();
    m_configurations.push_back(run::nativeRunConfiguration(utf8(tr("Unnamed")), "", m_configurations));
    refreshList();
    m_row = -1; // The fields were stored above; the new row's fields come next.
    m_list->setCurrentRow(static_cast<int>(m_configurations.size()) - 1);
    return m_configurations.back().id;
}

std::string RunConfigurationsDialog::copySelected() {
    storeFields();
    const int row = m_list->currentRow();
    if (!m_canAddNative || row < 0 || static_cast<std::size_t>(row) >= m_configurations.size()) {
        return {};
    }
    const run::RunConfiguration source = m_configurations[static_cast<std::size_t>(row)];
    if (source.type == run::RunType::Compound || source.type == run::RunType::Attach ||
        run::isFrostEngineRunType(source.type)) {
        run::RunConfiguration same = source;
        same.name = utf8(tr("%1 (copy)").arg(qs(source.name)));
        same.id = run::uniqueRunConfigurationId(run::runConfigurationIdFor(run::toString(source.type), same.name),
                                                m_configurations);
        m_configurations.push_back(std::move(same));
        refreshList();
        m_row = -1;
        m_list->setCurrentRow(static_cast<int>(m_configurations.size()) - 1);
        return m_configurations.back().id;
    }
    run::RunConfiguration copy =
        run::nativeRunConfiguration(utf8(tr("%1 (copy)").arg(qs(source.name))), source.program, m_configurations);
    copy.arguments = source.arguments;
    copy.workingDirectory = source.workingDirectory;
    copy.environment = source.environment;
    m_configurations.push_back(std::move(copy));
    refreshList();
    m_row = -1;
    m_list->setCurrentRow(static_cast<int>(m_configurations.size()) - 1);
    return m_configurations.back().id;
}

std::string RunConfigurationsDialog::addCompound() { return addOfType(run::RunType::Compound); }

std::string RunConfigurationsDialog::addAttach() { return addOfType(run::RunType::Attach); }

std::string RunConfigurationsDialog::addFrostEditor() { return addOfType(run::RunType::FrostEditor); }

std::string RunConfigurationsDialog::addFrostPlayer() { return addOfType(run::RunType::FrostPlayer); }

std::string RunConfigurationsDialog::addOfType(run::RunType type) {
    if (!m_canAddNative) {
        return {};
    }
    storeFields();
    run::RunConfiguration configuration;
    configuration.type = type;
    configuration.name = utf8(run::isFrostEngineRunType(type)  ? typeLabel(type)
                              : type == run::RunType::Compound ? tr("Unnamed compound")
                                                               : tr("Unnamed attach"));
    configuration.id = run::uniqueRunConfigurationId(
        run::runConfigurationIdFor(run::toString(type), configuration.name), m_configurations);
    m_configurations.push_back(std::move(configuration));
    refreshList();
    m_row = -1; // The fields were stored above; the new row's fields come next.
    m_list->setCurrentRow(static_cast<int>(m_configurations.size()) - 1);
    return m_configurations.back().id;
}

bool RunConfigurationsDialog::removeSelected() {
    const int row = m_list->currentRow();
    if (row < 0 || static_cast<std::size_t>(row) >= m_configurations.size() ||
        m_set.isGenerated(m_configurations[static_cast<std::size_t>(row)].id)) {
        return false;
    }
    m_configurations.erase(m_configurations.begin() + row);
    m_row = -1; // Nothing to store for the removed row.
    refreshList();
    const int next = std::min(row, static_cast<int>(m_configurations.size()) - 1);
    m_list->setCurrentRow(next);
    if (next < 0) {
        showConfiguration(-1);
    }
    return true;
}

void RunConfigurationsDialog::accept() {
    storeFields();
    for (std::size_t i = 0; i < m_configurations.size(); ++i) {
        if (auto valid = run::validateRunConfiguration(m_configurations[i]); !valid) {
            m_error->setText(qs(valid.error().message()));
            m_error->show();
            m_list->setCurrentRow(static_cast<int>(i));
            return;
        }
    }
    m_error->hide();
    QDialog::accept();
}

} // namespace frost::ui
