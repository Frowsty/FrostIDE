int coreA() { return 1; }
#ifdef NDEBUG
int coreReleaseOnly() { return 2; }
#else
int coreDebugOnly() { return 3; }
#endif
