// SPDX-License-Identifier: Apache-2.0
// Workbench's GUI-thread API for the live FrostEngine integration and automation journey.
#include "frost/app/workbench.hpp"

#include "engine_integration.hpp"
#include "engine_mixed_debug.hpp"
#include "engine_unit_tests_window.hpp"

namespace frost::app {
namespace {
bool sameTestSource(const engine::EngineTestProviderOptions& a, const engine::EngineTestProviderOptions& b) {
    return a.expectedProgram == b.expectedProgram && a.program == b.program && a.project == b.project && a.expectedProjectRoot == b.expectedProjectRoot &&
           a.expectedProjectFile == b.expectedProjectFile && a.folder == b.folder && a.frames == b.frames;
}
} // namespace
lang::Json Workbench::engineState() const {
    return m_engine ? m_engine->snapshot() : lang::Json::object().set("available", false);
}
Result<void> Workbench::configureEngineTools(std::string editorPath, std::string configDirectory,
                                             std::string automationDirectory, std::vector<std::string> arguments) {
    if (!m_engine)
        return fail(ErrorCode::Unsupported, "FrostEngine integration is unavailable");
    return m_engine->configureTools(std::move(editorPath), std::move(configDirectory), std::move(automationDirectory),
                                    std::move(arguments));
}
Result<void> Workbench::refreshEngineInstances() {
    return m_engine ? m_engine->refreshInstances()
                    : Result<void>(fail(ErrorCode::Unsupported, "FrostEngine integration is unavailable"));
}
Result<void> Workbench::pairEngineInstance(std::string_view instanceId) {
    return m_engine ? m_engine->pair(instanceId)
                    : Result<void>(fail(ErrorCode::Unsupported, "FrostEngine integration is unavailable"));
}
void Workbench::disconnectEngineSession() {
    if (m_engine)
        m_engine->disconnect();
}
Result<std::uint64_t> Workbench::sendEngineCommand(std::string method, lang::Json params) {
    if (!m_engine)
        return fail(ErrorCode::Unsupported, "FrostEngine integration is unavailable");
    return m_engine->command(std::move(method), std::move(params));
}
Result<build::RunId> Workbench::startEngineEditor() {
    return m_engine ? m_engine->startEditor()
                    : Result<build::RunId>(fail(ErrorCode::Unsupported, "FrostEngine integration is unavailable"));
}
Result<void> Workbench::validateEngineScript() {
    return m_engine ? m_engine->validateScript()
                    : Result<void>(fail(ErrorCode::Unsupported, "FrostEngine integration is unavailable"));
}
Result<void> Workbench::cancelEngineRequest() {
    return m_engine ? m_engine->cancelRequest()
                    : Result<void>(fail(ErrorCode::Unsupported, "FrostEngine integration is unavailable"));
}
Result<void> Workbench::startEngineLuaDebug(bool startPlay) {
    if (m_shutDown || !m_engineLuaDebug || !m_debugSessions)
        return fail(ErrorCode::Unsupported, "The engine Lua debugger is unavailable");
    return m_engineLuaDebug->startAttached(*m_debugSessions, startPlay);
}
ui::MixedDebugSnapshot Workbench::mixedDebugSnapshot() const {
    if (m_engineMixedDebug)
        return m_engineMixedDebug->snapshot();
    ui::MixedDebugSnapshot snapshot;
    snapshot.status = QStringLiteral("Mixed debugging is unavailable");
    return snapshot;
}
Result<void> Workbench::captureEngineNativeStack() {
    return m_engineMixedDebug ? m_engineMixedDebug->captureNativeStack()
                              : Result<void>(fail(ErrorCode::Unsupported, "Mixed debugging is unavailable"));
}
Result<void> Workbench::resumeEngineMixedDebug() {
    return m_engineMixedDebug ? m_engineMixedDebug->resumeBoth()
                              : Result<void>(fail(ErrorCode::Unsupported, "Mixed debugging is unavailable"));
}
Result<void> Workbench::debugEngineTestScene(const engine::EngineTestProviderOptions& options,
                                             const engine::EngineSceneTest& scene) {
    if (m_shutDown || !m_engine || !m_engineLuaDebug || !m_debugSessions || !m_engineTestsRetirements)
        return fail(ErrorCode::Unsupported, "The engine Lua debugger is unavailable");
    if (m_engineTestValidation && !m_engineTestValidation->ready())
        return fail(ErrorCode::Busy, "An engine test source check is still in progress");
    cancelEngineTestDebugPreparation();
    m_engineTestValidation.reset();
    auto current = m_engine->captureEngineUnitTestsSource();
    if (!current)
        return std::unexpected(current.error());
    if (!current->admitted || !current->admitted() || !sameTestSource(current->options, options))
        return fail(ErrorCode::PermissionDenied, "The engine test source changed; refresh the test listing");
    if (scene.scene.empty())
        return fail(ErrorCode::InvalidArgument, "The engine test scene is missing");
    const auto revision = current->revision;
    m_engineTestValidation = m_engineTestsRetirements->validate(std::move(*current));
    m_engineTestDebugPending = true;
    connect(&m_engineTestValidationTimer, &QTimer::timeout, this, [this, options, scene, revision] {
        if (!m_engineTestValidation || !m_engineTestValidation->ready())
            return;
        auto validated = m_engineTestValidation->takeResult();
        cancelEngineTestDebugPreparation();
        m_engineTestValidation.reset();
        if (m_shutDown || !m_engine || !m_engineLuaDebug || !m_debugSessions)
            return;
        auto report = [this](const Error& error) {
            notify(QStringLiteral("Engine test debugger"), QString::fromStdString(error.message()),
                   ui::NotificationKind::Error);
        };
        if (!validated)
            return;
        if (!*validated) {
            report(validated->error());
            return;
        }
        auto observed = m_engine->captureEngineUnitTestsSource();
        if (!observed || observed->revision != revision || !observed->admitted || !observed->admitted() ||
            !sameTestSource(observed->options, options)) {
            notify(QStringLiteral("Engine test debugger"),
                   QStringLiteral("The engine test source changed; refresh the test listing."),
                   ui::NotificationKind::Warning);
            return;
        }
        if (auto started = m_engineLuaDebug->debugScene(*m_debugSessions, scene.scene, options.frames); !started)
            report(started.error());
    });
    m_engineTestValidationTimer.start(25);
    return {};
}
void Workbench::cancelEngineTestDebugPreparation() {
    m_engineTestDebugPending = false;
    m_engineTestValidationTimer.stop();
    disconnect(&m_engineTestValidationTimer, nullptr, this, nullptr);
    if (m_engineTestValidation)
        m_engineTestValidation->cancel();
}
} // namespace frost::app
