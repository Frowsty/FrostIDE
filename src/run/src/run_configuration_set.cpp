// SPDX-License-Identifier: Apache-2.0
//
// RunConfigurationSet (see frost/run/run_configuration_set.hpp): the merge of generated and stored
// run configurations and the subset the store keeps.

#include "frost/run/run_configuration_set.hpp"

#include <algorithm>
#include <utility>

namespace frost::run {

namespace {

const RunConfiguration* findIn(std::span<const RunConfiguration> list, std::string_view id) noexcept {
    const auto it = std::ranges::find(list, id, &RunConfiguration::id);
    return it == list.end() ? nullptr : &*it;
}

} // namespace

void RunConfigurationSet::setStored(std::vector<RunConfiguration> stored) {
    m_stored = std::move(stored);
    merge();
}

void RunConfigurationSet::setGenerated(std::vector<RunConfiguration> generated) {
    m_generated = std::move(generated);
    merge();
}

const RunConfiguration* RunConfigurationSet::find(std::string_view id) const noexcept { return findIn(m_all, id); }

bool RunConfigurationSet::isGenerated(std::string_view id) const noexcept { return findIn(m_generated, id) != nullptr; }

std::vector<RunConfiguration> RunConfigurationSet::toStore(std::span<const RunConfiguration> edited) const {
    std::vector<RunConfiguration> out;
    for (const RunConfiguration& configuration : edited) {
        const RunConfiguration* generated = findIn(m_generated, configuration.id);
        if (generated == nullptr || !(*generated == configuration)) {
            out.push_back(configuration);
        }
    }
    return out;
}

void RunConfigurationSet::replace(std::span<const RunConfiguration> edited) {
    m_stored = toStore(edited);
    merge();
}

void RunConfigurationSet::merge() {
    m_all.clear();
    m_all.reserve(m_generated.size() + m_stored.size());
    for (const RunConfiguration& generated : m_generated) {
        const RunConfiguration* stored = findIn(m_stored, generated.id);
        if (stored == nullptr) {
            m_all.push_back(generated);
            continue;
        }
        // The user's edits, on what the model says the configuration is.
        RunConfiguration merged = *stored;
        merged.type = generated.type;
        merged.target = generated.target;
        m_all.push_back(std::move(merged));
    }
    for (const RunConfiguration& stored : m_stored) {
        if (findIn(m_generated, stored.id) == nullptr) {
            m_all.push_back(stored);
        }
    }
}

} // namespace frost::run
