// SPDX-License-Identifier: Apache-2.0
//
// Tests for frost::function_ref: binding lambdas, functors and function
// pointers, return-type conversion, and that it references (not copies) its target.

#include "frost/compat/function_ref.hpp"

#include <gtest/gtest.h>

#include <string>
#include <type_traits>

namespace {

int twice(int x) { return 2 * x; }

// Calls through the reference; function_ref is meant to be passed by value like this.
int apply(frost::function_ref<int(int)> fn, int x) { return fn(x); }

TEST(FunctionRef, BindsLambda) {
    int offset = 5; // Non-const so the lambda genuinely needs its capture.
    EXPECT_EQ(apply([offset](int x) { return x + offset; }, 1), 6);
}

TEST(FunctionRef, BindsFunctionPointer) {
    EXPECT_EQ(apply(&twice, 21), 42);
    EXPECT_EQ(apply(twice, 4), 8);
}

TEST(FunctionRef, ReferencesTargetInsteadOfCopying) {
    // A stateful functor: the counter must change in the original object.
    struct Counter {
        int calls = 0;
        int operator()(int x) { return x + ++calls; }
    } counter;
    frost::function_ref<int(int)> ref(counter);
    EXPECT_EQ(ref(10), 11);
    EXPECT_EQ(ref(10), 12);
    EXPECT_EQ(counter.calls, 2);
}

TEST(FunctionRef, ConvertsReturnType) {
    // The target returns int; the signature asks for long long. The lambda is named
    // so it outlives the function_ref (binding a temporary would dangle).
    auto seven = [] { return 7; };
    frost::function_ref<long long()> ref(seven);
    EXPECT_EQ(ref(), 7LL);
}

TEST(FunctionRef, VoidSignatureDiscardsResult) {
    int calls = 0;
    auto counted = [&calls] { return ++calls; };
    frost::function_ref<void()> ref(counted);
    ref();
    ref();
    EXPECT_EQ(calls, 2);
}

TEST(FunctionRef, ForwardsReferenceArguments) {
    std::string text = "a";
    auto append = [](std::string& s) { s += 'b'; };
    frost::function_ref<void(std::string&)> ref(append);
    ref(text);
    EXPECT_EQ(text, "ab");
}

TEST(FunctionRef, IsTriviallyCopyable) {
    static_assert(std::is_trivially_copyable_v<frost::function_ref<void()>>);
    static_assert(sizeof(frost::function_ref<void()>) <= 2 * sizeof(void*));
}

} // namespace
