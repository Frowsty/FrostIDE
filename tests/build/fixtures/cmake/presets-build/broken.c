/* The broken executable of the build-presets fixture: an unused variable on line 4, an error on line 9. */

static int helper(void) {
    int unused_local = 1;
    return 0;
}

int main(void) {
    return helper() + frost_undeclared_symbol;
}
