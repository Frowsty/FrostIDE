#define SCALE 3

#ifdef FROST_NOT_DEFINED
int disabledCode = 1;
#endif

struct Point {
    int x;
};

int scaled(int factor) {
    Point point{factor};
    int local = point.x * SCALE;
    return local;
}
