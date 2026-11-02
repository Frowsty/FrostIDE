// SPDX-License-Identifier: Apache-2.0
//
// The editor's insight vocabulary (frost/ui/editor/editor_insight.hpp): names, the summary text and
// the stripe kind of a severity.

#include "frost/ui/editor/editor_insight.hpp"

#include <format>

namespace frost::ui {

std::string_view toString(SemanticKind kind) noexcept {
    using K = SemanticKind;
    switch (kind) {
    case K::LocalVariable: return "localVariable";
    case K::Parameter: return "parameter";
    case K::Field: return "field";
    case K::StaticField: return "staticField";
    case K::GlobalVariable: return "globalVariable";
    case K::Function: return "function";
    case K::Method: return "method";
    case K::StaticMethod: return "staticMethod";
    case K::Class: return "class";
    case K::Enum: return "enum";
    case K::Enumerator: return "enumerator";
    case K::Namespace: return "namespace";
    case K::Macro: return "macro";
    case K::TemplateParameter: return "templateParameter";
    case K::Concept: return "concept";
    case K::TypeAlias: return "typeAlias";
    case K::kCount: break;
    }
    return "unknown";
}

std::string_view toString(InsightSeverity severity) noexcept {
    switch (severity) {
    case InsightSeverity::Error: return "error";
    case InsightSeverity::Warning: return "warning";
    case InsightSeverity::WeakWarning: return "weak warning";
    case InsightSeverity::Info: return "info";
    }
    return "error";
}

std::optional<InsightSeverity> InsightSummary::worst() const noexcept {
    for (std::size_t i = 0; i < kInsightSeverityCount; ++i) {
        if (counts[i] > 0) {
            return static_cast<InsightSeverity>(i);
        }
    }
    return std::nullopt;
}

std::string describe(const InsightSummary& summary) {
    std::string out;
    for (std::size_t i = 0; i < kInsightSeverityCount; ++i) {
        const int n = summary.counts[i];
        if (n == 0) {
            continue;
        }
        if (!out.empty()) {
            out += ", ";
        }
        out += std::format("{} {}{}", n, toString(static_cast<InsightSeverity>(i)), n == 1 ? "" : "s");
    }
    if (out.empty()) {
        return summary.analyzing ? "Analyzing..." : "No problems";
    }
    if (summary.analyzing) {
        out += " (analyzing...)";
    }
    return out;
}

StripeMarkKind stripeKindFor(InsightSeverity severity) noexcept {
    switch (severity) {
    case InsightSeverity::Error: return StripeMarkKind::Error;
    case InsightSeverity::Warning: return StripeMarkKind::Warning;
    case InsightSeverity::WeakWarning: return StripeMarkKind::WeakWarning;
    case InsightSeverity::Info: return StripeMarkKind::Info;
    }
    return StripeMarkKind::Error;
}

} // namespace frost::ui
