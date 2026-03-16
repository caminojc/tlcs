include("write_tables_helpers.jl")

nrgres_CBs_packed = load(string(@__DIR__, "/../../../codebooks/nrgres_tables.jld2"), "RESNRG_GAIN_CBs")
nrgres_CBs = nrgres_CBs_unpack(nrgres_CBs_packed)

cfg = config(16000)

io = open(string(@__DIR__, "/smpl_nrgres_tables.h"),"w")
print(io, "#ifndef SMPL_NRGRES_TABLES_H\n")
print(io, "#define SMPL_NRGRES_TABLES_H\n\n")
print(io, "#include <stdint.h>\n\n")
print(io, "#ifdef __cplusplus\n")
print(io, """extern "C" {\n""")
print(io, "#endif\n")
print(io, "#define SMPL_RES_NRG_MIN_DB", " ", Int(cfg.res_nrg_min_db), "\n")
print(io, "#define SMPL_RES_NRG_MAX_DB", " ", Int(cfg.res_nrg_max_db), "\n")
print(io, "#define SMPL_RES_NRG_BIAS", " ", 10 ^ (cfg.res_nrg_min_db / 10), "f\n")
print(io, "extern const int16_t smpl_nrg_step_db_Q14[3];\n")
print(io, "#define SMPL_RES_NRG_SHAPE_CB_N_4", " ", size(nrgres_CBs[(; numsubfrs=4)].nrgres_shape_CB)[2], "\n")
print(io, "extern const int16_t nrgres_shape_CB_4_Q10[SMPL_RES_NRG_SHAPE_CB_N_4 * 4];\n")
print(io, "extern const uint8_t nrgres_shape_CB_4_dcmf[SMPL_RES_NRG_SHAPE_CB_N_4];\n")
print(io, "#define SMPL_RES_NRG_SHAPE_CB_N_2", " ", size(nrgres_CBs[(; numsubfrs=2)].nrgres_shape_CB)[2], "\n")
print(io, "extern const int16_t nrgres_shape_CB_2_Q10[SMPL_RES_NRG_SHAPE_CB_N_2 * 2];\n")
print(io, "extern const uint8_t nrgres_shape_CB_2_dcmf[SMPL_RES_NRG_SHAPE_CB_N_2];\n")
print(io, "#define SMPL_RES_NRG_Q_STEPS_1", " ", length(nrgres_CBs[(; numsubfrs=1)].nrgres_gain_cmf)-1, "\n")
print(io, "#define SMPL_RES_NRG_Q_STEPS_2", " ", length(nrgres_CBs[(; numsubfrs=2)].nrgres_gain_cmf)-1, "\n")
print(io, "#define SMPL_RES_NRG_Q_STEPS_4", " ", length(nrgres_CBs[(; numsubfrs=4)].nrgres_gain_cmf)-1, "\n")
print(io, "extern const uint8_t smpl_nrgres_gain_1_dcmf[SMPL_RES_NRG_Q_STEPS_1];\n")
print(io, "extern const uint8_t smpl_nrgres_gain_2_dcmf[SMPL_RES_NRG_Q_STEPS_2];\n")
print(io, "extern const uint8_t smpl_nrgres_gain_4_dcmf[SMPL_RES_NRG_Q_STEPS_4];\n")
print(io, "#define SMPL_UV_FCBG_MIN_DB", " ", Int(cfg.uv_gain_min_db), "\n")
print(io, "#define SMPL_UV_FCBG_MAX_DB", " ", Int(cfg.uv_gain_max_db), "\n")
print(io, "#define SMPL_UV_GAIN_Q_STEP_DB", " ", Int(cfg.uv_gain_q_step_db), "\n")
print(io, "#define SMPL_UV_GAIN_IDX_LEN (SMPL_UV_FCBG_MAX_DB - SMPL_UV_FCBG_MIN_DB) / SMPL_UV_GAIN_Q_STEP_DB\n")
print(io, "#define SMPL_N_PULSES_STEP ", nrgres_CBs[(; numsubfrs=4)].n_pulses_step, "\n")
print(io, "#define SMPL_FCB_G_OFFSET_STEPS (SMPL_UV_FCBG_MAX_DB - SMPL_RES_NRG_MIN_DB) - (SMPL_UV_FCBG_MIN_DB - SMPL_RES_NRG_MAX_DB) + 1\n")
print(io, "#define SMPL_FCB_G_OFFSET_CMFS 4\n")
print(io, "extern const uint8_t smpl_fcbg_offset_dcmf[3][SMPL_FCB_G_OFFSET_CMFS][SMPL_FCB_G_OFFSET_STEPS];\n")
print(io, "#ifdef __cplusplus\n")
print(io, "}\n")
print(io, "#endif\n\n")
print(io, "#endif\n")
close(io)

io = open(string(@__DIR__, "/smpl_nrgres_tables.c"),"w")
print(io, "#include \"smpl_nrgres_tables.h\"\n\n")

print(io, "const int16_t smpl_nrg_step_db_Q14[3] = {\n   ")
print(io, float_to_int16(nrgres_CBs[(; numsubfrs=1)].nrg_step_db, 14),", ")
print(io, float_to_int16(nrgres_CBs[(; numsubfrs=2)].nrg_step_db, 14),", ")    
print(io, float_to_int16(nrgres_CBs[(; numsubfrs=4)].nrg_step_db, 14),"\n};\n")

for numsubfrs in [4, 2]
    print(io, "const int16_t nrgres_shape_CB_$(numsubfrs)_Q10[SMPL_RES_NRG_SHAPE_CB_N_$numsubfrs * $numsubfrs] = {\n")
    CB = nrgres_CBs[(; numsubfrs)].nrgres_shape_CB
    for j = 1 : size(CB)[2]-1
        print(io, "    ", float_to_int16(CB[1,j], 10),", ")
        for i = 2 : (size(CB)[1]-1)
            print(io, float_to_int16(CB[i,j], 10),", ")
        end
        print(io, float_to_int16(CB[size(CB)[1],j], 10),",\n")
    end
    print(io, "    ", float_to_int16(CB[1,end], 10),", ")
    for i = 2 : size(CB)[1]-1
        print(io, float_to_int16(CB[i,end], 10),", ")
    end    
    print(io, float_to_int16(CB[end,end],10),",")
    print(io, "\n")
    print(io, "};\n\n")
    print(io, "const uint8_t nrgres_shape_CB_$(numsubfrs)_dcmf[SMPL_RES_NRG_SHAPE_CB_N_$numsubfrs] = ")
    write_vector(io, view(nrgres_CBs[(; numsubfrs)].nrgres_shape_cmf, :) |> cmf_to_dcmf, Inf, "u")
end

for numsubfrs in [1, 2, 4]
    print(io, "const uint8_t smpl_nrgres_gain_$(numsubfrs)_dcmf[SMPL_RES_NRG_Q_STEPS_$numsubfrs] = ")
    write_vector(io, view(nrgres_CBs[(; numsubfrs)].nrgres_gain_cmf, :) |> cmf_to_dcmf, Inf, "u")
end

print(io, "const uint8_t smpl_fcbg_offset_dcmf[3][SMPL_FCB_G_OFFSET_CMFS][SMPL_FCB_G_OFFSET_STEPS] = {\n")
for numsubfrs in [1, 2, 4]
    print(io, "{\n    ")
    write_matrix(io, nrgres_CBs_packed[(; numsubfrs)].fcbg_offset_dcmf |> permutedims, "u", false)
end
print(io, "};\n")

close(io)


# max_db = ceil(cfg.uv_gain_max_db - cfg.res_nrg_min_db - minimum(nrgres_CBs[(; numsubfrs=4)].nrgres_shape_CB)) # lowest resnrg, highest fcbgain
# min_db = floor(cfg.uv_gain_min_db - cfg.res_nrg_max_db - maximum(nrgres_CBs[(; numsubfrs=4)].nrgres_shape_CB)) # highest resnrg, lowest fcbgain

# start_ix = Int((cfg.uv_gain_min_db - cfg.res_nrg_max_db) - min_db)
# len = Int((cfg.uv_gain_max_db - cfg.res_nrg_min_db) - (cfg.uv_gain_min_db - cfg.res_nrg_max_db) + 1)
# end_ix = Int(start_ix + len)

# cmf = nrgres_CBs[(; numsubfrs=4)].fcbg_offset_cmf
# p1 = plot(diff(cmf[1,:]))
# plot!(p1, diff(cmf[1,start_ix : end_ix]))
# p2 = plot(diff(cmf[2,:]))
# p3 = plot(diff(cmf[3,:]))
# p4 = plot(diff(cmf[4,:]))
# display(plot(p1, p2, p3, p4))

