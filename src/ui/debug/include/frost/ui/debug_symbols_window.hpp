// SPDX-License-Identifier: Apache-2.0
// Real adapter-backed modules, symbol configuration and Natvis reload controls.
#pragma once
#include <QStringList>
#include <QWidget>
class QTreeWidget;
class QPlainTextEdit;
class QLabel;
class QPushButton;
class QCheckBox;
class QFileSystemWatcher;
namespace frost::ui {
class DebugController;
class DebugSymbolsWindow final : public QWidget {
public:
    // Controller outlives this GUI-thread widget. No debug values are synthesized locally.
    explicit DebugSymbolsWindow(DebugController&, QWidget* parent = nullptr);
    // Absolute admitted files used by the launched target; saved replacements reload at next stop.
    void setVisualizers(QStringList files);

private:
    void refresh();
    void reloadVisualizers();
    void applySymbols();
    void reloadSelectedModule();
    void applySourceMap();
    void resultMessage(bool success, const QString& text);
    DebugController& m_controller;
    QTreeWidget* m_modules;
    QPlainTextEdit* m_paths;
    QPlainTextEdit* m_visualizers;
    QPlainTextEdit* m_sourceMap;
    QLabel* m_status;
    QLabel* m_natvisStatus;
    QPushButton* m_refresh;
    QPushButton* m_apply;
    QPushButton* m_reload;
    QPushButton* m_loadVisualizers;
    QPushButton* m_applySourceMap;
    QCheckBox* m_allowServer;
    QFileSystemWatcher* m_watcher;
    bool m_visualizersChanged = false;
};
} // namespace frost::ui
