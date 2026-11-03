// SPDX-License-Identifier: Apache-2.0
// Navigation bar over a live project tree model.
#pragma once

#include <QPointer>
#include <QWidget>

#include <cstdint>
#include <functional>
#include <string>

class QAbstractItemModel;
class QHBoxLayout;
class QMenu;
class QModelIndex;

namespace frost::ui {

// GUI-thread only. The model and callbacks must remain valid until clearModel().
class NavigationBar final : public QWidget {
    Q_OBJECT
public:
    using Find = std::function<QModelIndex(const std::string&)>;
    using IsDirectory = std::function<bool(const QModelIndex&)>;
    using Open = std::function<void(const std::string&)>;
    explicit NavigationBar(QWidget* parent = nullptr);
    void setModel(QAbstractItemModel* model, int pathRole, Find find, IsDirectory directory, Open open);
    void clearModel();
    void setPath(std::string path);
    void focusNavigation();
    [[nodiscard]] bool hasNavigation() const;
signals:
    void contentVisibilityChanged(bool visible);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void rebuild();
    void populate(QMenu* menu, const QModelIndex& parent);
    QPointer<QAbstractItemModel> m_model;
    QPointer<QMenu> m_menu;
    QHBoxLayout* m_layout;
    int m_pathRole = 0;
    Find m_find;
    IsDirectory m_directory;
    Open m_open;
    std::string m_path;
    std::uint64_t m_generation = 0;
};
} // namespace frost::ui
