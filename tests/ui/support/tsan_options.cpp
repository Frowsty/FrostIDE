// SPDX-License-Identifier: Apache-2.0
//
// ThreadSanitizer suppressions for the Qt widget tests, compiled into them only in TSan builds
// (FROST_SANITIZE=thread; tests/ui/CMakeLists.txt).
/*
 * Qt's raster engine and QImage scaling hand work to Qt's own thread pool, and Qt synchronises
 * those threads with futex-based QMutex and QSemaphore inside libQt6Core, which is not
 * instrumented. TSan cannot see that synchronisation, so the allocator calls Qt makes on both
 * sides are reported as races (exit status 66) even though every test assertion passes.
 *
 * Each called_from_lib line below makes TSan ignore the interceptor accesses (malloc, free,
 * memcpy, ...) made from code in that one library. Only Qt Core, Gui and Widgets are named:
 * they are the libraries the reports came from, and naming them is enough for every widget
 * test to run without a report (tests under TSAN_OPTIONS=ignore_noninstrumented_modules=0
 * produced 7 to 34 reports per executable, all with Qt frames; with these three lines none).
 * Every other uninstrumented module (libc, libstdc++, glib, fontconfig, freetype, ICU, D-Bus,
 * the platform plugins) and all of FrostIDE's instrumented code stay fully checked. Each line
 * names one library file: a pattern that matched two libraries would stop the TSan runtime.
 *
 * The cost: a real race whose other side is an interceptor call inside one of these three
 * libraries is hidden, for example a QByteArray or QImage copy of FrostIDE-owned memory racing
 * a FrostIDE write. FrostIDE's UI code runs on the GUI thread only and those buffers are
 * immutable once loaded, so nothing such is exercised today; but these executables are no race
 * oracle for FrostIDE code that shares data with Qt's threads. A future test of that kind
 * belongs in its own executable without this file (ADR 0015 decision 11, question 5).
 * TSAN_OPTIONS=suppressions=<file> from the environment adds to these lines.
 */

#if defined(__has_feature)
#if __has_feature(thread_sanitizer)

// The runtime looks this weak hook up by name at start-up; the reserved name is its API.
extern "C" const char* __tsan_default_suppressions();  // NOLINT(bugprone-reserved-identifier)
extern "C" const char* __tsan_default_suppressions() { // NOLINT(bugprone-reserved-identifier)
    return "called_from_lib:libQt6Core.so.6\n"
           "called_from_lib:libQt6Gui.so.6\n"
           "called_from_lib:libQt6Widgets.so.6\n";
}

#endif
#endif
