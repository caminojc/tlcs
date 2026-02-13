# ── CLI tools ───────────────────────────────────────────────────

add_executable(tlcs_enc tlcs_enc.c)
target_link_libraries(tlcs_enc PRIVATE tlcs_core)

add_executable(tlcs_dec tlcs_dec.c)
target_link_libraries(tlcs_dec PRIVATE tlcs_core)

add_executable(tlcs_diag tlcs_diag.c)
target_link_libraries(tlcs_diag PRIVATE tlcs_core)

add_executable(qmf_test qmf_test.c)
target_link_libraries(qmf_test PRIVATE tlcs_core)
