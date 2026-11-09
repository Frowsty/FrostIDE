// SPDX-License-Identifier: Apache-2.0
//
// BuildActions, ConfigurationCombo and the default-shortcut table's conversion
// (frost/ui/toolwindows/build_actions.hpp).

#include "frost/ui/toolwindows/build_actions.hpp"

#include "frost/input/key.hpp"
#include "frost/ui/icon_ids.hpp"
#include "frost/ui/toolwindows/build_controller.hpp"

#include "tool_icons.hpp"
#include <QAction>

#include <format>
#include <optional>
#include <string>
#include <utility>

namespace frost::ui {
namespace {

// The build action an action id runs; nullopt for Cancel Build.
std::optional<build::BuildAction> buildActionOf(std::string_view id) {
    if (id == build_actions::kBuildSolution) {
        return build::BuildAction::Build;
    }
    if (id == build_actions::kRebuildSolution) {
        return build::BuildAction::Rebuild;
    }
    if (id == build_actions::kCleanSolution) {
        return build::BuildAction::Clean;
    }
    return std::nullopt;
}

using CurrentFile = std::shared_ptr<std::function<std::string()>>;

std::string currentFileOf(const CurrentFile& provider) { return provider && *provider ? (*provider)() : std::string(); }

bool commandEnabled(const BuildController* controller, std::string_view id, const CurrentFile& currentFile) {
    if (controller == nullptr) {
        return false;
    }
    if (id == build_actions::kCancelBuild) {
        return controller->isBuilding();
    }
    if (id == build_actions::kCompileFile) {
        return controller->canCompile(currentFileOf(currentFile));
    }
    return buildActionOf(id).has_value() && controller->canBuild();
}

bool runCommand(BuildController* controller, std::string_view id, const CurrentFile& currentFile) {
    if (!commandEnabled(controller, id, currentFile)) {
        return false;
    }
    if (id == build_actions::kCompileFile) {
        static_cast<void>(controller->compileFile(currentFileOf(currentFile))); // Refusals: requestFailed().
    } else if (const std::optional<build::BuildAction> action = buildActionOf(id)) {
        static_cast<void>(controller->build(*action)); // A refusal is reported through requestFailed().
    } else {
        controller->cancel();
    }
    return true;
}

struct ActionText {
    std::string_view id;
    std::string_view name;
    std::string_view description;
    std::vector<std::string> synonyms;
};

} // namespace

Result<std::vector<input::Binding>> buildShortcutBindings() {
    std::vector<input::Binding> bindings;
    for (const BuildShortcutDefault& entry : kBuildShortcutDefaults) {
        if (entry.shortcut.empty()) {
            continue;
        }
        auto sequence = input::parseSequence(entry.shortcut, input::SyntaxMode::Strict);
        if (!sequence) {
            return fail(ErrorCode::InvalidArgument,
                        std::format("the default shortcut '{}' of {} does not parse: {}", entry.shortcut,
                                    entry.actionId, sequence.error().message()));
        }
        bindings.push_back(input::Binding{std::string(entry.actionId), {*sequence}});
    }
    return bindings;
}

// ---- BuildActions.

BuildActions::BuildActions(BuildController& controller, QObject* parent)
    : QObject(parent), m_controller(&controller), m_build(new QAction(tr("Build Solution"), this)),
      m_rebuild(new QAction(tr("Rebuild Solution"), this)), m_clean(new QAction(tr("Clean Solution"), this)),
      m_cancel(new QAction(tr("Cancel Build"), this)), m_compile(new QAction(tr("Compile"), this)),
      m_currentFile(std::make_shared<std::function<std::string()>>()) {
    detail::IconCache icons;
    const std::pair<QAction*, std::string_view> actions[] = {{m_build, build_actions::kBuildSolution},
                                                             {m_rebuild, build_actions::kRebuildSolution},
                                                             {m_clean, build_actions::kCleanSolution},
                                                             {m_cancel, build_actions::kCancelBuild},
                                                             {m_compile, build_actions::kCompileFile}};
    for (const auto& [action, id] : actions) {
        action->setObjectName(QString::fromLatin1(id.data(), static_cast<qsizetype>(id.size())));
        connect(action, &QAction::triggered, this, [this, id] { run(id); });
    }
    m_build->setIcon(icons.get(icons::kBuildBuild));
    m_rebuild->setIcon(icons.get(icons::kActionsRefresh));
    m_cancel->setIcon(icons.get(icons::kRunStop));
    connect(&controller, &BuildController::stateChanged, this, &BuildActions::updateEnabled);
    connect(&controller, &BuildController::solutionChanged, this, &BuildActions::updateEnabled);
    // Which project compiles a file depends on the active configuration (Compile's enablement).
    connect(&controller, &BuildController::activeConfigurationChanged, this, &BuildActions::updateEnabled);
    updateEnabled();
}

BuildActions::~BuildActions() = default;

Result<void> BuildActions::registerActions(input::ActionRegistry& registry) {
    const ActionText texts[] = {
        {build_actions::kBuildSolution,
         "Build Solution",
         "Build the projects of the solution that are out of date",
         {"compile", "make"}},
        {build_actions::kRebuildSolution,
         "Rebuild Solution",
         "Clean and build every project of the solution",
         {"rebuild all"}},
        {build_actions::kCleanSolution, "Clean Solution", "Delete the build outputs of the solution", {"clean"}},
        {build_actions::kCancelBuild, "Cancel Build", "Stop the build in progress", {"stop build", "abort build"}},
        {build_actions::kCompileFile,
         "Compile",
         "Compile the current C/C++ file with its project's settings (MSBuild)",
         {"compile file", "compile single file"}},
    };
    for (const ActionText& text : texts) {
        input::ActionSpec spec;
        spec.id = std::string(text.id);
        spec.name = std::string(text.name);
        spec.description = std::string(text.description);
        spec.group = std::string(build_actions::kGroup);
        spec.synonyms = text.synonyms;
        spec.priority = 1; // Wins Ctrl+Shift+B over Go to Type Declaration (header comment).
        const QPointer<BuildController> controller = m_controller;
        const std::string_view id = text.id;
        const CurrentFile currentFile = m_currentFile;
        spec.enabled = [controller, id, currentFile](const input::ActionContext&) {
            return commandEnabled(controller.data(), id, currentFile);
        };
        spec.handler = [controller, id, currentFile](const input::ActionContext&, const input::InvokeInfo&) {
            static_cast<void>(runCommand(controller.data(), id, currentFile));
        };
        auto added = registry.add(std::move(spec));
        if (!added) {
            return std::unexpected(std::move(added).error());
        }
    }
    return {};
}

bool BuildActions::isEnabled(std::string_view actionId) const {
    return commandEnabled(m_controller.data(), actionId, m_currentFile);
}

bool BuildActions::run(std::string_view actionId) { return runCommand(m_controller.data(), actionId, m_currentFile); }

void BuildActions::setCurrentFileProvider(std::function<std::string()> provider) {
    *m_currentFile = std::move(provider);
    updateEnabled();
}

void BuildActions::updateEnabled() {
    m_build->setEnabled(isEnabled(build_actions::kBuildSolution));
    m_rebuild->setEnabled(isEnabled(build_actions::kRebuildSolution));
    m_clean->setEnabled(isEnabled(build_actions::kCleanSolution));
    m_cancel->setEnabled(isEnabled(build_actions::kCancelBuild));
    m_compile->setEnabled(isEnabled(build_actions::kCompileFile));
}

// ---- ConfigurationCombo.

ConfigurationCombo::ConfigurationCombo(BuildController& controller, QWidget* parent)
    : QComboBox(parent), m_controller(controller) {
    setObjectName(QStringLiteral("configurationCombo"));
    setToolTip(tr("Solution configuration"));
    setSizeAdjustPolicy(QComboBox::AdjustToContents);
    connect(this, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (m_updating || index < 0) {
            return;
        }
        const QVariant data = itemData(index);
        if (data.isValid() && data.toInt() == -1) {
            showActive();
            if (m_configManagerHandler) {
                m_configManagerHandler();
            }
            return;
        }
        const std::size_t configIndex =
            data.isValid() ? static_cast<std::size_t>(data.toInt()) : static_cast<std::size_t>(index);
        if (!m_controller.setActiveConfiguration(configIndex)) {
            showActive(); // Refused (a build started, say): snap back; requestFailed() says why.
        }
    });
    connect(&m_controller, &BuildController::solutionChanged, this, &ConfigurationCombo::reload);
    connect(&m_controller, &BuildController::activeConfigurationChanged, this, &ConfigurationCombo::showActive);
    connect(&m_controller, &BuildController::stateChanged, this, &ConfigurationCombo::updateEnabled);
    reload();
}

ConfigurationCombo::~ConfigurationCombo() = default;

void ConfigurationCombo::setConfigurationManagerHandler(std::function<void()> handler) {
    m_configManagerHandler = std::move(handler);
    reload();
}

void ConfigurationCombo::reload() {
    m_updating = true;
    clear();
    const auto& configurations = m_controller.configurations();
    for (std::size_t i = 0; i < configurations.size(); ++i) {
        addItem(configurationLabel(configurations[i]), QVariant(static_cast<int>(i)));
    }
    if (!configurations.empty() && m_configManagerHandler) {
        addItem(tr("Configuration Manager..."), QVariant(-1));
    }
    m_updating = false;
    showActive();
    updateEnabled();
}

void ConfigurationCombo::showActive() {
    m_updating = true;
    const std::optional<std::size_t> active = m_controller.activeConfigurationIndex();
    const std::size_t configCount = m_controller.configurations().size();
    setCurrentIndex(active && *active < configCount && static_cast<int>(*active) < count() ? static_cast<int>(*active)
                                                                                           : -1);
    m_updating = false;
}

void ConfigurationCombo::updateEnabled() {
    setEnabled(m_controller.canBuild() && !m_controller.configurations().empty());
}

} // namespace frost::ui
