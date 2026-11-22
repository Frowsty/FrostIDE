// SPDX-License-Identifier: Apache-2.0
// Byte postings preserve fuzzy subsequence semantics; no guessed symbols or filesystem reads.
#include "frost/search/path_index.hpp"

#include <algorithm>
namespace frost::search {
namespace {
unsigned char fold(unsigned char c) {
    if (c >= 'A' && c <= 'Z')
        return static_cast<unsigned char>(c + ('a' - 'A'));
    return c == '\\' ? '/' : c;
}
} // namespace
bool PathIndex::build(std::span<const std::string> paths, const std::stop_token& stop) {
    for (auto& posting : m_postings)
        posting.clear();
    m_count = 0;
    for (std::size_t i = 0; i < paths.size(); ++i) {
        if (stop.stop_requested())
            return false;
        std::array<bool, 256> seen{};
        for (char c : paths[i])
            seen[fold(static_cast<unsigned char>(c))] = true;
        for (std::size_t c = 0; c < seen.size(); ++c)
            if (seen[c])
                m_postings[c].push_back(i);
    }
    m_count = paths.size();
    return !stop.stop_requested();
}
std::vector<RankedPath> PathIndex::query(std::string_view pattern, std::span<const std::string> paths,
                                         std::size_t limit, const std::stop_token& stop) const {
    std::vector<RankedPath> hits;
    if (!limit || pattern.size() > 256 || paths.size() != m_count || stop.stop_requested())
        return hits;
    PathMatcher matcher(pattern);
    if (matcher.empty()) {
        for (std::size_t i = 0; i < std::min(limit, paths.size()); ++i)
            hits.push_back({i, 0});
        return hits;
    }
    std::array<bool, 256> required{};
    // Matcher discards structural separators and boundary whitespace. Omitting these
    // bytes is conservative even when an interior whitespace byte is significant.
    for (char c : pattern)
        if (c != '/' && c != '\\' && c != ' ' && c != '\t' && c != '\r' && c != '\n')
            required[fold(static_cast<unsigned char>(c))] = true;
    const std::vector<std::size_t>* smallest = nullptr;
    for (std::size_t c = 0; c < required.size(); ++c)
        if (required[c]) {
            if (m_postings[c].empty())
                return hits;
            if (!smallest || m_postings[c].size() < smallest->size())
                smallest = &m_postings[c];
        }
    const auto better = [&](const RankedPath& a, const RankedPath& b) {
        if (a.score != b.score)
            return a.score > b.score;
        if (paths[a.index].size() != paths[b.index].size())
            return paths[a.index].size() < paths[b.index].size();
        return paths[a.index] < paths[b.index];
    };
    for (std::size_t ordinal = 0; ordinal < (smallest ? smallest->size() : paths.size()); ++ordinal) {
        const auto i = smallest ? (*smallest)[ordinal] : ordinal;
        if (stop.stop_requested())
            return {};
        bool candidate = true;
        for (std::size_t c = 0; c < required.size(); ++c)
            if (required[c] && !std::ranges::binary_search(m_postings[c], i)) {
                candidate = false;
                break;
            }
        if (!candidate)
            continue;
        const auto score = matcher.score(paths[i]);
        if (!score)
            continue;
        const RankedPath hit{i, *score};
        if (hits.size() < limit) {
            hits.push_back(hit);
            std::push_heap(hits.begin(), hits.end(), better);
        } else if (better(hit, hits.front())) {
            std::pop_heap(hits.begin(), hits.end(), better);
            hits.back() = hit;
            std::push_heap(hits.begin(), hits.end(), better);
        }
    }
    std::ranges::sort(hits, better);
    return hits;
}
} // namespace frost::search
