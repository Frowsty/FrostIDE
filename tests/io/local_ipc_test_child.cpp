// SPDX-License-Identifier: Apache-2.0
// Child owner intentionally terminated by the local IPC recovery integration test.
#include "frost/io/local_ipc.hpp"

#include <chrono>
#include <thread>
int main(int argc, char** argv) {
    if (argc != 2)
        return 2;
    auto owner = frost::io::LocalIpcOwner::claim(argv[1]);
    if (!owner)
        return 3;
    if (!(*owner)->start([](std::string message) -> frost::Result<std::string> { return message; }))
        return 4;
    std::this_thread::sleep_for(std::chrono::seconds(60));
    return 0;
}
