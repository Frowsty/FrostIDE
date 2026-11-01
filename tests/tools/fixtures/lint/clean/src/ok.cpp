// SPDX-License-Identifier: Apache-2.0
//
// A clean C++ file for the frost-lint tests: valid markers, and banned names that
// appear only in comments and string literals.
#include <string>

// Never call CreateFileA; frost-lint must not flag this comment.
// Budget: B5.
// TODO(M3): a valid to-do marker.
/* TODO(M17): also valid in a block comment; M17 is the highest milestone ID. */
// The TODO window lists markers; this is prose, not a marker.
int f(int x);
void post(int task);
void pre(int task);

void run(bool ready, int task, int* tasks, int n) {
    const char* s = "CreateFileA ^^ template for [: x :] TODO: x";
    char c = '\'';
    int big = 1'000'000;
    if (ready) post(task);
    for (int i = 0; i < n; ++i) pre(tasks[i]);
    if (n > 0) { post(n); }
    auto r = R"raw(contract_assert(x); "quoted" )raw";
    int value = ::f(1) + tasks[::f(2)];
    (void)s; (void)c; (void)big; (void)r; (void)value;
}
