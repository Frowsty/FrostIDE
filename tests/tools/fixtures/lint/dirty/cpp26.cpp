// SPDX-License-Identifier: Apache-2.0
// One banned C++26 construct per line for the frost-lint tests.
#embed "data.bin"
#include <execution>
#include <inplace_vector>
export module fixture;
import std;
module;
int f(int x) pre(x > 0);
auto g(int x) -> int post(r: r > 0);
void h() { contract_assert(true); }
constexpr auto r = ^^int;
using T = [: r :];
void k() { template for (auto x : {1}) {} }
auto l = [](int x) pre(x > 0) { return x; };
