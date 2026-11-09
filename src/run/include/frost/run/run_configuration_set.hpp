// SPDX-License-Identifier: Apache-2.0
//
// RunConfigurationSet (RUN-2/RUN-3, PLAN.md Appendix C R2): the run configurations of one workspace
// as the Run toolbar and the Run/Debug Configurations dialog show them: the generated ones
// (run_types.hpp) with the user's stored edits laid over them, then the user's own. Qt-free.
//
/*
 * Merge (all()). For each generated configuration, in generation order: the stored configuration
 * with the same id if there is one (the user edited it), keeping the generated type and target (the
 * model decides what it is), else the generated one. Then every stored configuration whose id was
 * not generated, in stored order: the user's native configurations, and edited generated ones whose
 * target is gone from the model right now (a model not loaded yet, a target renamed). Those stay so
 * that no edit is lost while a project configures; running one fails honestly (its target is not in
 * the model).
 *
 * Store (toStore()). What the workspace store (run_configuration.hpp) must hold so that the merge
 * gives `edited` back: every configuration of `edited` that is not generated, or differs from its
 * generated counterpart. A generated configuration the user left as generated is not stored (it is
 * generated again), so a deleted one comes back with the next generation, as Rider's and CLion's
 * generated CMake configurations do.
 *
 * Thread safety: a value type; not synchronised. Complexity: all members are O(n * m) in the
 * stored and generated counts (tens), except find (linear).
 */
#pragma once

#include "frost/run/run_configuration.hpp"

#include <span>
#include <string_view>
#include <vector>

namespace frost::run {

class RunConfigurationSet {
public:
    // The configurations read from the workspace store.
    void setStored(std::vector<RunConfiguration> stored);
    // The generated configurations (every generated type, already merged by the caller).
    void setGenerated(std::vector<RunConfiguration> generated);

    [[nodiscard]] const std::vector<RunConfiguration>& stored() const noexcept { return m_stored; }
    [[nodiscard]] const std::vector<RunConfiguration>& generated() const noexcept { return m_generated; }
    // The merged list (header comment).
    [[nodiscard]] const std::vector<RunConfiguration>& all() const noexcept { return m_all; }

    // The merged configuration with `id`, or null. The pointer lives until the next change.
    [[nodiscard]] const RunConfiguration* find(std::string_view id) const noexcept;
    // Whether `id` is one of the generated configurations.
    [[nodiscard]] bool isGenerated(std::string_view id) const noexcept;

    // What the store must hold for `edited` (header comment, "Store").
    [[nodiscard]] std::vector<RunConfiguration> toStore(std::span<const RunConfiguration> edited) const;
    // The dialog's OK: stored becomes toStore(edited), and all() is merged again.
    void replace(std::span<const RunConfiguration> edited);

private:
    void merge();

    std::vector<RunConfiguration> m_stored;
    std::vector<RunConfiguration> m_generated;
    std::vector<RunConfiguration> m_all;
};

} // namespace frost::run
