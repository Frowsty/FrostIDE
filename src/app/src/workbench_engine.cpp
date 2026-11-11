// SPDX-License-Identifier: Apache-2.0
// Workbench's GUI-thread API for the live FrostEngine integration and automation journey.
#include "frost/app/workbench.hpp"

#include "engine_integration.hpp"

namespace frost::app {
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
} // namespace frost::app
