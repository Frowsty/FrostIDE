// SPDX-License-Identifier: Apache-2.0
//
// The main toolbar (PLAN.md row 239, ADR 0038), Rider's layout: the project widget on the left; on
// the right the run/build configuration combo, Build, Run, Debug and Stop, then Search Everywhere
// and Settings. Its buttons are bound to action ids and invoke through the registry; later slices
// fill the slots by registering those actions and adding configurations to the combo.
#pragma once

#include "frost/input/action.hpp"
#include "frost/input/keymap_service.hpp"
#include "frost/ui/model/theme_registry.hpp"
#include "frost/ui/shell/engine_state.hpp"

#include <QToolBar>
#include <QToolButton>

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class QComboBox;
class QActionEvent;
class QMenu;
class QMouseEvent;
class QPaintEvent;

namespace frost::ui {

/*
 * The toolbar. refresh() shows each button enabled when its action is registered and enabled in
 * the context, disabled otherwise (the slots keep their place), with "Name (Shortcut)" tooltips.
 * GUI thread only.
 */
class MainToolbar final : public QToolBar {
    Q_OBJECT

public:
    using Invoker = std::function<void(std::string_view actionId)>;

    // The action ids of the buttons, left to right after the configuration combo.
    static constexpr std::string_view kBuild = "build.project";
    static constexpr std::string_view kRun = "run.run";
    static constexpr std::string_view kDebug = "run.debug";
    static constexpr std::string_view kStop = "run.stop";
    static constexpr std::string_view kSearch = "search.everywhere";
    static constexpr std::string_view kSettings = "file.settings";

    static constexpr std::string_view kEnginePlay = "engine.play";
    static constexpr std::string_view kEnginePause = "engine.pause";
    static constexpr std::string_view kEngineStep = "engine.step";
    static constexpr std::string_view kEngineStop = "engine.stop";
    static constexpr std::string_view kEngineSimulate = "engine.simulate";

    explicit MainToolbar(Invoker invoke, QWidget* parent = nullptr);

    // GUI thread; O(button count). nullopt hides engine controls for a non-engine project.
    // Actual push state plus schema policy gate actions; clicking never predicts state.
    void setEngineState(std::optional<EngineControlState> state);
    // Requested count selected in the Step dropdown (default 1); app submits it to schema policy.
    // This local command parameter is not an observed engine frame/state value. GUI thread, O(1).
    [[nodiscard]] int engineStepFrames() const noexcept { return m_engineStepFrames; }
    [[nodiscard]] const std::optional<EngineControlState>& engineState() const noexcept { return m_engineState; }

    // The unified Rider main-menu button on the far left of the header.
    [[nodiscard]] QToolButton* mainMenuButton() const noexcept { return m_mainMenuButton; }
    void setMainMenuBuilder(std::function<void(QMenu&)> builder);

    // Rider header Back / Forward navigation buttons next to the Main Menu button.
    [[nodiscard]] QToolButton* backButton() const noexcept { return m_backButton; }
    [[nodiscard]] QToolButton* forwardButton() const noexcept { return m_forwardButton; }

    // The project widget ("No project" until a solution opens).
    [[nodiscard]] QToolButton* projectButton() const noexcept { return m_project; }
    void setProjectName(const QString& name);
    // Installs the GUI-thread menu builder (called each time it opens). An empty builder
    // removes the interactive menu; the caller must clear it before captured services die.
    void setProjectMenuBuilder(std::function<void(QMenu&)> builder);

    // The VCS branch pill button right after the project widget (hidden when branchName is empty).
    [[nodiscard]] QToolButton* vcsBranchButton() const noexcept { return m_vcsBranch; }
    void setVcsBranchName(const QString& branchName);
    void setVcsMenuBuilder(std::function<void(QMenu&)> builder);

    // Installs the dropdown menu builder for the Build slot button (MenuButtonPopup mode).
    // Clicking the Build button itself still invokes kBuild directly.
    void setBuildMenuBuilder(std::function<void(QMenu&)> builder);

    // The run/build configuration slot; disabled while it has no items.
    [[nodiscard]] QComboBox* runConfigurations() const noexcept { return m_configurations; }
    // The Quick Switch Theme button in the header's utility area.
    [[nodiscard]] QToolButton* themeButton() const noexcept { return m_themeButton; }
    // The integrated window caption buttons on the far right of the header.
    [[nodiscard]] QToolButton* minimizeButton() const noexcept { return m_minimizeButton; }
    [[nodiscard]] QToolButton* maximizeButton() const noexcept { return m_maximizeButton; }
    [[nodiscard]] QToolButton* closeButton() const noexcept { return m_closeButton; }
    // The button whose slot is `actionId` (one of the k* ids above) or that is bound to it now, or
    // nullptr.
    [[nodiscard]] QToolButton* button(std::string_view actionId) const;
    /*
     * Binds the slot `slotId` (one of the k* ids) to `actionId` instead, for a slice whose action
     * has another id (the build tool windows' build.buildSolution in the kBuild slot). Unknown slots
     * are ignored. The next refresh() reads the new action.
     */
    void setSlotAction(std::string_view slotId, std::string_view actionId);
    // Every focusable control, left to right (keyboard navigation, Alt+Page Up).
    [[nodiscard]] std::vector<QWidget*> focusOrder() const;

    void refresh(input::ActionRegistry& registry, input::KeymapService& keymap, const input::ActionContext& context);

protected:
    void actionEvent(QActionEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;

private:
    struct Slot {
        std::string slotId;   // The k* id the slot was created with.
        std::string actionId; // The action it invokes now.
        std::string iconId;
        std::optional<IconRole> iconRole;
        QToolButton* button = nullptr;
        bool actionEnabled = false;
    };

    void addSlot(std::string_view actionId, std::string_view iconId, std::string_view fallbackText,
                 std::optional<IconRole> iconRole = std::nullopt);
    void updateTabOrder();
    void applyEngineState();

    Invoker m_invoke;
    std::optional<EngineControlState> m_engineState;
    int m_engineStepFrames = 1;
    QToolButton* m_mainMenuButton = nullptr;
    QMenu* m_mainMenu = nullptr;
    QToolButton* m_backButton = nullptr;
    QToolButton* m_forwardButton = nullptr;
    QToolButton* m_project = nullptr;
    QMenu* m_projectMenu = nullptr;
    QToolButton* m_vcsBranch = nullptr;
    QAction* m_vcsAction = nullptr;
    QMenu* m_vcsMenu = nullptr;
    QMenu* m_buildMenu = nullptr;
    QComboBox* m_configurations = nullptr;
    QToolButton* m_themeButton = nullptr;
    QToolButton* m_minimizeButton = nullptr;
    QToolButton* m_maximizeButton = nullptr;
    QToolButton* m_closeButton = nullptr;
    std::vector<Slot> m_slots;
    std::function<void(QMenu&)> m_mainMenuBuilder;
    std::function<void(QMenu&)> m_projectMenuBuilder;
    std::function<void(QMenu&)> m_vcsMenuBuilder;
    std::function<void(QMenu&)> m_buildMenuBuilder;
    QPoint m_dragStartGlobal;
    QPoint m_windowStartPos;
    bool m_draggingWindow = false;
};

} // namespace frost::ui
