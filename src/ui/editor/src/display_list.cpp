// SPDX-License-Identifier: Apache-2.0
//
// Lookups on the editor display list (frost/ui/editor/display_list.hpp) that tests and the view use,
// and the element kinds' names, clipping and counts.

#include "frost/ui/editor/display_list.hpp"

#include <algorithm>

namespace frost::ui {

const DisplayText* EditorDisplayList::findText(const QString& wanted) const noexcept {
    for (const DisplayText& run : text) {
        if (run.text == wanted) {
            return &run;
        }
    }
    return nullptr;
}

const DisplayText* EditorDisplayList::findTextAt(ByteOffset offset) const noexcept {
    for (const DisplayText& run : text) {
        if (run.source.begin <= offset && offset < run.source.end) {
            return &run;
        }
    }
    return nullptr;
}

const DisplayText* EditorDisplayList::findLineNumber(int number) const noexcept {
    const QString wanted = QString::number(number);
    for (const DisplayText& run : gutterText) {
        if (run.text == wanted) {
            return &run;
        }
    }
    return nullptr;
}

std::vector<const DisplayDecoration*> EditorDisplayList::decorationsWithStyle(DecorationStyle style) const {
    std::vector<const DisplayDecoration*> out;
    for (const DisplayDecoration& decoration : decorations) {
        if (decoration.style == style) {
            out.push_back(&decoration);
        }
    }
    return out;
}

bool clippedToTextArea(DisplayElementKind kind) noexcept {
    switch (kind) {
    case DisplayElementKind::BackgroundDecorations:
    case DisplayElementKind::Underlines:
    case DisplayElementKind::Text:
    case DisplayElementKind::Decorations:
    case DisplayElementKind::Overlays:
    case DisplayElementKind::Carets: return true;
    case DisplayElementKind::Fills:
    case DisplayElementKind::GutterBackground:
    case DisplayElementKind::GutterText:
    case DisplayElementKind::ChangeStripes:
    case DisplayElementKind::Markers:
    case DisplayElementKind::StripeBackground:
    case DisplayElementKind::StripeMarks:
    case DisplayElementKind::StripeSummary:
    case DisplayElementKind::Chrome:
    case DisplayElementKind::ChromeText:
    case DisplayElementKind::kCount: return false;
    }
    return false;
}

std::string_view toString(DisplayElementKind kind) noexcept {
    switch (kind) {
    case DisplayElementKind::Fills: return "fills";
    case DisplayElementKind::BackgroundDecorations: return "backgroundDecorations";
    case DisplayElementKind::Underlines: return "underlines";
    case DisplayElementKind::Text: return "text";
    case DisplayElementKind::Decorations: return "decorations";
    case DisplayElementKind::Overlays: return "overlays";
    case DisplayElementKind::Carets: return "carets";
    case DisplayElementKind::GutterBackground: return "gutterBackground";
    case DisplayElementKind::GutterText: return "gutterText";
    case DisplayElementKind::ChangeStripes: return "changeStripes";
    case DisplayElementKind::Markers: return "markers";
    case DisplayElementKind::StripeBackground: return "stripeBackground";
    case DisplayElementKind::StripeMarks: return "stripeMarks";
    case DisplayElementKind::StripeSummary: return "stripeSummary";
    case DisplayElementKind::Chrome: return "chrome";
    case DisplayElementKind::ChromeText: return "chromeText";
    case DisplayElementKind::kCount: break;
    }
    return "unknown";
}

std::size_t displayElementCount(const EditorDisplayList& list, DisplayElementKind kind) noexcept {
    const auto backgrounds = static_cast<std::size_t>(
        std::ranges::count(list.decorations, DecorationStyle::Background, &DisplayDecoration::style));
    const bool gutter = !list.gutter.isEmpty();
    const bool stripe = !list.stripe.isEmpty();
    switch (kind) {
    case DisplayElementKind::Fills: return list.fills.size();
    case DisplayElementKind::BackgroundDecorations: return backgrounds;
    case DisplayElementKind::Underlines: return list.underlines.size();
    case DisplayElementKind::Text: return list.text.size();
    case DisplayElementKind::Decorations: return list.decorations.size() - backgrounds;
    case DisplayElementKind::Overlays: return list.overlays.size();
    case DisplayElementKind::Carets: return list.carets.size();
    case DisplayElementKind::GutterBackground: return gutter ? 1 : 0;
    case DisplayElementKind::GutterText: return gutter ? list.gutterText.size() : 0;
    case DisplayElementKind::ChangeStripes: return gutter ? list.changeStripes.size() : 0;
    case DisplayElementKind::Markers: return gutter ? list.markers.size() : 0;
    case DisplayElementKind::StripeBackground: return stripe ? 1 : 0;
    case DisplayElementKind::StripeMarks: return stripe ? list.stripeMarks.size() : 0;
    case DisplayElementKind::StripeSummary: return stripe && !list.stripeSummary.isEmpty() ? 1 : 0;
    case DisplayElementKind::Chrome: return list.chrome.size();
    case DisplayElementKind::ChromeText: return list.chromeText.size();
    case DisplayElementKind::kCount: break;
    }
    return 0;
}

} // namespace frost::ui
