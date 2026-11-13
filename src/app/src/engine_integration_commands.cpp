// SPDX-License-Identifier: Apache-2.0
// Preserve explicit script validation and local request cancellation on persistent sessions.
#include "frost/io/path.hpp"
#include "frost/lang/uri.hpp"

#include "engine_integration_internal.hpp"
#include <QDir>
#include <QMetaObject>

namespace frost::app {
namespace {
QString qs(std::string_view value) { return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size())); }
} // namespace

Result<void> EngineIntegration::cancelRequest() {
    auto& impl = *m_impl;
    if (!impl.session || impl.requests.empty())
        return fail(ErrorCode::NotFound, "No engine request is pending");
    impl.session->cancel(impl.requests.rbegin()->first);
    impl.setStatus(QStringLiteral("Cancelling the local request; an engine action already sent may continue"));
    return {};
}

Result<void> EngineIntegration::validateScript() {
    auto& impl = *m_impl;
    if (!impl.admitted(impl.epoch, impl.root, impl.trustRevision))
        return fail(ErrorCode::Untrusted, "Trust the project before validating its scripts");
    if (!impl.session || !impl.identity || !impl.session->connected())
        return fail(ErrorCode::Busy, "Pair an editor before validating a script");
    auto* view = impl.workbench.currentView();
    if (!view || !qs(view->path()).endsWith(QStringLiteral(".lua"), Qt::CaseInsensitive))
        return fail(ErrorCode::InvalidArgument, "Open a Lua script to validate");
    const std::string path = view->path();
    const text::Snapshot source = view->document().document().text();
    const auto currentEpoch = impl.epoch;
    const auto currentSessionEpoch = impl.sessionEpoch;
    const auto expectedRoot = impl.root;
    const auto originalPhysicalRoot = impl.physicalRoot;
    const auto expectedProjectKey = impl.projectKey;
    const auto admission = impl.physicalAdmission;
    const auto revision = impl.trustRevision;
    const auto stop = impl.workStop.get_token();
    auto* fs = &impl.workbench.m_services.fs;
    QPointer<EngineIntegration> self(this);
    impl.setStatus(QStringLiteral("Validating the current script buffer..."));
    impl.background([self, fs, currentEpoch, currentSessionEpoch, expectedRoot, originalPhysicalRoot,
                     expectedProjectKey, admission, revision, stop, path, source] {
        if (stop.stop_requested() || !retainEngineProjectIdentity(*fs, expectedRoot, expectedProjectKey, admission))
            return;
        auto actual = fs->physicalPath(path);
        Result<lang::Json> params =
            fail(ErrorCode::InvalidArgument, "The script is outside this project's Assets folder");
        if (actual) {
            const QString relative = QDir(qs(originalPhysicalRoot)).relativeFilePath(qs(*actual));
            const QString portable = QDir::fromNativeSeparators(relative);
            auto status = fs->status(*actual);
            if (status && status->type == io::FileType::Regular && portable.startsWith(QStringLiteral("Assets/")) &&
                !portable.split('/').contains(QStringLiteral("..")) &&
                portable.endsWith(QStringLiteral(".lua"), Qt::CaseInsensitive))
                params = lang::Json::object().set("path", portable.toUtf8().toStdString()).set("source", source.text());
        }
        if (!self || stop.stop_requested())
            return;
        QMetaObject::invokeMethod(
            self.data(),
            [self, currentEpoch, currentSessionEpoch, expectedRoot, revision, params = std::move(params)]() mutable {
                if (!self || self->m_impl->sessionEpoch != currentSessionEpoch ||
                    !self->m_impl->admitted(currentEpoch, expectedRoot, revision))
                    return;
                if (!params) {
                    self->m_impl->setStatus(qs(params.error().message()));
                    return;
                }
                if (auto sent = self->command("script.validate", std::move(*params)); !sent)
                    self->m_impl->setStatus(qs(sent.error().message()));
            },
            Qt::QueuedConnection);
    });
    return {};
}

} // namespace frost::app
