// SPDX-License-Identifier: Apache-2.0
//
// C++ Code Vision counts (CPP-9), derived only from clangd's index-backed LSP answers.
#include "frost/lang/code_vision.hpp"

#include "frost/lang/navigation.hpp"

namespace frost::lang {

sched::Task<Result<CodeVisionCounts>> CppCodeVisionProvider::counts(ILspRequester& server, std::string path,
                                                                    CodeVisionSymbol symbol,
                                                                    std::stop_token token) const {
    CodeVisionCounts counts;

    auto references = co_await requestReferences(server, path, symbol.position, false, token);
    if (!references) {
        co_return std::unexpected(std::move(references.error()));
    }
    counts.usages = references->size();

    if (symbol.kind == SymbolKind::Method || symbol.kind == SymbolKind::Function ||
        symbol.kind == SymbolKind::Constructor) {
        auto implementations =
            co_await requestLocations(server, NavigationTarget::Implementation, path, symbol.position, token);
        if (!implementations) {
            co_return std::unexpected(std::move(implementations.error()));
        }
        // LSP implementation answers for a method are its concrete overrides/implementations.
        counts.overrides = implementations->size();
    }

    if (symbol.kind == SymbolKind::Class || symbol.kind == SymbolKind::Struct || symbol.kind == SymbolKind::Interface) {
        auto roots = co_await requestPrepareTypeHierarchy(server, path, symbol.position, token);
        if (!roots) {
            co_return std::unexpected(std::move(roots.error()));
        }
        if (!roots->empty()) {
            auto subtypes = co_await requestSubtypes(server, roots->front(), token);
            if (!subtypes) {
                co_return std::unexpected(std::move(subtypes.error()));
            }
            counts.implementations = subtypes->size();
        }
    }

    co_return counts;
}

} // namespace frost::lang
