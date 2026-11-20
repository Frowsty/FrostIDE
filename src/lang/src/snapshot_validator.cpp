// SPDX-License-Identifier: Apache-2.0
// Encodes captured text and dispatches the worker's strictly parsed request.
#include "frost/lang/cmake_refactoring.hpp"
#include "frost/lang/json_schema.hpp"
#include "frost/text/text_buffer.hpp"

#include "snapshot_validator_protocol.hpp"

namespace frost::lang {
namespace {
constexpr auto kInvalidRequest = "Invalid snapshot validation request";

Result<void> checkInput(std::string_view path, std::size_t sourceSize, const Json* schema) {
    if (path.empty() || path.size() > kSnapshotValidationMaxPathBytes || path.find('\0') != path.npos)
        return fail(ErrorCode::InvalidArgument, "Invalid snapshot validation path");
    const bool cmake = isCMakeSource(path);
    if ((schema && (cmake || !schema->isObject())) || (!cmake && !schema && !hasBundledJsonSchema(path)))
        return fail(ErrorCode::Unsupported, "Unsupported snapshot validation profile");
    const std::size_t limit = cmake ? CMakeLanguageService::kMaxInputBytes : kSnapshotValidationMaxJsonBytes;
    if (sourceSize > limit)
        return fail(ErrorCode::OutOfRange, "Snapshot exceeds the source validation budget");
    return {};
}
} // namespace

Result<std::string> makeSnapshotValidationRequest(std::string_view path, const text::Snapshot& source,
                                                  const std::optional<Json>& engineSchema) {
    if (auto checked = checkInput(path, source.size().value(), engineSchema ? &*engineSchema : nullptr); !checked)
        return std::unexpected(checked.error());
    auto envelope =
        Json::object().set("version", kSnapshotValidationVersion).set("path", path).set("text", source.text());
    if (engineSchema) {
        if (engineSchema->dump().size() > kSnapshotValidationMaxJsonBytes)
            return fail(ErrorCode::OutOfRange, "Engine schema exceeds the validation budget");
        envelope.set("engineSchema", *engineSchema);
    }
    return envelope.dump();
}

Result<std::string> validateSnapshotRequest(std::string_view request) {
    if (request.size() > kSnapshotValidationMaxInputBytes)
        return fail(ErrorCode::OutOfRange, "Snapshot validation request exceeds the protocol budget");
    auto envelope = rpc::parseJsonStrict(request);
    if (!envelope || !envelope->isObject())
        return fail(ErrorCode::Corrupt, kInvalidRequest);
    const auto* version = envelope->find("version");
    if (!version || version->kind() != Json::Kind::Integer || version->asInteger() != kSnapshotValidationVersion)
        return fail(ErrorCode::Unsupported, "Unsupported snapshot validation protocol version");
    const auto* path = envelope->find("path");
    const auto* source = envelope->find("text");
    const auto* schema = envelope->find("engineSchema");
    if (!path || !path->isString() || !source || !source->isString() || envelope->size() != (schema ? 4U : 3U))
        return fail(ErrorCode::Corrupt, kInvalidRequest);
    if (auto checked = checkInput(*path->asString(), source->asString()->size(), schema); !checked)
        return std::unexpected(checked.error());
    if (schema && schema->dump().size() > kSnapshotValidationMaxJsonBytes)
        return fail(ErrorCode::OutOfRange, "Engine schema exceeds the validation budget");
    const auto buffer = text::TextBuffer::fromUtf8(*source->asString());
    if (!buffer)
        return std::unexpected(buffer.error());
    const auto snapshot = buffer->snapshot();
    auto issues = isCMakeSource(*path->asString()) ? inspectCMakeSource(snapshot)
                                                   : inspectJsonSnapshot(*path->asString(), snapshot, schema);
    if (!issues)
        return std::unexpected(issues.error());

    std::string response = "{\"version\":" + std::to_string(kSnapshotValidationVersion) + ",\"issues\":[";
    std::string_view separator;
    for (const auto& issue : *issues) {
        auto encoded = Json::object()
                           .set("source", issue.source)
                           .set("code", issue.code)
                           .set("message", issue.message)
                           .set("begin", issue.range.begin.value())
                           .set("end", issue.range.end.value())
                           .dump();
        if (response.size() + separator.size() + encoded.size() + 2 > kSnapshotValidationMaxOutputBytes)
            return fail(ErrorCode::OutOfRange, "Snapshot validation result exceeds the protocol budget");
        response += separator;
        response += encoded;
        separator = ",";
    }
    response += "]}";
    return response;
}
} // namespace frost::lang
