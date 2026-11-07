// SPDX-License-Identifier: Apache-2.0
// An allowlisted parser for the frost-lint truth-rule tests: it may build usages.
#include "navigation.hpp"

std::vector<UsageGroup> parseReferences(const Json& reply) {
    std::vector<UsageGroup> groups;
    for (const Json& location : reply) {
        Usage usage;
        usage.path = location.path();
        groups.push_back(UsageGroup{.path = usage.path, .usages = {usage}});
    }
    return groups;
}
