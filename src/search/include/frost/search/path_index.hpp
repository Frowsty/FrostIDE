// SPDX-License-Identifier: Apache-2.0
// Immutable posting index for fuzzy file search; source paths remain owned by the project snapshot.
#pragma once
#include "frost/search/fuzzy_path.hpp"

#include <array>
#include <stop_token>
namespace frost::search {
class PathIndex {
public:
    // Builds distinct folded-byte postings in path order. Cancelled construction is discarded.
    // O(total path bytes), immutable/thread-safe after publication with the matching path snapshot.
    bool build(std::span<const std::string> paths, const std::stop_token& stop = {});
    // Intersects required-byte postings, then uses the existing exact fuzzy score. Retains only
    // `limit` ranked hits, O(candidates * score + candidates log limit), cancellable between paths.
    // Empty input follows snapshot order. Queries over 256 bytes or zero limit return no hits.
    [[nodiscard]] std::vector<RankedPath> query(std::string_view pattern, std::span<const std::string> paths,
                                                std::size_t limit, const std::stop_token& stop = {}) const;

private:
    std::array<std::vector<std::size_t>, 256> m_postings;
    std::size_t m_count = 0;
};
} // namespace frost::search
