// SPDX-License-Identifier: Apache-2.0
// Authenticated native shader diagnostics and engine-rendered image snapshots, guarded by current images/state.
#include "frost/engine/shader_preview.hpp"
#include "frost/io/path.hpp"
#include "frost/io/spawn_log.hpp"
#include "frost/lang/uri.hpp"
#include "frost/ui/shell/tool_window_manager.hpp"
#include "frost/ui/toolwindows/problems.hpp"

#include "engine_integration_internal.hpp"
#include <QDialog>
#include <QDir>
#include <QFileDialog>
#include <QImage>
#include <QLabel>
#include <QMetaObject>
#include <QPixmap>
#include <QVBoxLayout>

#include <algorithm>
#include <map>
#include <tuple>
namespace frost::app {
namespace {
QString qs(std::string_view s) { return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size())); }
bool within(std::string_view root, std::string_view file) {
    auto prefix = lang::documentKey(root);
    if (!prefix.ends_with('/') && !prefix.ends_with('\\'))
        prefix += '/';
    auto key = lang::documentKey(file);
    std::replace(key.begin(), key.end(), '\\', '/');
    std::replace(prefix.begin(), prefix.end(), '\\', '/');
    return key.starts_with(prefix);
}
using Images = std::map<std::string, std::string>;
struct Prepared {
    std::string path, physical, source, hash;
};
Result<Prepared> prepare(io::IFileSystem& fs, std::string_view root, std::string_view path,
                         const std::optional<std::string>& overlay, bool preview) {
    auto physical = fs.physicalPath(path);
    if (!physical || !within(root, *physical))
        return fail(ErrorCode::PermissionDenied, "Selected shader/asset is outside the actual project");
    const auto relative = QDir(qs(root)).relativeFilePath(qs(*physical));
    const auto virtualPath = QDir::fromNativeSeparators(relative).toStdString();
    if (!virtualPath.starts_with("Assets/") || virtualPath.find("../") != std::string::npos)
        return fail(ErrorCode::InvalidArgument, "Select a source below Assets/");
    auto bytes = fs.readFileSnapshot(*physical, preview ? 32U * 1024U * 1024U : 256U * 1024U);
    if (!bytes)
        return std::unexpected(bytes.error());
    if (preview && overlay && *overlay != *bytes)
        return fail(ErrorCode::Busy, "Save the asset before rendering its on-disk preview");
    Prepared result{virtualPath, *physical, preview ? std::string{} : overlay.value_or(*bytes), io::sha256Hex(*bytes)};
    return result;
}
Result<void> dependencies(io::IFileSystem& fs, std::string_view root, const std::vector<engine::ShaderInput>& inputs,
                          bool text = true) {
    size_t total = 0;
    for (const auto& input : inputs) {
        if (input.missing) {
            auto target = input.path.starts_with("Assets/") ? io::joinPath(root, input.path)
                                                            : io::normalizePath(input.physicalPath);
            if (!target)
                return std::unexpected(target.error());
            auto exists = fs.exists(*target);
            if (!exists || *exists)
                return fail(ErrorCode::Busy, "A missing shader include appeared");
            continue;
        }
        auto actual = fs.physicalPath(input.physicalPath);
        if (!actual)
            return std::unexpected(actual.error());
        if (input.path.starts_with("Assets/")) {
            auto joined = io::joinPath(root, input.path);
            if (!joined)
                return std::unexpected(joined.error());
            auto expected = fs.physicalPath(*joined);
            if (!expected || !within(root, *actual) || lang::documentKey(*expected) != lang::documentKey(*actual))
                return fail(ErrorCode::Busy, "Shader dependency identity changed");
        }
        auto bytes = fs.readFileSnapshot(*actual, text ? 4U * 1024U * 1024U : 32U * 1024U * 1024U);
        if (!bytes)
            return std::unexpected(bytes.error());
        total += bytes->size();
        if (text && bytes->starts_with("\xEF\xBB\xBF"))
            bytes->erase(0, 3);
        if (total > (text ? 4U * 1024U * 1024U : 128U * 1024U * 1024U) || io::sha256Hex(*bytes) != input.sha256)
            return fail(ErrorCode::Busy, "Shader dependency changed since engine validation");
    }
    return {};
}
} // namespace
void EngineIntegration::invalidateShaderPreview() { m_impl->clearShaderPreview(); }
void EngineIntegration::installShaderPreviewActions() {
    for (const auto& [id, name, preview] : {std::tuple{"engine.validateShader", "Validate Shader / Material", false},
                                            std::tuple{"engine.previewAsset", "Preview Engine Asset...", true}}) {
        input::ActionSpec action;
        action.id = id;
        action.name = name;
        action.group = "FrostEngine";
        const QPointer<EngineIntegration> self(this);
        action.handler = [self, preview](const input::ActionContext&, const input::InvokeInfo&) {
            if (!self)
                return;
            auto result = self->shaderPreview(preview);
            if (self && !result)
                self->m_impl->setStatus(qs(result.error().message()));
        };
        action.enabled = [self](const input::ActionContext&) {
            if (!self)
                return false;
            auto& impl = *self->m_impl;
            return impl.available && impl.session && impl.state && impl.state->state == "Edit" &&
                   impl.admitted(impl.epoch, impl.root, impl.trustRevision);
        };
        if (auto registered = m_impl->workbench.m_registry.add(std::move(action)); !registered)
            m_impl->setStatus(qs(registered.error().message()));
    }
}
void EngineIntegration::Impl::clearShaderPreview() {
    ++shaderPreviewGeneration;
    if (previewDialog)
        previewDialog->close();
    previewDialog = nullptr;
    if (workbench.m_problems)
        workbench.m_problems->model()->setProblems(ui::DiagnosticSource::EngineShader, {});
}
Result<void> EngineIntegration::shaderPreview(bool preview, std::string absolutePath) {
    auto& impl = *m_impl;
    if (!impl.admitted(impl.epoch, impl.root, impl.trustRevision) || !impl.session || !impl.state ||
        impl.state->state != "Edit")
        return fail(ErrorCode::Busy, "Pair a trusted engine in Edit state first");
    const QPointer<EngineIntegration> modalSelf(this);
    const auto modalEpoch = impl.epoch, modalSession = impl.sessionEpoch, modalTrust = impl.trustRevision;
    const auto modalRoot = impl.root;
    auto* view = impl.workbench.currentView();
    if (absolutePath.empty() && view && (!preview || qs(view->path()).endsWith(".fmat")))
        absolutePath = view->path();
    if (absolutePath.empty() && preview)
        absolutePath = QFileDialog::getOpenFileName(
                           impl.workbench.m_window, QStringLiteral("Preview Engine Asset"), qs(impl.root) + "/Assets",
                           QStringLiteral("Engine visual assets (*.png *.jpg *.jpeg *.tga *.bmp *.fmat *.fsheet)"))
                           .toStdString();
    if (!modalSelf || !impl.admitted(modalEpoch, modalRoot, modalTrust) || impl.sessionEpoch != modalSession)
        return fail(ErrorCode::Cancelled, "Engine project/session changed while choosing the asset");
    if (absolutePath.empty())
        return fail(ErrorCode::Cancelled, "No asset selected");
    if (!preview && !qs(absolutePath).endsWith(".glsl") && !qs(absolutePath).endsWith(".fmat"))
        return fail(ErrorCode::InvalidArgument, "Open an engine GLSL program or material");
    std::optional<std::string> overlay;
    if (auto* editor = impl.workbench.m_editors->find(absolutePath))
        overlay = editor->document().document().text().text();
    Images images;
    for (const auto& document : impl.workbench.m_documents->documents())
        if (!document->path().empty())
            images.emplace(lang::documentKey(document->path()), document->document().text().text());
    const auto epoch = impl.epoch, sessionEpoch = impl.sessionEpoch, revision = impl.trustRevision;
    const auto root = impl.physicalRoot;
    const auto logicalRoot = impl.root;
    const auto ticket = ++impl.shaderPreviewGeneration;
    auto* fs = &impl.workbench.m_services.fs;
    QPointer<EngineIntegration> self(this);
    impl.background([self, fs, epoch, sessionEpoch, revision, root, logicalRoot, ticket, absolutePath, overlay, images,
                     preview] {
        auto prepared = prepare(*fs, root, absolutePath, overlay, preview);
        if (!self)
            return;
        QMetaObject::invokeMethod(
            self.data(),
            [self, epoch, sessionEpoch, revision, root, logicalRoot, ticket, absolutePath, overlay, images, preview,
             prepared = std::move(prepared)]() mutable {
                if (!self)
                    return;
                auto& impl = *self->m_impl;
                if (ticket != impl.shaderPreviewGeneration || sessionEpoch != impl.sessionEpoch ||
                    !impl.admitted(epoch, logicalRoot, revision))
                    return;
                if (!prepared) {
                    impl.setStatus(qs(prepared.error().message()));
                    return;
                }
                auto params = lang::Json::object().set("path", prepared->path);
                if (preview)
                    params.set("size", 256).set("frame", 0);
                else
                    params.set("source", prepared->source)
                        .set("shaderRoot", prepared->path.ends_with(".fmat") ? "Engine/Shaders" : "Assets/Shaders");
                auto sent = impl.send(
                    preview ? "assets.preview" : "shader.validate", std::move(params),
                    [self, epoch, sessionEpoch, revision, root, logicalRoot, ticket, absolutePath, overlay, images,
                     preview, prepared = std::move(*prepared)](Result<lang::Json> reply) mutable {
                        if (!self)
                            return;
                        auto& impl = *self->m_impl;
                        if (!reply) {
                            impl.setStatus(qs(reply.error().message()));
                            return;
                        }
                        auto* fs = &impl.workbench.m_services.fs;
                        impl.background([self, fs, epoch, sessionEpoch, revision, root, logicalRoot, ticket,
                                         absolutePath, overlay, images, preview, prepared,
                                         reply = std::move(*reply)]() mutable {
                            Result<engine::ShaderValidation> diagnostics = fail(ErrorCode::Unsupported);
                            Result<engine::AssetPreview> image = fail(ErrorCode::Unsupported);
                            Result<void> valid;
                            if (preview) {
                                image = engine::parseAssetPreview(reply, prepared.path, prepared.hash);
                                if (!image)
                                    valid = std::unexpected(image.error());
                                else
                                    valid = dependencies(*fs, root, image->inputs, false);
                            } else {
                                diagnostics = engine::parseShaderValidation(reply, prepared.path, prepared.source);
                                if (!diagnostics)
                                    valid = std::unexpected(diagnostics.error());
                                else {
                                    valid = dependencies(*fs, root, diagnostics->inputs);
                                    for (const auto& input : diagnostics->inputs) {
                                        const auto open = images.find(lang::documentKey(input.physicalPath));
                                        if (open != images.end() && io::sha256Hex(open->second) != input.sha256)
                                            valid = fail(ErrorCode::Busy,
                                                         "Save included shader buffers before validating this program");
                                    }
                                }
                            }
                            auto current = fs->physicalPath(absolutePath);
                            if (!current || lang::documentKey(*current) != lang::documentKey(prepared.physical))
                                valid = fail(ErrorCode::Busy, "Asset identity changed");
                            if (preview || !overlay) {
                                auto bytes = fs->readFileSnapshot(prepared.physical, 32U * 1024U * 1024U);
                                if (!bytes || io::sha256Hex(*bytes) != prepared.hash)
                                    valid = fail(ErrorCode::Busy, "Asset changed since request");
                            }
                            if (!self)
                                return;
                            QMetaObject::invokeMethod(
                                self.data(),
                                [self, epoch, sessionEpoch, revision, root, logicalRoot, ticket, absolutePath, overlay,
                                 images, preview, prepared, valid = std::move(valid),
                                 diagnostics = std::move(diagnostics), image = std::move(image)]() mutable {
                                    if (!self)
                                        return;
                                    auto& impl = *self->m_impl;
                                    if (ticket != impl.shaderPreviewGeneration || sessionEpoch != impl.sessionEpoch ||
                                        !impl.admitted(epoch, logicalRoot, revision) || !impl.state ||
                                        impl.state->state != "Edit")
                                        return;
                                    Images currentImages;
                                    for (const auto& document : impl.workbench.m_documents->documents())
                                        if (!document->path().empty())
                                            currentImages.emplace(lang::documentKey(document->path()),
                                                                  document->document().text().text());
                                    if (currentImages != images)
                                        return;
                                    auto* view = impl.workbench.m_editors->find(absolutePath);
                                    if (overlay && (!view || view->document().document().text().text() != *overlay))
                                        return;
                                    if (!valid) {
                                        impl.setStatus(qs(valid.error().message()));
                                        return;
                                    }
                                    if (preview) {
                                        QImage pixels =
                                            QImage::fromData(reinterpret_cast<const uchar*>(image->png.data()),
                                                             static_cast<int>(image->png.size()), "PNG");
                                        if (pixels.isNull() || pixels.width() != static_cast<int>(image->size) ||
                                            pixels.height() != static_cast<int>(image->size)) {
                                            impl.setStatus(QStringLiteral("Engine PNG could not be decoded"));
                                            return;
                                        }
                                        if (impl.previewDialog)
                                            impl.previewDialog->close();
                                        auto* dialog = new QDialog(impl.workbench.m_window);
                                        dialog->setAttribute(Qt::WA_DeleteOnClose);
                                        dialog->setWindowTitle(QStringLiteral("Engine-rendered preview snapshot"));
                                        auto* layout = new QVBoxLayout(dialog);
                                        auto* label = new QLabel(dialog);
                                        label->setObjectName(QStringLiteral("engineRenderedPreviewImage"));
                                        label->setPixmap(QPixmap::fromImage(pixels));
                                        layout->addWidget(label);
                                        layout->addWidget(new QLabel(
                                            qs(prepared.path) +
                                                QStringLiteral("\nActual engine frame; snapshot of saved asset."),
                                            dialog));
                                        impl.previewDialog = dialog;
                                        dialog->show();
                                    } else {
                                        std::vector<build::BuildDiagnostic> rows;
                                        for (const auto& issue : diagnostics->issues) {
                                            build::BuildDiagnostic row;
                                            row.diagnostic.code = "engine-shader";
                                            row.diagnostic.message = issue.message;
                                            row.diagnostic.severity = issue.severity == "error"
                                                                          ? build::Severity::Error
                                                                          : build::Severity::Warning;
                                            row.diagnostic.file = issue.path;
                                            row.diagnostic.line = static_cast<int>(issue.line);
                                            row.diagnostic.column = static_cast<int>(issue.column);
                                            if (issue.path.starts_with("Assets/")) {
                                                auto joined = io::joinPath(root, issue.path);
                                                if (joined) {
                                                    row.path = *joined;
                                                    row.jumpable = issue.line > 0;
                                                }
                                            }
                                            rows.push_back(std::move(row));
                                        }
                                        if (impl.workbench.m_problems)
                                            impl.workbench.m_problems->model()->setProblems(
                                                ui::DiagnosticSource::EngineShader, rows);
                                        if (impl.workbench.m_window)
                                            impl.workbench.m_window->toolWindows().show(ui::tool_windows::kProblems,
                                                                                        false);
                                        impl.setStatus(
                                            QStringLiteral("Engine base-variant validation: %1 diagnostic(s)")
                                                .arg(static_cast<qulonglong>(rows.size())));
                                    }
                                },
                                Qt::QueuedConnection);
                        });
                    });
                if (!sent)
                    impl.setStatus(qs(sent.error().message()));
            },
            Qt::QueuedConnection);
    });
    return {};
}
} // namespace frost::app
