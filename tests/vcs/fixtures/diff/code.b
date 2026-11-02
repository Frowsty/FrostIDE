#include <cstdio>
#include <cstdlib>

int add(int x, int y) {
    return x + y;
}

int sub(int x, int y) {
    return x - y;
}

int main(int argc, char** argv) {
    std::printf("%d
", sub(add(1, 2), argc));
    return EXIT_SUCCESS;
}
