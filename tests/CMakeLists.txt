# ── Tests ───────────────────────────────────────────────────────

add_executable(test_scaffold test_scaffold.c)
target_include_directories(test_scaffold PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(test_scaffold PRIVATE tlcs_core)
add_test(NAME scaffold COMMAND test_scaffold)
