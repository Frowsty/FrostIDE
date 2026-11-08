# The resolve-context fixture of lang_cmake_tests (CMK-4, CPP-3): two libraries list shared.h and
# compile with different defines, so clangd's diagnostics for the header name the target whose
# flags it was parsed with. alpha also has an include directory, a system include directory, a
# define whose value is quoted and a precompiled header; beta asks for C++20 (a -std fragment).
cmake_minimum_required(VERSION 3.21)
project(Contexts CXX)

add_library(alpha STATIC alpha.cpp shared.h)
target_compile_definitions(alpha PRIVATE CONTEXT_ALPHA=1 "QUOTED=\"\"")
target_include_directories(alpha PRIVATE inc)
target_include_directories(alpha SYSTEM PRIVATE sys)
target_precompile_headers(alpha PRIVATE pch.h)

add_library(beta STATIC beta.cpp shared.h)
target_compile_definitions(beta PRIVATE CONTEXT_BETA=1)
target_compile_features(beta PRIVATE cxx_std_20)
