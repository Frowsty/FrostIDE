// SPDX-License-Identifier: Apache-2.0
// GUI admission/publication for normal GLSL editor requests; compiler work uses the authenticated engine.
#include "frost/io/path.hpp"
#include "frost/lang/uri.hpp"
#include "frost/ui/lang/editor_insight_binding.hpp"
#include "frost/ui/toolwindows/problems.hpp"

#include "engine_integration_internal.hpp"
#include "engine_shader_language_input.hpp"
#include "engine_shader_language_requester.hpp"
#include "language_bridge.hpp"
#include <QMetaObject>
#include <QTimer>

#include <algorithm>
#include <mutex>
namespace frost::app {
namespace {
using Reply = std::function<void(Result<lang::Json>)>;
struct ReplyGate {
    std::mutex Mutex;
    Reply Callback;
    explicit ReplyGate(Reply callback) : Callback(std::move(callback)) {}
    void finish(Result<lang::Json> value) {
        Reply callback;
        {
            std::lock_guard lock(Mutex);
            callback = std::move(Callback);
        }
        if (callback)
            callback(std::move(value));
    }
};
struct Cancellation {
    std::stop_source Stop;
    std::shared_ptr<ReplyGate> Gate;
    using Callback = std::stop_callback<std::function<void()>>;
    std::optional<Callback> Caller, Owner;
    Cancellation(std::stop_token caller, std::stop_token owner, std::shared_ptr<ReplyGate> gate)
        : Gate(std::move(gate)) {
        const std::function<void()> cancelled = [this] {
            Stop.request_stop();
            Gate->finish(fail(ErrorCode::Cancelled, "GLSL source/session changed"));
        };
        Caller.emplace(caller, cancelled);
        Owner.emplace(owner, cancelled);
    }
};
sched::Task<void> diagnose(lang::ILspRequester* requester, std::string path, std::stop_token stop) {
    auto uri = lang::pathToUri(path);
    if (!uri)
        co_return;
    static_cast<void>(co_await requester->request(
        "textDocument/diagnostic", lang::Json::object().set("textDocument", lang::Json::object().set("uri", *uri)),
        stop));
    co_return;
}
} // namespace
std::string EngineIntegration::shaderLanguageIdentity() const {
    return std::to_string(m_impl->epoch) + ":" + std::to_string(m_impl->sessionEpoch) + ":" +
           std::to_string(m_impl->shaderPreviewGeneration);
}
void EngineIntegration::scheduleShaderDiagnostics(std::string path) {
    auto& impl = *m_impl;
    if (!isEngineShaderSource(path) || !impl.session || !impl.state || impl.state->state != "Edit" ||
        !impl.admitted(impl.epoch, impl.root, impl.trustRevision))
        return;
    const auto generation = impl.shaderPreviewGeneration;
    if (impl.shaderDiagnosticScheduled == generation &&
        lang::documentKey(impl.shaderDiagnosticScheduledPath) == lang::documentKey(path))
        return;
    impl.shaderDiagnosticScheduled = generation;
    impl.shaderDiagnosticScheduledPath = path;
    QPointer<EngineIntegration> self(this);
    QTimer::singleShot(700, this, [self, path = std::move(path), generation] {
        if (!self)
            return;
        auto& impl = *self->m_impl;
        if (impl.shaderPreviewGeneration != generation || !impl.state || impl.state->state != "Edit" ||
            !impl.admitted(impl.epoch, impl.root, impl.trustRevision))
            return;
        const auto settings = impl.workbench.m_services.settings.snapshot();
        if (settings->registry().indexOf("analysis.powerSave") && std::get<bool>(settings->value("analysis.powerSave")))
            return;
        if (!impl.workbench.m_bridge)
            return;
        impl.tasks.push_back(impl.workbench.m_pool->spawn(
            sched::Lane::Background, diagnose(impl.workbench.m_bridge.get(), path, impl.workStop.get_token())));
    });
}
void EngineIntegration::requestShaderLanguage(std::string method, lang::Json params, std::stop_token caller,
                                              Reply completed) {
    auto gate = std::make_shared<ReplyGate>(std::move(completed));
    auto& impl = *m_impl;
    if (!impl.session || !impl.state || impl.state->state != "Edit" ||
        !impl.admitted(impl.epoch, impl.root, impl.trustRevision)) {
        gate->finish(fail(ErrorCode::Busy, "Pair a trusted engine in Edit state for GLSL language features"));
        return;
    }
    auto operation = std::make_shared<Cancellation>(caller, impl.shaderLanguageStop.get_token(), gate);
    if (operation->Stop.stop_requested())
        return;
    const auto* document = params.find("textDocument");
    auto path = document ? lang::uriToPath(document->stringAt("uri"))
                         : Result<std::string>(fail(ErrorCode::InvalidArgument, "GLSL query needs a source URI"));
    if (!path || !isEngineShaderSource(*path)) {
        gate->finish(fail(ErrorCode::InvalidArgument, "Not an engine GLSL source"));
        return;
    }
    auto* view = impl.workbench.m_editors->find(*path);
    if (!view) {
        gate->finish(fail(ErrorCode::Busy, "GLSL query source is no longer open"));
        return;
    }
    const auto snapshot = view->document().document().text();
    if (snapshot.size().value() > 256 * 1024) {
        gate->finish(fail(ErrorCode::OutOfRange, "GLSL source exceeds 256KiB"));
        return;
    }
    size_t offset = 0;
    if (const auto* point = params.find("position")) {
        auto position = lang::parsePosition(*point);
        auto byte = position ? lang::fromLspPosition(snapshot, *position, lang::PositionEncoding::Utf8)
                             : Result<ByteOffset>(fail(ErrorCode::InvalidArgument));
        if (!byte) {
            gate->finish(std::unexpected(byte.error()));
            return;
        }
        offset = byte->value();
    }
    std::string program = *path;
    if (!isEngineShaderProgram(program)) {
        program = impl.shaderLanguageProgram;
        if (program.empty()) {
            gate->finish(
                fail(ErrorCode::Unsupported, "Open/query the owning .glsl program before editing its includes"));
            return;
        }
    }
    ShaderLanguageImages captured;
    std::map<std::string, std::pair<const document::FileDocument*, std::uint64_t>> stamps;
    size_t total = 0;
    auto shaderRoot = io::joinPath(impl.physicalRoot, "Assets/Shaders");
    if (!shaderRoot) {
        gate->finish(std::unexpected(shaderRoot.error()));
        return;
    }
    auto prefix = lang::documentKey(*shaderRoot);
    std::replace(prefix.begin(), prefix.end(), '\\', '/');
    if (!prefix.ends_with('/'))
        prefix += '/';
    for (const auto& openDocument : impl.workbench.m_documents->documents())
        if (!openDocument->path().empty()) {
            auto key = lang::documentKey(openDocument->path());
            std::replace(key.begin(), key.end(), '\\', '/');
            if (!key.starts_with(prefix))
                continue;
            const auto text = openDocument->document().text();
            if (text.size().value() > 256 * 1024 || text.size().value() > 4 * 1024 * 1024 - total ||
                captured.size() >= 128) {
                gate->finish(fail(ErrorCode::OutOfRange, "GLSL open snapshots exceed input budget"));
                return;
            }
            total += text.size().value();
            captured.emplace(openDocument->path(), text.text());
            stamps.emplace(openDocument->path(),
                           std::pair{openDocument.get(), static_cast<std::uint64_t>(text.version())});
        }
    const auto epoch = impl.epoch, sessionEpoch = impl.sessionEpoch, trust = impl.trustRevision,
               generation = impl.shaderPreviewGeneration;
    const auto root = impl.root, physicalRoot = impl.physicalRoot;
    const auto query = *path;
    auto* fs = &impl.workbench.m_services.fs;
    QPointer<EngineIntegration> self(this);
    impl.background([self, fs, epoch, sessionEpoch, trust, generation, root, physicalRoot, query, program, method,
                     offset, captured, stamps, gate, operation] {
        auto input = captureShaderLanguage(*fs, physicalRoot, program, query, captured, method, offset);
        if (!self) {
            gate->finish(fail(ErrorCode::Cancelled));
            return;
        }
        QMetaObject::invokeMethod(
            self.data(),
            [self, epoch, sessionEpoch, trust, generation, root, query, method, captured, stamps, gate, operation,
             input = std::move(input)]() mutable {
                if (!self) {
                    gate->finish(fail(ErrorCode::Cancelled));
                    return;
                }
                auto& impl = *self->m_impl;
                if (operation->Stop.stop_requested())
                    return;
                if (generation != impl.shaderPreviewGeneration || sessionEpoch != impl.sessionEpoch ||
                    !impl.admitted(epoch, root, trust)) {
                    gate->finish(fail(ErrorCode::Cancelled));
                    return;
                }
                if (!input) {
                    gate->finish(std::unexpected(input.error()));
                    return;
                }
                auto params = input->params;
                auto sent = impl.send(
                    "shader.language", std::move(params),
                    [self, epoch, sessionEpoch, trust, generation, root, query, method, captured, stamps, gate,
                     operation, input = std::move(*input)](Result<lang::Json> reply) mutable {
                        if (!self) {
                            gate->finish(fail(ErrorCode::Cancelled));
                            return;
                        }
                        if (operation->Stop.stop_requested())
                            return;
                        if (!reply) {
                            gate->finish(std::unexpected(reply.error()));
                            return;
                        }
                        auto* fs = &self->m_impl->workbench.m_services.fs;
                        self->m_impl->background([self, fs, epoch, sessionEpoch, trust, generation, root, query, method,
                                                  captured, stamps, gate, operation, input,
                                                  reply = std::move(*reply)]() mutable {
                            auto answer = validateShaderLanguage(*fs, input, reply, method);
                            if (!self) {
                                gate->finish(fail(ErrorCode::Cancelled));
                                return;
                            }
                            QMetaObject::invokeMethod(
                                self.data(),
                                [self, epoch, sessionEpoch, trust, generation, root, query, captured, stamps, gate,
                                 operation, input, answer = std::move(answer)]() mutable {
                                    if (!self) {
                                        gate->finish(fail(ErrorCode::Cancelled));
                                        return;
                                    }
                                    auto& impl = *self->m_impl;
                                    if (operation->Stop.stop_requested())
                                        return;
                                    if (generation != impl.shaderPreviewGeneration ||
                                        sessionEpoch != impl.sessionEpoch || !impl.admitted(epoch, root, trust) ||
                                        !impl.state || impl.state->state != "Edit") {
                                        gate->finish(fail(ErrorCode::Cancelled));
                                        return;
                                    }
                                    for (const auto& [capturedPath, bytes] : captured) {
                                        auto* current = impl.workbench.m_documents->find(capturedPath);
                                        const auto stamp = stamps.find(capturedPath);
                                        if (!current || stamp == stamps.end() || current != stamp->second.first ||
                                            current->document().text().version() != stamp->second.second ||
                                            current->document().text().text() != bytes) {
                                            gate->finish(fail(ErrorCode::Busy, "GLSL editor images changed"));
                                            return;
                                        }
                                    }
                                    if (!answer) {
                                        gate->finish(std::unexpected(answer.error()));
                                        return;
                                    }
                                    if (auto* current = impl.workbench.currentView();
                                        current && lang::documentKey(current->path()) == lang::documentKey(query))
                                        impl.shaderLanguageProgram = input.programPhysical;
                                    std::vector<build::BuildDiagnostic> rows;
                                    for (auto* editor : impl.workbench.m_editors->views())
                                        if (isEngineShaderSource(editor->path()))
                                            editor->editor().setDiagnostics(ui::EditorDiagnosticSource::Live, {});
                                    for (const auto& issue : answer->result.diagnostics) {
                                        const auto found = std::ranges::find(answer->sources, issue.path,
                                                                             &engine::ShaderLanguageSource::path);
                                        if (found == answer->sources.end())
                                            continue;
                                        build::BuildDiagnostic row;
                                        row.path = found->physicalPath;
                                        row.jumpable = issue.line > 0;
                                        row.diagnostic.file = row.path;
                                        row.diagnostic.line = static_cast<int>(issue.line);
                                        row.diagnostic.column = static_cast<int>(issue.column);
                                        row.diagnostic.code = "engine-shader";
                                        row.diagnostic.message = issue.message;
                                        row.diagnostic.severity = issue.severity == "error" ? build::Severity::Error
                                                                                            : build::Severity::Warning;
                                        rows.push_back(std::move(row));
                                    }
                                    if (impl.workbench.m_problems)
                                        impl.workbench.m_problems->model()->setProblems(
                                            ui::DiagnosticSource::EngineShader, rows);
                                    for (auto* editor : impl.workbench.m_editors->views())
                                        if (isEngineShaderSource(editor->path())) {
                                            lang::FileDiagnostics diagnostics;
                                            diagnostics.file = editor->path();
                                            const auto text = editor->document().document().text();
                                            diagnostics.textVersion = text.version();
                                            for (const auto& row : rows)
                                                if (lang::documentKey(row.path) == lang::documentKey(editor->path()) &&
                                                    row.diagnostic.line > 0 &&
                                                    static_cast<uint32_t>(row.diagnostic.line) <= text.lineCount()) {
                                                    auto line = text.lineInfo(
                                                        LineIndex{static_cast<uint32_t>(row.diagnostic.line - 1)});
                                                    if (!line)
                                                        continue;
                                                    lang::LiveDiagnostic diagnostic;
                                                    diagnostic.file = row.path;
                                                    diagnostic.message = row.diagnostic.message;
                                                    diagnostic.code = "engine-shader";
                                                    diagnostic.source = "FrostEngine GLSL";
                                                    diagnostic.line = row.diagnostic.line;
                                                    diagnostic.column = row.diagnostic.column;
                                                    diagnostic.severity =
                                                        row.diagnostic.severity == build::Severity::Error
                                                            ? lang::DiagnosticSeverity::Error
                                                            : lang::DiagnosticSeverity::Warning;
                                                    diagnostic.range = line->content;
                                                    diagnostic.textVersion = text.version();
                                                    diagnostics.diagnostics.push_back(std::move(diagnostic));
                                                }
                                            editor->editor().setDiagnostics(ui::EditorDiagnosticSource::Live,
                                                                            ui::lang::toEditorDiagnostics(diagnostics));
                                        }
                                    gate->finish(std::move(answer->result.response));
                                },
                                Qt::QueuedConnection);
                        });
                    },
                    false, operation->Stop.get_token());
                if (!sent)
                    gate->finish(std::unexpected(sent.error()));
            },
            Qt::QueuedConnection);
    });
}
} // namespace frost::app
