# A toolchain file runs inside the configure: it starts a planted program (FND-8 fixture).
execute_process(COMMAND "${CMAKE_CURRENT_LIST_DIR}/planted/toolchain-hook${CMAKE_EXECUTABLE_SUFFIX}" toolchain)
