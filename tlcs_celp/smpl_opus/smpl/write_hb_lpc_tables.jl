include("../../../entropy.jl")
include("../../../tables.jl")
include("write_tables_helpers.jl")

HB_LPC_CBs_packed  = load(string(@__DIR__, "/../../../codebooks/BWE_LPC_CBs.jld2"), "BWE_LPC_CBs")

# Header
open(string(@__DIR__, "/smpl_hb_lpc_tables.h"),"w") do io
    print(io, "#ifndef SMPL_HB_LPC_H\n")
    print(io, "#define SMPL_HB_LPC_H\n\n")

    print(io, "#include \"smpl_defines.h\"\n")
    print(io, "#include <stdint.h>\n\n")

    print(io, "#ifdef __cplusplus\n")
    print(io, """extern "C" {\n""")
    print(io, "#endif\n")
    print(io, "#define SMPL_HB_LPC_CB_N_COND $(size(HB_LPC_CBs[(lowRate = true, voiced = false)].cmf_cond, 2))", "\n")

    for (lr, v) in Iterators.product([true, false], [true, false])
        v_str  = v ? "V" : "UV"
        lr_str = lr ? "LR" : "HR"
        tail   = "$(v_str)_$(lr_str)"
        CB_packed = HB_LPC_CBs_packed[(; lowRate=lr, voiced=v)]
        N      = size(CB_packed.cb_lsf.dlsf_q8, 2)
    end

    print(io, "extern const int16_t   hb_lpc_vq_sizes[2][2];     // uv/v, hr/lr\n")
    print(io, "extern const uint8_t*  hb_lpc_vq_dcmfs[2][2];     // uv/v, hr/lr\n")
    print(io, "extern const float*    hb_lpc_vq_cb_lsfs[2][2];   // uv/v, hr/lr\n")
    print(io, "extern const uint8_t*  hb_lpc_vq_dcmfs_cond[2];   // uv/v\n")
    print(io, "extern const int8_t*   hb_lpc_vq_sel_cond[2];     // uv/v\n")
    print(io, "extern const float     hb_lpc_vq_lambdas[2][2];   // uv/v, hr/lr\n")
    print(io, "extern const uint8_t*  hb_lpc_vq_cb_dlsf[2][2];   // uv/v, hr/lr\n")
    print(io, "extern const uint8_t*  hb_lpc_vq_cb_scales[2][2]; // uv/v, hr/lr\n")
    print(io, "extern const int16_t*  hb_lpc_vq_cb_min[2][2];    // uv/v, hr/lr\n")

    print(io, "#ifdef __cplusplus\n")
    print(io, "}\n")
    print(io, "#endif\n\n")
    print(io, "#endif\n")
end

open(string(@__DIR__, "/smpl_hb_lpc_tables.c"),"w") do io
    print(io, "#include \"smpl_hb_lpc_tables.h\"\n\n")
    for (lr, v) in Iterators.product([true, false], [true, false])
        v_str  = v ? "V" : "UV"
        lr_str = lr ? "LR" : "HR"
        tail   = "$(v_str)_$(lr_str)"
        CB_packed = HB_LPC_CBs_packed[(; lowRate=lr, voiced=v)]
        N      = size(CB_packed.cb_lsf.dlsf_q8, 2)

        print(io, "#define SMPL_HB_LPC_CB_N_$(tail)", " ", N, "\n")
        print(io, "const uint8_t hb_lpc_dcmf_$(tail)[SMPL_HB_LPC_CB_N_$(tail)] = ")
        write_vector(io, CB_packed.dcmf, 24, "u")
        print(io, "#define SMPL_HB_LPC_LAMBDA_$(tail)", " ", CB_packed.lambda, "f", "\n")
        print(io, "const uint8_t hb_lpc_vq_cb_dlsf_Q8_$(tail)[SMPL_HB_LPC_CB_N_$(tail)*SMPL_HB_LPC_ORDER] = ")
        write_vector(io, view(CB_packed.cb_lsf.dlsf_q8, :), 16, "u")
        print(io, "const uint8_t hb_lpc_vq_cb_scales_Q8_$(tail)[SMPL_HB_LPC_ORDER] = ")
        write_vector(io, view(CB_packed.cb_lsf.scales_q8, :), 16, "u")
        print(io, "const int16_t hb_lpc_vq_cb_min_Q15_$(tail)[SMPL_HB_LPC_ORDER] = ")
        write_vector(io, view(CB_packed.cb_lsf.min_q15, :), 16)
        if lr
            print(io, "const uint8_t hb_lpc_dcmf_cond_$(v_str)[(SMPL_HB_LPC_CB_N_$(tail)+1)*SMPL_HB_LPC_CB_N_COND] = ")
            write_vector(io, view(CB_packed.dcmf_cond, :), min(N+1, 16), "u")
            print(io, "const int8_t hb_lpc_sel_cond_$(v_str)[SMPL_HB_LPC_CB_N_$(tail)] = ")
            write_vector(io, CB_packed.sel_cond .- 1, 16) # Julia -> C indexing
        end
        print(io, "\n")
    end

    print(io, "const int16_t hb_lpc_vq_sizes[2][2] = {\n    ")
    write_matrix(io, permutedims("SMPL_HB_LPC_CB_N_" .* join.(Iterators.product(["UV", "V"], ["HR", "LR"]), "_")), "", true)
    print(io, "\n")

    print(io, "const uint8_t* hb_lpc_vq_dcmfs[2][2] = {\n    ")
    write_matrix(io, permutedims("hb_lpc_dcmf_" .* join.(Iterators.product(["UV", "V"], ["HR", "LR"]), "_")), "", true)
    print(io, "\n")

    print(io, "const uint8_t* hb_lpc_vq_dcmfs_cond[2] = ")
    write_vector(io, permutedims("hb_lpc_dcmf_cond_" .* join.(Iterators.product(["UV", "V"]), "_")), true)
    print(io, "\n")

    print(io, "const int8_t* hb_lpc_vq_sel_cond[2] = ")
    write_vector(io, permutedims("hb_lpc_sel_cond_" .* join.(Iterators.product(["UV", "V"]), "_")), true)
    print(io, "\n")

    print(io, "const float hb_lpc_vq_lambdas[2][2] = {\n    ")
    write_matrix(io, permutedims("SMPL_HB_LPC_LAMBDA_" .* join.(Iterators.product(["UV", "V"], ["HR", "LR"]), "_")), "", true)
    print(io, "\n")

    print(io, "const uint8_t* hb_lpc_vq_cb_dlsf[2][2] = {\n    ")
    write_matrix(io, permutedims("hb_lpc_vq_cb_dlsf_Q8_" .* join.(Iterators.product(["UV", "V"], ["HR", "LR"]), "_")), "", true)

    print(io, "const uint8_t* hb_lpc_vq_cb_scales[2][2] = {\n    ")
    write_matrix(io, permutedims("hb_lpc_vq_cb_scales_Q8_" .* join.(Iterators.product(["UV", "V"], ["HR", "LR"]), "_")), "", true)

    print(io, "const int16_t* hb_lpc_vq_cb_min[2][2] = {\n    ")
    write_matrix(io, permutedims("hb_lpc_vq_cb_min_Q15_" .* join.(Iterators.product(["UV", "V"], ["HR", "LR"]), "_")), "", true)
end