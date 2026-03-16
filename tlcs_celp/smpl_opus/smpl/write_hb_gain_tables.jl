include("../../../tables.jl")
include("write_tables_helpers.jl")


# Header
open(string(@__DIR__, "/smpl_hb_gain_tables.h"),"w") do io
    print(io, "#include \"smpl_defines.h\"\n")
    print(io, "#include <stdint.h>\n\n")

    print(io, "#ifdef __cplusplus\n")
    print(io, """extern "C" {\n""")
    print(io, "#endif\n")

    for (lr, v, sz) in Iterators.product([true, false], [true, false], [10, 20])
        v_str     = v ? "V" : "UV"
        lr_str    = lr ? "LR" : "HR"
        tail      = "$(sz)_$(v_str)_$(lr_str)"
        CB_packed = bwe_gain_CBs_packed[sz][(; lowRate=lr, voiced=v)]
        N         = size(CB_packed.CB.data)[2]
    end

    print(io, "extern const int       hb_gain_vq_sizes[2][2][2];      // 10/20, uv/v, hr/lr\n")
    print(io, "extern const uint8_t*  hb_gain_vq_cb_dshapes[2][2][2]; // 10/20, uv/v, hr/lr\n")
    print(io, "extern const float     hb_gain_vq_cb_min[2][2][2];     // 10/20, uv/v, hr/lr\n")
    print(io, "extern const float     hb_gain_vq_cb_scale[2][2][2];   // 10/20, uv/v, hr/lr\n")
    print(io, "extern const uint8_t*  hb_gain_vq_dcmfs[2][2][2];      // 10/20, uv/v, hr/lr\n")
    print(io, "extern const float     hb_gain_vq_lambdas[2][2][2];    // 10/20, uv/v, hr/lr\n")
    print(io, "extern const float     hb_gain_vq_pwrs[2][2][2];       // 10/20, uv/v, hr/lr\n")

    print(io, "#ifdef __cplusplus\n")
    print(io, "}\n")
    print(io, "#endif\n")
end


# Eventually the corresponding source code below should load from the packed CBs
# rather than storing the unpacked CBs directly (this is an optimization)
open(string(@__DIR__, "/smpl_hb_gain_tables.c"),"w") do io
    print(io, "#include \"smpl_hb_gain_tables.h\"\n\n")
    for (lr, v, sz) in Iterators.product([true, false], [true, false], [10, 20])
        v_str     = v ? "V" : "UV"
        lr_str    = lr ? "LR" : "HR"
        tail      = "$(sz)_$(v_str)_$(lr_str)"
        CB_packed = bwe_gain_CBs_packed[sz][(; lowRate=lr, voiced=v)]
        N         = size(CB_packed.CB.data)[2]

        print(io, "#define SMPL_HB_GAIN_CB_N_$(tail)", " ", N, "\n")
        print(io, "#define HB_GAIN_PWR_$(tail)", " ", CB_packed.hb_gain_pwr, "f\n\n")
        print(io, "const uint8_t hb_gain_cb_dshapes_$(tail)[SMPL_HB_GAIN_CB_N_$(tail)*$(sz ÷ 5)] = ")
        write_vector(io, view(CB_packed.CB.data, :), (sz ÷ 5), "u")
        print(io, "#define HB_GAIN_CB_MIN_$(tail)", " ", CB_packed.CB.min, "f", "\n")
        print(io, "#define HB_GAIN_CB_SCALE_$(tail)", " ", CB_packed.CB.scale, "f",  "\n")
        print(io, "const uint8_t hb_gain_dcmf_$(tail)[SMPL_HB_GAIN_CB_N_$(tail)] = ")
        write_vector(io, CB_packed.DCMF, 16, "u")
        print(io, "#define SMPL_HB_GAIN_CB_LAMBDA_$(tail)", " ", CB_packed.lambda, "f", "\n")
        print(io, "\n")
    end


    print(io, "const int hb_gain_vq_sizes[2][2][2] = {\n")
    print(io, "{\n    ")
    write_matrix(io, permutedims("SMPL_HB_GAIN_CB_N_10_" .* join.(Iterators.product(["UV", "V"], ["HR", "LR"]), "_")), "", false)
    print(io, "{\n    ")
    write_matrix(io, permutedims("SMPL_HB_GAIN_CB_N_20_" .* join.(Iterators.product(["UV", "V"], ["HR", "LR"]), "_")), "", false)
    print(io, "};\n\n")

    print(io, "const uint8_t* hb_gain_vq_cb_dshapes[2][2][2] = {\n")
    print(io, "{\n    ")
    write_matrix(io, permutedims("hb_gain_cb_dshapes_10_" .* join.(Iterators.product(["UV", "V"], ["HR", "LR"]), "_")), "", false)
    print(io, "{\n    ")
    write_matrix(io, permutedims("hb_gain_cb_dshapes_20_" .* join.(Iterators.product(["UV", "V"], ["HR", "LR"]), "_")), "", false)
    print(io, "};\n\n")

    print(io, "const float hb_gain_vq_cb_min[2][2][2] = {\n")
    print(io, "{\n    ")
    write_matrix(io, permutedims("HB_GAIN_CB_MIN_10_" .* join.(Iterators.product(["UV", "V"], ["HR", "LR"]), "_")), "", false)
    print(io, "{\n    ")
    write_matrix(io, permutedims("HB_GAIN_CB_MIN_20_" .* join.(Iterators.product(["UV", "V"], ["HR", "LR"]), "_")), "", false)
    print(io, "};\n\n")

    print(io, "const float hb_gain_vq_cb_scale[2][2][2] = {\n")
    print(io, "{\n    ")
    write_matrix(io, permutedims("HB_GAIN_CB_SCALE_10_" .* join.(Iterators.product(["UV", "V"], ["HR", "LR"]), "_")), "", false)
    print(io, "{\n    ")
    write_matrix(io, permutedims("HB_GAIN_CB_SCALE_20_" .* join.(Iterators.product(["UV", "V"], ["HR", "LR"]), "_")), "", false)
    print(io, "};\n\n")

    print(io, "const uint8_t* hb_gain_vq_dcmfs[2][2][2] = {\n")
    print(io, "{\n    ")
    write_matrix(io, permutedims("hb_gain_dcmf_10_" .* join.(Iterators.product(["UV", "V"], ["HR", "LR"]), "_")), "", false)
    print(io, "{\n    ")
    write_matrix(io, permutedims("hb_gain_dcmf_20_" .* join.(Iterators.product(["UV", "V"], ["HR", "LR"]), "_")), "", false)
    print(io, "};\n\n")

    print(io, "const float hb_gain_vq_lambdas[2][2][2] = {\n")
    print(io, "{\n    ")
    write_matrix(io, permutedims("SMPL_HB_GAIN_CB_LAMBDA_10_" .* join.(Iterators.product(["UV", "V"], ["HR", "LR"]), "_")), "", false)
    print(io, "{\n    ")
    write_matrix(io, permutedims("SMPL_HB_GAIN_CB_LAMBDA_20_" .* join.(Iterators.product(["UV", "V"], ["HR", "LR"]), "_")), "", false)
    print(io, "};\n\n")

    print(io, "const float hb_gain_vq_pwrs[2][2][2] = {\n")
    print(io, "{\n    ")
    write_matrix(io, permutedims("HB_GAIN_PWR_10_" .* join.(Iterators.product(["UV", "V"], ["HR", "LR"]), "_")), "", false)
    print(io, "{\n    ")
    write_matrix(io, permutedims("HB_GAIN_PWR_20_" .* join.(Iterators.product(["UV", "V"], ["HR", "LR"]), "_")), "", false)
    print(io, "};\n\n")
end
