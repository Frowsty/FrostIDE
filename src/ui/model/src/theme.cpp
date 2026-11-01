// SPDX-License-Identifier: Apache-2.0
//
// Theme validation (frost/ui/model/theme.hpp) and the UiScale arithmetic that reads themes
// (frost/ui/model/ui_scale.hpp).

#include "frost/ui/model/theme.hpp"

#include "frost/io/utf.hpp"

#include <cmath>
#include <format>
#include <initializer_list>

namespace frost::ui {

bool isValidThemeId(std::string_view id) noexcept {
    if (id.empty() || id.size() > 64) {
        return false;
    }
    const auto lowerOrDigit = [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); };
    if (!lowerOrDigit(id[0])) {
        return false;
    }
    for (const char c : id) {
        if (!lowerOrDigit(c) && c != '-') {
            return false;
        }
    }
    return true;
}

Result<Theme> Theme::fromData(const ThemeData& data) {
    if (!isValidThemeId(data.id)) {
        return fail(ErrorCode::InvalidArgument, std::format("invalid theme id '{}'", data.id));
    }
    if (data.name.empty() || data.name.size() > kMaxThemeNameBytes || !io::isValidUtf8(data.name)) {
        return fail(ErrorCode::InvalidArgument, "theme name must be 1..64 bytes of valid UTF-8");
    }
    for (const MetricKeyInfo& key : kMetricKeys) {
        const auto index = static_cast<std::size_t>(key.key);
        const float normal = data.metrics[index];
        const float compact = data.compactMetrics[index];
        for (const float value : {normal, compact}) {
            if (!std::isfinite(value) || value < key.min || value > key.max) {
                return fail(ErrorCode::InvalidArgument,
                            std::format("metric {} = {} is outside [{}, {}]", key.name, value, key.min, key.max));
            }
        }
        if (!key.hasCompact && compact != normal) {
            return fail(ErrorCode::InvalidArgument,
                        std::format("metric {} has no compact value but its compact slot differs", key.name));
        }
        if (key.hasCompact && key.unit == MetricUnit::Px && compact > normal) {
            return fail(
                ErrorCode::InvalidArgument,
                std::format("metric {}: compact value {} exceeds the normal value {}", key.name, compact, normal));
        }
    }
    for (const ColorKeyInfo& key : kColorKeys) {
        if (key.opaque && !data.colors[static_cast<std::size_t>(key.key)].opaque()) {
            return fail(ErrorCode::InvalidArgument, std::format("colour {} must be opaque", key.name));
        }
    }
    return Theme(data);
}

int UiScale::px(double logical) const noexcept {
    // Multiplying by the integer percent first keeps integer logical values exact (14 x 125 / 100
    // is exactly 17.5); std::round rounds half away from zero.
    const double scaled = std::round(logical * zoomPercent / 100.0);
    return scaled <= 0.0 ? 0 : static_cast<int>(scaled);
}

int UiScale::metricPx(const Theme& theme, MetricKey key) const noexcept { return px(theme.metric(key, density)); }

double UiScale::fontPx(const Theme& theme, MetricKey key) const noexcept {
    return theme.metric(key, density) * factor();
}

} // namespace frost::ui
