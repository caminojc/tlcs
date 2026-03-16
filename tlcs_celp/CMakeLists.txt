cmake_minimum_required(VERSION 3.16.3)

project(SmplCodec)


OPTION(BUILD_WITH_SANITIZERS "Build with clang sanitizers" OFF)
if (BUILD_WITH_SANITIZERS)
  add_compile_options(-fsanitize=undefined,address -fno-sanitize-recover=all -fno-omit-frame-pointer -g)
  SET(ENV{UBSAN_OPTIONS} "print_stacktrace=1:halt_on_error=1")
  add_link_options(-fsanitize=undefined,address)
endif()

OPTION(CMAKE_BUILD_PROFILE "Add build flags to support profiling" OFF) # Disabled by default
if (CMAKE_BUILD_PROFILE)
  if(NOT MSVC)
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -pg")
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -pg")
    set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -pg")
    set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -pg")
  endif()
endif()

# Make sure we have a valid build type specified, assume Release by default.
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
else()
  if(NOT (CMAKE_BUILD_TYPE STREQUAL "Debug" OR CMAKE_BUILD_TYPE STREQUAL "Release"))
    message(FATAL_ERROR "Invalid build mode ${CMAKE_BUILD_TYPE}. Choices 'Debug' or 'Release'")
  endif()

  if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -DDEBUG")
    set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -DDEBUG")
  endif()
endif()

OPTION(DUMP_FEATURES "Dump decoder features for ML enhancement" OFF) # Disabled by default
if (DUMP_FEATURES)
    ADD_DEFINITIONS(-DSMPL_DUMP_FEATURES=1)
else()
    ADD_DEFINITIONS(-DSMPL_DUMP_FEATURES=0)
endif()

include(CheckLibraryExists)
CHECK_LIBRARY_EXISTS(m round "" HAVE_LIB_M)
if (HAVE_LIB_M)
    set(EXTRA_LIBS ${EXTRA_LIBS} m)
endif (HAVE_LIB_M)
#set(CMAKE_VERBOSE_MAKEFILE on) # Activate for verbose output on compile parameters

#
# Opus (library) target (`opus`)
#
option(BUILD_USING_WA_CMAKE "Build using CMake file similar to how WA builds opus_mlow" OFF)
if(BUILD_USING_WA_CMAKE)
    set(ARCH_ABI "x86_64")
    if (CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "(arm64|aarch64)")
        set(ARCH_ABI "arm64-v8a")
    elseif (CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^arm")
        set(ARCH_ABI "armeabi-v7a")
    endif()
    message("Building for arch ${ARCH_ABI} (host processor ${CMAKE_HOST_SYSTEM_PROCESSOR})")
    set(WA_BUILD_MLOW_CODEC ON)
    add_subdirectory(wa_build_cfg)
else()
    add_subdirectory(smpl_opus)
endif()

set(CMAKE_CXX_STANDARD 20)
if(NOT MSVC)
  set(CMAKE_C_FLAGS_RELEASE "-O3")
  set(CMAKE_CXX_FLAGS_RELEASE "-O3")
  if (CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "AMD64" OR CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "x86_64")
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -mavx")
    set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -mavx")
  endif()
endif ()
if ((CMAKE_SYSTEM_PROCESSOR MATCHES "^arm") AND NOT (CMAKE_SYSTEM_PROCESSOR MATCHES "arm64"))
  set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -mfpu=neon")
  set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -mfpu=neon")
  set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -mfpu=neon")
  set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -mfpu=neon")
endif ()

set(ALL_INCLUDE_DIRECTORIES
    ${CMAKE_SOURCE_DIR}/smpl_opus/smpl
    ${CMAKE_SOURCE_DIR}/smpl_opus/smpl/pffft
    ${CMAKE_SOURCE_DIR}/audiofile
    ${CMAKE_SOURCE_DIR}/benchmark
    ${CMAKE_SOURCE_DIR}/smpl_opus/include/opus
    ${CMAKE_SOURCE_DIR}/smpl_opus/silk
    ${CMAKE_SOURCE_DIR}/smpl_opus/silk/float
    ${CMAKE_SOURCE_DIR}/smpl_opus/src
    ${CMAKE_SOURCE_DIR}/smpl_opus/celt
    ${CMAKE_SOURCE_DIR}/smpl_opus)

#
# SMPL (dll) target - only used for julia. Includes both SMPL and OPUS sources for all functions to be accessable by julia
# This target only builds when --target SmplCodecDLL is specified
#

# Normally it's not recommended to use GLOB but use it here as opus isn't changing
# These are only used when building for dynamic lib as julia reference some of the opus functions directly
file(GLOB smpl_opus_srcs
        ${CMAKE_SOURCE_DIR}/smpl_opus/src/*.c
        ${CMAKE_SOURCE_DIR}/smpl_opus/silk/*.c
        ${CMAKE_SOURCE_DIR}/smpl_opus/silk/float/*.c
        ${CMAKE_SOURCE_DIR}/smpl_opus/smpl/*.c
        ${CMAKE_SOURCE_DIR}/smpl_opus/smpl/pffft/pffft.c
        ${CMAKE_SOURCE_DIR}/smpl_opus/celt/*.c)
file(GLOB smpl_opus_srcs_rm ${CMAKE_SOURCE_DIR}/smpl_opus/*/*_demo.c ${CMAKE_SOURCE_DIR}/smpl_opus/*/*opus_compare.c)
list(REMOVE_ITEM smpl_opus_srcs ${smpl_opus_srcs_rm})

if(NOT OPUS_DISABLE_SMPL_FASTMATH)
  if (CMAKE_BUILD_TYPE STREQUAL "Release")
      # Use -Ofast for smpl added files (but not files that include opus headers)
      set(smpl_fastmath_srcs
          ${CMAKE_SOURCE_DIR}/smpl_opus/smpl/smpl_celp_util.c
          ${CMAKE_SOURCE_DIR}/smpl_opus/smpl/smpl_filt.c
          ${CMAKE_SOURCE_DIR}/smpl_opus/smpl/smpl_harm_postfilter_util.c
          ${CMAKE_SOURCE_DIR}/smpl_opus/smpl/smpl_lpc_util.c
          ${CMAKE_SOURCE_DIR}/smpl_opus/smpl/smpl_lsf_quant.c
          ${CMAKE_SOURCE_DIR}/smpl_opus/smpl/smpl_pitch_util.c
          ${CMAKE_SOURCE_DIR}/smpl_opus/smpl/smpl_codec_util.c)
      if(NOT MSVC)
        set_source_files_properties( ${smpl_fastmath_srcs} PROPERTIES COMPILE_FLAGS "-Ofast")
      else()
        set_source_files_properties( ${smpl_fastmath_srcs} PROPERTIES COMPILE_FLAGS "/fp:fast")
      endif()
  endif()
endif()

set(smpl_dll_srcs ${smpl_opus_srcs})
set(PROJECT_NAME_DLL ${PROJECT_NAME}DLL)
add_library(${PROJECT_NAME_DLL} SHARED ${smpl_dll_srcs})
target_link_libraries(${PROJECT_NAME_DLL} ${EXTRA_LIBS})
set_target_properties(${PROJECT_NAME_DLL} PROPERTIES EXCLUDE_FROM_ALL TRUE)
target_compile_definitions(${PROJECT_NAME_DLL} PRIVATE OPUS_BUILD FLOAT_APPROX USE_ALLOCA=1 ENABLE_SMPL=1)
target_include_directories(${PROJECT_NAME_DLL} PRIVATE ${ALL_INCLUDE_DIRECTORIES})

#
# SMPL test program target
#

set(PROJECT_NAME_DEMO ${PROJECT_NAME}Demo)
add_executable(${PROJECT_NAME_DEMO} ${CMAKE_SOURCE_DIR}/smpl_opus/smpl/test/smpl_codec.c)
add_dependencies(${PROJECT_NAME_DEMO} opus)
target_compile_definitions(${PROJECT_NAME_DEMO} PRIVATE ENABLE_SMPL=1)
target_link_libraries(${PROJECT_NAME_DEMO} opus ${EXTRA_LIBS})
target_include_directories(${PROJECT_NAME_DEMO} PRIVATE ${ALL_INCLUDE_DIRECTORIES})

#
# FEC / secondary encoder test program
#

set(PROJECT_NAME_FEC_DEMO ${PROJECT_NAME}FECDemo)
add_executable(${PROJECT_NAME_FEC_DEMO} ${CMAKE_SOURCE_DIR}/smpl_opus/smpl/test/smpl_fec_demo.c)
add_dependencies(${PROJECT_NAME_FEC_DEMO} opus)
target_compile_definitions(${PROJECT_NAME_FEC_DEMO} PRIVATE ENABLE_SMPL=1)
target_link_libraries(${PROJECT_NAME_FEC_DEMO} opus ${EXTRA_LIBS})
target_include_directories(${PROJECT_NAME_FEC_DEMO} PRIVATE ${ALL_INCLUDE_DIRECTORIES})

#
# MLow decoder fuzzing target.
#
option(BUILD_DECODER_FUZZER "Build MLow decoder fuzzer (linux only?)" OFF)
if (BUILD_DECODER_FUZZER)
  set(DEC_FUZZER ${PROJECT_NAME}DecoderFuzzer)
  add_executable(${DEC_FUZZER} ${CMAKE_SOURCE_DIR}/smpl_opus/tests/opus_decode_fuzzer.c)
  add_dependencies(${DEC_FUZZER} opus)
  target_compile_options(${DEC_FUZZER} PRIVATE -fsanitize=fuzzer,address -g)
  target_link_libraries(${DEC_FUZZER} opus ${EXTRA_LIBS})
  target_link_options(${DEC_FUZZER} PRIVATE -fsanitize=fuzzer,address)
  target_include_directories(${DEC_FUZZER} PRIVATE ${ALL_INCLUDE_DIRECTORIES})
endif()

# Build opus_mlow_demo binary
add_subdirectory(demo)

#
# GTest for SMPL code, including code coverage
#

# Below blob of code taken from Google Test website
# https://github.com/google/googletest/blob/main/googletest/README.md#incorporating-into-an-existing-cmake-project
include(FetchContent)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG        release-1.12.1
)
# For Windows: Prevent overriding the parent project's compiler/linker settings
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

include_directories(${GTEST_INCLUDE_DIRS})
file(GLOB unittest_srcs ${CMAKE_SOURCE_DIR}/smpl_opus/smpl/test/test_*.cpp)
add_executable(runUnitTests ${unittest_srcs})
add_dependencies(runUnitTests opus)
target_compile_definitions(runUnitTests PRIVATE OPUS_BUILD FLOAT_APPROX USE_ALLOCA=1 ENABLE_SMPL=1)
target_link_libraries(runUnitTests opus gtest_main ${EXTRA_LIBS})
target_include_directories(runUnitTests PRIVATE ${ALL_INCLUDE_DIRECTORIES})
enable_testing()
add_test(NAME runUnitTests COMMAND runUnitTests)

# Build all integration tests. Currently we are not able to build integration
# tests statically, so only build these if dynamic linking is allowed.
if(NOT BUILD_STATIC_LIBS)
  add_subdirectory(test)
endif()

# Benchmarks
# For now one needs to manually run the benchmarks.
add_executable(bench_encode_decode ${CMAKE_SOURCE_DIR}/benchmark/bench_encode_decode.cpp)
add_dependencies(bench_encode_decode opus)
target_compile_definitions(bench_encode_decode PRIVATE ENABLE_SMPL=1)
target_link_libraries(bench_encode_decode opus)
target_include_directories(bench_encode_decode PRIVATE ${ALL_INCLUDE_DIRECTORIES})

#
# Install (copy over) the dynamic library to C folder
#

install(TARGETS ${PROJECT_NAME_DLL} DESTINATION ${CMAKE_SOURCE_DIR} COMPONENT ${PROJECT_NAME_DLL})
