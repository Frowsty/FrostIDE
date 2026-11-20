// SPDX-License-Identifier: Apache-2.0
// Declares the dialog that displays Unreal Editor partner data.
#pragma once
#include "frost/lang/unreal_partner.hpp"

#include <QByteArray>
#include <QDialog>
#include <QElapsedTimer>
#include <QList>
#include <QPointer>
#include <QTimer>

#include <map>
#include <optional>
class QPushButton;
class QCloseEvent;
class QTcpSocket;
class QLabel;
class QSpinBox;
class QLineEdit;
class QTableWidget;
class QPlainTextEdit;
namespace frost::project {
class TrustStore;
}
namespace frost::app {
// Modeless GUI-thread client; never connects until the user selects Connect. No plugin payload,
// discovery, installation, process launch, whole-project usage count or inferred native mapping.
class UnrealPartnerWindow final : public QDialog {
public:
    UnrealPartnerWindow(std::string projectFile, project::TrustStore& trust, QWidget* parent = nullptr);
    ~UnrealPartnerWindow() override;

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void beginSession();
    void endSession(const QString& reason);
    void send(std::string operation);
    void readFrames();
    void receive(const lang::UnrealPartnerEvent& event);
    void checkSession();
    bool admitted() const;
    void updateControls();
    void applyBlueprintFilter();
    std::string m_project, m_root, m_token;
    project::TrustStore& m_trust;
    std::uint64_t m_revision = 0, m_epoch = 0, m_nextId = 0;
    bool m_ready = false;
    struct Pending {
        std::string operation;
        qint64 deadline = 0;
    };
    std::map<std::uint64_t, Pending> m_pending;
    QPointer<QTcpSocket> m_socket;
    QTimer m_watch;
    QElapsedTimer m_elapsed;
    qint64 m_lastFrame = 0;
    QByteArray m_input;
    QLabel* m_status = nullptr;
    QLabel* m_state = nullptr;
    QSpinBox* m_port = nullptr;
    QLineEdit* m_secret = nullptr;
    QLineEdit* m_filter = nullptr;
    QTableWidget* m_calls = nullptr;
    QPlainTextEdit* m_logs = nullptr;
    QList<QWidget*> m_controls;
    QPushButton* m_connectButton = nullptr;
    std::optional<lang::UnrealPartnerEvent> m_initialState;
};
} // namespace frost::app
