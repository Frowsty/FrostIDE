// SPDX-License-Identifier: Apache-2.0
// Native backend selection, launch and attach; presentation/lifecycle remain in DebugController.
#include "frost/ui/debug_controller.hpp"
#include "frost/debug/gdb_dap.hpp"
#include "frost/io/path.hpp"
#include "frost/project/trust_store.hpp"
#include <QProcessEnvironment>
#include <algorithm>
#include <format>
#include <utility>

namespace frost::ui {
Result<debug::AdapterFactory> DebugController::adapterFactory(bool* nativeOsThreadIdsVerified,
                                                              debug::DapProcessOutput output) {
    if (nativeOsThreadIdsVerified)
        *nativeOsThreadIdsVerified = false;
    if (m_options.adapterFactory) {
        return m_options.adapterFactory;
    }
    if (m_options.fs == nullptr || m_options.launcher == nullptr) {
        return fail(ErrorCode::InvalidArgument, "the debugger has no process launcher");
    }
    const auto backend = m_options.nativeBackendSetting ? m_options.nativeBackendSetting() : "lldb-dap";
    if (backend == "gdb") {
#ifndef __linux__
        return fail(ErrorCode::Unsupported, "The GDB DAP backend is supported on Linux only");
#else
        auto path = debug::findGdbDap(*m_options.fs, m_options.gdbSetting ? m_options.gdbSetting() : std::string{});
        if (!path) return std::unexpected(std::move(path.error()));
        return debug::gdbDapAdapterFactory(*m_options.launcher, std::move(*path), std::move(output));
#endif
    }
    if (backend != "lldb-dap") return fail(ErrorCode::Unsupported, "Unknown native debugger backend: " + backend);
    const auto configured = m_options.lldbDapSetting ? m_options.lldbDapSetting() : std::string{};
    auto path = debug::findLldbDap(*m_options.fs, debug::defaultLldbDapSearch(configured));
    if (!path) {
        return std::unexpected(std::move(path.error()));
    }
    if (nativeOsThreadIdsVerified)
        *nativeOsThreadIdsVerified = configured.empty();
    return debug::lldbDapAdapterFactory(*m_options.launcher, std::move(*path), std::move(output));
}

Result<void> DebugController::startTarget(const debug::DebugTarget& target) {
    return startNativeTarget(target, {}, {}, {});
}
Result<void> DebugController::startNativeEngineTarget(const debug::DebugTarget& target, PrivateOutput output,
                                                      std::function<void(std::string_view)> adapterStderr,
                                                      std::function<Result<void>()> launchAdmission) {
    if (!output || !launchAdmission || m_options.adapterFactory)
        return fail(ErrorCode::Unsupported,
                    "Private engine debugging requires the built-in pinned adapter and private admission");
    return startNativeTarget(target, std::move(output), {true, std::move(adapterStderr)},
                             debug::NativeDebugPrivacy{std::move(launchAdmission)});
}
Result<void> DebugController::startNativeTarget(const debug::DebugTarget& target, PrivateOutput output,
                                                debug::DapProcessOutput processOutput,
                                                std::optional<debug::NativeDebugPrivacy> privacy) {
    if (m_shutdown)
        return fail(ErrorCode::Cancelled, "the workbench is shutting down");
    if (m_session) {
        return fail(ErrorCode::Busy, "a debug session is running already; stop it first");
    }
    if (m_options.trust == nullptr) {
        return fail(ErrorCode::InvalidArgument, "the debugger has no trust store");
    }
    auto resolvedTarget = target;
    if (m_options.sourceMapSetting) {
        const auto [from, to] = m_options.sourceMapSetting();
        if (!from.empty() || !to.empty()) {
            if (!m_options.fs || from.empty() || to.empty())
                return fail(ErrorCode::InvalidArgument, "set both debug source mapping roots");
            auto roots = target.trustRoots;
            if (roots.empty())
                roots.emplace_back(io::parentPath(target.program));
            for (auto& root : roots) {
                auto canonical = m_options.fs->canonicalPath(root);
                if (!canonical || !m_options.trust->isTrusted(root))
                    return fail(ErrorCode::Untrusted, "debug project root is not trusted or does not exist");
                root = std::move(*canonical);
            }
            auto mapping = debug::makeSourceMap(*m_options.fs, *m_options.trust, from, to, roots);
            if (!mapping)
                return std::unexpected(std::move(mapping.error()));
            resolvedTarget.trustRoots = std::move(roots);
            resolvedTarget.sourceMap = std::move(*mapping);
        }
    }
    bool osThreadIdsVerified = false;
    const bool useGdb = !m_options.adapterFactory && m_options.nativeBackendSetting && m_options.nativeBackendSetting() == "gdb";
    if (privacy && useGdb) return fail(ErrorCode::Unsupported, "Private engine debugging requires the built-in pinned LLDB adapter; select LLDB in Native debugger settings" );
    auto factory = adapterFactory(&osThreadIdsVerified, std::move(processOutput));
    if (!factory)
        return std::unexpected(std::move(factory.error()));
    if (privacy && !osThreadIdsVerified)
        return fail(ErrorCode::Unsupported, "Private engine debugging requires the built-in pinned LLDB adapter");
    auto started = startSession(
        resolvedTarget.name.empty() ? std::string(io::fileName(resolvedTarget.program)) : resolvedTarget.name,
        resolvedTarget.program, useGdb ? "gdb" : "lldb-dap",
        [this, resolvedTarget, useGdb, factory = std::move(*factory),
         privacy = std::move(privacy)](debug::DebugSessionCallbacks callbacks) mutable {
            if (useGdb) {
                std::map<std::string, std::string> inherited;
                const auto snapshot = QProcessEnvironment::systemEnvironment();
                for (const auto& key : snapshot.keys()) inherited[key.toStdString()] = snapshot.value(key).toStdString();
                return debug::startGdbDebugSession(*m_options.trust, resolvedTarget, std::move(factory),
                    inherited, m_breakpoints.sessionLines(), std::move(callbacks), std::move(privacy),
                    m_breakpoints.sessionFunctions(), m_breakpoints.sessionExceptionFilters());
            }
            return debug::startDebugSession(*m_options.trust, resolvedTarget, std::move(factory),
                                            m_breakpoints.sessionLines(), std::move(callbacks),
                                            m_breakpoints.sessionFunctions(), m_breakpoints.sessionExceptionFilters(),
                                            m_breakpoints.sessionThrowTypes(), std::move(privacy));
        },
        false, std::move(output));
    if (started) {
        m_lastCoreClrSession = false;
        m_nativeSession = true;
        m_nativeOsThreadIdsVerified = osThreadIdsVerified;
    }
    return started;
}

Result<void> DebugController::startAdapterTarget(const debug::DebugTarget& target, debug::AdapterFactory factory,
                                                 rpc::Json arguments, std::string adapterId,
                                                 debug::DebugTargetMode mode, bool readOnly,
                                                 std::function<Result<void>()> launchAdmission) {
    if (m_pendingRun || m_alternatePending || (m_hooks.mayStart && !m_hooks.mayStart()))
        return fail(ErrorCode::Busy, "Another run or debug launch is still preparing");
    if (!m_options.trust || !factory || !arguments.isObject() || adapterId.empty())
        return fail(ErrorCode::InvalidArgument, "An adapter, arguments and trusted target are required");
    if (launchAdmission) {
        if (auto admitted = launchAdmission(); !admitted)
            return admitted;
    }
    auto roots = target.trustRoots;
    if (roots.empty() && !target.program.empty())
        roots.emplace_back(io::parentPath(target.program));
    if (roots.empty())
        return fail(ErrorCode::Untrusted, "The adapter target has no trusted workspace");
    for (const auto& root : roots)
        if (!io::isAbsolutePath(root) || !m_options.trust->isTrusted(root))
            return fail(ErrorCode::Untrusted, "The adapter target workspace is not trusted");
    if (readOnly) {
        const auto dump = arguments.stringAt("dumpFile");
        const bool localVerbatim = dump.size() >= 7 && dump.starts_with("\\\\?\\") && dump[5] == ':' && dump[6] == '\\';
        if (!io::isAbsolutePath(dump) || (dump.starts_with("\\\\") && !localVerbatim) || dump.starts_with("//") ||
            dump.find('\0') != std::string_view::npos)
            return fail(ErrorCode::InvalidArgument, "A read-only adapter target requires an absolute local dump file");
    }
    if ((!readOnly || !target.program.empty()) && !io::isAbsolutePath(target.program))
        return fail(ErrorCode::InvalidArgument, "The adapter program path must be absolute");
    const auto actualMode = mode;
    const auto display = adapterId;
    auto started = startSession(
        target.name, target.program, display,
        [this, factory = std::move(factory), arguments = std::move(arguments), adapterId, actualMode, readOnly,
         launchAdmission = std::move(launchAdmission)](
            debug::DebugSessionCallbacks callbacks) mutable -> Result<std::unique_ptr<debug::DebugSession>> {
            debug::DebugSessionOptions options;
            options.adapterId = adapterId;
            options.targetMode = actualMode;
            options.readOnly = readOnly;
            options.launchAdmission = std::move(launchAdmission);
            if (!readOnly) {
                options.functionBreakpoints = m_breakpoints.sessionFunctions();
                options.exceptionFilters = m_breakpoints.sessionExceptionFilters();
            }
            return debug::DebugSession::start(std::move(factory), std::move(arguments),
                                              readOnly ? std::map<std::string, std::vector<debug::BreakpointLine>>{}
                                                       : m_breakpoints.sessionLines(),
                                              std::move(callbacks), std::move(options));
        },
        readOnly || actualMode == debug::DebugTargetMode::Attach);
    if (started) {
        m_nativeSession = !readOnly && (adapterId == "lldb-dap" || adapterId == "frost-dbgeng");
        m_lastCoreClrSession = false;
    }
    return started;
}

Result<void> DebugController::attachTarget(const debug::DebugTarget& target, debug::AdapterFactory factory,
                                           rpc::Json attachArguments, const std::function<bool()>& ownerAdmission) {
    const bool admitted = ownerAdmission ? ownerAdmission() : (!m_hooks.mayStart || m_hooks.mayStart());
    if (m_pendingRun || m_alternatePending || !admitted)
        return fail(ErrorCode::Busy, "another run or debug launch is already pending");
    const std::string_view typeView =
        attachArguments.isObject() ? attachArguments.stringAt("type") : std::string_view{};
    std::string adapterId = !typeView.empty() ? std::string(typeView) : std::string("lldb-dap");
    if (!factory && adapterId == "lldb-dap" && !m_options.adapterFactory && m_options.nativeBackendSetting &&
        m_options.nativeBackendSetting() == "gdb") {
        adapterId = "gdb";
        attachArguments.set("type", "gdb");
    }
    bool osThreadIdsVerified = false;
    if (!factory && adapterId == "gdb") {
#ifndef __linux__
        return fail(ErrorCode::Unsupported, "The GDB DAP backend is supported on Linux only");
#else
        if (!m_options.fs || !m_options.launcher) return fail(ErrorCode::InvalidArgument, "GDB has no process launcher");
        auto path = debug::findGdbDap(*m_options.fs, m_options.gdbSetting ? m_options.gdbSetting() : std::string{});
        if (!path) return std::unexpected(std::move(path.error()));
        factory = debug::gdbDapAdapterFactory(*m_options.launcher, std::move(*path));
#endif
    }
    if (!factory && adapterId == "lldb-dap") {
        auto resolved = adapterFactory(&osThreadIdsVerified);
        if (!resolved)
            return std::unexpected(resolved.error());
        factory = std::move(*resolved);
    }
    const std::string displayAdapter =
        adapterId == "coreclr" ? "CoreCLR" : (!typeView.empty() ? adapterId : std::string("Lua"));
    auto started = startSession(
        target.name.empty() ? std::string(io::fileName(target.program)) : target.name, target.program, displayAdapter,
        [this, target, factory = std::move(factory), arguments = std::move(attachArguments),
         adapterId](debug::DebugSessionCallbacks callbacks) mutable {
            if (adapterId == "gdb") {
                return debug::attachGdbDebugSession(*m_options.trust, target, arguments.integerAt("pid").value_or(0),
                    std::move(factory), m_breakpoints.sessionLines(), std::move(callbacks),
                    m_breakpoints.sessionFunctions(), m_breakpoints.sessionExceptionFilters());
            }
            auto session = debug::attachDebugSession(*m_options.trust, target, std::move(factory), std::move(arguments),
                                                     m_breakpoints.sessionLines(), std::move(callbacks), adapterId);
            if (session && adapterId == "lldb-dap") {
                // Recorded now and sent when the adapter is configured (DebugSession).
                (*session)->setFunctionBreakpoints(m_breakpoints.sessionFunctions());
                (*session)->setExceptionFilters(m_breakpoints.sessionExceptionFilters());
                (*session)->setThrowTypes(m_breakpoints.sessionThrowTypes());
            }
            return session;
        },
        true);
    if (started) {
        m_lastCoreClrSession = (adapterId == "coreclr");
        m_nativeSession = adapterId == "lldb-dap" || adapterId == "gdb";
        m_nativeOsThreadIdsVerified = osThreadIdsVerified;
    }
    return started;
}

} // namespace frost::ui
