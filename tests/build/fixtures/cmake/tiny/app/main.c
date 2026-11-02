#include "core.h"

int main(void) { return tiny_core(-1) + (tiny_version()[0] == '1' ? 0 : 1); }
