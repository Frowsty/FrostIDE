// SPDX-License-Identifier: Apache-2.0
//
// Custom themed Rider-style window header and frame chrome for FrostIDE sub-windows and dialogs.
#pragma once

#include <QDialog>
#include <QPoint>
#include <QPointer>
#include <QWidget>

class QMouseEvent;
class QPaintEvent;
class QToolButton;

namespace frost::ui {

// Custom themed title bar placed at the top of FrostIDE sub-windows and dialogs (Settings,
// New Solution, Merge External Changes, Trust Project, Evaluate, etc.), replacing the native
// OS title bar with a theme-aware header containing the Glacial FrostIDE logo, dialog title,
// and window caption buttons.
class DialogTitleBar final : public QWidget {
    Q_OBJECT

public:
    explicit DialogTitleBar(QDialog* dialog, bool resizable = true);
    ~DialogTitleBar() override;

    [[nodiscard]] QToolButton* minimizeButton() const noexcept { return m_minimizeButton; }
    [[nodiscard]] QToolButton* maximizeButton() const noexcept { return m_maximizeButton; }
    [[nodiscard]] QToolButton* closeButton() const noexcept { return m_closeButton; }
    [[nodiscard]] bool isResizable() const noexcept { return m_resizable; }
    void setResizable(bool resizable);

    [[nodiscard]] static int preferredHeight();
    void syncState();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;

private:
    QPointer<QDialog> m_dialog;
    QToolButton* m_minimizeButton = nullptr;
    QToolButton* m_maximizeButton = nullptr;
    QToolButton* m_closeButton = nullptr;
    QPoint m_dragOffset;
    bool m_dragging = false;
    bool m_resizable = true;
};

// Installs the custom themed DialogTitleBar and 1px themed border frame on `dialog`, replacing
// the standard OS title bar. Safe to call multiple times on the same dialog.
DialogTitleBar* installDialogChrome(QDialog* dialog, bool resizable = true);

} // namespace frost::ui
