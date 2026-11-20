// SPDX-License-Identifier: Apache-2.0
// FEC-9b: actual engine proposals admitted to one native REF1 edge and its exact inverse.
#include "frost/engine/asset_move_plan.hpp"
#include "frost/io/path.hpp"
#include "frost/lang/uri.hpp"

#include "engine_integration_internal.hpp"
#include "workbench_source_rename.hpp"
#include <QDialog>
#include <QDialogButtonBox>
#include <QEventLoop>
#include <QLabel>
#include <QPlainTextEdit>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
namespace frost::app {
namespace {
struct MoveBoundary {
    std::function<Result<lang::Json>(std::string, lang::Json)> request;
    std::function<Result<void>()> current;
    std::function<void()> refresh;
    engine::AssetMovePlan original;
    std::string lease, activePath, activeSource;
    bool applied{}, nativeSettled{};
    ~MoveBoundary() { cancel(); }
    void cancel() {
        if (lease.empty() || nativeSettled)
            return;
        auto reply = request("assets.finish-move-plan", lang::Json::object().set("lease", lease).set("applied", false));
        if (reply && reply->boolAt("verified") == true && reply->boolAt("applied") == false &&
            reply->stringAt("lease") == lease)
            lease.clear();
    }
    Result<void> validate() {
        if (auto valid = current(); !valid)
            return valid;
        if (lease.empty())
            return fail(ErrorCode::Busy, "The engine move lease is unavailable");
        auto reply = request("assets.validate-move-plan", lang::Json::object().set("lease", lease));
        if (!reply)
            return std::unexpected(reply.error());
        if (reply->boolAt("verified") != true || reply->stringAt("lease") != lease ||
            reply->stringAt("receipt") != original.receipt || reply->boolAt("applied") != false ||
            reply->boolAt("refreshed") != false || reply->stringAt("path") != activeSource ||
            engine::engineHandleAt(*reply, "handle") != std::optional(original.handle))
            return fail(ErrorCode::Corrupt, "Engine lease validation receipt is incomplete");
        return {};
    }
};
QString q(std::string_view s) { return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size())); }
} // namespace
Result<void> EngineIntegration::prepareAssetMove(document::RefactoringPlan& plan, std::string oldPath,
                                                 std::string newPath) {
    auto& impl = *m_impl;
    if (!impl.assetIndex || !impl.identity || !impl.session || !impl.session->connected())
        return fail(ErrorCode::Unsupported, "Pair FrostEditor and refresh its actual asset index before moving assets");
    auto old = lang::locateFrostEngineAsset(impl.workbench.m_services.fs, oldPath);
    auto target = lang::locateFrostEngineAsset(impl.workbench.m_services.fs, newPath);
    if (!old || !target || lang::documentKey(old->projectRoot) != lang::documentKey(target->projectRoot) ||
        lang::documentKey(old->projectRoot) != lang::documentKey(impl.root))
        return fail(ErrorCode::Unsupported, "Engine asset moves must stay inside the paired project's Assets folder");
    const auto* asset = impl.assetIndex->find(old->projectAssetPath);
    if (!asset)
        return fail(ErrorCode::Busy, "Refresh the engine index; the source is not an engine-listed asset");
    const auto handle = asset->handle;
    auto state = std::make_shared<MoveBoundary>();
    QPointer<EngineIntegration> self(this);
    const auto epoch = impl.epoch, sessionEpoch = impl.sessionEpoch, revision = impl.trustRevision;
    const auto root = impl.root, physicalRoot = impl.physicalRoot, projectKey = impl.projectKey;
    const auto identity = *impl.identity;
    const auto admission = impl.physicalAdmission;
    auto* exact = impl.session.get();
    auto* fs = &impl.workbench.m_services.fs;
    state->current = [self, epoch, sessionEpoch, revision, root, identity, exact, fs, projectKey,
                      admission]() -> Result<void> {
        if (!self || !self->m_impl->admitted(epoch, root, revision) || self->m_impl->sessionEpoch != sessionEpoch ||
            self->m_impl->session.get() != exact || !self->m_impl->currentIdentity(identity) ||
            !retainEngineProjectIdentity(*fs, root, projectKey, admission))
            return fail(ErrorCode::Cancelled, "The exact engine session/project/trust boundary changed");
        for (const auto& doc : self->m_impl->workbench.m_documents->documents())
            if (doc->dirty())
                return fail(ErrorCode::Busy, "Save open documents before changing engine assets");
        return {};
    };
    const auto current = state->current;
    state->request = [self, current](std::string method, lang::Json params) -> Result<lang::Json> {
        if (auto valid = current(); !valid)
            return std::unexpected(valid.error());
        struct Reply {
            std::mutex mutex;
            std::condition_variable ready;
            std::optional<Result<lang::Json>> value;
        };
        auto reply = std::make_shared<Reply>();
        auto sent = self->m_impl->send(
            std::move(method), std::move(params),
            [reply](Result<lang::Json> value) {
                std::scoped_lock lock(reply->mutex);
                reply->value = std::move(value);
                reply->ready.notify_one();
            },
            true);
        if (!sent)
            return std::unexpected(sent.error());
        std::unique_lock lock(reply->mutex);
        const bool completed =
            reply->ready.wait_for(lock, std::chrono::seconds(30), [&] { return reply->value.has_value(); });
        if (auto valid = current(); !valid)
            return std::unexpected(valid.error());
        if (!completed)
            return fail(ErrorCode::Timeout,
                        "Engine move request outcome is unknown; inspect the paired editor lease before retrying");
        return std::move(*reply->value);
    };
    state->refresh = [self] {
        if (self) {
            self->m_impl->clearAssetUsages();
            self->m_impl->refreshProjectContent();
        }
    };
    auto raw =
        state->request("assets.plan-move",
                       lang::Json::object().set("path", old->projectAssetPath).set("target", target->projectAssetPath));
    if (!raw)
        return std::unexpected(raw.error());
    state->lease = raw->stringAt("lease"); // Retain cancellation even if the typed response is refused.
    auto parsed = engine::parseAssetMovePlan(*raw, old->projectAssetPath, target->projectAssetPath, handle);
    if (!parsed)
        return std::unexpected(parsed.error());
    state->original = *parsed;
    state->activePath = parsed->path;
    state->activeSource = parsed->oldPath;
    // Every virtual identity is mapped through the physical project before it becomes disk authority.
    std::map<std::string, std::string> physical;
    auto pathFor = [fs, root, physicalRoot, &physical](const std::string& relative,
                                                       bool absent = false) -> Result<std::string> {
        auto path = io::joinPath(root, relative);
        if (!path)
            return std::unexpected(path.error());
        auto actual = fs->physicalPath(absent ? std::string(io::parentPath(*path)) : *path);
        if (!actual)
            return std::unexpected(actual.error());
        auto key = lang::documentKey(*actual), prefix = lang::documentKey(physicalRoot);
        if (key != prefix &&
            !key.starts_with(prefix + std::string(1, io::kNativePathStyle == io::PathStyle::Windows ? '\\' : '/')))
            return fail(ErrorCode::PermissionDenied, "Engine move input physically escapes the paired project");
        physical.emplace(absent ? std::string(io::parentPath(*path)) : *path, *actual);
        return *path;
    };
    for (const auto& guard : parsed->guards) {
        auto path = pathFor(guard.path);
        if (!path)
            return std::unexpected(path.error());
        auto input = captureRefactoringInput(*fs, *path);
        if (!input)
            return std::unexpected(input.error());
        if (!input->disk || input->disk->size() != guard.size || engine::assetMoveHash(*input->disk) != guard.hash)
            return fail(ErrorCode::Busy, "Engine move input changed before preview: " + guard.path);
        if (auto added = appendRefactoringDependency(plan, *input); !added)
            return added;
    }
    for (const auto& move : parsed->moves) {
        auto from = pathFor(move.from), to = pathFor(move.to, true);
        if (!from)
            return std::unexpected(from.error());
        if (!to)
            return std::unexpected(to.error());
        if (std::ranges::none_of(plan.renames, [&](const auto& r) {
                return lang::documentKey(r.from) == lang::documentKey(*from) &&
                       lang::documentKey(r.to) == lang::documentKey(*to);
            }))
            return fail(ErrorCode::Corrupt, "Engine proposal disagrees with the native asset/sidecar move");
    }
    for (const auto& edit : parsed->files) {
        auto path = pathFor(edit.path);
        if (!path)
            return std::unexpected(path.error());
        auto input = captureRefactoringInput(*fs, *path);
        if (!input)
            return std::unexpected(input.error());
        if (auto added = appendRefactoringReplacement(plan, *input, edit.before, edit.after); !added)
            return added;
    }
    // Refuse dirty editor overlays rather than publishing disk-only guesses into engine documents.
    for (const auto& edit : plan.disk)
        for (const auto& doc : impl.workbench.m_documents->documents())
            if (lang::documentKey(doc->path()) == lang::documentKey(edit.path))
                return fail(ErrorCode::Busy, "Close referencing documents before moving an engine asset");
    const auto prior = plan.revalidateContext;
    plan.revalidateContext = [prior, state, fs, physical]() -> Result<void> {
        if (prior) {
            if (auto valid = prior(); !valid)
                return valid;
        }
        for (const auto& [path, expected] : physical) {
            auto actual = fs->physicalPath(path);
            if (!actual || lang::documentKey(*actual) != lang::documentKey(expected))
                return fail(ErrorCode::Busy, "Physical engine move input changed");
        }
        return state->validate();
    };
    auto external = std::make_shared<document::RefactoringExternalGuard>();
    external->cancelled = [state] { state->cancel(); };
    external->admit = [state] { return state->validate(); };
    auto finish = [state](bool applied) -> Result<void> {
        state->nativeSettled = true;
        state->applied = applied;
        if (state->lease.empty())
            return fail(ErrorCode::Busy, "Native files settled, but the engine lease is unavailable");
        auto reply = state->request("assets.finish-move-plan",
                                    lang::Json::object().set("lease", state->lease).set("applied", true));
        state->refresh();
        if (!reply)
            return std::unexpected(
                reply.error().withContext("Native transaction settled; engine validation/refresh not confirmed"));
        if (reply->boolAt("verified") != true || reply->boolAt("refreshed") != true ||
            reply->boolAt("applied") != true || reply->stringAt("lease") != state->lease ||
            reply->stringAt("receipt") != state->original.receipt || reply->stringAt("path") != state->activePath ||
            engine::engineHandleAt(*reply, "handle") != std::optional(state->original.handle))
            return fail(ErrorCode::Corrupt, "Native transaction settled; engine decision receipt is incomplete");
        state->lease.clear();
        return {};
    };
    external->settled = finish;
    external->history = [state, finish](bool redo) -> Result<void> {
        if (!state->lease.empty()) {
            if (!state->nativeSettled)
                return fail(ErrorCode::Busy, "Settle the retained engine lease before changing history");
            if (auto settled = finish(state->applied); !settled)
                return settled;
        }
        auto raw = state->request("assets.reopen-move-plan",
                                  lang::Json::object().set("receipt", state->original.receipt).set("reverse", !redo));
        if (!raw)
            return std::unexpected(raw.error());
        state->nativeSettled = false;
        state->lease = raw->stringAt("lease");
        const auto& original = state->original;
        auto parsed = engine::parseAssetMovePlan(*raw, redo ? original.oldPath : original.path,
                                                 redo ? original.path : original.oldPath, original.handle);
        if (!parsed) {
            state->cancel();
            return std::unexpected(parsed.error());
        }
        if (parsed->receipt != original.receipt || parsed->files.size() != original.files.size()) {
            state->cancel();
            return fail(ErrorCode::Corrupt, "Engine history receipt changed");
        }
        for (const auto& edit : original.files) {
            auto found = std::ranges::find_if(parsed->files, [&](const auto& f) { return f.path == edit.path; });
            if (found == parsed->files.end() || found->before != (redo ? edit.before : edit.after) ||
                found->after != (redo ? edit.after : edit.before)) {
                state->cancel();
                return fail(ErrorCode::Corrupt, "Engine inverse receipt does not restore exact native bytes");
            }
        }
        state->activePath = parsed->path;
        state->activeSource = parsed->oldPath;
        if (auto valid = state->validate(); !valid) {
            state->cancel();
            return valid;
        }
        return {};
    };
    plan.external = std::move(external);
    QDialog preview;
    QObject::connect(this, &QObject::destroyed, &preview, &QDialog::reject);
    preview.setWindowTitle(QStringLiteral("Move engine asset — one transaction"));
    preview.setWindowModality(Qt::ApplicationModal);
    preview.resize(850, 650);
    auto* layout = new QVBoxLayout(&preview);
    layout->addWidget(new QLabel(
        QStringLiteral(
            "Asset, metadata and actual engine reference edits. Global Undo restores the retained before images."),
        &preview));
    auto* text = new QPlainTextEdit(&preview);
    text->setReadOnly(true);
    std::string shown;
    for (const auto& move : plan.renames)
        shown += move.from + " -> " + move.to + "\n";
    for (const auto& edit : plan.disk) {
        if (edit.expectedBytes.size() + edit.afterBytes.size() + shown.size() > 8 * 1024 * 1024)
            return fail(ErrorCode::Unsupported, "Engine move preview exceeds its display limit; no files changed");
        shown += "\n" + edit.path + "\nBEFORE\n" + edit.expectedBytes + "\nAFTER\n" + edit.afterBytes + "\n";
    }
    text->setPlainText(q(shown));
    layout->addWidget(text);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &preview);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &preview, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &preview, &QDialog::reject);
    layout->addWidget(buttons);
    if (preview.exec() != QDialog::Accepted) {
        state->cancel();
        return fail(ErrorCode::Cancelled, "Engine asset move declined; no files changed");
    }
    if (!self)
        return fail(ErrorCode::Cancelled, "Engine owner closed during asset preview");
    return plan.revalidateContext();
}
} // namespace frost::app
