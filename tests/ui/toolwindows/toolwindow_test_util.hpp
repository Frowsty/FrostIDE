// SPDX-License-Identifier: Apache-2.0
//
// Test support for the build tool windows (ADR 0039): a fake IBuildBackend that records requests,
// the SDK fixture solution loaded from tests/project/fixtures, synthesized key events, and helpers
// that run the GUI thread's posted events. Header-only; test code only.
#pragma once

#include "frost/build/build_service.hpp"
#include "frost/io/file_system.hpp"
#include "frost/project/solution_model.hpp"
#include "frost/ui/toolwindows/build_controller.hpp"

#include "project/project_test_util.hpp"
#include <QCoreApplication>
#include <QKeyEvent>
#include <QObject>
#include <QString>
#include <QVariant>
#include <QWidget>
#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace frost::ui::test {

// Records what the tool windows ask of the build service; answers from its public fields.
class FakeBuildBackend final : public IBuildBackend {
public:
    struct BuildCall {
        build::BuildAction action;
        std::optional<project::ProjectId> project;
        friend bool operator==(const BuildCall&, const BuildCall&) = default;
    };

    std::shared_ptr<const project::SolutionModel> model;
    std::vector<project::SolutionConfiguration> configs;
    std::optional<std::size_t> active;
    build::BuildState currentState = build::BuildState::Idle;
    std::vector<BuildCall> builds;
    std::vector<std::string> trusted;
    std::vector<std::string> untrusted;
    std::vector<build::BuildDiagnostic> diagnosticList;
    int cancels = 0;
    build::SaveAllHook hook;
    std::optional<Error> buildError;  // The next build() fails with this.
    std::optional<Error> switchError; // setActiveConfiguration() fails with this (and keeps the old one).
    std::string failTrustOn;          // trust() of this root fails.

    Result<void> openSolution(std::shared_ptr<const project::SolutionModel> solution) override {
        if (!solution) {
            return fail(ErrorCode::InvalidArgument, "no solution");
        }
        model = std::move(solution);
        configs.assign(model->configurations().begin(), model->configurations().end());
        active = configs.empty() ? std::nullopt : std::optional<std::size_t>(0);
        return {};
    }
    Result<void> closeSolution() override {
        model.reset();
        configs.clear();
        active.reset();
        return {};
    }
    std::shared_ptr<const project::SolutionModel> solution() const override { return model; }
    std::vector<project::SolutionConfiguration> configurations() const override { return configs; }
    std::optional<std::size_t> activeConfigurationIndex() const override { return active; }
    Result<void> setActiveConfiguration(std::size_t index) override {
        if (switchError) {
            return std::unexpected(*switchError);
        }
        if (index >= configs.size()) {
            return fail(ErrorCode::OutOfRange, "no such configuration");
        }
        active = index;
        return {};
    }
    void setSaveAllHook(build::SaveAllHook value) override { hook = std::move(value); }
    Result<void> build(build::BuildAction action, std::optional<project::ProjectId> project) override {
        if (buildError) {
            Error error = *buildError;
            buildError.reset();
            return std::unexpected(std::move(error));
        }
        builds.push_back({action, project});
        return {};
    }
    void cancel() override { ++cancels; }
    Result<void> trust(std::string_view root) override {
        if (root == failTrustOn) {
            return fail(ErrorCode::PermissionDenied, "the trust store is read-only");
        }
        trusted.emplace_back(root);
        return {};
    }
    build::BuildState state() const override { return currentState; }
    std::vector<std::string> untrustedRoots() const override { return untrusted; }
    std::vector<build::BuildDiagnostic> diagnostics() const override { return diagnosticList; }
    // Compile (Ctrl+F7, SLN-9): the files compileFile() was asked for, and the ones canCompile() accepts.
    std::vector<std::string> compiles;
    std::vector<std::string> compilable;
    Result<void> compileFile(std::string_view path) override {
        if (buildError) {
            Error error = *buildError;
            buildError.reset();
            return std::unexpected(std::move(error));
        }
        compiles.emplace_back(path);
        return {};
    }
    bool canCompile(std::string_view path) const override { return std::ranges::contains(compilable, path); }
};

// Records each emission of a Qt signal as a list of its arguments (QSignalSpy without QtTest).
template<typename Sender, typename... Args>
class SignalLog {
public:
    SignalLog(Sender* sender, void (Sender::*signal)(Args...)) {
        QObject::connect(sender, signal, &m_receiver, [this](Args... args) {
            m_calls.push_back(QVariantList{QVariant::fromValue(std::remove_cvref_t<Args>(args))...});
        });
    }
    [[nodiscard]] int count() const noexcept { return static_cast<int>(m_calls.size()); }
    [[nodiscard]] const QVariantList& at(int index) const { return m_calls.at(static_cast<std::size_t>(index)); }
    [[nodiscard]] const QVariantList& last() const { return m_calls.back(); }

private:
    QObject m_receiver; // Disconnects when the log goes.
    std::vector<QVariantList> m_calls;
};

// Runs the events posted to the GUI thread so far (the controller's marshalled callbacks), and
// deferred deletes.
inline void runPostedEvents() {
    QCoreApplication::sendPostedEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents();
}

// The SDK-shaped fixture solution (tests/project/fixtures/sdk_solution/SdkApp.<extension>).
inline std::shared_ptr<const project::SolutionModel> loadSdk(std::string_view extension = "sln") {
    auto model =
        project::SolutionModel::load(io::realFileSystem(), project::test::sdk("SdkApp." + std::string(extension)));
    EXPECT_TRUE(model.has_value()) << (model ? std::string() : model.error().describe());
    return model ? std::make_shared<const project::SolutionModel>(std::move(*model)) : nullptr;
}

// The node of `model` with this display name under `parent` (by path of names from the root when nested).
inline std::optional<project::NodeId> findChild(const project::SolutionModel& model, project::NodeId parent,
                                                std::string_view name) {
    for (const project::NodeId id : model.children(parent)) {
        if (model.node(id).displayName == name) {
            return id;
        }
    }
    return std::nullopt;
}

// Follows names from the solution node down ("Libraries", "Core", "Source Files", "core.cpp").
inline project::NodeId nodeAt(const project::SolutionModel& model, std::initializer_list<std::string_view> names) {
    project::NodeId id = project::SolutionModel::root();
    for (const std::string_view name : names) {
        const auto next = findChild(model, id, name);
        EXPECT_TRUE(next.has_value()) << "no node " << name;
        if (!next) {
            return id;
        }
        id = *next;
    }
    return id;
}

// Sends a key press (and release) to `widget` the way the platform would, through event filters.
inline void pressKey(QWidget* widget, int key, const QString& text = {}, Qt::KeyboardModifiers modifiers = {}) {
    QKeyEvent press(QEvent::KeyPress, key, modifiers, text);
    QCoreApplication::sendEvent(widget, &press);
    QKeyEvent release(QEvent::KeyRelease, key, modifiers, text);
    QCoreApplication::sendEvent(widget, &release);
}

// Types `text` one character at a time.
inline void typeText(QWidget* widget, const QString& text) {
    for (const QChar c : text) {
        pressKey(widget, QChar(c).toUpper().unicode(), QString(c));
    }
}

// A resolved build diagnostic as the service reports it.
inline build::BuildDiagnostic diagnostic(build::Severity severity, std::string code, std::string message,
                                         std::string path, int line, int column, std::string project = {}) {
    build::BuildDiagnostic d;
    d.diagnostic.severity = severity;
    d.diagnostic.code = std::move(code);
    d.diagnostic.message = std::move(message);
    d.diagnostic.file = path;
    d.diagnostic.line = line;
    d.diagnostic.column = column;
    d.diagnostic.project = std::move(project);
    d.path = std::move(path);
    d.jumpable = !d.path.empty() && line > 0;
    return d;
}

} // namespace frost::ui::test
