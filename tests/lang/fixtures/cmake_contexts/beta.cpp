#include <span>

int beta() { return static_cast<int>(std::span<const int>{}.size()) + CONTEXT_BETA; }
