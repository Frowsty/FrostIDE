// SPDX-License-Identifier: Apache-2.0
//
// ToolWindowManager v1 (PLAN.md row 240, ADR 0035): tool windows docked on fixed sides (left,
// right, bottom) around the editor area, one visible per side, shown and hidden from their stripe
// buttons or their activation actions (Alt+1 Project, Alt+0 Commit, Alt+4 Run, Alt+6 Problems),
// resizable with splitters, created lazily on first show, with sizes and visibility remembered
// through IShellSettings.
#pragma once

#include "frost/core/error.hpp"
#include "frost/ui/shell/shell_settings.hpp"

#include <QObject>
#include <QString>
#include <QToolButton>
#include <QWidget>

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class QLabel;
class QSplitter;
class QStackedWidget;

namespace frost::ui {

enum class ToolWindowSide : std::uint8_t { Left, Right, Bottom };

// Everything a tool window is registered with.
struct ToolWindowSpec {
    // [a-z][a-zA-Z0-9]*, unique ("project", "problems").
    std::string id;
    // Already translated.
    QString title;
    ToolWindowSide side = ToolWindowSide::Left;
    // An icon id of the atlas (frost/ui/icon_ids.hpp); empty: text only.
    std::string iconId;
    // Shown in the stripe button's tooltip ("Alt+1"); the shell fills it from the keymap.
    QString shortcutText;
    // Creates the content on first show (the manager owns it). Empty: a label with the title.
    std::function<QWidget*()> createContent;
    // Width (left, right) or height (bottom) in logical pixels; 0: the side's default.
    int defaultSize = 0;
};

/*
 * A stripe button. On the left and right stripes it paints its text rotated (bottom-to-top on the
 * left, top-to-bottom on the right), as the classic IntelliJ stripes do; on the bottom stripe it
 * is an ordinary checkable tool button. Checked while its window is visible.
 */
class StripeButton final : public QToolButton {
    Q_OBJECT

public:
    StripeButton(ToolWindowSide side, QWidget* parent = nullptr);

    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override { return sizeHint(); }

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    ToolWindowSide m_side;
};

/*
 * Builds the docking layout inside `host` around `center` and manages the windows. `host` must be
 * empty (the manager installs its layout); `center` (the editor area) is reparented into it. The
 * settings must outlive the manager. GUI thread only. Every operation is O(windows).
 */
class ToolWindowManager final : public QObject {
    Q_OBJECT

public:
    static constexpr int kDefaultSideSize = 280;
    static constexpr int kDefaultBottomSize = 220;
    static constexpr int kMinimumSize = 60;

    ToolWindowManager(QWidget& host, QWidget& center, IShellSettings& settings, QObject* parent = nullptr);
    ~ToolWindowManager() override;

    // Registers a window and applies its remembered layout (shown if it was visible). Errors:
    // InvalidArgument for a bad id, AlreadyExists for a duplicate.
    [[nodiscard]] Result<void> add(ToolWindowSpec spec);
    [[nodiscard]] bool contains(std::string_view id) const;
    // Registered ids in registration order.
    [[nodiscard]] std::vector<std::string> ids() const;

    // Shows `id` (hiding the other window on its side) and, with `focus`, focuses its content.
    void show(std::string_view id, bool focus = true);
    void hide(std::string_view id);
    // The stripe click: visible windows hide, hidden ones show and take focus.
    void toggle(std::string_view id);
    // The activation action (Alt+1 and so on): hidden shows and focuses; visible without focus
    // takes focus; visible with focus hides.
    void activate(std::string_view id);
    // Hides the window with focus, or the last activated visible one.
    void hideActive();
    // Hides every window (Ctrl+Shift+F12) and remembers them for jumpToLast().
    void hideAll();
    // Shows and focuses the last activated window (F12).
    void jumpToLast();

    [[nodiscard]] bool isVisible(std::string_view id) const;
    // The window whose content has keyboard focus, or "".
    [[nodiscard]] std::string focusedWindow() const;
    // The content widget, or nullptr before the window was first shown.
    [[nodiscard]] QWidget* content(std::string_view id) const;
    // Replaces the content (a later slice fills a placeholder, the Solution Explorer for one); the
    // manager takes ownership and deletes the old content.
    void setContent(std::string_view id, QWidget* content);
    // The remembered size in logical pixels.
    [[nodiscard]] int size(std::string_view id) const;
    // Sets the remembered size (clamped to kMinimumSize) and applies it when the window is visible.
    void setSize(std::string_view id, int size);
    // Reads the splitters' current sizes into the visible windows' remembered sizes (the splitters
    // call it after a drag) and stores the layout.
    void syncSizesFromSplitters();

    [[nodiscard]] StripeButton* stripeButton(std::string_view id) const;
    [[nodiscard]] QSplitter* horizontalSplitter() const noexcept { return m_horizontal; }
    [[nodiscard]] QSplitter* verticalSplitter() const noexcept { return m_vertical; }
    [[nodiscard]] QWidget* panel(ToolWindowSide side) const noexcept;
    // The stripe widgets (left, right, bottom), for keyboard navigation and tests.
    [[nodiscard]] QWidget* stripe(ToolWindowSide side) const noexcept;

signals:
    void visibilityChanged(const QString& id, bool visible);
    // Storing the layout failed (reported once per failure).
    void storeFailed(const QString& message);

private:
    struct Window {
        ToolWindowSpec spec;
        StripeButton* button = nullptr;
        QWidget* content = nullptr;
        int size = 0;
        bool visible = false;
    };
    struct Side {
        QWidget* panel = nullptr;
        QLabel* title = nullptr;
        QStackedWidget* stack = nullptr;
        QWidget* stripe = nullptr;
        std::string shown;
    };

    [[nodiscard]] Window* find(std::string_view id);
    [[nodiscard]] const Window* find(std::string_view id) const;
    [[nodiscard]] Side& side(ToolWindowSide side) noexcept { return m_sides[static_cast<std::size_t>(side)]; }
    void buildSide(ToolWindowSide which);
    void ensureContent(Window& window);
    void setVisible(Window& window, bool visible, bool focus);
    void applySizes();
    void storeLayout();

    QWidget& m_host;
    QWidget& m_center;
    IShellSettings& m_settings;
    QSplitter* m_horizontal = nullptr;
    QSplitter* m_vertical = nullptr;
    std::array<Side, 3> m_sides{};
    std::deque<Window> m_windows; // A deque: find()'s pointers survive add().
    std::string m_lastActive;
    std::vector<std::string> m_hiddenByHideAll;
    bool m_applying = false;
    bool m_storeFailed = false;
};

} // namespace frost::ui
