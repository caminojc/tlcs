include("../../../tables.jl")
include("./write_tables_helpers.jl")
include("../../../filt.jl")

cfg = config(16000)

acb_cmf_Tbl_packed = load(string(@__DIR__, "/../../../codebooks/LSF_CBs_25_38bits.jld2"), "LSF_CBs")

io = open(string(@__DIR__, "/smpl_tables.h"),"w")
print(io, "#ifndef SMPL_TABLES_H\n")
print(io, "#define SMPL_TABLES_H\n\n")

print(io, "#include <stdint.h>\n")
print(io, "#include \"smpl_defines.h\" \n\n")
print(io, "#ifdef __cplusplus\n")
print(io, """extern "C" {\n""")
print(io, "#endif\n")
print(io, "#define SMPL_G_ACB_RD_MU", " ", gacb_rd_mu * log2(10), "f\n")
print(io, "#define SMPL_ACBG_N", " ",size(cb_acbgains_Tbl[true])[2], "\n")
print(io, "#define SMPL_ACBG_M", " ", size(cb_acbgains_Tbl[true])[1], "\n")
print(io, "#define SMPL_V_GAIN_MAX_DB", " $(cfg.v_gain_max_db)f\n")
print(io, "#define SMPL_V_GAIN_MIN_DB", " $(cfg.v_gain_min_db)f\n")
print(io, "#define SMPL_V_GAIN_Q_STEP_DB", " $(cfg.v_gain_q_step_db)f\n")
print(io, "extern const int16_t smpl_cb_acbgains_lr_Q14[SMPL_ACBG_N*SMPL_ACBG_M];\n")
print(io, "extern const uint8_t smpl_acbgains_dcmf_lr[(SMPL_ACBG_N+1)*(SMPL_ACBG_N)];\n")
print(io, "extern const int16_t smpl_cb_acbgains_hr_Q14[SMPL_ACBG_N*SMPL_ACBG_M];\n")
print(io, "extern const uint8_t smpl_acbgains_dcmf_hr[(SMPL_ACBG_N+1)*(SMPL_ACBG_N)];\n")
print(io, "#define SMPL_FCBG_V_N", " ", length(fcb_gain_v_pdf_Tbl), "\n")
print(io, "extern const uint8_t smpl_fcbg_v_dcmf[SMPL_FCBG_V_N];\n")
print(io, "#define SMPL_FCBG_V_DELTA_N", " ", length(fcb_gain_v_delta_pdf_Tbl), "\n")
print(io, "extern const uint8_t smpl_fcbg_v_delta_dcmf[SMPL_FCBG_V_DELTA_N];\n")
print(io, "#define SMPL_LTP_INTERPOL_DELAY", " ", cfg.ltp_interpol_delay, "\n")
print(io, "extern const float smpl_interpol_kernel[2*SMPL_LTP_INTERPOL_DELAY];\n")
print(io, "extern const float smpl_plc_inject_coef[2];\n")

print(io, "extern const uint16_t smpl_lsf_interp_cmf[3];\n")
print(io, "#define SMPL_HP_A_LEN 3\n")
print(io, "//extern const float smpl_hp_a2[SMPL_HP_A_LEN];\n")
print(io, "//extern const float smpl_hp_b2[SMPL_HP_A_LEN];\n")
print(io, "#define SMPL_FILTERBANK_A_LEN 3\n")
print(io, "extern const float smpl_filterbankL_coef[SMPL_FILTERBANK_A_LEN];\n")
print(io, "extern const float smpl_filterbankH_coef[SMPL_FILTERBANK_A_LEN];\n")
print(io, "#define SMPL_AP_LEN_32_48 $(length(AP_coefs_32_48))\n")
print(io, "extern const float smpl_ap_coefs_32_48[SMPL_AP_LEN_32_48];\n")
print(io, "#define SMPL_FIR_M_32_48 $(size(FIR_coefs_32_48, 2))\n")
print(io, "#define SMPL_FIR_N_32_48 $(size(FIR_coefs_32_48, 1))\n")
print(io, "extern const float smpl_fir_coefs_32_48[SMPL_FIR_M_32_48][SMPL_FIR_N_32_48];\n")
print(io, "#define SMPL_HB_WGHT_LEN $(length(cfg.highband_wght_coef))\n")
print(io, "extern const float smpl_hb_wght_coef[SMPL_HB_WGHT_LEN];\n")
print(io, "#define SMPL_HB_POST_LEN $(length(cfg.highband_post_coef))\n")
print(io, "extern const float smpl_hb_post_coef[SMPL_HB_POST_LEN];\n")
print(io, "#define SMPL_LB_WGHT_LEN $(length(cfg.lowband_wght_coef))\n")
print(io, "#define SMPL_GEN_LOG_PWR $(cfg.bwe_gen_log_order)f\n")
print(io, "extern const float smpl_lb_wght_coef[SMPL_LB_WGHT_LEN];\n")
print(io, "extern const float smpl_perc_emph_pitch;\n")
print(io, "extern const float smpl_perc_emph_v[ 2];\n")
print(io, "extern const float smpl_perc_emph_uv[2];\n\n")
print(io, "extern const float smpl_lsf_interpol_1;\n")
print(io, "extern const float smpl_lsf_interpol_2[2][2];\n")
print(io, "extern const float smpl_lsf_interpol_4[2][4];\n\n")
print(io, "extern const float smpl_lsf_interpol_dtx_1;\n")
print(io, "extern const float smpl_lsf_interpol_dtx_2[2];\n")
print(io, "extern const float smpl_lsf_interpol_dtx_4[4];\n\n")
print(io, "extern const float smpl_post_tilt_coefs[2][2];\n\n")
print(io, "extern const float smpl_uv_pulse_shaping_coefs[2][2][2];\n\n")
print(io, "#define SMPL_RESNRG_UPD_FACTOR_DTX $(cfg.nrg_res_upd_factor_dtx)f\n\n")
print(io, "extern const float smpl_low_rate_thr[2][4];\n")
print(io, "extern const uint8_t smpl_max_pulses_per_frame[2][3];\n")
print(io, "extern const int smpl_fcb_tot_surv_20ms_max[2];\n")
print(io, "extern const float smpl_vuv_weights[6];\n")
print(io, "extern const uint16_t smpl_vuv_cmfs[3][3];\n\n")
print(io, "extern const float smpl_rate_control_model_comp5[4][2][$(length(view(ratecontrol_models_comp5[20], 2, :)))];\n\n")
print(io, "extern const uint16_t smpl_rate_control_thrs_comp5[4][2];\n\n")
print(io, "extern const float smpl_plc_cng_init[SMPL_LPC_ORDER];\n")

print(io, "#ifdef __cplusplus\n")
print(io, "}\n")
print(io, "#endif\n\n")
print(io, "#endif\n")
close(io)

io = open(string(@__DIR__, "/smpl_tables.c"),"w")
print(io, "#include \"smpl_tables.h\" \n\n")
dcmf_keys = Dict(false => "acbTransition_hr", true => "acbTransition_lr")
for lr in [true, false]
    str = lr ? "_lr" : "_hr"
    print(io, "const int16_t smpl_cb_acbgains$(str)_Q14[SMPL_ACBG_N*SMPL_ACBG_M] = ")
    write_vector(io, float_to_int16.(cb_acbgains_Tbl[lr], 14)[:], 2)
    print(io, "\n")

    print(io, "const uint8_t smpl_acbgains_dcmf$str[(SMPL_ACBG_N+1)*(SMPL_ACBG_N)] = ")
    write_vector(io, permutedims(acb_Tbl_packed[dcmf_keys[lr]])[:], size(acb_Tbl_packed[dcmf_keys[lr]], 2,), "u")
    print(io, "\n")
end

print(io, "const uint8_t smpl_fcbg_v_dcmf[SMPL_FCBG_V_N] = \n")
write_vector(io, fcb_Tbl_packed["fcb_gain_v_dcmf"], 16, "u")
print(io, "\n")

print(io, "const uint8_t smpl_fcbg_v_delta_dcmf[SMPL_FCBG_V_DELTA_N] = \n")
write_vector(io, fcb_Tbl_packed["fcb_gain_v_delta_dcmf"], 16, "u")
print(io, "\n")

print(io, "const float smpl_interpol_kernel[2*SMPL_LTP_INTERPOL_DELAY] = \n")
write_vector(io, cfg.interpol_kernel, 8, "f")
print(io, "\n")

print(io, "const float smpl_plc_inject_coef[2] = ")
write_vector(io, cfg.plc_noise_inject_coef, Inf, "f")
print(io, "\n")

print(io, "const uint16_t smpl_lsf_interp_cmf[3] = {0, 5, 7};\n")

print(io, "// HP filter with cutoff at 50Hz\n")
print(io, "//const float smpl_hp_a2[SMPL_HP_A_LEN] = {1.0f, -1.97362f, 0.9740658f};\n")
print(io, "//const float smpl_hp_b2[SMPL_HP_A_LEN] = {0.986927f, -1.9738388f, 0.986927f};\n")

print(io, "const float smpl_filterbankL_coef[SMPL_FILTERBANK_A_LEN] = "); write_vector(io, cfg.filterbankL_coef, Inf, "f")
print(io, "const float smpl_filterbankH_coef[SMPL_FILTERBANK_A_LEN] = "); write_vector(io, cfg.filterbankH_coef, Inf, "f")
print(io, "const float smpl_ap_coefs_32_48[SMPL_AP_LEN_32_48] = "); write_vector(io, AP_coefs_32_48, Inf, "f")
print(io, "const float smpl_fir_coefs_32_48[SMPL_FIR_M_32_48][SMPL_FIR_N_32_48] = {\n    "); write_float_matrix(io, FIR_coefs_32_48)
print(io, "const float smpl_hb_wght_coef[SMPL_HB_WGHT_LEN] = "); write_vector(io, cfg.highband_wght_coef, Inf, "f")
print(io, "const float smpl_hb_post_coef[SMPL_HB_POST_LEN] = "); write_vector(io, cfg.highband_post_coef, Inf, "f")
print(io, "const float smpl_lb_wght_coef[SMPL_LB_WGHT_LEN] = "); write_vector(io, cfg.lowband_wght_coef, Inf, "f")
print(io, "const float smpl_perc_emph_pitch = $(cfg.perc_emph_pitch)f;\n")
print(io, "const float smpl_perc_emph_v[ 2] = {$(cfg.perc_emph_v[ 0])f, $(cfg.perc_emph_v[ 1])f};\n")
print(io, "const float smpl_perc_emph_uv[2] = {$(cfg.perc_emph_uv[0])f, $(cfg.perc_emph_uv[1])f};\n\n")

print(io, "const float smpl_lsf_interpol_1 = ",cfg.lsf_interpol[1][1][1],"f;\n")
for numsubfr in [2,4]
    print(io, "const float smpl_lsf_interpol_$(numsubfr)[2][$(numsubfr)] = {\n")
    for j = 1 : length(cfg.lsf_interpol[numsubfr])
        print(io, "    {")
        for i = 1 : length(cfg.lsf_interpol[numsubfr][j])
            if i == length(cfg.lsf_interpol[numsubfr][j])
                print(io, cfg.lsf_interpol[numsubfr][j][i], "f")
            else
                print(io, cfg.lsf_interpol[numsubfr][j][i], "f, ")
            end
        end
        if j == length(cfg.lsf_interpol[numsubfr])
            print(io, "}\n")
        else
            print(io, "},\n")
        end
    end
    print(io, "};\n")
end
print(io, "\n")

print(io, "const float smpl_lsf_interpol_dtx_1 = ",cfg.lsf_interpol_dtx[1][1][1],"f;\n")
for numsubfr in [2,4]
    print(io, "const float smpl_lsf_interpol_dtx_$(numsubfr)[$(numsubfr)] = {")
    for i = 1 : length(cfg.lsf_interpol_dtx[numsubfr])
        if i == length(cfg.lsf_interpol_dtx[numsubfr])
            print(io, cfg.lsf_interpol_dtx[numsubfr][i], "f")
        else
            print(io, cfg.lsf_interpol_dtx[numsubfr][i], "f, ")
        end
    end
    print(io, "};\n")
end
print(io, "\n")

print(io, "const float smpl_post_tilt_coefs[2][2] = {{$(cfg.post_tilt_coefs[false][1])f, $(cfg.post_tilt_coefs[false][2])f},     // highRate\n")
print(io, "                                          {$(cfg.post_tilt_coefs[true][1])f, $(cfg.post_tilt_coefs[true][2])f}};   // lowRate\n\n")

print(io, "const float smpl_uv_pulse_shaping_coefs[2][2][2] = {\n")
print(io, "    {{$(cfg.uv_pulse_shaping_coefs[false][1][1])f, $(cfg.uv_pulse_shaping_coefs[false][1][2])f}, {$(cfg.uv_pulse_shaping_coefs[false][2][1])f, $(cfg.uv_pulse_shaping_coefs[false][2][2])f}},     // highRate\n")
print(io, "    {{$(cfg.uv_pulse_shaping_coefs[true][1][1])f, $(cfg.uv_pulse_shaping_coefs[true][1][2])f}, {$(cfg.uv_pulse_shaping_coefs[true][2][1])f, $(cfg.uv_pulse_shaping_coefs[true][2][2])f}}};  // lowRate\n\n")

print(io, "// please refer to the comment for the field `low_rate_thr` in config.jl (for the Julia version of the codec) if you manually adjust these values to find the valid ranges for these parameters\n")
print(io, "const float smpl_low_rate_thr[2][4] = {\n    ")
for fs in [16000, 32000]
    print(io, "{")
    for packet_ms in [10, 20, 60, 120]
        print(io, config_ms.low_rate_thr[fs, packet_ms])
        if (packet_ms != 120)
            print(io, ", ")
        else
            if (fs != 32000)
                print(io, "},\n    ")
            else
                print(io, "}\n")
            end
        end
    end
end
print(io, "};\n")

print(io, "const uint8_t smpl_max_pulses_per_frame[2][3] = {  // [lowRate][BACKGROUND_NOISE/UNVOICED/VOICED]\n    ")
for rate in ["highRate", "lowRate"]
    print(io, "{$(cfg.max_pulses_per_frame[rate][BACKGROUND_NOISE]), $(cfg.max_pulses_per_frame[rate][UNVOICED]), $(cfg.max_pulses_per_frame[rate][VOICED])}")
    if (rate == "highRate")
        print(io, ", ")
    end
end
print(io, "\n};\n")

print(io, "const int smpl_fcb_tot_surv_20ms_max[2] = {")
print(io, "$(cfg.fcb_tot_surv_20ms_max[0]), $(cfg.fcb_tot_surv_20ms_max[1])};\n")

print(io, "const float smpl_vuv_weights[6] = {")
for i = 1 : length(cfg.vuv_weights)
    if i == length(cfg.vuv_weights)
        print(io, cfg.vuv_weights[i], "f")
    else
        print(io, cfg.vuv_weights[i], "f, ")
    end
end
print(io, "}; // weights on : corrs, vad, tilt, harmonicity, short lags\n")

print(io, "const uint16_t smpl_vuv_cmfs[3][3] = {\n")
print(io, "    {", cfg.vuv_cmfs[1][1], ", ", cfg.vuv_cmfs[1][2], ", ", cfg.vuv_cmfs[1][3], "}, // unconditional\n")
print(io, "    {", cfg.vuv_cmfs[2][1], ", ", cfg.vuv_cmfs[2][2], ",  ", cfg.vuv_cmfs[2][3], "}, // prev_voiced = false\n")
print(io, "    {", cfg.vuv_cmfs[3][1], ", ", cfg.vuv_cmfs[3][2], ", ", cfg.vuv_cmfs[3][3], "}  // prev_voiced = true\n")
print(io, "};\n")

print(io, "const float smpl_rate_control_model_comp5[4][2][$(length(view(ratecontrol_models_comp5[20], 2, :)))] = { //[framelenidx][lowrate][]\n    ")
for framelen in [10 20 60 120]
    print(io, "{")
    for lowRate in [0 1]
        print(io, "{")
        coeffs = view(ratecontrol_models_comp5[framelen], lowRate+1, :)
        for i = 1:length(coeffs)
            if i == length(coeffs)
                print(io, coeffs[i], "f")
            else
                print(io, coeffs[i], "f, ")
            end
            if (i == 1length(coeffs))
                print(io, "},\n    ")
            end
        end
    end
    if (framelen != 120)
        print(io, "},\n    ")
    end
end
print(io, "}};\n")

write_matrix(io, Int.(reduce(hcat, last(k) for k in sort(ratecontrol_thrs_comp5))),  "smpl_rate_control_thrs_comp5[4][2]", "uint16_t")

print(io, "const float smpl_plc_cng_init[SMPL_LPC_ORDER] = ");
write_vector(io, Float32[0.065961175, 0.21926339, 0.40487507, 0.59738964,
    0.7911506, 0.98644555, 1.1819322, 1.3775148,
    1.573289, 1.7692552, 1.9650295, 2.1610913,
    2.357345, 2.5532153, 2.7495646, 2.94601], 4, "f")

close(io)
