# Writes the generated source: cmake -DOUT=<file> -P version.cmake.
file(WRITE "${OUT}" "const char* tiny_version(void) { return \"1.0\"; }\n")
