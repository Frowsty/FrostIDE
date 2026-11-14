// SPDX-License-Identifier: Apache-2.0
//
// Workbench (frost/app/workbench.hpp): Clone Repository (FND-9a), one `git clone` on a worker
// thread with its progress dialog, cancellation and the open of the cloned folder (split from
// workbench.cpp, FND-12a). The clone's state is GitClone (git_clone.hpp).

#include "frost/app/workbench.hpp"
#include "frost/io/path.hpp"
#include "frost/io/process.hpp"
#include "frost/ui/lang/language_controller.hpp"
#include "frost/ui/toolwindows/solution_explorer.hpp"
#include "frost/ui/tr.hpp"
#include "frost/vcs/git_locator.hpp"

#include "cmake_integration.hpp"
#include "engine_integration.hpp"
#include "git_clone.hpp"
#include "git_integration.hpp"
#include "language_bridge.hpp"
#include "unity_control.hpp"
#include <QDir>
#include <QProgressDialog>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <stop_token>
#include <thread>
#include <utility>

namespace frost::app {

namespace {

QString text(const char* source) { return ui::qtr("frost::app::Workbench", source); }

QString qs(std::string_view utf8) { return QString::fromUtf8(utf8.data(), static_cast<qsizetype>(utf8.size())); }

// --- Git clone (FND-9a) ---

// The percentage in a git progress line ("Receiving objects:  45% (9/20)"), or -1.
int clonePercent(std::string_view line) {
    const std::size_t percent = line.find('%');
    if (percent == std::string_view::npos || percent == 0)
        return -1;
    std::size_t begin = percent;
    while (begin > 0 && line[begin - 1] >= '0' && line[begin - 1] <= '9')
        --begin;
    if (begin == percent || percent - begin > 3)
        return -1;
    int value = 0;
    for (std::size_t i = begin; i < percent; ++i)
        value = value * 10 + (line[i] - '0');
    return std::min(value, 100);
}

// The userinfo of a URL with an authority ("user:token" in https://user:token@host/repo), or empty.
std::string cloneUserInfo(std::string_view url) {
    const std::size_t scheme = url.find("://");
    if (scheme == std::string_view::npos)
        return {}; // A local path or scp-like "user@host:path": no credential can be embedded.
    const std::string_view rest = url.substr(scheme + 3);
    const std::string_view authority = rest.substr(0, rest.find_first_of("/?#"));
    const std::size_t at = authority.rfind('@');
    return at == std::string_view::npos ? std::string() : std::string(authority.substr(0, at));
}

// `line` with every "<userinfo>@" replaced by "***@", so a token in the URL never reaches a label
// or the notification log (git's own messages may quote the URL).
std::string redactUserInfo(std::string line, std::string_view userInfo) {
    if (userInfo.empty())
        return line;
    const std::string needle = std::string(userInfo) + '@';
    for (std::size_t at = line.find(needle); at != std::string::npos; at = line.find(needle, at + 4))
        line.replace(at, needle.size(), "***@");
    return line;
}

} // namespace

Result<void> Workbench::cloneRepository(const std::string& url, const std::string& directory) {
    if (m_shutDown)
        return fail(ErrorCode::Cancelled, "FrostIDE is shutting down");
    if (m_clone)
        return fail(ErrorCode::Busy, "A repository is already being cloned");
    // A leading '-' would be read as an option even before "--" by some transports; refuse it.
    if (url.empty() || url.front() == '-' || url.find('\0') != std::string::npos)
        return fail(ErrorCode::InvalidArgument, "Enter the URL or path of the repository to clone");
    if (!io::isAbsolutePath(directory))
        return fail(ErrorCode::InvalidArgument, "The clone directory must be an absolute path");

    auto clone = std::make_unique<GitClone>();
    clone->url = url;
    clone->userInfo = cloneUserInfo(url);
    clone->shownUrl = redactUserInfo(url, clone->userInfo);
    clone->directory = directory;
    auto* dialog =
        new QProgressDialog(text("Cloning %1…").arg(qs(clone->shownUrl)), text("Cancel"), 0, 0, activeTopWindow());
    dialog->setObjectName(QStringLiteral("gitCloneProgressDialog"));
    dialog->setWindowTitle(text("Git Clone"));
    dialog->setAutoClose(false);
    dialog->setAutoReset(false);
    dialog->setMinimumDuration(0);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &QProgressDialog::canceled, this, [this] { cancelClone(); });
    clone->dialog = dialog;

    const std::string configured = vcs::configuredGitPath(*m_services.settings.snapshot());
    io::IFileSystem* fs = &m_services.fs;
    io::IProcessLauncher* launcher = &m_services.launcher;
    clone->worker = std::jthread([this, fs, launcher, configured, url, directory](const std::stop_token& stop) {
        // Every post is queued to this Workbench; the worker is joined before it is destroyed.
        const auto finish = [this](bool ok, std::string message) {
            QMetaObject::invokeMethod(
                this, [this, ok, message = std::move(message)] { finishClone(ok, message); }, Qt::QueuedConnection);
        };
        auto git = vcs::findGit(*fs, vcs::defaultGitSearch(configured));
        if (!git) {
            finish(false, git.error().message());
            return;
        }
        io::ProcessSpec spec;
        spec.executable = git->path;
        // ext:: runs arbitrary commands; refuse it explicitly. "--" ends the options.
        spec.arguments = {"-c", "protocol.ext.allow=never", "clone", "--progress", "--", url, directory};
        // No terminal exists to answer a credential prompt; fail instead of waiting forever.
        spec.environment = {{"GIT_TERMINAL_PROMPT", std::string("0")}};
        spec.stdinMode = io::Stdio::Null;
        spec.stdoutMode = io::Stdio::Null;
        spec.stderrMode = io::Stdio::Pipe;
        auto process = launcher->launch(spec);
        if (!process) {
            finish(false, std::format("Cannot start {}: {}", git->path, process.error().message()));
            return;
        }
        io::IProcess& child = **process;
        // Cancel and shutdown end the whole contained tree (git-remote-*, index-pack), which closes stderr.
        const std::stop_callback onStop(stop, [&child] { (void)child.terminate(); });
        std::vector<std::string> tail; // The last lines of git's own output, for the failure message.
        std::string line;
        auto lastPost = std::chrono::steady_clock::now() - std::chrono::seconds(1);
        const auto flushLine = [&] {
            if (line.empty())
                return;
            const auto now = std::chrono::steady_clock::now();
            if (now - lastPost >= std::chrono::milliseconds(50)) {
                lastPost = now;
                QMetaObject::invokeMethod(
                    this, [this, progress = line] { cloneProgress(progress); }, Qt::QueuedConnection);
            }
            if (!line.starts_with("Receiving objects") && !line.starts_with("Resolving deltas") &&
                !line.starts_with("remote: Counting") && !line.starts_with("remote: Compressing")) {
                tail.push_back(line);
                if (tail.size() > 8)
                    tail.erase(tail.begin());
            }
            line.clear();
        };
        std::array<char, 4096> buffer{};
        for (;;) {
            auto read = child.readStderr(buffer);
            if (!read || *read == 0)
                break;
            for (std::size_t i = 0; i < *read; ++i) {
                const char c = buffer[i];
                if (c == '\r' || c == '\n')
                    flushLine();
                else if (line.size() < 4096)
                    line.push_back(c);
            }
        }
        flushLine();
        auto status = child.wait();
        if (stop.stop_requested()) {
            finish(false, "cancelled");
            return;
        }
        if (!status) {
            finish(false, status.error().message());
            return;
        }
        if (status->exitCode != 0) {
            std::string message;
            for (const std::string& t : tail)
                message += (message.empty() ? "" : "\n") + t;
            finish(false, message.empty() ? std::format("git clone exited with code {}", status->exitCode) : message);
            return;
        }
        finish(true, {});
    });
    m_clone = std::move(clone);
    dialog->show();
    return {};
}

bool Workbench::cloneRunning() const noexcept { return m_clone != nullptr; }

void Workbench::cancelClone() {
    if (!m_clone)
        return;
    m_clone->cancelled = true;
    m_clone->worker.request_stop();
}

void Workbench::cloneProgress(const std::string& line) {
    if (!m_clone || !m_clone->dialog)
        return;
    m_clone->dialog->setLabelText(
        text("Cloning %1…\n%2").arg(qs(m_clone->shownUrl), qs(redactUserInfo(line, m_clone->userInfo))));
    if (const int percent = clonePercent(line); percent >= 0) {
        m_clone->dialog->setRange(0, 100);
        m_clone->dialog->setValue(percent);
    }
}

void Workbench::finishClone(bool ok, const std::string& message) {
    if (!m_clone)
        return;
    std::unique_ptr<GitClone> clone = std::move(m_clone);
    clone->worker.join(); // Posting this was the worker's last step.
    if (clone->dialog) {
        clone->dialog->blockSignals(true); // close() must not report a cancel.
        clone->dialog->close();
    }
    if (m_shutDown)
        return;
    if (ok) {
        if (auto opened = openPath(clone->directory); !opened)
            notify(text("Git Clone"), qs(opened.error().message()), ui::NotificationKind::Warning);
        return;
    }
    if (clone->cancelled) {
        notify(text("Git Clone cancelled"),
               text("Cloning %1 was cancelled; %2 may hold a partial clone.")
                   .arg(qs(clone->shownUrl), QDir::toNativeSeparators(qs(clone->directory))),
               ui::NotificationKind::Info);
        return;
    }
    notify(text("Git Clone failed"),
           text("Cloning %1 failed:\n%2").arg(qs(clone->shownUrl), qs(redactUserInfo(message, clone->userInfo))),
           ui::NotificationKind::Warning);
}

} // namespace frost::app
