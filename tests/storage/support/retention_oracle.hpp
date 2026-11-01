// SPDX-License-Identifier: Apache-2.0
//
// RetentionOracle: a plain, independent C++ model of LocalHistory's recording and retention rules
// (local_history.hpp; ADR 0014), written from the rule text and sharing no code with the
// implementation. It keeps whole contents instead of blobs: dedup is by content, and two live
// revisions with equal content always share one blob, so "distinct blobs referenced" is
// "distinct contents of the surviving revisions". Test code only.
#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace frost::storage::test {

class RetentionOracle {
public:
    struct Revision {
        std::int64_t id = 0;
        std::string pathKey;
        std::int64_t createdNs = 0;
        std::string content;
    };

    /*
     * Records `content` for `pathKey` at `createdNs`. Without a label, content equal to the
     * path's newest revision (highest id) adds nothing and returns that revision's id with
     * added = false. Ids count up from 1 and are never reused.
     */
    struct RecordResult {
        std::int64_t id = 0;
        bool added = false;
    };
    RecordResult record(std::string_view pathKey, std::string_view content, bool labelled, std::int64_t createdNs) {
        if (!labelled) {
            const Revision* newest = nullptr;
            for (const Revision& revision : m_revisions) {
                if (revision.pathKey == pathKey && (newest == nullptr || revision.id > newest->id)) {
                    newest = &revision;
                }
            }
            if (newest != nullptr && newest->content == content) {
                return {newest->id, false};
            }
        }
        const std::int64_t id = m_nextId++;
        m_revisions.push_back({id, std::string(pathKey), createdNs, std::string(content)});
        return {id, true};
    }

    // Applies the three rules; returns how many revisions were removed.
    std::size_t prune(std::int64_t nowNs, std::int64_t maxAgeNs, std::uint64_t maxTotalBytes, bool keepNewestPerPath) {
        std::set<std::int64_t> protectedIds;
        if (keepNewestPerPath) {
            std::map<std::string, std::int64_t> newest;
            for (const Revision& revision : m_revisions) {
                std::int64_t& slot = newest[revision.pathKey];
                slot = std::max(slot, revision.id);
            }
            for (const auto& entry : newest) {
                protectedIds.insert(entry.second);
            }
        }
        // Rule 1: createdNs < now - maxAge, computed without overflow.
        auto isOld = [&](const Revision& revision) {
            // revision.createdNs < nowNs - maxAgeNs  <=>  its age, nowNs - createdNs, exceeds
            // maxAgeNs. An age that overflows int64 exceeds every maxAge if positive.
            std::int64_t age = 0;
            if (__builtin_sub_overflow(nowNs, revision.createdNs, &age)) {
                return nowNs > revision.createdNs;
            }
            return age > maxAgeNs;
        };
        std::vector<Revision> kept;
        std::size_t removed = 0;
        for (const Revision& revision : m_revisions) {
            if (isOld(revision) && !protectedIds.contains(revision.id)) {
                ++removed;
            } else {
                kept.push_back(revision);
            }
        }
        // Rule 2: oldest first (createdNs, then id) until the distinct contents fit.
        std::vector<Revision> byAge = kept;
        std::sort(byAge.begin(), byAge.end(), [](const Revision& a, const Revision& b) {
            return a.createdNs < b.createdNs || (a.createdNs == b.createdNs && a.id < b.id);
        });
        std::set<std::int64_t> dropped;
        for (const Revision& candidate : byAge) {
            if (distinctBytes(kept, dropped) <= maxTotalBytes) {
                break;
            }
            if (protectedIds.contains(candidate.id)) {
                continue;
            }
            dropped.insert(candidate.id);
            ++removed;
        }
        std::vector<Revision> survivors;
        for (const Revision& revision : kept) {
            if (!dropped.contains(revision.id)) {
                survivors.push_back(revision);
            }
        }
        m_revisions = std::move(survivors);
        return removed;
    }

    // Surviving revision ids, ascending.
    [[nodiscard]] std::vector<std::int64_t> ids() const {
        std::vector<std::int64_t> out;
        for (const Revision& revision : m_revisions) {
            out.push_back(revision.id);
        }
        std::sort(out.begin(), out.end());
        return out;
    }

    // The content bytes of the distinct contents the surviving revisions hold.
    [[nodiscard]] std::uint64_t referencedBytes() const { return distinctBytes(m_revisions, {}); }

    [[nodiscard]] const std::vector<Revision>& revisions() const noexcept { return m_revisions; }

private:
    static std::uint64_t distinctBytes(const std::vector<Revision>& revisions, const std::set<std::int64_t>& except) {
        std::set<std::string_view> contents;
        for (const Revision& revision : revisions) {
            if (!except.contains(revision.id)) {
                contents.insert(revision.content);
            }
        }
        std::uint64_t total = 0;
        for (const std::string_view content : contents) {
            total += content.size();
        }
        return total;
    }

    std::vector<Revision> m_revisions;
    std::int64_t m_nextId = 1;
};

} // namespace frost::storage::test
