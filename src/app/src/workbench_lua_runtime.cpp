// SPDX-License-Identifier: Apache-2.0
// Standalone Lua: RunManager/DebugController own execution; no engine backend or runtime fallback.
#include "frost/app/workbench.hpp"
#include "frost/debug/standalone_lua_dap.hpp"
#include "frost/io/path.hpp"
#include "frost/run/run_manager.hpp"
#include "frost/settings/keys.hpp"
#include "frost/ui/debug_controller.hpp"
#include "frost/ui/nav/document_registry.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QPointer>
namespace frost::app {
std::string Workbench::defaultLuaInterpreterPath() const {
    const auto configured = m_services.settings.snapshot()->get(settings::key::toolsLuaInterpreterPath);
    if (!configured.empty())
        return configured;
#ifdef _WIN32
    constexpr auto leaf = "lua54/lua.exe";
#else
    constexpr auto leaf = "lua54/lua";
#endif
    return QDir(QCoreApplication::applicationDirPath()).filePath(QString::fromLatin1(leaf)).toStdString();
}
std::function<Result<void>()> Workbench::luaLaunchAdmissionFor(const run::RunConfiguration& configuration) const {
    if (!run::isLuaRunType(configuration.type) || m_projectRoot.empty())
        return {};
    const auto root = m_projectRoot;
    const auto revision = m_services.trust.snapshot(root).revision;
    const auto stop = m_luaRuntimeStop.get_token();
    return [trust = &m_services.trust, fs = &m_services.fs, root, revision, stop, configuration]() -> Result<void> {
        if (stop.stop_requested())
            return fail(ErrorCode::Cancelled, "Lua workspace launch was cancelled");
        const auto live = trust->snapshot(root);
        if (!live.trusted || live.revision != revision)
            return fail(ErrorCode::Untrusted, "Lua workspace trust changed before execution");
        const auto regular = [&](const std::string& path) -> Result<void> {
            auto status = fs->status(path);
            if (!status)
                return std::unexpected(status.error());
            if (status->type != io::FileType::Regular)
                return fail(ErrorCode::InvalidArgument, "Lua executable/script must be a regular file: " + path);
            return {};
        };
        if (auto checked = regular(configuration.program); !checked)
            return checked;
        if (configuration.type == run::RunType::LuaScript)
            return regular(configuration.target);
        return {};
    };
}
Result<void> Workbench::prepareStandaloneLua(const run::RunConfiguration& configuration) {
    if (!hasProject())
        return fail(ErrorCode::InvalidArgument, "open a trusted workspace for standalone Lua");
    if (auto valid = run::validateRunConfiguration(configuration); !valid)
        return valid;
    const QPointer<Workbench> owner(this);
    const auto root = m_projectRoot;
    const auto generation = m_luaRuntimeStop.get_token();
    if (m_services.settings.snapshot()->get(settings::key::buildSaveAllBeforeBuild)) {
        const auto saved = saveAllDocuments(); // Same Save All policy/owner as the existing build/run hook.
        if (!saved.failed.empty())
            return std::unexpected(saved.failed.front().error);
    }
    if (!owner || generation.stop_requested())
        return fail(ErrorCode::Cancelled, "Lua workspace lifetime changed while saving");
    if (root != m_projectRoot || m_shutDown)
        return fail(ErrorCode::Cancelled, "Lua workspace changed while saving");
    auto admission = luaLaunchAdmissionFor(configuration);
    if (!admission)
        return fail(ErrorCode::Untrusted, "Lua has no admitted workspace");
    return admission();
}
// NOLINTNEXTLINE(performance-unnecessary-value-param): Immutable selection survives reentrant Save All.
Result<void> Workbench::startStandaloneLuaDebug(run::RunConfiguration configuration) {
    if (configuration.type != run::RunType::LuaScript)
        return fail(ErrorCode::Unsupported, "Lua REPL is interactive Run only; debug a Lua script configuration");
    if (!m_debug)
        return fail(ErrorCode::Unsupported, "the debug controller is unavailable");
    if (auto admitted = mayRunConfiguration(configuration); !admitted)
        return admitted;
    auto plan = run::launchPlanFor(configuration);
    if (!plan)
        return std::unexpected(plan.error());
    auto adapter = m_services.settings.snapshot()->get(settings::key::toolsLuaDebugAdapterPath);
    if (!io::isAbsolutePath(adapter))
        return fail(ErrorCode::NotFound, "configure the absolute installed lua-debug adapter path in Toolchains");
    auto exists = m_services.fs.status(adapter);
    if (!exists || exists->type != io::FileType::Regular)
        return fail(ErrorCode::NotFound, "the configured standalone lua-debug adapter is unavailable");
    debug::StandaloneLuaTarget lua;
    lua.interpreter = plan->program; // The very same selected executable as launchPlanFor(Run).
    lua.script = configuration.target;
    lua.workingDirectory = plan->workingDirectory;
    lua.interpreterArguments = configuration.interpreterArguments;
    lua.arguments = configuration.arguments;
    for (const auto& entry : configuration.environment) {
        if (!entry.value)
            return fail(ErrorCode::Unsupported, "lua-debug cannot represent environment removals; use explicit values");
        lua.environment.emplace_back(entry.name, *entry.value);
    }
    auto arguments = debug::standaloneLuaLaunchArguments(lua);
    if (!arguments)
        return std::unexpected(arguments.error());
    debug::DebugTarget target;
    target.name = configuration.name;
    target.program = plan->program;
    target.arguments = plan->arguments;
    target.workingDirectory = plan->workingDirectory;
    target.trustRoots = {m_projectRoot};
    return m_debug->startAdapterTarget(target, debug::standaloneLuaAdapterFactory(m_services.launcher, adapter),
                                       std::move(*arguments), "lua", debug::DebugTargetMode::Launch, false,
                                       luaLaunchAdmissionFor(configuration));
}
} // namespace frost::app
