cmake_minimum_required(VERSION 3.16)
project(TLCS C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Warnings — treat the codebase seriously
add_compile_options(-Wall -Wextra -Wpedantic -Wconversion -Wshadow)

# Optimization for release
set(CMAKE_C_FLAGS_RELEASE "-O2 -DNDEBUG")
set(CMAKE_C_FLAGS_RELWITHDEBINFO "-O2 -g -DNDEBUG")

# ── Core codec library ──────────────────────────────────────────
add_subdirectory(src)

# ── CLI tools ───────────────────────────────────────────────────
add_subdirectory(tools)

# ── Tests ───────────────────────────────────────────────────────
enable_testing()
add_subdirectory(tests)
