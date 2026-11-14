// SPDX-License-Identifier: Apache-2.0
//
// Windows diagnostics regressions: module references outlive a caller's FreeLibrary, unwind
// metadata must be in that retained image, and copied-stack bounds exclude original frame pointers.
#include "frost/diag/module_identity.hpp"
#include "frost/diag/thread_sampler.hpp"
#include "frost/io/utf.hpp"

#include "win_image.hpp"
#include "win_sampler_retry.hpp"
#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <utility>

namespace {

namespace detail = frost::diag::detail;

TEST(ThreadSamplerWindows, RetriesOnlyImageLossAndDiscardsEveryUnfinishedWalk) {
    frost::diag::StackSample sample;
    unsigned attempts = 0;
    unsigned yields = 0;
    const auto yield = [&] { ++yields; };
    const auto captured = detail::captureWithRetries(
        sample,
        [&]() -> frost::Result<detail::CaptureAttempt> {
            EXPECT_EQ(sample.count, 0U);
            EXPECT_EQ(sample.outcome, frost::diag::StackSample::Outcome::Empty);
            ++attempts;
            sample.count = 1;
            sample.frames[0] = attempts;
            if (attempts < detail::kMaxCaptureAttempts) {
                return detail::CaptureAttempt::ImageUnavailable;
            }
            sample.outcome = frost::diag::StackSample::Outcome::Captured;
            return detail::CaptureAttempt::Finished;
        },
        yield);
    ASSERT_TRUE(captured.has_value());
    EXPECT_EQ(attempts, detail::kMaxCaptureAttempts);
    EXPECT_EQ(yields, detail::kMaxCaptureAttempts - 1);
    EXPECT_TRUE(sample.captured());
    EXPECT_EQ(sample.frames[0], detail::kMaxCaptureAttempts);

    attempts = 0;
    yields = 0;
    const auto exhausted = detail::captureWithRetries(
        sample,
        [&]() -> frost::Result<detail::CaptureAttempt> {
            ++attempts;
            sample.count = 7;
            return detail::CaptureAttempt::ImageUnavailable;
        },
        yield);
    ASSERT_FALSE(exhausted.has_value());
    EXPECT_EQ(exhausted.error().code(), frost::ErrorCode::Unsupported);
    EXPECT_EQ(attempts, detail::kMaxCaptureAttempts);
    EXPECT_EQ(yields, detail::kMaxCaptureAttempts - 1);
    EXPECT_EQ(sample.count, 0U);
    EXPECT_FALSE(sample.captured());

    for (const auto outcome :
         {frost::diag::StackSample::Outcome::Busy, frost::diag::StackSample::Outcome::ResumeFailed}) {
        attempts = 0;
        yields = 0;
        const auto unavailable = detail::captureWithRetries(
            sample,
            [&]() -> frost::Result<detail::CaptureAttempt> {
                ++attempts;
                sample.outcome = outcome;
                return detail::CaptureAttempt::Finished;
            },
            yield);
        EXPECT_TRUE(unavailable.has_value());
        EXPECT_EQ(attempts, 1U);
        EXPECT_EQ(yields, 0U);
        EXPECT_EQ(sample.outcome, outcome);
        EXPECT_EQ(sample.count, 0U);
    }

    attempts = 0;
    yields = 0;
    const auto invalidMetadata = detail::captureWithRetries(
        sample,
        [&]() -> frost::Result<detail::CaptureAttempt> {
            ++attempts;
            return frost::fail(frost::ErrorCode::Unsupported, "metadata has no lifetime owner");
        },
        yield);
    ASSERT_FALSE(invalidMetadata.has_value());
    EXPECT_EQ(invalidMetadata.error().message(), "metadata has no lifetime owner");
    EXPECT_EQ(attempts, 1U);
    EXPECT_EQ(yields, 0U);
    EXPECT_EQ(sample.count, 0U);
}

class LoadedProbe {
public:
    LoadedProbe() {
        const auto path = frost::io::toWide(FROST_DIAG_UNLOAD_PROBE);
        if (path) {
            m_handle =
                LoadLibraryExW(path->c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        }
    }
    ~LoadedProbe() {
        if (m_handle != nullptr) {
            FreeLibrary(m_handle);
        }
    }
    LoadedProbe(const LoadedProbe&) = delete;
    LoadedProbe& operator=(const LoadedProbe&) = delete;
    [[nodiscard]] std::uintptr_t address() const {
        return m_handle == nullptr
                   ? 0
                   : reinterpret_cast<std::uintptr_t>(GetProcAddress(m_handle, "frost_diag_unload_probe"));
    }
    [[nodiscard]] HMODULE release() noexcept { return std::exchange(m_handle, nullptr); }

private:
    HMODULE m_handle = nullptr;
};

TEST(ModuleIdentityWindows, TemporaryReferenceKeepsImageMappedUntilItsMetadataHasBeenRead) {
    LoadedProbe loaded;
    const std::uintptr_t address = loaded.address();
    ASSERT_NE(address, 0U);
    {
        auto reference = detail::ImageReference::acquire(address);
        ASSERT_TRUE(reference.has_value());
        ASSERT_TRUE(FreeLibrary(loaded.release()));
        // The original loader reference is gone. Both PE and unwind data must remain mapped
        // through our scoped reference, which must be releasable rather than pinning forever.
        const auto image = detail::imageView(*reference);
        ASSERT_TRUE(image.has_value());
        EXPECT_TRUE(image->contains(address, 1));
        const auto identity = frost::diag::moduleForAddress(address);
        ASSERT_TRUE(identity.has_value()) << identity.error().describe();
        EXPECT_EQ(identity->base, image->base);
        EXPECT_EQ(identity->size, image->size);
        EXPECT_EQ(identity->kind, frost::diag::ModuleIdentity::Kind::Pdb70);
    }
    MEMORY_BASIC_INFORMATION region{};
    ASSERT_NE(VirtualQuery(reinterpret_cast<const void*>(address), &region, sizeof(region)), 0U);
    EXPECT_EQ(region.State, MEM_FREE);
    EXPECT_FALSE(frost::diag::moduleForAddress(address).has_value());
}

TEST(ThreadSamplerWindows, RejectsFunctionTablesThatAreNotOwnedByTheRetainedImage) {
    LoadedProbe loaded;
    ASSERT_NE(loaded.address(), 0U);
    auto reference = detail::ImageReference::acquire(loaded.address());
    ASSERT_TRUE(reference.has_value());
    const auto image = detail::imageView(*reference);
    ASSERT_TRUE(image.has_value());
    DWORD64 imageBase = 0;
    const RUNTIME_FUNCTION* function = RtlLookupFunctionEntry(loaded.address(), &imageBase, nullptr);
    ASSERT_NE(function, nullptr);
    ASSERT_TRUE(image->ownsFunctionEntry(function, imageBase));
    const RUNTIME_FUNCTION externalTable = *function;
    // A dynamically registered table may describe the same addresses, but a module reference
    // does not retain its external allocation. Do not dereference or pass it to the unwinder.
    EXPECT_FALSE(image->ownsFunctionEntry(&externalTable, imageBase));
    EXPECT_FALSE(image->ownsFunctionEntry(function, imageBase + 1));
    const auto* unaligned = reinterpret_cast<const RUNTIME_FUNCTION*>(reinterpret_cast<std::uintptr_t>(function) + 1);
    EXPECT_FALSE(image->ownsFunctionEntry(unaligned, imageBase));
}

TEST(ThreadSamplerWindows, RejectsFramePointersOutsideTheCopyWithoutReadingTheirGuardPage) {
    LoadedProbe loaded;
    ASSERT_NE(loaded.address(), 0U);
    auto reference = detail::ImageReference::acquire(loaded.address());
    ASSERT_TRUE(reference.has_value());
    const auto image = detail::imageView(*reference);
    ASSERT_TRUE(image.has_value());
    DWORD64 imageBase = 0;
    const RUNTIME_FUNCTION* function = RtlLookupFunctionEntry(loaded.address(), &imageBase, nullptr);
    ASSERT_NE(function, nullptr);
    ASSERT_TRUE(image->ownsFunctionEntry(function, imageBase));
    const auto* info = reinterpret_cast<const unsigned char*>(imageBase + function->UnwindData);
    ASSERT_EQ(info[3] & 15U, 5U) << "the fixture must retain its RBP frame pointer";
    const DWORD64 frameOffset = static_cast<DWORD64>(info[3] >> 4U) * 16;
    std::array<DWORD64, 256> copy{};
    const auto low = reinterpret_cast<DWORD64>(copy.data());
    const DWORD64 high = low + sizeof(copy);
    CONTEXT context{};
    context.Rip = loaded.address() + info[1]; // After the frame pointer is established in the prologue.
    context.Rsp = low;
    context.Rbp = low + frameOffset + 128;
    ASSERT_TRUE(detail::imageUnwindSupported(*image, function, imageBase, context, low, high));
    void* guard = VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS);
    ASSERT_NE(guard, nullptr);
    context.Rbp = reinterpret_cast<DWORD64>(guard);
    EXPECT_FALSE(detail::imageUnwindSupported(*image, function, imageBase, context, low, high));
    context.Rbp = high + frameOffset;
    EXPECT_FALSE(detail::imageUnwindSupported(*image, function, imageBase, context, low, high));
    EXPECT_TRUE(VirtualFree(guard, 0, MEM_RELEASE));
}

TEST(ThreadSamplerWindows, SamplingDoesNotResumeAnotherOwnersSuspensionOrEnterTheLoader) {
    std::atomic<bool> ready{false};
    std::atomic<bool> release{false};
    std::unique_ptr<frost::diag::IThreadSampler> sampler;
    bool heapLocked = false;
    std::thread target([&] {
        auto created = frost::diag::createSamplerForCurrentThread();
        if (created) {
            sampler = std::move(*created);
        }
        if (sampler) {
            heapLocked = HeapLock(GetProcessHeap()) != FALSE;
        }
        ready.store(true, std::memory_order_release);
        while (!release.load(std::memory_order_acquire)) {
            YieldProcessor();
        }
        if (heapLocked) {
            HeapUnlock(GetProcessHeap());
        }
    });
    while (!ready.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    if (!sampler || !heapLocked) {
        release = true;
        target.join();
        FAIL() << "bounded sampler creation or target heap lock failed";
    }
    const HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME, FALSE, static_cast<DWORD>(sampler->threadId()));
    if (thread == nullptr) {
        release = true;
        target.join();
        FAIL() << "cannot open the sample target";
    }
    const DWORD previousSuspendCount = SuspendThread(thread);
    frost::diag::StackSample stack;
    // Destroy the Result temporary while the other owner still suspends the heap-locked
    // target. Returning even an empty Error would allocate/free Debug STL iterator proxies.
    const bool completed = sampler->sample(stack).has_value();
    const DWORD remainingSuspendCount = ResumeThread(thread);
    CloseHandle(thread);
    release = true;
    target.join();
    EXPECT_EQ(previousSuspendCount, 0U);
    EXPECT_EQ(remainingSuspendCount, 1U) << "the sampler must undo only its own suspension";
    EXPECT_TRUE(completed);
    EXPECT_EQ(stack.outcome, frost::diag::StackSample::Outcome::Busy);
    EXPECT_EQ(stack.count, 0U);
    EXPECT_FALSE(stack.captured());
}

TEST(ThreadSamplerWindows, BoundedNativeUnwindRejectsAnUnownedLeafStackBeforeReadingIt) {
    const auto unwind = detail::virtualUnwind2();
    ASSERT_NE(unwind, nullptr);
    std::array<DWORD64, 8> copy{};
    DWORD64 low = reinterpret_cast<DWORD64>(copy.data());
    DWORD64 high = low + sizeof(copy);
    void* guard = VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS);
    ASSERT_NE(guard, nullptr);
    CONTEXT context{};
    context.Rip = reinterpret_cast<DWORD64>(&detail::virtualUnwind2);
    context.Rsp = reinterpret_cast<DWORD64>(guard);
    void* handlerData = nullptr;
    DWORD64 frame = 0;
    const LONG status = unwind(UNW_FLAG_NHANDLER, 0, context.Rip, nullptr, &context, nullptr, &handlerData, &frame,
                               nullptr, &low, &high, nullptr, 0);
    constexpr LONG kStatusBadStack = static_cast<LONG>(0xC0000028UL);
    EXPECT_EQ(status, kStatusBadStack);
    EXPECT_TRUE(VirtualFree(guard, 0, MEM_RELEASE));
}

} // namespace
