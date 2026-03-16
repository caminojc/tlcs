project(IntegrationTests)

set(TEST_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
set(OPUS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../smpl_opus")

include_directories(${OPUS_ROOT}/include ${OPUS_ROOT}/silk ${OPUS_ROOT}/celt)
include_directories(${TEST_ROOT}/kissfft)
include_directories(${CMAKE_CURRENT_SOURCE_DIR})

include(FetchContent)
FetchContent_Declare(
  glog
  GIT_REPOSITORY https://github.com/google/glog.git
  GIT_TAG        v0.6.0
)
set(WITH_GFLAGS OFF)
set(WITH_GTEST OFF)
set(WITH_UNWIND OFF)
set(BUILD_SHARED_LIBS OFF)
SET(BUILD_TESTING 0)
FetchContent_MakeAvailable(glog)

include(FetchContent)
FetchContent_Declare(
  fmtlib
  GIT_REPOSITORY https://github.com/fmtlib/fmt.git
  GIT_TAG        9.1.0
)
set(BUILD_SHARED_LIBS 0)
FetchContent_MakeAvailable(fmtlib)

# Common utilities library which all tests can rely on.
add_library(utils STATIC base.cpp dsp_utils.cpp vad.cpp stop_watch.cpp)
add_dependencies(utils opus)
target_link_libraries(utils opus gtest_main glog fmt)

# opus integration tests
macro(add_opus_smpl_test FILE_NAME_PREFIX)
    SET(TEST_NAME opus_smpl_test_${FILE_NAME_PREFIX})
    add_executable(${TEST_NAME} test_setup.cpp ${FILE_NAME_PREFIX}.cpp)
    target_link_libraries(${TEST_NAME} utils)
    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME} WORKING_DIRECTORY ${TEST_ROOT}/..)
endmacro()

add_opus_smpl_test(basic_encode_decode_tests)
add_opus_smpl_test(bitrate_tests)
add_opus_smpl_test(complexity_tests)
add_opus_smpl_test(dtx_tests)
add_opus_smpl_test(fec_tests)
add_opus_smpl_test(plc_tests)
add_opus_smpl_test(sample_rate_tests)
add_opus_smpl_test(stereo_tests)
add_opus_smpl_test(vad_tests)
add_opus_smpl_test(voice_music_detection_tests)
add_opus_smpl_test(repacketizer_tests)
