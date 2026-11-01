// SPDX-License-Identifier: Apache-2.0
//
// Rgba formatting (frost/ui/model/rgba.hpp).

#include "frost/ui/model/rgba.hpp"

#include <format>

namespace frost::ui {

std::string Rgba::toHex() const {
    if (opaque()) {
        return std::format("#{:02X}{:02X}{:02X}", r(), g(), b());
    }
    return std::format("#{:02X}{:02X}{:02X}{:02X}", r(), g(), b(), a());
}

} // namespace frost::ui
