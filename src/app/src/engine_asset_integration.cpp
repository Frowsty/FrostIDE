// SPDX-License-Identifier: Apache-2.0
// FEC-4/5 actual authenticated engine usage observations, never source-scan estimates.
#include "frost/io/path.hpp"
#include "frost/lang/uri.hpp"
#include "frost/ui/nav/document_registry.hpp"
#include "frost/ui/shell/tool_window_manager.hpp"
#include "frost/ui/toolwindows/engine_view.hpp"

#include "engine_integration_internal.hpp"
namespace frost::app {
namespace {
QString qs(std::string_view s) { return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size())); }
} // namespace
void EngineIntegration::Impl::clearAssetUsages() {
    ++assetInvalidationGeneration;
    assetQueries.clear();
    assetUsages.reset();
    if (projectView)
        projectView->clearAssetUsages();
    if (workbench.m_editors)
        for (auto* view : workbench.m_editors->views())
            view->editor().setCodeVisionLensesForProvider("engine.asset", {});
}
void EngineIntegration::invalidateAssetUsages() { m_impl->clearAssetUsages(); }
Result<std::uint64_t> EngineIntegration::contentCommand(std::string method, lang::Json params,
                                                        std::function<void(Result<lang::Json>)> completed) {
    return m_impl->send(std::move(method), std::move(params), std::move(completed));
}
Result<void> EngineIntegration::Impl::findAssetUsages(std::string relative, engine::EngineHandle handle) {
    if (!assetIndex)
        return fail(ErrorCode::NotFound, "Refresh the actual engine asset index first");
    const auto* asset = assetIndex->find(handle);
    if (!asset || asset->path != relative)
        return fail(ErrorCode::InvalidArgument, "The asset row is no longer in this engine index");
    const auto generation = ++assetQueryGeneration;
    const auto invalidation = assetInvalidationGeneration;
    assetQueries[handle.value] = generation;
    auto index = assetIndex;
    assetUsages.reset();
    if (projectView)
        projectView->clearAssetUsages(QStringLiteral("Querying actual engine asset usages..."));
    auto path = io::joinPath(index->content().directory, asset->path);
    if (path && workbench.m_editors)
        for (auto* view : workbench.m_editors->views())
            if (lang::documentKey(view->path()) == lang::documentKey(*path))
                view->editor().setCodeVisionLensesForProvider("engine.asset", {});
    const QPointer<EngineIntegration> self(&owner);
    auto sent = send(
        "assets.find-references", lang::Json::object().set("asset", engine::toJson(handle)),
        [self, index, generation, invalidation, handle](Result<lang::Json> reply) {
            if (!self)
                return;
            auto& impl = *self->m_impl;
            auto pending = impl.assetQueries.find(handle.value);
            if (impl.assetIndex != index || impl.assetInvalidationGeneration != invalidation ||
                pending == impl.assetQueries.end() || pending->second != generation)
                return;
            impl.assetQueries.erase(pending);
            if (!reply) {
                if (impl.projectView && impl.assetQueryGeneration == generation)
                    impl.projectView->clearAssetUsages(qs(reply.error().message()));
                return;
            }
            auto parsed = engine::parseAssetUsages(*index, handle, *reply);
            if (!parsed) {
                impl.setStatus(qs(parsed.error().message()));
                return;
            }
            const auto count = parsed->codeVisionCount();
            const auto full = io::joinPath(index->content().directory, parsed->target.path);
            if (full && impl.workbench.m_editors)
                for (auto* view : impl.workbench.m_editors->views())
                    if (lang::documentKey(view->path()) == lang::documentKey(*full)) {
                        std::vector<ui::EditorCodeVisionLens> lenses;
                        if (count)
                            lenses.push_back(
                                {.line = LineIndex{0},
                                 .symbolOffset = ByteOffset{0},
                                 .label = std::to_string(*count) + " asset usage rows",
                                 .actionId = "engine.assetUsages",
                                 .tooltip = "Engine query snapshot (not live): " + std::to_string(parsed->occurrences) +
                                            " occurrences. Includes unsaved FrostEditor scenes; click to refresh."});
                        view->editor().setCodeVisionLensesForProvider("engine.asset", std::move(lenses));
                    }
            if (impl.assetQueryGeneration == generation) {
                impl.assetUsages = std::move(*parsed);
                if (impl.projectView)
                    impl.projectView->setAssetUsages(*impl.assetUsages);
            }
        });
    if (!sent) {
        assetQueries.erase(handle.value);
        return std::unexpected(sent.error());
    }
    return {};
}
Result<void> EngineIntegration::findAssetUsagesForDocument(std::string_view path, bool reveal) {
    if (!m_impl->assetIndex)
        return fail(ErrorCode::NotFound, "No actual engine asset index");
    const auto key = lang::documentKey(path);
    auto prefix = lang::documentKey(m_impl->assetIndex->content().directory);
    if (!prefix.ends_with('/') && !prefix.ends_with('\\'))
        prefix += io::kNativePathStyle == io::PathStyle::Windows ? '\\' : '/';
    if (!key.starts_with(prefix))
        return fail(ErrorCode::NotFound, "This document is outside the engine project");
    const auto* asset = m_impl->assetIndex->find(key.substr(prefix.size()));
    if (!asset)
        return fail(ErrorCode::NotFound, "This document is not an engine-listed asset");
    auto result = m_impl->findAssetUsages(asset->path, asset->handle);
    if (result && reveal && m_impl->workbench.m_window)
        m_impl->workbench.m_window->toolWindows().show(kEngineProjectWindow);
    return result;
}
} // namespace frost::app
