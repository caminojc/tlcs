using JLD2

# include("../../../includes.jl")
include("write_tables_helpers.jl")
include("../../../shortfuns.jl")

LSF_CBs_packed = load(string(@__DIR__, "/../../../codebooks/LSF_CBs_25_38bits.jld2"), "LSF_CBs")

function get_St2_data(LSF_CBs_packed)
    # Combine all Quantization levels into one vector
    allQlvls = Float32[]
    allQlvlDCMFs = UInt16[]
    lengthCMFs = 0
    N = length(LSF_CBs_packed[1][:St2_hi])
    for i in 1:2
        for sym in [:St2_hi, :St2_lo]
            for n = 1 : N
                Qlvls = LSF_CBs_packed[i][sym][n][:Qlvls]
                DCMFs = LSF_CBs_packed[i][sym][n][:DCMF]
                qstep = LSF_CBs_packed[i][sym][n][:qstep]
                for (Qlvl, DCMF, max_qi, min_qi) in zip(Qlvls, DCMFs, LSF_CBs_packed[i][sym][n][:max_qi], LSF_CBs_packed[i][sym][n][:min_qi])
                    @assert(length(Qlvl) == (max_qi - min_qi + 1))
                    Qlvl = Qlvl ./ qstep .- (min_qi : max_qi)
                    append!(allQlvls, Qlvl)
                    @assert(length(DCMF) == (max_qi - min_qi + 1))
                    append!(allQlvlDCMFs, DCMF)                
                    lengthCMFs += length(DCMF) + 1
                end
            end
        end
    end
    return allQlvls, allQlvlDCMFs, lengthCMFs
end
allQlvls, allQlvlDCMFs, lengthCMFs = get_St2_data(LSF_CBs_packed)
allQlvls .= min.(max.(allQlvls, -0.45f0), 0.45f0)

allQlvls8 = pack8(allQlvls)

cb_v_16  = pack16(LSF_CBs_packed[2][:St1][:cb] .- LSF_CBs_packed[2][:LSFs_mean])
cb_uv_16 = pack16(LSF_CBs_packed[1][:St1][:cb] .- LSF_CBs_packed[1][:LSFs_mean])
# mean(abs2.(cb_v_16.min .+ cb_v_16.data .* cb_v_16.scale .- (LSF_CBs_packed[2][:St1][:cb]  .- LSF_CBs_packed[2][:LSFs_mean])))
cinv_v_16  = pack16(LSF_CBs_packed[2][:St1][:Cinv])
cinv_uv_16 = pack16(LSF_CBs_packed[1][:St1][:Cinv])
rot_cond_v  = [LSF_CBs_packed[2][:St1][:Rot_cond_hi], LSF_CBs_packed[2][:St1][:Rot_cond_lo]]
rot_cond_uv = [LSF_CBs_packed[1][:St1][:Rot_cond_hi], LSF_CBs_packed[1][:St1][:Rot_cond_lo]]
rot_cond_v_8  = pack8(rot_cond_v)
rot_cond_uv_8 = pack8(rot_cond_uv)
# mean(abs2.(rot_cond_v_8.min .+ rot_cond_v_8.data[2] .* rot_cond_v_8.scale .- LSF_CBs_packed[2][:St1][:Rot_cond_lo]))
rot_v_8  = pack8(LSF_CBs_packed[2][:St1][:Rot])
rot_uv_8 = pack8(LSF_CBs_packed[1][:St1][:Rot])

# assure symmetry, and keep only upper triangle
@assert permutedims(cinv_v_16.data,  (2, 1)) == cinv_v_16.data
@assert permutedims(cinv_uv_16.data, (2, 1)) == cinv_uv_16.data
cinv_uv_16 = (min = cinv_uv_16.min, scale = cinv_uv_16.scale, data = reduce(vcat, [cinv_uv_16.data[1:k, k] for k = 1:16]))
cinv_v_16  = (min = cinv_v_16.min,  scale = cinv_v_16.scale,  data = reduce(vcat, [cinv_v_16.data[ 1:k, k] for k = 1:16]))



io = open(string(@__DIR__, "/smpl_lsf_tables.h"),"w")
print(io, "#ifndef SMPL_LSF_TABLES_H\n")
print(io, "#define SMPL_LSF_TABLES_H\n\n")

print(io, "#include <stdint.h> \n")
print(io, "#include \"smpl_defines.h\" \n\n")
print(io, "#ifdef __cplusplus\n")
print(io, """extern "C" {\n""")
print(io, "#endif\n")
print(io, "#define LSF_CB_CENTROIDS ", size(LSF_CBs_packed[1][:St1][:cb], 2), "\n")
print(io, "#define LSF_CB_V_MIN ",   cb_v_16.min, "f\n")
print(io, "#define LSF_CB_V_SCALE ", cb_v_16.scale, "f\n")
print(io, "extern const uint16_t smpl_LSF_cb_v_16[LSF_CB_CENTROIDS][SMPL_LPC_ORDER];\n")
print(io, "#define LSF_CB_UV_MIN ",   cb_uv_16.min, "f\n")
print(io, "#define LSF_CB_UV_SCALE ", cb_uv_16.scale, "f\n")
print(io, "extern const uint16_t smpl_LSF_cb_uv_16[LSF_CB_CENTROIDS][SMPL_LPC_ORDER];\n")
print(io, "#define LSF_CINV_V_MIN ",   cinv_v_16.min, "f\n")
print(io, "#define LSF_CINV_V_SCALE ", cinv_v_16.scale, "f\n")
print(io, "extern const uint16_t smpl_LSF_cinv_v_16[SMPL_LPC_ORDER * (SMPL_LPC_ORDER + 1) / 2];\n")
print(io, "#define LSF_CINV_UV_MIN ",   cinv_uv_16.min, "f\n")
print(io, "#define LSF_CINV_UV_SCALE ", cinv_uv_16.scale, "f\n")
print(io, "extern const uint16_t smpl_LSF_cinv_uv_16[SMPL_LPC_ORDER * (SMPL_LPC_ORDER + 1) / 2];\n")
print(io, "#define LSF_ROT_COND_V_MIN ",   rot_cond_v_8.min, "f\n")
print(io, "#define LSF_ROT_COND_V_SCALE ", rot_cond_v_8.scale, "f\n")
print(io, "extern const uint8_t smpl_LSF_rot_cond_v_8[2][SMPL_LPC_ORDER][SMPL_LPC_ORDER]; // HR/LR\n")
print(io, "#define LSF_ROT_COND_UV_MIN ",   rot_cond_uv_8.min, "f\n")
print(io, "#define LSF_ROT_COND_UV_SCALE ", rot_cond_uv_8.scale, "f\n")
print(io, "extern const uint8_t smpl_LSF_rot_cond_uv_8[2][SMPL_LPC_ORDER][SMPL_LPC_ORDER]; // HR/LR\n")
print(io, "#define LSF_ROT_V_MIN ",   rot_v_8.min, "f\n")
print(io, "#define LSF_ROT_V_SCALE ", rot_v_8.scale, "f\n")
print(io, "extern const uint8_t smpl_LSF_Rot_v_8[LSF_CB_CENTROIDS][SMPL_LPC_ORDER][SMPL_LPC_ORDER];\n")
print(io, "#define LSF_ROT_UV_MIN ",   rot_uv_8.min, "f\n")
print(io, "#define LSF_ROT_UV_SCALE ", rot_uv_8.scale, "f\n")
print(io, "extern const uint8_t smpl_LSF_Rot_uv_8[LSF_CB_CENTROIDS][SMPL_LPC_ORDER][SMPL_LPC_ORDER];\n")

print(io, "#define LSF_QSTEP_COND_MULT ", LSF_CBs_packed[1][:qstep_cond_mult], "f\n")
print(io, "extern const float smpl_LSF_reg_cond[2]; // uv/v\n")
print(io, "extern const float smpl_LSF_mean_v[SMPL_LPC_ORDER];\n")
print(io, "extern const float smpl_LSF_mean_uv[SMPL_LPC_ORDER];\n")
print(io, "extern const uint16_t smpl_LSF_CMF_v[LSF_CB_CENTROIDS+1];\n")
print(io, "extern const uint16_t smpl_LSF_CMF_uv[LSF_CB_CENTROIDS+1];\n")
print(io, "extern const uint16_t smpl_LSF_CMF_cond_v[LSF_CB_CENTROIDS+2];\n")
print(io, "extern const uint16_t smpl_LSF_CMF_cond_uv[LSF_CB_CENTROIDS+2];\n")
print(io, "extern const float smpl_LSF_min_dist_v[SMPL_LPC_ORDER+1];\n")
print(io, "extern const float smpl_LSF_min_dist_uv[SMPL_LPC_ORDER+1];\n")
print(io, "extern const int8_t smpl_LSF_St2_min_qi[2][2][LSF_CB_CENTROIDS+1][SMPL_LPC_ORDER]; // uv/v, HR/LR\n")
print(io, "extern const int8_t smpl_LSF_St2_max_qi[2][2][LSF_CB_CENTROIDS+1][SMPL_LPC_ORDER]; // uv/v, HR/LR\n")
print(io, "extern const float smpl_LSF_qstep[2][2]; // uv/v, HR/LR\n")
print(io, "#define LSF_ST2_ALL_QLVLS_LEN ", length(allQlvls), "\n")
print(io, "#define LSF_ST2_ALL_QLVL_CMFS_LEN ", lengthCMFs, "\n")
print(io, "#define LSF_ST2_ALL_QLVLS_MIN ",   allQlvls8.min, "f\n")
print(io, "#define LSF_ST2_ALL_QLVLS_SCALE ", allQlvls8.scale, "f\n")
print(io, "extern const uint8_t smpl_LSF_St2_all_qlvls_8[LSF_ST2_ALL_QLVLS_LEN]; \n")
print(io, "extern const uint8_t smpl_LSF_St2_all_qlvl_dcmfs[LSF_ST2_ALL_QLVLS_LEN]; \n")
print(io, "#ifdef __cplusplus\n")
print(io, "}\n")
print(io, "#endif\n\n")
print(io, "#endif\n")
close(io)

function write_float_3Dim(io, MM, name)
    print(io, "const float $name = {\n")
    for k in 1:length(MM)
        print(io, "{\n    ")       
        write_float_matrix(io, MM[k], false)
    end
    print(io, "};\n")
end
function write_uint16_3Dim(io, MM, name)
    print(io, "const uint16_t $name = {\n")
    for k in 1:length(MM)
        print(io, "{\n    ")       
        write_matrix(io, MM[k], "u", false)
    end
    print(io, "};\n")
end
function write_uint8_3Dim(io, MM, name)
    print(io, "const uint8_t $name = {\n")
    for k in 1:length(MM)
        print(io, "{\n    ")       
        write_matrix(io, MM[k], "u", false)
    end
    print(io, "};\n")
end


io = open(string(@__DIR__, "/smpl_lsf_tables_st1.c"),"w")
print(io, "#include \"smpl_lsf_tables.h\" \n\n")
write_matrix(io, cb_v_16.data,  "smpl_LSF_cb_v_16[LSF_CB_CENTROIDS][SMPL_LPC_ORDER]", "uint16_t")
write_matrix(io, cb_uv_16.data, "smpl_LSF_cb_uv_16[LSF_CB_CENTROIDS][SMPL_LPC_ORDER]", "uint16_t")
print(io, "const uint16_t smpl_LSF_cinv_v_16[SMPL_LPC_ORDER * (SMPL_LPC_ORDER + 1) / 2] = ")
write_vector(io, cinv_v_16.data, 16, "u")
print(io, "const uint16_t smpl_LSF_cinv_uv_16[SMPL_LPC_ORDER * (SMPL_LPC_ORDER + 1) / 2] = ")
write_vector(io, cinv_uv_16.data, 16, "u")
write_uint8_3Dim(io, rot_cond_v_8.data,  "smpl_LSF_rot_cond_v_8[2][SMPL_LPC_ORDER][SMPL_LPC_ORDER]")
write_uint8_3Dim(io, rot_cond_uv_8.data, "smpl_LSF_rot_cond_uv_8[2][SMPL_LPC_ORDER][SMPL_LPC_ORDER]")
write_uint8_3Dim(io, rot_v_8.data,  "smpl_LSF_Rot_v_8[LSF_CB_CENTROIDS][SMPL_LPC_ORDER][SMPL_LPC_ORDER]")
write_uint8_3Dim(io, rot_uv_8.data, "smpl_LSF_Rot_uv_8[LSF_CB_CENTROIDS][SMPL_LPC_ORDER][SMPL_LPC_ORDER]")

print(io, "const float smpl_LSF_reg_cond[2] = {",LSF_CBs_packed[1][:reg_cond],"f, ", LSF_CBs_packed[2][:reg_cond],"f};\n")

print(io, "const float smpl_LSF_mean_v[SMPL_LPC_ORDER] = {\n    ")
for j = 1 : length(LSF_CBs_packed[2][:LSFs_mean])-1    
    print(io, LSF_CBs_packed[2][:LSFs_mean][j],"f, ")
end
print(io, LSF_CBs_packed[2][:LSFs_mean][end],"f\n}; \n")

print(io, "const float smpl_LSF_mean_uv[SMPL_LPC_ORDER] = {\n    ")
for j = 1 : length(LSF_CBs_packed[1][:LSFs_mean])-1    
    print(io, LSF_CBs_packed[1][:LSFs_mean][j],"f, ")
end
print(io, LSF_CBs_packed[1][:LSFs_mean][end],"f\n}; \n")

print(io, "const uint16_t smpl_LSF_CMF_v[LSF_CB_CENTROIDS+1] = {\n    ")
for j = 1 : length(LSF_CBs_packed[2][:St1][:CMF])-1    
    print(io, LSF_CBs_packed[2][:St1][:CMF][j],"u, ")
end
print(io, LSF_CBs_packed[2][:St1][:CMF][end],"u\n}; \n")

print(io, "const uint16_t smpl_LSF_CMF_uv[LSF_CB_CENTROIDS+1] = {\n    ")
for j = 1 : length(LSF_CBs_packed[1][:St1][:CMF])-1    
    print(io, LSF_CBs_packed[1][:St1][:CMF][j],"u, ")
end
print(io, LSF_CBs_packed[1][:St1][:CMF][end],"u\n}; \n")

print(io, "const uint16_t smpl_LSF_CMF_cond_v[LSF_CB_CENTROIDS+2] = {\n    ")
for j = 1 : length(LSF_CBs_packed[2][:St1][:CMF_cond])-1    
    print(io, LSF_CBs_packed[2][:St1][:CMF_cond][j],"u, ")
end
print(io, LSF_CBs_packed[2][:St1][:CMF_cond][end],"u\n}; \n")

print(io, "const uint16_t smpl_LSF_CMF_cond_uv[LSF_CB_CENTROIDS+2] = {\n    ")
for j = 1 : length(LSF_CBs_packed[1][:St1][:CMF_cond])-1    
    print(io, LSF_CBs_packed[1][:St1][:CMF_cond][j],"u, ")
end
print(io, LSF_CBs_packed[1][:St1][:CMF_cond][end],"u\n}; \n")

print(io, "const float smpl_LSF_min_dist_v[SMPL_LPC_ORDER+1] = {\n    ")
for j = 1 : length(LSF_CBs_packed[2][:min_dist])-1    
    print(io, LSF_CBs_packed[2][:min_dist][j],"f, ")
end
print(io, LSF_CBs_packed[2][:min_dist][end],"f\n}; \n")

print(io, "const float smpl_LSF_min_dist_uv[SMPL_LPC_ORDER+1] = {\n    ")
for j = 1 : length(LSF_CBs_packed[1][:min_dist])-1    
    print(io, LSF_CBs_packed[1][:min_dist][j],"f, ")
end
print(io, LSF_CBs_packed[1][:min_dist][end],"f\n}; \n")

N = length(LSF_CBs_packed[2][:St2_lo])
M = length(LSF_CBs_packed[2][:St2_lo][1][:min_qi])
qi = zeros(Int8, M, N)

for name in ["min", "max"]
    print(io, "const int8_t smpl_LSF_St2_$(name)_qi[2][2][LSF_CB_CENTROIDS+1][SMPL_LPC_ORDER] = {\n")
    for i in 1:2
        print(io, "{\n")
        for sym in [:St2_hi, :St2_lo]
            for n = 1 : N
                if name == "min"
                    qi[:,n] = LSF_CBs_packed[i][sym][n][:min_qi]
                else
                    qi[:,n] = LSF_CBs_packed[i][sym][n][:max_qi]
                end
            end
            print(io, "{\n    ")
            write_matrix(io, qi, "", false)
        end
        if i == 2
            print(io, "}\n")
        else
            print(io, "},\n")        
        end
    end
    print(io, "};\n")
end

close(io)


io = open(string(@__DIR__, "/smpl_lsf_tables_st2.c"), "w")
print(io, "#include \"smpl_lsf_tables.h\" \n\n")

print(io, "const float smpl_LSF_qstep[2][2] = { ")
print(io, "{", LSF_CBs_packed[1][:qstep_hi],"f, ", LSF_CBs_packed[1][:qstep_lo], "f}, ")
print(io, "{", LSF_CBs_packed[2][:qstep_hi],"f, ", LSF_CBs_packed[2][:qstep_lo], "f} }; \n")

print(io, "const uint8_t smpl_LSF_St2_all_qlvls_8[LSF_ST2_ALL_QLVLS_LEN] = {\n    ")
for j = 1 : length(allQlvls8.data)-1
    lineend = mod(j, 32) == 0 ? "\n    " : "" 
    print(io, allQlvls8.data[j], "u, $lineend")
end
print(io, allQlvls8.data[end], "u\n}; \n")

print(io, "const uint8_t smpl_LSF_St2_all_qlvl_dcmfs[LSF_ST2_ALL_QLVLS_LEN] = {\n    ")
for j = 1 : length(allQlvlDCMFs)-1    
    lineend = mod(j, 32) == 0 ? "\n    " : "" 
    print(io, allQlvlDCMFs[j], "u, $lineend")
end
print(io, allQlvlDCMFs[end],"u\n}; \n")

close(io)

