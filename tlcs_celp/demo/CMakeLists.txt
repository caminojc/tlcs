set(INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/../audiofile)

set(EXEC_NAME opus_mlow_demo)
add_executable(${EXEC_NAME} opus_mlow_demo.cc)
target_include_directories(${EXEC_NAME} PRIVATE ${INCLUDE_DIRS})
target_link_libraries(${EXEC_NAME} PRIVATE opus)
