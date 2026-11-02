// SPDX-License-Identifier: Apache-2.0
// Stand-in for a discovery reader blocked in a filesystem operation.
#include <chrono>
#include <thread>
int main() {
    std::this_thread::sleep_for(std::chrono::minutes(1));
    return 0;
}
