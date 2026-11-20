// SPDX-License-Identifier: Apache-2.0
// Length-delimited parsing keeps path bytes separate from the stage/object header.
#include "frost/vcs/conflict_sources.hpp"

#include <algorithm>
namespace frost::vcs {
Result<std::array<std::optional<ConflictStage>, 3>> parseConflictStages(std::string_view bytes,
                                                                        std::string_view relative) {
    if (bytes.empty() || bytes.size() > 1024 * 1024 || relative.empty() || relative.find('\0') != relative.npos)
        return fail(ErrorCode::Corrupt, "Missing or oversized actual conflict stage records");
    std::array<std::optional<ConflictStage>, 3> result{};
    std::size_t records = 0;
    for (std::size_t start = 0; start < bytes.size();) {
        const auto end = bytes.find('\0', start);
        if (end == bytes.npos || ++records > 3)
            return fail(ErrorCode::Corrupt, "Malformed conflict stage record framing");
        const auto record = bytes.substr(start, end - start);
        const auto tab = record.find('\t');
        if (tab == record.npos || record.substr(tab + 1) != relative)
            return fail(ErrorCode::Corrupt, "Conflict stage names another file");
        const auto header = record.substr(0, tab);
        const auto first = header.find(' '), second = first == header.npos ? header.npos : header.find(' ', first + 1);
        if (first == header.npos || second == header.npos || header.find(' ', second + 1) != header.npos)
            return fail(ErrorCode::Corrupt, "Malformed conflict stage fields");
        const auto mode = header.substr(0, first), object = header.substr(first + 1, second - first - 1),
                   stage = header.substr(second + 1);
        if ((mode != "100644" && mode != "100755") || (object.size() != 40 && object.size() != 64) ||
            !std::ranges::all_of(object, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }) ||
            stage.size() != 1 || stage[0] < '1' || stage[0] > '3')
            return fail(ErrorCode::Unsupported,
                        "Semantic conflict sources require regular files and exact Git object/stage identities");
        const auto index = static_cast<std::size_t>(stage[0] - '1');
        if (result[index])
            return fail(ErrorCode::Corrupt, "Duplicate conflict stage identity");
        result[index] = ConflictStage{std::string(object), std::string(mode)};
        start = end + 1;
    }
    return result;
}
} // namespace frost::vcs
