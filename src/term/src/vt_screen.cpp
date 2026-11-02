// SPDX-License-Identifier: Apache-2.0
//
// VtScreen (frost/term/vt_screen.hpp) over libvterm's screen layer: the screen callbacks keep the
// cursor, the modes, the title, the bell and the scrollback; cells are converted on access.
/*
 * libvterm notes (0.3.3, third_party/libvterm): a wide character's right half holds the code point
 * 0xFFFFFFFF, and vterm_screen_get_cell copies code points only up to the first 0, so the rest of the
 * array is never read. String properties (the title) arrive as fragments between `initial` and
 * `final`. Colours keep their palette index unless converted, so the UI applies the theme's palette.
 */

#include "frost/term/vt_screen.hpp"

#include <algorithm>
#include <array>
#include <deque>
#include <format>
#include <span>
#include <utility>

#include <vterm.h>

namespace frost::term {

namespace {

constexpr std::uint32_t kWideContinuation = 0xFFFFFFFFU;

void appendUtf8(std::string& out, char32_t c) {
    const auto u = static_cast<std::uint32_t>(c);
    if (u < 0x80) {
        out += static_cast<char>(u);
    } else if (u < 0x800) {
        out += static_cast<char>(0xC0 | (u >> 6));
        out += static_cast<char>(0x80 | (u & 0x3F));
    } else if (u < 0x10000) {
        out += static_cast<char>(0xE0 | (u >> 12));
        out += static_cast<char>(0x80 | ((u >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (u & 0x3F));
    } else if (u < 0x110000) {
        out += static_cast<char>(0xF0 | (u >> 18));
        out += static_cast<char>(0x80 | ((u >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((u >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (u & 0x3F));
    }
}

// Decodes well-formed UTF-8 into at most `out.size()` code points; returns how many. Malformed bytes
// are skipped (the text came from appendUtf8, so there are none in practice).
std::size_t decodeUtf8(std::string_view text, std::span<std::uint32_t> out) {
    std::size_t count = 0;
    std::size_t i = 0;
    while (i < text.size() && count < out.size()) {
        const auto lead = static_cast<unsigned char>(text[i]);
        int extra = 0;
        std::uint32_t value = 0;
        if (lead < 0x80) {
            value = lead;
        } else if ((lead & 0xE0) == 0xC0) {
            extra = 1;
            value = lead & 0x1FU;
        } else if ((lead & 0xF0) == 0xE0) {
            extra = 2;
            value = lead & 0x0FU;
        } else if ((lead & 0xF8) == 0xF0) {
            extra = 3;
            value = lead & 0x07U;
        } else {
            ++i;
            continue;
        }
        if (i + static_cast<std::size_t>(extra) >= text.size()) {
            break; // Truncated sequence.
        }
        for (int k = 1; k <= extra; ++k) {
            value = (value << 6) | (static_cast<unsigned char>(text[i + static_cast<std::size_t>(k)]) & 0x3FU);
        }
        out[count++] = value;
        i += static_cast<std::size_t>(extra) + 1;
    }
    return count;
}

/*
 * The length of `bytes` without a trailing UTF-8 sequence that is still missing continuation bytes:
 * a lead byte among the last three bytes whose sequence runs past the end. O(1).
 */
std::size_t completeUtf8Prefix(std::string_view bytes) noexcept {
    const std::size_t size = bytes.size();
    for (std::size_t back = 1; back <= 3 && back <= size; ++back) {
        const auto c = static_cast<unsigned char>(bytes[size - back]);
        if ((c & 0xC0) == 0x80) {
            continue; // A continuation byte: look further back for its lead.
        }
        const std::size_t length = c >= 0xF8 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
        return length > back ? size - back : size;
    }
    return size;
}

CellColor toCellColor(const VTermColor& color) noexcept {
    if ((color.type & VTERM_COLOR_DEFAULT_MASK) != 0) {
        return CellColor{};
    }
    if (VTERM_COLOR_IS_INDEXED(&color)) {
        return CellColor::indexed(color.indexed.idx);
    }
    return CellColor::rgb(color.rgb.red, color.rgb.green, color.rgb.blue);
}

Cell toCell(const VTermScreenCell& source) {
    Cell cell;
    if (source.chars[0] == kWideContinuation) {
        cell.width = 0;
    } else {
        for (std::size_t i = 0; i < VTERM_MAX_CHARS_PER_CELL && source.chars[i] != 0; ++i) {
            appendUtf8(cell.text, static_cast<char32_t>(source.chars[i]));
        }
        cell.width = source.width == 2 ? 2 : 1;
    }
    cell.foreground = toCellColor(source.fg);
    cell.background = toCellColor(source.bg);
    cell.style.bold = source.attrs.bold != 0;
    cell.style.italic = source.attrs.italic != 0;
    cell.style.blink = source.attrs.blink != 0;
    cell.style.inverse = source.attrs.reverse != 0;
    cell.style.strike = source.attrs.strike != 0;
    cell.style.conceal = source.attrs.conceal != 0;
    switch (source.attrs.underline) {
    case VTERM_UNDERLINE_SINGLE: cell.style.underline = Underline::Single; break;
    case VTERM_UNDERLINE_DOUBLE: cell.style.underline = Underline::Double; break;
    case VTERM_UNDERLINE_CURLY: cell.style.underline = Underline::Curly; break;
    default: cell.style.underline = Underline::None; break;
    }
    return cell;
}

// True for a cell that looks like empty space: no text, single width, default colours and style.
bool isBlank(const Cell& cell) noexcept {
    return cell.text.empty() && cell.width == 1 && cell.background == CellColor{} && cell.style == CellStyle{};
}

VTermModifier toVTermMods(KeyMods mods) noexcept {
    int bits = VTERM_MOD_NONE;
    if (mods.shift) {
        bits |= VTERM_MOD_SHIFT;
    }
    if (mods.alt) {
        bits |= VTERM_MOD_ALT;
    }
    if (mods.ctrl) {
        bits |= VTERM_MOD_CTRL;
    }
    return static_cast<VTermModifier>(bits);
}

VTermKey toVTermKey(TermKey key) noexcept {
    switch (key) {
    case TermKey::Enter: return VTERM_KEY_ENTER;
    case TermKey::Tab: return VTERM_KEY_TAB;
    case TermKey::Backspace: return VTERM_KEY_BACKSPACE;
    case TermKey::Escape: return VTERM_KEY_ESCAPE;
    case TermKey::Up: return VTERM_KEY_UP;
    case TermKey::Down: return VTERM_KEY_DOWN;
    case TermKey::Left: return VTERM_KEY_LEFT;
    case TermKey::Right: return VTERM_KEY_RIGHT;
    case TermKey::Insert: return VTERM_KEY_INS;
    case TermKey::Delete: return VTERM_KEY_DEL;
    case TermKey::Home: return VTERM_KEY_HOME;
    case TermKey::End: return VTERM_KEY_END;
    case TermKey::PageUp: return VTERM_KEY_PAGEUP;
    case TermKey::PageDown: return VTERM_KEY_PAGEDOWN;
    default: break;
    }
    const int function = static_cast<int>(key) - static_cast<int>(TermKey::F1) + 1;
    return static_cast<VTermKey>(VTERM_KEY_FUNCTION(function));
}

/*
 * The classic control code of Ctrl+`c` (c lower-case), as xterm and the Windows console send it, or
 * -1 when there is none. libvterm would send CSI u for Ctrl+I, J, M, [ and the digits, which shells
 * and ConPTY's input parser do not understand.
 */
int controlCode(char32_t c) noexcept {
    if (c >= U'a' && c <= U'z') {
        return static_cast<int>(c) & 0x1F;
    }
    switch (c) {
    case U' ':
    case U'@':
    case U'2': return 0x00;
    case U'[':
    case U'3': return 0x1B;
    case U'\\':
    case U'4': return 0x1C;
    case U']':
    case U'5': return 0x1D;
    case U'^':
    case U'6': return 0x1E;
    case U'_':
    case U'-':
    case U'/':
    case U'7': return 0x1F;
    case U'?':
    case U'8': return 0x7F;
    default: return -1;
    }
}

} // namespace

struct VtScreen::Impl {
    explicit Impl(VtScreen& screen) : owner(screen) {}
    Impl(const Impl&) = delete ("refers to its owner");
    Impl& operator=(const Impl&) = delete ("refers to its owner");
    ~Impl() {
        if (vt != nullptr) {
            vterm_free(vt);
        }
        if (observer != nullptr) {
            vterm_free(observer);
        }
    }

    VtScreen& owner;
    VTerm* vt = nullptr;
    VTerm* observer = nullptr;
    bool win32Input = false;
    std::deque<bool> win32Queries;
    // Main and alternate screen keyboard stacks are independent (kitty protocol).
    std::array<std::deque<unsigned>, 2> keyboardStacks;
    bool keyboardAlternate = false;
    std::deque<unsigned> keyboardQueries;

    unsigned keyboardFlags() const noexcept {
        const auto& stack = keyboardStacks[keyboardAlternate ? 1 : 0];
        return stack.empty() ? 0U : stack.back();
    }
    static int fallbackCsi(const char* leader, const long args[], int count, const char* intermediate, char command,
                           void* user) {
        auto& self = *static_cast<Impl*>(user);
        if (leader && std::string_view(leader) == "?" && !intermediate && command == 'u' && count == 1 &&
            CSI_ARG_IS_MISSING(args[0]) && !self.keyboardQueries.empty()) {
            self.output += std::format("\x1b[?{}u", self.keyboardQueries.front());
            self.keyboardQueries.pop_front();
            return 1;
        }
        return 0;
    }
    static constexpr VTermStateFallbacks kFallbacks{
        .control = nullptr,
        .csi = &fallbackCsi,
        .osc = nullptr,
        .dcs = nullptr,
        .apc = nullptr,
        .pm = nullptr,
        .sos = nullptr,
    };

    static int observeText(const char*, std::size_t length, void*) { return static_cast<int>(length); }
    static int observeCsi(const char* leader, const long args[], int count, const char* intermediate, char command,
                          void* user) {
        auto& self = *static_cast<Impl*>(user);
        if (leader && !intermediate && command == 'u') {
            const auto prefix = std::string_view(leader);
            auto& stack = self.keyboardStacks[self.keyboardAlternate ? 1 : 0];
            const unsigned arg =
                count > 0 && !CSI_ARG_IS_MISSING(args[0]) ? static_cast<unsigned>(CSI_ARG(args[0])) : 0U;
            if (prefix == "?" && count == 1 && CSI_ARG_IS_MISSING(args[0])) {
                self.keyboardQueries.push_back(self.keyboardFlags());
                return 1;
            }
            if (prefix == ">") {
                if (stack.size() == 32)
                    stack.pop_front();
                stack.push_back(arg & 5U);
                return 1;
            }
            if (prefix == "<") {
                unsigned remaining = arg == 0 ? 1U : arg;
                while (remaining-- && !stack.empty())
                    stack.pop_back();
                return 1;
            }
            if (prefix == "=") {
                if (stack.empty())
                    stack.push_back(0);
                const unsigned mode =
                    count > 1 && !CSI_ARG_IS_MISSING(args[1]) ? static_cast<unsigned>(CSI_ARG(args[1])) : 1U;
                if (mode == 1)
                    stack.back() = arg & 5U;
                else if (mode == 2)
                    stack.back() |= arg & 5U;
                else if (mode == 3)
                    stack.back() &= ~(arg & 5U);
                return 1;
            }
        }
        if (leader == nullptr && intermediate != nullptr && std::string_view(intermediate) == "!" && command == 'p') {
            self.win32Input = false;
        }
        if (leader != nullptr && std::string_view(leader) == "?" && intermediate != nullptr &&
            std::string_view(intermediate) == "$" && command == 'p' && count == 1 && !CSI_ARG_IS_MISSING(args[0]) &&
            CSI_ARG(args[0]) == 9001) {
            self.win32Queries.push_back(self.win32Input);
            return 1;
        }
        if (leader == nullptr || std::string_view(leader) != "?" ||
            (intermediate != nullptr && intermediate[0] != '\0') || (command != 'h' && command != 'l')) {
            return 0;
        }
        for (int i = 0; i < count; ++i) {
            if (!CSI_ARG_IS_MISSING(args[i]) && CSI_ARG(args[i]) == 9001) {
                self.win32Input = command == 'h';
            }
            if (!CSI_ARG_IS_MISSING(args[i]) &&
                (CSI_ARG(args[i]) == 1049 || CSI_ARG(args[i]) == 1047 || CSI_ARG(args[i]) == 47)) {
                self.keyboardAlternate = command == 'h';
            }
        }
        return 1;
    }
    static int observeEscape(const char* bytes, std::size_t length, void* user) {
        if (std::string_view(bytes, length) == "c") {
            auto& self = *static_cast<Impl*>(user);
            self.win32Input = false;
            self.keyboardStacks = {};
            self.keyboardAlternate = false;
        }
        return 1;
    }
    static constexpr VTermParserCallbacks kObserverCallbacks{
        .text = &observeText,
        .control = nullptr,
        .escape = &observeEscape,
        .csi = &observeCsi,
        .osc = nullptr,
        .dcs = nullptr,
        .apc = nullptr,
        .pm = nullptr,
        .sos = nullptr,
        .resize = nullptr,
    };
    VTermScreen* screen = nullptr;
    VTermState* state = nullptr;
    // Oldest first; back() is row -1. Each line has its trailing blank cells trimmed.
    std::deque<std::vector<Cell>> scrollback;
    std::string output;
    std::string titleBuffer;
    // The start of a UTF-8 sequence the last feed() ended inside (at most 3 bytes).
    std::string pendingUtf8;

    static void onOutput(const char* bytes, std::size_t length, void* user) {
        auto& self = *static_cast<Impl*>(user);
        // libvterm reports mode 9001 as unknown. Replace only that reply, using the
        // mode at each query, since several toggles and queries may share one feed.
        if (std::string_view(bytes, length) == "\x1b[?9001;0$y" && !self.win32Queries.empty()) {
            self.output += self.win32Queries.front() ? "\x1b[?9001;1$y" : "\x1b[?9001;2$y";
            self.win32Queries.pop_front();
        } else {
            self.output.append(bytes, length);
        }
    }

    static int onDamage(VTermRect, void* user) {
        static_cast<Impl*>(user)->owner.m_events.changed = true;
        return 1;
    }

    static int onMoveRect(VTermRect, VTermRect, void* user) {
        static_cast<Impl*>(user)->owner.m_events.changed = true;
        return 0; // libvterm then damages the moved area.
    }

    static int onMoveCursor(VTermPos pos, VTermPos, int visible, void* user) {
        VtScreen& self = static_cast<Impl*>(user)->owner;
        self.m_cursor.row = pos.row;
        self.m_cursor.column = pos.col;
        self.m_cursor.visible = visible != 0;
        self.m_events.changed = true;
        return 1;
    }

    static int onSetTermProp(VTermProp prop, VTermValue* value, void* user) {
        Impl& impl = *static_cast<Impl*>(user);
        VtScreen& self = impl.owner;
        switch (prop) {
        case VTERM_PROP_CURSORVISIBLE: self.m_cursor.visible = value->boolean != 0; break;
        case VTERM_PROP_CURSORBLINK: self.m_cursor.blinking = value->boolean != 0; break;
        case VTERM_PROP_ALTSCREEN: self.m_altScreen = value->boolean != 0; break;
        case VTERM_PROP_REVERSE: self.m_reverse = value->boolean != 0; break;
        case VTERM_PROP_CURSORSHAPE:
            self.m_cursor.shape = value->number == VTERM_PROP_CURSORSHAPE_UNDERLINE  ? CursorShape::Underline
                                  : value->number == VTERM_PROP_CURSORSHAPE_BAR_LEFT ? CursorShape::Bar
                                                                                     : CursorShape::Block;
            break;
        case VTERM_PROP_MOUSE:
            self.m_mouseMode = value->number == VTERM_PROP_MOUSE_CLICK  ? MouseMode::Click
                               : value->number == VTERM_PROP_MOUSE_DRAG ? MouseMode::Drag
                               : value->number == VTERM_PROP_MOUSE_MOVE ? MouseMode::Move
                                                                        : MouseMode::None;
            break;
        case VTERM_PROP_TITLE: {
            const VTermStringFragment& fragment = value->string;
            if (fragment.initial) {
                impl.titleBuffer.clear();
            }
            impl.titleBuffer.append(fragment.str, fragment.len);
            if (fragment.final) {
                self.m_title = impl.titleBuffer;
                self.m_events.titleChanged = true;
            }
            break;
        }
        default: break;
        }
        self.m_events.changed = true;
        return 1;
    }

    static int onBell(void* user) {
        ++static_cast<Impl*>(user)->owner.m_events.bells;
        return 1;
    }

    static int onResize(int, int, void* user) {
        static_cast<Impl*>(user)->owner.m_events.changed = true;
        return 1;
    }

    static int onPushLine(int columns, const VTermScreenCell* cells, void* user) {
        Impl& impl = *static_cast<Impl*>(user);
        VtScreen& self = impl.owner;
        if (self.m_scrollbackLimit == 0) {
            return 1;
        }
        std::vector<Cell> line;
        line.reserve(static_cast<std::size_t>(columns));
        for (int column = 0; column < columns; ++column) {
            line.push_back(toCell(cells[column]));
        }
        while (!line.empty() && isBlank(line.back())) {
            line.pop_back();
        }
        line.shrink_to_fit();
        impl.scrollback.push_back(std::move(line));
        while (impl.scrollback.size() > self.m_scrollbackLimit) {
            impl.scrollback.pop_front();
        }
        ++self.m_events.linesScrolledOff;
        ++self.m_linesPushed;
        self.m_events.changed = true;
        return 1;
    }

    static int onPopLine(int columns, VTermScreenCell* cells, void* user) {
        Impl& impl = *static_cast<Impl*>(user);
        if (impl.scrollback.empty()) {
            return 0;
        }
        const std::vector<Cell> line = std::move(impl.scrollback.back());
        impl.scrollback.pop_back();
        VTermColor defaultForeground;
        VTermColor defaultBackground;
        vterm_state_get_default_colors(impl.state, &defaultForeground, &defaultBackground);
        const auto toVTermColor = [](const CellColor& color, const VTermColor& fallback) {
            VTermColor out = fallback;
            if (color.kind == CellColor::Kind::Indexed) {
                vterm_color_indexed(&out, color.index);
            } else if (color.kind == CellColor::Kind::Rgb) {
                vterm_color_rgb(&out, color.red, color.green, color.blue);
            }
            return out;
        };
        for (int column = 0; column < columns; ++column) {
            VTermScreenCell& target = cells[column];
            target = VTermScreenCell{};
            target.width = 1;
            target.fg = defaultForeground;
            target.bg = defaultBackground;
            if (static_cast<std::size_t>(column) >= line.size()) {
                continue;
            }
            const Cell& cell = line[static_cast<std::size_t>(column)];
            if (cell.width == 0) {
                target.chars[0] = kWideContinuation;
            } else {
                const std::size_t count =
                    decodeUtf8(cell.text, std::span<std::uint32_t>(target.chars, VTERM_MAX_CHARS_PER_CELL - 1));
                target.chars[count] = 0;
                target.width = static_cast<char>(cell.width);
            }
            target.fg = toVTermColor(cell.foreground, defaultForeground);
            target.bg = toVTermColor(cell.background, defaultBackground);
            target.attrs.bold = cell.style.bold ? 1 : 0;
            target.attrs.italic = cell.style.italic ? 1 : 0;
            target.attrs.blink = cell.style.blink ? 1 : 0;
            target.attrs.reverse = cell.style.inverse ? 1 : 0;
            target.attrs.strike = cell.style.strike ? 1 : 0;
            target.attrs.conceal = cell.style.conceal ? 1 : 0;
            target.attrs.underline = static_cast<unsigned int>(cell.style.underline) & 0x3U;
        }
        impl.owner.m_events.changed = true;
        return 1;
    }

    static int onClearScrollback(void* user) {
        Impl& impl = *static_cast<Impl*>(user);
        impl.scrollback.clear();
        impl.owner.m_events.changed = true;
        return 1;
    }

    static constexpr VTermScreenCallbacks kCallbacks{
        .damage = &onDamage,
        .moverect = &onMoveRect,
        .movecursor = &onMoveCursor,
        .settermprop = &onSetTermProp,
        .bell = &onBell,
        .resize = &onResize,
        .sb_pushline = &onPushLine,
        .sb_popline = &onPopLine,
        .sb_clear = &onClearScrollback,
    };
};

VtScreen::VtScreen(PtySize size, std::size_t scrollbackLines)
    : m_size{std::max<std::uint16_t>(size.columns, 1), std::max<std::uint16_t>(size.rows, 1)},
      m_scrollbackLimit(scrollbackLines), m_impl(std::make_unique<Impl>(*this)) {
    m_impl->vt = vterm_new(m_size.rows, m_size.columns);
    m_impl->observer = vterm_new(m_size.rows, m_size.columns);
    vterm_set_utf8(m_impl->observer, 1);
    vterm_parser_set_callbacks(m_impl->observer, &Impl::kObserverCallbacks, m_impl.get());
    vterm_set_utf8(m_impl->vt, 1);
    vterm_output_set_callback(m_impl->vt, &Impl::onOutput, m_impl.get());
    m_impl->screen = vterm_obtain_screen(m_impl->vt);
    m_impl->state = vterm_obtain_state(m_impl->vt);
    vterm_screen_set_callbacks(m_impl->screen, &Impl::kCallbacks, m_impl.get());
    vterm_screen_set_unrecognised_fallbacks(m_impl->screen, &Impl::kFallbacks, m_impl.get());
    vterm_screen_enable_altscreen(m_impl->screen, 1);
    vterm_screen_enable_reflow(m_impl->screen, true);
    vterm_screen_reset(m_impl->screen, 1);
    m_events = ScreenEvents{};
}

VtScreen::~VtScreen() = default;

void VtScreen::feed(std::string_view bytes) {
    // A UTF-8 sequence cut by a read boundary waits for its continuation bytes: libvterm 0.3.3 shows
    // U+FFFD for a sequence split across two writes (VtScreen.SequencesMaySplitAcrossFeeds).
    std::string joined;
    if (!m_impl->pendingUtf8.empty()) {
        joined = std::exchange(m_impl->pendingUtf8, std::string());
        joined.append(bytes);
        bytes = joined;
    }
    const std::size_t complete = completeUtf8Prefix(bytes);
    m_impl->pendingUtf8.assign(bytes.substr(complete));
    if (complete > 0) {
        static_cast<void>(vterm_input_write(m_impl->observer, bytes.data(), complete));
        static_cast<void>(vterm_input_write(m_impl->vt, bytes.data(), complete));
    }
}

void VtScreen::resize(PtySize size) {
    const PtySize clamped{std::max<std::uint16_t>(size.columns, 1), std::max<std::uint16_t>(size.rows, 1)};
    if (clamped == m_size) {
        return;
    }
    m_size = clamped;
    vterm_set_size(m_impl->vt, m_size.rows, m_size.columns);
    m_events.changed = true;
}

std::size_t VtScreen::scrollbackSize() const noexcept { return m_impl->scrollback.size(); }

Cell VtScreen::cell(int row, int column) const {
    if (column < 0 || column >= columns() || row >= rows()) {
        return Cell{};
    }
    if (row >= 0) {
        VTermScreenCell source{};
        if (vterm_screen_get_cell(m_impl->screen, VTermPos{row, column}, &source) == 0) {
            return Cell{};
        }
        return toCell(source);
    }
    const auto back = static_cast<std::size_t>(-static_cast<long long>(row));
    if (back > m_impl->scrollback.size()) {
        return Cell{};
    }
    const std::vector<Cell>& line = m_impl->scrollback[m_impl->scrollback.size() - back];
    return static_cast<std::size_t>(column) < line.size() ? line[static_cast<std::size_t>(column)] : Cell{};
}

std::vector<Cell> VtScreen::line(int row) const {
    std::vector<Cell> cells;
    cells.reserve(static_cast<std::size_t>(columns()));
    for (int column = 0; column < columns(); ++column) {
        cells.push_back(cell(row, column));
    }
    return cells;
}

std::string VtScreen::lineText(int row) const { return text(ScreenPos{row, 0}, ScreenPos{row, columns() - 1}); }

std::string VtScreen::text(ScreenPos from, ScreenPos to) const {
    if (to < from) {
        std::swap(from, to);
    }
    std::string out;
    for (int row = from.row; row <= to.row; ++row) {
        const int first = row == from.row ? std::max(from.column, 0) : 0;
        const int last = row == to.row ? std::min(to.column, columns() - 1) : columns() - 1;
        std::string rowText;
        for (int column = first; column <= last; ++column) {
            const Cell c = cell(row, column);
            if (c.width == 0) {
                continue;
            }
            rowText += c.text.empty() ? std::string(" ") : c.text;
        }
        while (!rowText.empty() && rowText.back() == ' ') {
            rowText.pop_back();
        }
        if (row != from.row) {
            out += '\n';
        }
        out += rowText;
    }
    return out;
}

std::string VtScreen::screenText() const { return text(ScreenPos{0, 0}, ScreenPos{rows() - 1, columns() - 1}); }

ScreenEvents VtScreen::takeEvents() noexcept { return std::exchange(m_events, ScreenEvents{}); }

bool VtScreen::sendWin32Key(const Win32KeyEvent& event) {
    if (!m_impl->win32Input) {
        return false;
    }
    // Microsoft Terminal's win32-input-mode specification, CSI Vk;Sc;Uc;Kd;Cs;Rc _.
    m_impl->output += std::format("\x1b[{};{};{};{};{};{}_", event.virtualKey, event.scanCode, event.unicode,
                                  event.down ? 1 : 0, event.controlState, event.repeatCount);
    return true;
}

void VtScreen::sendKey(TermKey key, KeyMods mods) {
    const unsigned flags = m_impl->keyboardFlags();
    if ((flags & 8U) != 0 ||
        ((flags & 1U) != 0 && (key == TermKey::Escape || (key == TermKey::Enter && !mods.none())))) {
        unsigned code = 0;
        switch (key) {
        case TermKey::Enter: code = 13; break;
        case TermKey::Tab: code = 9; break;
        case TermKey::Backspace: code = 127; break;
        case TermKey::Escape: code = 27; break;
        default: break;
        }
        if (code != 0) {
            const unsigned modifiers = 1U + (mods.shift ? 1U : 0U) + (mods.alt ? 2U : 0U) + (mods.ctrl ? 4U : 0U);
            m_impl->output += std::format("\x1b[{};{}u", code, modifiers);
            return;
        }
    }
    // Encodings libvterm would give as CSI u, which shells and ConPTY's input parser do not read.
    switch (key) {
    case TermKey::Backspace:
        if (mods.ctrl) {
            m_impl->output += mods.alt ? "\x1b\x08" : "\x08";
            return;
        }
        if (mods.shift) {
            m_impl->output += mods.alt ? "\x1b\x7f" : "\x7f";
            return;
        }
        break;
    case TermKey::Enter:
        if (mods.shift || mods.ctrl) {
            // TODO(M4): Shift+Enter with its modifier (win32-input-mode / kitty keyboard, spike S8).
            m_impl->output += mods.alt ? "\x1b\r" : "\r";
            return;
        }
        break;
    case TermKey::Escape:
        if (!mods.none()) {
            m_impl->output += "\x1b";
            return;
        }
        break;
    default: break;
    }
    vterm_keyboard_key(m_impl->vt, toVTermKey(key), toVTermMods(mods));
}

void VtScreen::sendChar(char32_t character, KeyMods mods) {
    const unsigned flags = m_impl->keyboardFlags();
    if ((flags & 8U) != 0 || ((flags & 1U) != 0 && (mods.ctrl || mods.alt))) {
        char32_t shifted = 0;
        if (character >= U'A' && character <= U'Z') {
            shifted = character;
            character += U'a' - U'A';
        }
        const unsigned modifiers = 1U + (mods.shift ? 1U : 0U) + (mods.alt ? 2U : 0U) + (mods.ctrl ? 4U : 0U);
        if ((flags & 4U) != 0 && shifted != 0 && mods.shift) {
            m_impl->output += std::format("\x1b[{}:{};{}u", static_cast<std::uint32_t>(character),
                                          static_cast<std::uint32_t>(shifted), modifiers);
        } else {
            m_impl->output += std::format("\x1b[{};{}u", static_cast<std::uint32_t>(character), modifiers);
        }
        return;
    }
    if (!mods.ctrl && !mods.alt) {
        appendUtf8(m_impl->output, character);
        return;
    }
    char32_t base = character;
    if (base >= U'A' && base <= U'Z') {
        base = base - U'A' + U'a';
    }
    if (mods.ctrl) {
        if (const int code = controlCode(base); code >= 0) {
            if (mods.alt) {
                m_impl->output += '\x1b';
            }
            m_impl->output += static_cast<char>(code);
            return;
        }
        vterm_keyboard_unichar(m_impl->vt, static_cast<std::uint32_t>(character), toVTermMods(mods));
        return;
    }
    // Alt alone: ESC and the character (Shift already chose the character).
    m_impl->output += '\x1b';
    appendUtf8(m_impl->output, character);
}

void VtScreen::sendText(std::string_view utf8) { m_impl->output.append(utf8); }

void VtScreen::paste(std::string_view utf8) {
    std::string body;
    body.reserve(utf8.size());
    for (std::size_t i = 0; i < utf8.size(); ++i) {
        const char c = utf8[i];
        if (c == '\r') {
            body += '\r';
            if (i + 1 < utf8.size() && utf8[i + 1] == '\n') {
                ++i;
            }
        } else if (c == '\n') {
            body += '\r';
        } else {
            body += c;
        }
    }
    // A pasted end marker would let the clipboard end the bracket early and run what follows.
    constexpr std::string_view kEndMarker = "\x1b[201~";
    for (std::size_t at = body.find(kEndMarker); at != std::string::npos; at = body.find(kEndMarker, at)) {
        body.erase(at, kEndMarker.size());
    }
    vterm_keyboard_start_paste(m_impl->vt);
    m_impl->output += body;
    vterm_keyboard_end_paste(m_impl->vt);
}

void VtScreen::mouseMove(int row, int column, KeyMods mods) {
    // Without a mouse mode libvterm would still track buttons, and a press seen then would swallow
    // the first press reported after the program enables one.
    if (m_mouseMode == MouseMode::None) {
        return;
    }
    vterm_mouse_move(m_impl->vt, std::clamp(row, 0, rows() - 1), std::clamp(column, 0, columns() - 1),
                     toVTermMods(mods));
}

void VtScreen::mouseButton(MouseButton button, bool pressed, int row, int column, KeyMods mods) {
    if (m_mouseMode == MouseMode::None) {
        return;
    }
    mouseMove(row, column, mods);
    vterm_mouse_button(m_impl->vt, static_cast<int>(button), pressed, toVTermMods(mods));
}

void VtScreen::focusChanged(bool focused) {
    if (focused) {
        vterm_state_focus_in(m_impl->state);
    } else {
        vterm_state_focus_out(m_impl->state);
    }
}

std::string VtScreen::takeOutput() { return std::exchange(m_impl->output, std::string()); }

} // namespace frost::term
