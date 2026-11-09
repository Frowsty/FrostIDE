// SPDX-License-Identifier: Apache-2.0
//
// The MSVC debuggee (msvc_debug_test.cpp, DBG-2b and DBG-4a): built by the test with cl.exe of the
// pinned MSVC toolset as a Visual Studio Debug build would be (no optimisation, a PDB, incremental
// linking), never by FrostIDE's own clang-cl build. It throws five types in a row, each caught, and
// holds the STL containers whose display the test checks. The FROST_* markers name lines the test
// looks up in this source; the values here are the test's expected values.

#include <array>
#include <cstdio>
#include <deque>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <variant>
#include <vector>

struct UserError {
    int code;
};

void throwKind(int kind) {
    switch (kind) {
    case 0: throw std::logic_error("logic"); // FROST_THROW std::logic_error
    case 1: throw std::runtime_error("runtime"); // FROST_THROW std::runtime_error
    case 2: throw std::range_error("range"); // FROST_THROW std::range_error
    case 3: throw UserError{7}; // FROST_THROW UserError
    default: throw 42; // FROST_THROW int
    }
}

int callee(int x) { // FROST_CALLEE_OPEN
    int y = x * 2; // FROST_CALLEE_FIRST
    return y + 1;
}

int main() {
    std::vector<int> numbers{1, 2, 3};
    std::string text = "hello msvc";
    std::string longText = "a string longer than the small string buffer";
    std::map<int, std::string> table{{1, "one"}, {2, "two"}};
    auto owned = std::make_unique<int>(5);
    std::optional<int> maybe = 9;
    std::optional<int> none;
    // Beyond the DBG-4a acceptance list: the gaps the test reports.
    std::wstring wide = L"wide text";
    std::string_view view = text;
    auto shared = std::make_shared<int>(6);
    std::deque<int> queue{4, 5};
    std::list<int> linked{7, 8};
    std::set<int> sorted{3, 1, 2};
    std::unordered_map<int, int> hashed{{1, 10}};
    std::array<int, 3> fixed{9, 8, 7};
    std::variant<int, std::string> either = 11;
    std::tuple<int, char> pair{12, 'x'};
    int r = callee(3); // FROST_STL_BREAK
    int caught = 0;
    for (int kind = 0; kind < 5; ++kind) {
        try {
            throwKind(kind);
        } catch (...) {
            ++caught;
        }
    }
    std::printf("%d %d %zu %zu %zu %zu %zu %zu %zu %d %zu %d\n", r, caught, numbers.size(), wide.size(), view.size(),
                queue.size(), linked.size(), sorted.size(), hashed.size(), *shared + fixed[0], either.index(),
                std::get<0>(pair));
    return caught == 5 ? 0 : 1;
}
