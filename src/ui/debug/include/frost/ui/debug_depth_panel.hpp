// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <QWidget>
#include <cstdint>
class QLineEdit;
namespace frost::ui {
class DebugController;
// Adapter-backed call targets, registers and hardware watchpoints. No backend data is synthesized.
class DebugDepthPanel final : public QWidget {
public:
    explicit DebugDepthPanel(DebugController& controller, QWidget* parent = nullptr);
    void chooseVariable(std::int64_t reference, const QString& name);
private:
    QLineEdit* m_reference = nullptr;
    QLineEdit* m_name = nullptr;
};
}
