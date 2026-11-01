// SPDX-License-Identifier: Apache-2.0
//
// The input_keymap fuzz core (keymap_fuzz_core.hpp).

#include "input/fuzz/keymap_fuzz_core.hpp"

#include "frost/input/key.hpp"
#include "frost/input/keymap.hpp"

#include <format>
#include <string>
#include <string_view>

namespace frost::input::test {

namespace {

// Keeps the property checks bounded for huge inputs (the parser itself rejects > 1 MiB).
constexpr std::size_t kMaxLineChecks = 4096;

Result<void> checkSequences(std::string_view text) {
    std::size_t start = 0;
    std::size_t checked = 0;
    while (start <= text.size() && checked < kMaxLineChecks) {
        const std::size_t end = std::min(text.find('\n', start), text.size());
        const std::string_view line = text.substr(start, end - start);
        ++checked;
        static_cast<void>(parseSequence(line, SyntaxMode::Lenient));
        if (const auto strict = parseSequence(line, SyntaxMode::Strict)) {
            const std::string formatted = formatSequence(*strict);
            const auto again = parseSequence(formatted, SyntaxMode::Strict);
            if (!again || *again != *strict) {
                return fail(ErrorCode::Internal, std::format("Strict round trip failed for '{}'", formatted));
            }
        }
        if (const auto lenient = parseSequence(line, SyntaxMode::Lenient); lenient && lenient->valid()) {
            // Lenient results format to something Lenient reads back identically.
            const std::string formatted = formatSequence(*lenient);
            const auto again = parseSequence(formatted, SyntaxMode::Lenient);
            if (!again || *again != *lenient) {
                return fail(ErrorCode::Internal, std::format("Lenient round trip failed for '{}'", formatted));
            }
        }
        start = end + 1;
    }
    return {};
}

} // namespace

Result<void> runKeymapFuzzInput(std::span<const std::uint8_t> data) {
    const std::string_view text(reinterpret_cast<const char*>(data.data()), data.size());
    if (auto keymap = parseKeymap(text)) {
        const std::string formatted = formatKeymap(*keymap);
        auto reparsed = parseKeymap(formatted);
        if (!reparsed) {
            return fail(ErrorCode::Internal,
                        std::format("formatted keymap does not parse: {}", reparsed.error().message()));
        }
        if (*reparsed != *keymap) {
            return fail(ErrorCode::Internal, "parse(format(k)) != k");
        }
        if (formatKeymap(*reparsed) != formatted) {
            return fail(ErrorCode::Internal, "formatKeymap is not a fixpoint");
        }
        for (const KeymapPlatform platform : {KeymapPlatform::Windows, KeymapPlatform::Linux, KeymapPlatform::MacOS}) {
            static_cast<void>(keymap->bindings(platform));
        }
    }
    return checkSequences(text);
}

} // namespace frost::input::test
