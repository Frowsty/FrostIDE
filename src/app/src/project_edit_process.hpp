// SPDX-License-Identifier: Apache-2.0
// Owned two-phase ProjectHost process for one project-definition edit.
#pragma once

#include "frost/build/run_session.hpp"

#include <QObject>
#include <QTimer>

#include <functional>
#include <memory>
#include <string>

namespace frost::app {

class ProjectEditProcess final : public QObject {
public:
    using Admit = std::function<bool(const std::string&)>;
    using Complete = std::function<void(bool, bool, const std::string&)>;
    ProjectEditProcess(io::IProcessLauncher& launcher, IClock& clock, QObject* parent = nullptr);
    ~ProjectEditProcess() override;
    [[nodiscard]] Result<void> start(io::ProcessSpec spec, std::string request, std::string transactionId, Admit admit,
                                     Complete complete);
    void stop();
    [[nodiscard]] bool busy() const noexcept { return m_session != nullptr; }

private:
    struct Relay;
    void output(build::RunStream stream, const std::string& chunk);
    void exited(const build::RunExit& result);
    io::IProcessLauncher& m_launcher;
    IClock& m_clock;
    std::shared_ptr<Relay> m_relay;
    std::unique_ptr<build::RunSession> m_session;
    QTimer m_timeout;
    Admit m_admit;
    Complete m_complete;
    std::string m_id;
    std::string m_output;
    std::string m_error;
    bool m_accepted = false;
    bool m_resultSeen = false;
    bool m_success = false;
    bool m_cancelled = false;
};
} // namespace frost::app
