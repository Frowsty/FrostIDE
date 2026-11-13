// SPDX-License-Identifier: Apache-2.0
// Pure structural checks for proposed file plans; no filesystem calls or transaction effects.
#include "frost/io/file_transaction_plan.hpp"

#include "frost/io/utf.hpp"

#include <initializer_list>
#include <string_view>

namespace frost::io {
namespace {
bool textField(std::string_view text) {
    return !text.empty() && text.find('\0') == std::string_view::npos && isValidUtf8(text);
}
bool identity(const FilePlanObjectIdentity& value) { return textField(value.volume) && textField(value.object); }
Result<void> image(const FilePlanImage& value) {
    if (!value.bytesDigest || !value.metadataDigest || !textField(value.metadataProfile))
        return fail(ErrorCode::InvalidArgument, "File image needs bytes and metadata declarations");
    return {};
}
Result<void> endpoint(const FilePlanEndpoint& value, PathStyle style) {
    if (!identity(value.parent) || !textField(value.leaf) || !textField(value.lookupKey) || value.leaf == "." ||
        value.leaf == ".." || value.leaf.find('/') != std::string::npos ||
        (style == PathStyle::Windows && value.leaf.find('\\') != std::string::npos) ||
        (value.caseRule != FilePlanCaseRule::Sensitive && value.caseRule != FilePlanCaseRule::Insensitive))
        return fail(ErrorCode::InvalidArgument, "Malformed file endpoint declaration");
    const auto parent = normalizePath(value.parentPath, style);
    if (!parent || *parent != value.parentPath || !isValidUtf8(value.parentPath))
        return fail(ErrorCode::InvalidArgument, "Endpoint parent must be absolute and normalized");
    const auto path = joinPath(value.parentPath, value.leaf, style);
    if (!path || fileName(*path, style) != value.leaf)
        return fail(ErrorCode::InvalidArgument, "Endpoint leaf is not an exact path component");
    const auto normalized = normalizePath(*path, style);
    if (!normalized || *normalized != *path)
        return fail(ErrorCode::InvalidArgument, "Full endpoint spelling must satisfy the selected path rules");
    return {};
}
Result<void> existing(const FilePlanExisting& value, PathStyle style) {
    if (auto valid = endpoint(value.endpoint, style); !valid)
        return valid;
    if (!identity(value.object) || value.object.volume != value.endpoint.parent.volume ||
        value.object == value.endpoint.parent)
        return fail(ErrorCode::InvalidArgument, "Existing file identity disagrees with its parent");
    return image(value.image);
}
bool consistentParents(const FilePlanEndpoint& a, const FilePlanEndpoint& b) {
    return !(a.parent == b.parent && a.caseRule != b.caseRule) &&
           !(a.parentPath == b.parentPath && a.parent != b.parent);
}
bool sameNamespace(const FilePlanEndpoint& a, const FilePlanEndpoint& b) {
    return (a.parent == b.parent && (a.lookupKey == b.lookupKey || a.leaf == b.leaf)) ||
           (a.parentPath == b.parentPath && a.leaf == b.leaf);
}
struct View {
    const FilePlanEndpoint* first;
    const FilePlanEndpoint* second = nullptr;
    const FilePlanExisting* original = nullptr;
};
Result<View> operation(const FilePlanOperation& value, PathStyle style) {
    if (const auto* creation = std::get_if<FilePlanCreate>(&value.effect)) {
        if (auto valid = endpoint(creation->before.endpoint, style); !valid)
            return std::unexpected(valid.error());
        if (auto valid = image(creation->after); !valid)
            return std::unexpected(valid.error());
        return View{&creation->before.endpoint};
    }
    const FilePlanExisting* before = nullptr;
    if (const auto* replacement = std::get_if<FilePlanReplace>(&value.effect)) {
        before = &replacement->before;
        if (auto valid = image(replacement->after); !valid)
            return std::unexpected(valid.error());
    } else if (const auto* deletion = std::get_if<FilePlanDelete>(&value.effect)) {
        before = &deletion->before;
    } else if (const auto* renaming = std::get_if<FilePlanRename>(&value.effect)) {
        before = &renaming->before;
    }
    if (!before)
        return fail(ErrorCode::InvalidArgument, "Missing file operation variant");
    if (auto valid = existing(*before, style); !valid)
        return std::unexpected(valid.error());
    View view{&before->endpoint, nullptr, before};
    const auto* rename = std::get_if<FilePlanRename>(&value.effect);
    if (!rename)
        return view;
    if (const auto* absent = std::get_if<FilePlanAbsent>(&rename->destination)) {
        view.second = &absent->endpoint;
        if (sameNamespace(*view.first, *view.second))
            return fail(ErrorCode::InvalidArgument, "Rename destination cannot declare the source absent");
    } else if (const auto* spelling = std::get_if<FilePlanCaseOnly>(&rename->destination)) {
        view.second = &spelling->endpoint;
        if (spelling->expectedSource != before->object || view.first->parent != view.second->parent ||
            view.first->lookupKey != view.second->lookupKey || view.first->leaf == view.second->leaf ||
            view.first->caseRule != FilePlanCaseRule::Insensitive ||
            view.second->caseRule != FilePlanCaseRule::Insensitive)
            return fail(ErrorCode::InvalidArgument, "Case-only destination must declare the same source and lookup");
    } else {
        return fail(ErrorCode::InvalidArgument, "Missing rename destination variant");
    }
    if (auto valid = endpoint(*view.second, style); !valid)
        return std::unexpected(valid.error());
    if (!consistentParents(*view.first, *view.second))
        return fail(ErrorCode::InvalidArgument, "Rename has contradictory parent identity or case declarations");
    if (view.first->parent.volume != view.second->parent.volume)
        return fail(ErrorCode::Unsupported, "Cross-volume rename is outside this plan profile");
    return view;
}
} // namespace

Result<void> validateFileTransactionPlan(const FileTransactionPlan& plan) {
    if (!plan.id.assigned() || plan.operations.empty() ||
        (plan.pathStyle != PathStyle::Posix && plan.pathStyle != PathStyle::Windows))
        return fail(ErrorCode::InvalidArgument, "File plan needs an identity, operations and a path style");
    std::vector<View> views;
    views.reserve(plan.operations.size());
    for (std::size_t i = 0; i < plan.operations.size(); ++i) {
        const auto& current = plan.operations[i];
        if (!current.id.assigned() || !current.artifact.assigned())
            return fail(ErrorCode::InvalidArgument, "File operation needs assigned operation and artifact identities");
        auto view = operation(current, plan.pathStyle);
        if (!view)
            return std::unexpected(view.error());
        for (std::size_t j = 0; j < i; ++j) {
            const auto& previous = plan.operations[j];
            if (current.id == previous.id || current.artifact == previous.artifact)
                return fail(ErrorCode::InvalidArgument, "Duplicate operation or competing artifact writer");
            if (view->original && views[j].original && view->original->object == views[j].original->object)
                return fail(ErrorCode::InvalidArgument, "Competing writers declare the same physical file");
            for (const auto* a : {view->first, view->second})
                for (const auto* b : {views[j].first, views[j].second})
                    if (a && b) {
                        if (!consistentParents(*a, *b))
                            return fail(ErrorCode::InvalidArgument,
                                        "Operations have contradictory parent identity or case declarations");
                        if (sameNamespace(*a, *b))
                            return fail(ErrorCode::InvalidArgument,
                                        "Operation endpoint collision requires a composed plan");
                    }
        }
        views.push_back(*view);
    }
    return {};
}

FilePlanRecovery classifyFilePlanRecovery(FilePlanDecisionEvidence evidence) noexcept {
    switch (evidence.decision) {
    case FilePlanDecision::Unknown:
        return evidence.participantVerifiedCommit ? FilePlanRecovery::CommittedForward : FilePlanRecovery::Unresolved;
    case FilePlanDecision::VerifiedCommit: return FilePlanRecovery::CommittedForward;
    case FilePlanDecision::ProvenNoCommit:
    case FilePlanDecision::VerifiedAbort:
        return evidence.managedEffectsObserved || evidence.participantVerifiedCommit
                   ? FilePlanRecovery::Conflict
                   : FilePlanRecovery::UncommittedPreparation;
    }
    return FilePlanRecovery::Conflict;
}
} // namespace frost::io
