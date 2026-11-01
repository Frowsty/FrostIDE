// SPDX-License-Identifier: Apache-2.0
//
// buildLayoutTable: turns the answers of an ILayoutQuery into a FullTable LayoutTable
// (layout.hpp; ADR 0016 decision 12). OS-independent, so the reader logic is tested with
// a scripted query; the Win32 query lives in layout_reader_win.cpp.

#include "frost/input/layout.hpp"

#include <array>
#include <string>
#include <utility>
#include <vector>

namespace frost::input {

namespace {

// A character a table can hold: not a control character, surrogate or out of range.
bool storable(char32_t c) noexcept {
    return c >= 0x20 && c != 0x7F && !(c >= 0x80 && c <= 0x9F) && !(c >= 0xD800 && c <= 0xDFFF) && c <= 0x10FFFF;
}

} // namespace

Result<LayoutTable> buildLayoutTable(const ILayoutQuery& query) {
    std::vector<std::pair<Code, KeyLevels>> keys;
    keys.reserve(kWritingKeyCount);
    for (std::size_t i = index(Code::KeyA); i <= index(Code::IntlYen); ++i) {
        const auto code = static_cast<Code>(i);
        const std::uint16_t scan = windowsScanCodeFor(code);
        const std::uint8_t vk = query.vkFromScan(scan);
        if (vk == 0) {
            continue;
        }
        KeyLevels levels;
        for (unsigned l = 0; l < 4; ++l) {
            const auto level = static_cast<Level>(l);
            std::array<char32_t, 4> out{};
            const int n = query.toUnicode(vk, scan, level, out);
            if (n == 0 || n < -1 || !storable(out[0])) {
                continue;
            }
            const auto bit = static_cast<std::uint8_t>(1U << l);
            char32_t& slot = level == Level::Base    ? levels.base
                             : level == Level::Shift ? levels.shift
                             : level == Level::AltGr ? levels.altGr
                                                     : levels.altGrShift;
            slot = out[0];
            if (n == -1) {
                levels.deadMask = static_cast<std::uint8_t>(levels.deadMask | bit);
            } else if (n > 1) {
                levels.ligatureMask = static_cast<std::uint8_t>(levels.ligatureMask | bit);
            }
        }
        if (levels.base != 0 || levels.shift != 0 || levels.altGr != 0 || levels.altGrShift != 0) {
            keys.emplace_back(code, levels);
        }
    }

    // "<locale>:<KLID>", cut to fit the 23-byte id.
    std::string klid(query.klid().substr(0, 22));
    std::string locale(query.localeName());
    const std::size_t room = 23 - 1 - klid.size();
    if (locale.size() > room) {
        locale.resize(room);
    }
    const std::string id = locale + ":" + klid;
    return LayoutTable::make(id, MappingMode::FullTable, keys, {});
}

} // namespace frost::input
