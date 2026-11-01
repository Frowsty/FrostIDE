// SPDX-License-Identifier: Apache-2.0
//
// ThreadSanitizer defaults for the Qt widget tests, compiled into them only in TSan builds
// (FROST_SANITIZE=thread; tests/ui/CMakeLists.txt).
/*
 * Qt's raster engine and QImage scaling hand work to Qt's own thread pool, and Qt synchronises
 * those threads with futex-based QMutex and QSemaphore inside libQt6Core, which is not
 * instrumented. TSan cannot see that synchronisation, so the allocator calls Qt makes on both
 * sides are reported as races (exit status 66) even though every test assertion passes.
 * ignore_noninstrumented_modules=1 drops only interceptor accesses (malloc, free, memcpy, ...)
 * made from uninstrumented modules (Qt and the system libraries); every access in FrostIDE's
 * instrumented code is still checked. FrostIDE's UI code runs on the GUI thread only, so the
 * threads involved are Qt's. TSAN_OPTIONS from the environment is parsed after these defaults
 * and can override them.
 */

#if defined(__has_feature)
#if __has_feature(thread_sanitizer)

// The runtime looks this weak hook up by name at start-up; the reserved name is its API.
extern "C" const char* __tsan_default_options();  // NOLINT(bugprone-reserved-identifier)
extern "C" const char* __tsan_default_options() { // NOLINT(bugprone-reserved-identifier)
    return "ignore_noninstrumented_modules=1";
}

#endif
#endif
