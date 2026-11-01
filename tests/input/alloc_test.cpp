// SPDX-License-Identifier: Apache-2.0
//
// InputAlloc: the keystroke path (KeymapService::handle and peek over a warm index)
// allocates nothing. Every form of the global operator new and delete is replaced with
// counting versions, so this is its own executable; counting happens only inside a window
// opened around the measured calls. Sanitizer runtimes replace operator new themselves, so
// under ASan, TSan or MSan the replacement is compiled out and the test skips; the debug
// and release legs run it.

#include "frost/input/keymap_service.hpp"
#include "frost/input/simulated_keyboard.hpp"

#include "input/support/test_actions.hpp"
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <new>
#include <vector>

#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || __has_feature(memory_sanitizer)
#define FROST_INPUT_COUNT_ALLOCATIONS 0
#endif
#endif
#if !defined(FROST_INPUT_COUNT_ALLOCATIONS) && defined(__SANITIZE_ADDRESS__)
#define FROST_INPUT_COUNT_ALLOCATIONS 0
#endif
#ifndef FROST_INPUT_COUNT_ALLOCATIONS
#define FROST_INPUT_COUNT_ALLOCATIONS 1
#endif

#if FROST_INPUT_COUNT_ALLOCATIONS

namespace {

std::atomic<bool> g_counting{false};
std::atomic<std::size_t> g_allocations{0};
std::atomic<std::size_t> g_bytes{0};

void note(std::size_t size) noexcept {
    if (g_counting.load(std::memory_order_relaxed)) {
        g_allocations.fetch_add(1, std::memory_order_relaxed);
        g_bytes.fetch_add(size, std::memory_order_relaxed);
    }
}

void* allocate(std::size_t size) noexcept {
    note(size);
    return std::malloc(size == 0 ? 1 : size);
}

void* allocateAligned(std::size_t size, std::align_val_t align) noexcept {
    note(size);
    const auto alignment = static_cast<std::size_t>(align);
#ifdef _WIN32
    return _aligned_malloc(size == 0 ? 1 : size, alignment);
#else
    const std::size_t rounded = (size + alignment - 1) / alignment * alignment;
    return std::aligned_alloc(alignment, rounded == 0 ? alignment : rounded);
#endif
}

void release(void* p) noexcept { std::free(p); }

void releaseAligned(void* p) noexcept {
#ifdef _WIN32
    _aligned_free(p);
#else
    std::free(p);
#endif
}

void* orThrow(void* p) {
    if (p == nullptr) {
        throw std::bad_alloc();
    }
    return p;
}

} // namespace

// The replaceable global allocation functions ([new.delete]): scalar, array, aligned,
// nothrow and sized forms.
void* operator new(std::size_t size) { return orThrow(allocate(size)); }
void* operator new[](std::size_t size) { return orThrow(allocate(size)); }
void* operator new(std::size_t size, std::align_val_t align) { return orThrow(allocateAligned(size, align)); }
void* operator new[](std::size_t size, std::align_val_t align) { return orThrow(allocateAligned(size, align)); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept { return allocate(size); }
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept { return allocate(size); }
void* operator new(std::size_t size, std::align_val_t align, const std::nothrow_t&) noexcept {
    return allocateAligned(size, align);
}
void* operator new[](std::size_t size, std::align_val_t align, const std::nothrow_t&) noexcept {
    return allocateAligned(size, align);
}
void operator delete(void* p) noexcept { release(p); }
void operator delete[](void* p) noexcept { release(p); }
void operator delete(void* p, std::size_t) noexcept { release(p); }
void operator delete[](void* p, std::size_t) noexcept { release(p); }
void operator delete(void* p, std::align_val_t) noexcept { releaseAligned(p); }
void operator delete[](void* p, std::align_val_t) noexcept { releaseAligned(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { releaseAligned(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { releaseAligned(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { release(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { release(p); }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept { releaseAligned(p); }
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept { releaseAligned(p); }

#endif // FROST_INPUT_COUNT_ALLOCATIONS

namespace frost::input {
namespace {

#if FROST_INPUT_COUNT_ALLOCATIONS

// Counts the allocations made while it is alive.
class CountingWindow {
public:
    CountingWindow() noexcept {
        g_allocations.store(0);
        g_bytes.store(0);
        g_counting.store(true);
    }
    ~CountingWindow() { g_counting.store(false); }
    CountingWindow(const CountingWindow&) = delete;
    CountingWindow& operator=(const CountingWindow&) = delete;
    [[nodiscard]] std::size_t allocations() const noexcept { return g_allocations.load(); }
    [[nodiscard]] std::size_t bytes() const noexcept { return g_bytes.load(); }
};

void* volatile g_sink = nullptr;

// Key events covering every matcher path: single strokes, a sequence, autorepeat, dead
// keys, AltGr text, double taps, IME, releases and unknown codes.
std::vector<KeyEvent> mixedEvents(const IClock& clock) {
    SimulatedKeyboard keyboard(simulatedLayout(SimulatedLayoutId::DeDe), KeyPlatform::Windows, clock);
    std::vector<KeyEvent> events;
    const auto add = [&](std::vector<KeyEvent> more) { events.insert(events.end(), more.begin(), more.end()); };
    add(keyboard.chord({Code::ControlLeft}, Code::KeyS));
    add(keyboard.chord({Code::ControlLeft, Code::AltLeft}, Code::KeyL));
    add(keyboard.chord({Code::ControlLeft}, Code::KeyK));
    add(keyboard.chord({Code::ControlLeft}, Code::KeyC));
    add(keyboard.press(Code::ControlLeft));
    add(keyboard.press(Code::KeyD));
    add(keyboard.press(Code::KeyD));
    add(keyboard.press(Code::KeyD));
    add(keyboard.release(Code::KeyD));
    add(keyboard.release(Code::ControlLeft));
    add(keyboard.tap(Code::Backquote));
    add(keyboard.tap(Code::KeyE));
    add(keyboard.chord({Code::AltRight}, Code::KeyQ));
    add(keyboard.chord({Code::AltRight}, Code::KeyL));
    add(keyboard.tap(Code::ShiftLeft));
    add(keyboard.tap(Code::ShiftLeft));
    add(keyboard.tap(Code::KeyA));
    add(keyboard.chord({Code::ShiftLeft}, Code::F6));
    keyboard.setImeComposing(true);
    add(keyboard.tap(Code::KeyB));
    keyboard.setImeComposing(false);
    KeyEvent unknown = test::keyPress(Code::Unknown, Modifier::Ctrl);
    events.push_back(unknown);
    return events;
}

#endif

TEST(InputAlloc, KeystrokePathDoesNotAllocate) {
#if !FROST_INPUT_COUNT_ALLOCATIONS
    GTEST_SKIP() << "sanitizer runtimes replace operator new; the allocation count runs in the debug and release legs";
#else
    {
        CountingWindow probe;
        g_sink = ::operator new(16);
        ::operator delete(g_sink);
        ASSERT_EQ(probe.allocations(), 1U) << "the replacement counts";
    }

    ActionRegistry registry;
    static_cast<void>(test::registerKeymapActions(registry, test::intellij()));
    ActionSpec chord;
    chord.id = "test.chord";
    chord.name = "Chord";
    chord.handler = [](const ActionContext&, const InvokeInfo&) {};
    ASSERT_TRUE(registry.add(std::move(chord)).has_value());
    MemoryKeymapSettings settings(
        KeymapPreferences{.overrides = {{"test.chord", {*parseSequence("Ctrl+K, Ctrl+C", SyntaxMode::Strict)}}}});
    auto service =
        KeymapService::create(registry, settings, KeymapPlatform::Windows, simulatedLayout(SimulatedLayoutId::DeDe));
    ASSERT_TRUE(service.has_value());
    ManualClock clock;
    const std::vector<KeyEvent> events = mixedEvents(clock);
    const ActionContext context = test::allContexts();

    // Warm-up: the index is built and every path ran once.
    std::size_t fires = 0;
    for (const KeyEvent& e : events) {
        fires += service->handle(e, context).outcome == MatchOutcome::Fire ? 1U : 0U;
        static_cast<void>(service->peek(e, context));
    }
    ASSERT_GT(fires, 3U);

    std::size_t outcomes = 0;
    {
        CountingWindow window;
        for (std::size_t i = 0; i < 10'000; ++i) {
            const KeyEvent& e = events[i % events.size()];
            if (i % 2 == 0) {
                outcomes += static_cast<std::size_t>(service->handle(e, context).outcome);
            } else {
                outcomes += static_cast<std::size_t>(service->peek(e, context));
            }
        }
        EXPECT_EQ(window.allocations(), 0U);
        EXPECT_EQ(window.bytes(), 0U);
    }
    EXPECT_GT(outcomes, 0U);
#endif
}

} // namespace
} // namespace frost::input
