// SPDX-License-Identifier: Apache-2.0
//
// The main toolbar (PLAN.md row 239, ADR 0038), Rider's layout: the project widget on the left; on
// the right the run/build configuration combo, Build, Run, Debug and Stop, then Search Everywhere
// and Settings. Its buttons are bound to action ids and invoke through the registry; later slices
// fill the slots by registering those actions and adding configurations to the combo.
#pragma once

#include "frost/input/action.hpp"
#include "frost/input/keymap_service.hpp"

#include <QToolBar>
#include <QToolButton>

#include <functional>
#include <string>
#include <string_view>
#include <vector>

class QComboBox;

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

    MainToolbar(Invoker invoke, QWidget* parent = nullptr);

    // The project widget ("No project" until a solution opens).
    [[nodiscard]] QToolButton* projectButton() const noexcept { return m_project; }
    void setProjectName(const QString& name);
    // The run/build configuration slot; disabled while it has no items.
    [[nodiscard]] QComboBox* runConfigurations() const noexcept { return m_configurations; }
    // The button bound to `actionId`, or nullptr.
    [[nodiscard]] QToolButton* button(std::string_view actionId) const;
    // Every focusable control, left to right (keyboard navigation, Alt+Page Up).
    [[nodiscard]] std::vector<QWidget*> focusOrder() const;

    void refresh(input::ActionRegistry& registry, input::KeymapService& keymap, const input::ActionContext& context);

private:
    struct Slot {
        std::string actionId;
        QToolButton* button = nullptr;
    };

    void addSlot(std::string_view actionId, std::string_view iconId, std::string_view fallbackText);

    Invoker m_invoke;
    QToolButton* m_project;
    QComboBox* m_configurations;
    std::vector<Slot> m_slots;
};

} // namespace frost::ui
