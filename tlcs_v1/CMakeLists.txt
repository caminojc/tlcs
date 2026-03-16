cmake_minimum_required(VERSION 3.16)
project(TLCS_V1 C)

set(CMAKE_C_STANDARD 11)

add_library(tlcs SHARED
    src/tlcs_encoder.c
    src/tlcs_decoder.c
    src/tlcs_lpc.c
    src/tlcs_pitch.c
    src/tlcs_algebraic_cb.c
    src/tlcs_postfilter.c
    src/tlcs_quantization.c
    src/tlcs_bitstream.c
)
target_include_directories(tlcs PUBLIC include src)
target_compile_definitions(tlcs PRIVATE TLCS_BUILD)
if(NOT MSVC)
    target_compile_options(tlcs PRIVATE -O3 -ffast-math -Wall)
endif()
target_link_libraries(tlcs m)

add_executable(tlcs_demo demo/tlcs_demo.c)
target_link_libraries(tlcs_demo tlcs m)
target_include_directories(tlcs_demo PRIVATE include src)
