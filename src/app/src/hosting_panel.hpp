// SPDX-License-Identifier: Apache-2.0
// Explicit provider CLI UI: no network request occurs until the user invokes an operation.
#pragma once
#include "frost/core/mutex.hpp"

#include "hosting_cli.hpp"
#include <QWidget>

#include <atomic>
#include <memory>
#include <thread>
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTreeWidget;
class QTabWidget;
namespace frost::app {
struct HostingEnvironment {
    io::IProcessLauncher* launcher = nullptr;        // Outlives the panel and its cancelled/drained worker.
    std::function<std::string()> root;               // GUI-thread current project.
    std::function<bool(const std::string&)> trusted; // Thread-safe live trust check.
    std::function<std::uint64_t(const std::string&)>
        trustRevision; // Thread-safe; production captures immutable admission.
};
class HostingPanel final : public QWidget {
public:
    explicit HostingPanel(HostingEnvironment environment, QWidget* parent = nullptr);
    ~HostingPanel() override;
    // Called synchronously on project close/switch before reusing the pane; cancels and discards old work.
    void invalidate();

private:
    void start(HostingOperation operation);
    void refreshButtons();
    void clearReviewData();
    void reviewSelectionChanged();
    void anchorChanged();
    void showDiff(std::shared_ptr<const HostedDiffSnapshot> diff);
    void showComments(std::shared_ptr<const HostedCommentPage> comments);
    HostingEnvironment m_environment;
    QComboBox* m_provider;
    QLineEdit *m_executable, *m_gitExecutable, *m_host, *m_repository;
    QSpinBox* m_page;
    QTreeWidget* m_reviews;
    QPlainTextEdit *m_details, *m_body;
    QTreeWidget *m_lines, *m_comments;
    QPlainTextEdit* m_commentDetails;
    QComboBox* m_lineSide;
    QTabWidget* m_tabs;
    QLabel* m_anchor;
    std::shared_ptr<const HostedDiffSnapshot> m_diff;
    std::shared_ptr<const HostedCommentPage> m_commentRows;
    bool m_submissionUncertain = false;
    QLabel* m_status;
    std::vector<std::pair<QPushButton*, HostingOperation>> m_buttons;
    QPushButton* m_cancel;
    std::vector<HostedReview> m_rows;
    struct CallbackGate {
        Mutex mutex;
        bool open = true;
    };
    std::shared_ptr<CallbackGate> m_gate = std::make_shared<CallbackGate>();
    std::uint64_t m_epoch = 0;
    std::jthread m_worker;
    std::shared_ptr<std::atomic_bool> m_current;
    bool m_busy = false;
};
} // namespace frost::app
