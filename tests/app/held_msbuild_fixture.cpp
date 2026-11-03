// SPDX-License-Identifier: Apache-2.0
// A real child that holds a compile-command evaluation until the IDE terminates it.
#include <chrono>
#include <thread>

int main() {
    std::this_thread::sleep_for(std::chrono::minutes(2));
    return 0;
}
