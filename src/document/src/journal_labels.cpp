// SPDX-License-Identifier: Apache-2.0
//
// The hot-exit journal's encoding labels (journal_labels.hpp): writing a DocumentEncoding into
// DocumentState's label fields and reading it back with full validation.

#include "journal_labels.hpp"

#include "frost/text/encoding.hpp"
#include "frost/text/encoding_detection.hpp"
#include "frost/text/line_separator.hpp"

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string_view>
#include <system_error>

namespace frost::document::detail {
namespace {

constexpr std::size_t kFields = 9;

template<class T>
std::optional<T> parseNumber(std::string_view digits) noexcept {
    if (digits.empty()) {
        return std::nullopt;
    }
    T value{};
    const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), value);
    if (error != std::errc{} || end != digits.data() + digits.size()) {
        return std::nullopt;
    }
    return value;
}

std::string offsetField(const std::optional<std::uint64_t>& offset) {
    return offset ? std::format("{}", *offset) : std::string();
}

// Splits `label` at '|' into exactly kFields fields.
std::optional<std::array<std::string_view, kFields>> split(std::string_view label) noexcept {
    std::array<std::string_view, kFields> fields{};
    for (std::size_t i = 0; i < kFields; ++i) {
        const std::size_t bar = label.find('|');
        if (i + 1 == kFields) {
            if (bar != std::string_view::npos) {
                return std::nullopt;
            }
            fields[i] = label;
        } else {
            if (bar == std::string_view::npos) {
                return std::nullopt;
            }
            fields[i] = label.substr(0, bar);
            label.remove_prefix(bar + 1);
        }
    }
    return fields;
}

std::optional<text::DocumentEncoding> parse(const storage::DocumentState& state) {
    const auto fields = split(state.encoding);
    if (!fields) {
        return std::nullopt;
    }
    const auto& f = *fields;
    const auto encoding = text::findEncoding(f[0]);
    const auto loss = text::findLossKind(f[1]);
    const auto replacements = parseNumber<std::uint64_t>(f[2]);
    const auto detected = text::findEncoding(f[5]);
    const auto confidence = parseNumber<std::uint8_t>(f[7]);
    const auto source = text::findDetectionSource(f[8]);
    const auto separator = text::findSeparator(state.lineEnding);
    if (!encoding || !loss || !replacements || !detected || (f[6] != "0" && f[6] != "1") || !confidence ||
        *confidence > 100 || !source || !separator) {
        return std::nullopt;
    }
    text::DocumentEncoding result;
    result.format = text::TextFileFormat{*encoding, state.hasBom, *separator};
    if (!result.format.isValid()) {
        return std::nullopt;
    }
    result.fidelity.loss = *loss;
    result.fidelity.replacements = *replacements;
    if (!f[3].empty()) {
        const auto offset = parseNumber<std::uint64_t>(f[3]);
        if (!offset) {
            return std::nullopt;
        }
        result.fidelity.firstLossFileOffset = *offset;
    }
    if (!f[4].empty()) {
        const auto offset = parseNumber<std::uint64_t>(f[4]);
        if (!offset) {
            return std::nullopt;
        }
        result.fidelity.firstLossTextOffset = ByteOffset{*offset};
    }
    result.detection = text::EncodingGuess{*detected, f[6] == "1", *confidence, *source};
    return result;
}

} // namespace

void encodeJournalLabels(const text::DocumentEncoding& encoding, storage::DocumentState& state) {
    const auto& fidelity = encoding.fidelity;
    const auto& detection = encoding.detection;
    state.encoding = std::format(
        "{}|{}|{}|{}|{}|{}|{}|{}|{}", text::encodingName(encoding.format.encoding), text::lossKindName(fidelity.loss),
        fidelity.replacements, offsetField(fidelity.firstLossFileOffset),
        fidelity.firstLossTextOffset ? std::format("{}", fidelity.firstLossTextOffset->value()) : std::string(),
        text::encodingName(detection.encoding), detection.bom ? "1" : "0", detection.confidence,
        text::detectionSourceName(detection.source));
    state.hasBom = encoding.format.bom;
    state.lineEnding = std::string(text::separatorName(encoding.format.lineSeparator));
}

text::DocumentEncoding decodeJournalLabels(const storage::DocumentState& state) {
    if (auto parsed = parse(state)) {
        return *parsed;
    }
    // Conservative (ADR 0018 D18): the text is UTF-8; saving it needs consent.
    text::DocumentEncoding fallback;
    fallback.format = text::TextFileFormat{text::Encoding::utf8(), false,
                                           text::findSeparator(state.lineEnding).value_or(text::LineSeparator::Lf)};
    fallback.fidelity.loss = text::LossKind::Replaced;
    fallback.detection = text::EncodingGuess{text::Encoding::utf8(), false, 0, text::DetectionSource::Lossy};
    return fallback;
}

} // namespace frost::document::detail
