# ── TLCS core library ───────────────────────────────────────────

add_library(tlcs_core STATIC
    # Common
    common/tlcs_config.c
    common/tlcs_frame.c

    # Bitstream
    bitstream/tlcs_bitstream.c

    # WAV I/O
    wav/tlcs_wav.c

    # Entropy coding
    entropy/tlcs_range_coder.c
    entropy/tlcs_ec_models.c

    # Codec
    codec/tlcs_lpc.c
    codec/tlcs_pitch.c
    codec/tlcs_codebook.c
    codec/tlcs_qmf.c
    codec/tlcs_lsf_vq.c
    codec/tlcs_lsf_vq_tables.c
    codec/tlcs_encoder.c
    codec/tlcs_decoder.c
    codec/tlcs_diag_encode.c
)

target_link_libraries(tlcs_core PRIVATE m)  # libm for math functions

target_include_directories(tlcs_core
    PUBLIC  ${PROJECT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)
