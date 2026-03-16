using Libdl

function filt_ma_c(x::AbstractVector{Float32}, coef::AbstractVector{Float32}, state=Float32[]::AbstractVector{Float32})
    N = length(x)
    order = length(coef) - 1
    if isempty(state)
        append!(state, zeros(Float32, order))
    else
        @assert length(state) == order "$(length(state)) $order"
    end
    y = similar(x)
    x = [state; x]
    state .= @views x[end-order+1:end]
    ccall(Libdl.dlsym(lib, :smpl_filt_ma), Cvoid, (Ptr{Float32}, Int, Ptr{Float32}, Int, Ptr{Float32}), view(x, order+1:length(x)), Int(N), coef, Int(order+1), y)
    y
end

function filt_ar_c(x::AbstractVector{Float32}, coef::AbstractVector{Float32}, state=Float32[]::AbstractVector{Float32})
    @assert coef[1] == 1
    N = length(x)
    order = length(coef) - 1
    if isempty(state)
        append!(state, zeros(Float32, order))
    else
        @assert length(state) == order "$(length(state)) $order"
    end
    y = [state; similar(x)]

    if order == 4
        ccall(Libdl.dlsym(lib, :smpl_filt_ar4),  Cvoid, (Ptr{Float32}, Int, Ptr{Float32}, Ptr{Float32}), x, Int(N), coef, view(y, order+1:length(y)))
    elseif order == 16
        ccall(Libdl.dlsym(lib, :smpl_filt_ar16), Cvoid, (Ptr{Float32}, Int, Ptr{Float32}, Ptr{Float32}), x, Int(N), coef, view(y, order+1:length(y)))
    else
        @assert false "filter order ($order) must be 4 or 16"
    end
    state .= @views y[end-order+1:end]
    y[order+1:end]
end

# 1st order MA filter
function filt_ma1_c(x::AbstractVector{Float32}, coef_ma::AbstractVector{Float32}, state=Float32[]::AbstractVector{Float32})
    @assert length(coef_ma) == 2
    if isempty(state)
        append!(state, zeros(Float32, 1))
    else
        @assert length(state) == 1
    end
    N = length(x)
    y = similar(x)

    order = 1;
    ccall(Libdl.dlsym(lib, :smpl_filt_ma1), Cvoid, (Ptr{Float32}, Int, Ptr{Float32}, Int, Ptr{Float32}, Int, Ptr{Float32}), x, Int(N), coef_ma, length(coef_ma), state, Int(order), y)
    y
end

# 2nd order MA filter
function filt_ma2_c(x::AbstractVector{Float32}, coef_ma::AbstractVector{Float32}, state=Float32[]::AbstractVector{Float32})
    @assert length(coef_ma) == 3
    if isempty(state)
        append!(state, zeros(Float32, 2))
    else
        @assert length(state) == 2
    end
    N = length(x)
    y = similar(x)

    order = 2;
    ccall(Libdl.dlsym(lib, :smpl_filt_ma2), Cvoid, (Ptr{Float32}, Int, Ptr{Float32}, Int, Ptr{Float32}, Int, Ptr{Float32}), x, Int(N), coef_ma, length(coef_ma), state, Int(order), y)
    y
end

# 1st order AR filter
function filt_ar1_c(x::AbstractVector{Float32}, coef_ar::AbstractVector{Float32}, state=Float32[]::AbstractVector{Float32})
    @assert coef_ar[1] == 1
    @assert length(coef_ar) == 2
    if isempty(state)
        append!(state, zeros(Float32, 1))
    else
        @assert length(state) == 1
    end
    N = length(x)
    y = similar(x)

    order = 1;
    ccall(Libdl.dlsym(lib, :smpl_filt_ar1), Cvoid, (Ptr{Float32}, Int, Ptr{Float32}, Int, Ptr{Float32}, Int, Ptr{Float32}), x, Int(N), coef_ar, length(coef_ar), state, Int(order), y)
    y
end

# 2nd order AR filter
function filt_ar2_c(x::AbstractVector{Float32}, coef_ar::AbstractVector{Float32}, state=Float32[]::AbstractVector{Float32})
    @assert coef_ar[1] == 1
    @assert length(coef_ar) == 3
    if isempty(state)
        append!(state, zeros(Float32, 2))
    else
        @assert length(state) == 2
    end
    N = length(x)
    y = similar(x)

    order = 2;
    ccall(Libdl.dlsym(lib, :smpl_filt_ar2), Cvoid, (Ptr{Float32}, Int, Ptr{Float32}, Int, Ptr{Float32}, Int, Ptr{Float32}), x, Int(N), coef_ar, length(coef_ar), state, Int(order), y)
    y
end

# 1st order ARMA filter, does not have to be monic (coef_ma[1] can differ from 1)
function filt_arma1_c(x::AbstractVector{Float32}, coef_ma::AbstractVector{Float32}, coef_ar::AbstractVector{Float32}, state=Float32[]::AbstractVector{Float32})
    @assert coef_ar[1] == 1
    @assert length(coef_ar) == 2
    @assert length(coef_ma) == 2
    if isempty(state)
        append!(state, zeros(Float32, 2))
    else
        @assert length(state) == 2
    end
    N = length(x)
    y = similar(x)

    order = 2;
    ccall(Libdl.dlsym(lib, :smpl_filt_arma1), Cvoid, (Ptr{Float32}, Int, Ptr{Float32}, Int, Ptr{Float32}, Int, Ptr{Float32}, Int, Ptr{Float32}), x, Int(N), coef_ma, Int(order), coef_ar, Int(order), state, Int(order), y)
    y
end

# 2nd order ARMA filter, does not have to be monic (coef_ma[1] can differ from 1)
function filt_arma2_c(x::AbstractVector{Float32}, coef_ma::AbstractVector{Float32}, coef_ar::AbstractVector{Float32}, state=Float32[]::AbstractVector{Float32})
    @assert coef_ar[1] == 1
    @assert length(coef_ar) == 3
    @assert length(coef_ma) == 3
    if isempty(state)
        append!(state, zeros(Float32, 4))
    else
        @assert length(state) == 4
    end
    N = length(x)
    y = similar(x)
    N == 0 && return y

    order = 3;
    ccall(Libdl.dlsym(lib, :smpl_filt_arma2), Cvoid, (Ptr{Float32}, Int, Ptr{Float32}, Int, Ptr{Float32}, Int, Ptr{Float32}, Int, Ptr{Float32}), 
        x, Int(N), coef_ma, Int(order), coef_ar, Int(order), state, Int(order+1), y)
    y
end

function lsf2a_silk(lsf)
    order = length(lsf)
    @assert order == 4 || order == 10 || order == 16
    a_Q12 = zeros(Int16, order)
    lsf_Q15 = Int16.(round.(lsf .* (2^15) ./ pi))
    arch = 0 # For now run without optimization. Arch should be retrieved with opus_cpu_feature_check() globally
    ccall(Libdl.dlsym(lib, :silk_NLSF2A), Cvoid, (Ptr{Int16}, Ptr{Int32}, Int32, Int32), a_Q12, lsf_Q15, Int32(order), arch)
    return [1.0f0; .-a_Q12 ./ (2.0f0^12)]
end

function lsf2a_smpl(lsf)
    order = length(lsf)
    @assert order == 4 || order == 10 || order == 16
    a = zeros(Float32, order + 1)
    ccall(Libdl.dlsym(lib, :smpl_NLSF2A), Cvoid, (Ptr{Float32}, Ptr{Float32}, Int32), a, lsf, Int32(order))
    a = lpc_stabilize(a)
    return a
end

function a2lsf_silk(a)
    order = length(a) - 1
    a_Q16 = Int32.(.-round.(a[2:(order+1)] .* 2^16))
    lsf_Q15 = zeros(Int16, order)
    ccall(Libdl.dlsym(lib, :silk_A2NLSF), Cvoid, (Ptr{Int16}, Ptr{Int32}, Int32), lsf_Q15, a_Q16, Int32(order))
    return (lsf_Q15 ./ (2.0f0^15)) * pi
end

function rc2a_c(rc)
    order = length(rc)
    a = zeros(Float32, order+1)
    ccall(Libdl.dlsym(lib, :smpl_rc2a), Cvoid, (Ptr{Float32}, Int32, Ptr{Float32}), Float32.(rc), order, a)
    a
end

lpc_is_stable_c(a) = ccall(Libdl.dlsym(lib, :smpl_lpc_is_stable), Int, (Ptr{Float32}, Int32), Float32.(a), length(a)-1) == 1

function lsf_weights_laroia_c(lsfs)
    W = zeros(Float32, 16)
    ccall(Libdl.dlsym(lib, :smpl_lsf_weights_laroia), Cvoid, (Ptr{Float32}, Ptr{Float32}), Float32.(lsfs), W)
    W
end

function spec_fact2_c(c)
    A = zeros(Float32, 3)
    ccall(Libdl.dlsym(lib, :smpl_spec_fact2), Cvoid, (Ptr{Float32}, Ptr{Float32}), Float32.(c), A)
    A
end

function create_ec_encoder(buf_bytes)
    ec_enc = ccall(Libdl.dlsym(lib, :smpl_create_ec_encoder), Ptr{Cvoid}, (UInt32,), buf_bytes)
end

function free(obj)
    obj = ccall(Libdl.dlsym(lib, :smpl_free), Int, (Ptr{Cvoid},), obj)
end

function ec_enc_bits(ec_enc, bits::UInt32, nbits)
    @assert nbits < 26 "Opus code cant handle more than 25 bits at a time"
    ret = ccall(Libdl.dlsym(lib, :smpl_ec_enc_bits), Int, (Ptr{Cvoid}, UInt32, UInt32), ec_enc, bits, nbits)
end

function ec_encode(ec_enc, sym::Integer, cmf::AbstractVector{UInt16})
    @assert 0 <= sym <= length(cmf) - 2 "cmf index: $sym, cmf length: $(length(cmf))"
    if cmf[1] == 0
        ccall(Libdl.dlsym(lib, :smpl_ec_encode), Cvoid, (Ptr{Cvoid}, UInt32, UInt32, UInt32), ec_enc, cmf[sym+1], cmf[sym+2], cmf[end])
    else  # cmf[1] > 0
        ccall(Libdl.dlsym(lib, :smpl_ec_encode), Cvoid, (Ptr{Cvoid}, UInt32, UInt32, UInt32), ec_enc, cmf[sym+1] - cmf[1], cmf[sym+2] - cmf[1], cmf[end] - cmf[1])
    end
end

function ec_tell(ec_enc)
    ret = ccall(Libdl.dlsym(lib, :smpl_ec_tell), UInt32, (Ptr{Cvoid},), ec_enc)
end

function ec_enc_done(ec_enc)
    nbytes = (ec_tell(ec_enc) + 7) ÷ 8
    payload = zeros(UInt8, nbytes)
    ret = ccall(Libdl.dlsym(lib, :smpl_ec_enc_done), Int, (Ptr{Cvoid}, Ptr{UInt8}, UInt32), ec_enc, payload, nbytes)
    return ret > 0 ? payload : UInt8[]
end

function create_ec_decoder(payload::AbstractVector{UInt8})
    ec_dec = ccall(Libdl.dlsym(lib, :smpl_create_ec_decoder), Ptr{Cvoid}, (Ptr{UInt8}, UInt32), payload, length(payload))
    return ec_dec
end

function ec_dec_bits(ec_dec, nbits)
    bits = ccall(Libdl.dlsym(lib, :smpl_ec_dec_bits), UInt32, (Ptr{Cvoid}, UInt32), ec_dec, nbits)
end

function ec_decode(ec_dec, cmf::AbstractVector{UInt16})
    if cmf[1] == 0
        cmf_low = ccall(Libdl.dlsym(lib, :smpl_ec_decode), Int, (Ptr{Cvoid}, UInt32), ec_dec, cmf[end])
        sym = -1
        for s = 0:length(cmf)-2
            if cmf_low >= cmf[s+1] && cmf_low < cmf[s+2]
                sym = s
                break
            end
        end
        @assert sym != -1
        ccall(Libdl.dlsym(lib, :smpl_ec_dec_update), Cvoid, (Ptr{Cvoid}, UInt32, UInt32, UInt32), ec_dec, cmf[sym+1], cmf[sym+2], cmf[end])
    else  # cmf[1] > 0
        cmf_low = ccall(Libdl.dlsym(lib, :smpl_ec_decode), Int, (Ptr{Cvoid}, UInt32), ec_dec, cmf[end] - cmf[1]) + cmf[1]
        sym = -1
        for s = 0:length(cmf)-2
            if cmf_low >= cmf[s+1] && cmf_low < cmf[s+2]
                sym = s
                break
            end
        end
        @assert sym != -1
        ccall(Libdl.dlsym(lib, :smpl_ec_dec_update), Cvoid, (Ptr{Cvoid}, UInt32, UInt32, UInt32), ec_dec, cmf[sym+1] - cmf[1], cmf[sym+2] - cmf[1], cmf[end] - cmf[1])
    end
    return sym
end

function create_resampler(fs_in::Int32, fs_out::Int32)
    ccall(Libdl.dlsym(lib, :smpl_silk_resampler_wrapper_create), Ptr{Cvoid}, (Int32, Int32), fs_in, fs_out)
end

function silk_resampler(resampler, xin::Vector{Int16})
    fs_in = ccall(Libdl.dlsym(lib, :smpl_resampler_get_fs_in), Int32, (Ptr{Cvoid},), resampler)
    fs_out = ccall(Libdl.dlsym(lib, :smpl_resampler_get_fs_out), Int32, (Ptr{Cvoid},), resampler)
    fs_ratio = fs_out / fs_in
    xout = zeros(Int16, Int(ceil(length(xin) * fs_ratio)))
    ccall(Libdl.dlsym(lib, :silk_resampler), Int32, (Ptr{Cvoid}, Ptr{Int16}, Ptr{Int16}, Int32), resampler, xout, xin, length(xin))
    xout ./ 32768f0
end

function free_resampler(resampler)
    ccall(Libdl.dlsym(lib, :smpl_silk_resampler_wrapper_free), Cvoid, (Ptr{Cvoid},), resampler)
end

# function wnrg_c(x::AbstractVector{Float32}, C::AbstractMatrix{Float32})
#     return ccall(Libdl.dlsym(lib, :smpl_wnrg), Float32, (Ptr{Float32}, Ptr{Float32}, Int), view(C, :, :), view(x, :), length(x))  
# end

# function wnrg_c(x::AbstractVector{Float32}, C::SymmetricToeplitz)
#     return ccall(Libdl.dlsym(lib, :smpl_wnrg_symtoepl), Float32, (Ptr{Float32}, Ptr{Float32}, Int), view(C, :, 1), view(x, :), length(x))  
# end

function mult_symtoepl_c(x::AbstractVector{Float32}, C::SymmetricToeplitz)
    y = similar(x)
    ccall(Libdl.dlsym(lib, :smpl_mult_symtoepl), Cvoid, (Ptr{Float32}, Ptr{Float32}, Ptr{Float32}, Int), view(C, :, 1), view(x, :), y, length(x))
    return y
end

function matrix_mult_c(C::AbstractMatrix{Float32}, x::AbstractVector{Float32})
    @assert length(x) == size(C, 2)
    y = zeros(Float32, size(C, 1))
    C = permutedims(C, (2, 1))
    ccall(Libdl.dlsym(lib, :smpl_matrix_mult), Cvoid, (Ptr{Float32}, Ptr{Float32}, Ptr{Float32}, Int, Int), C, x, y, length(y), length(x))
    return y
end

function matrix_mult_transp_16_c(C::AbstractMatrix{Float32}, x::AbstractVector{Float32})
    @assert length(x) == size(C, 1) == size(C, 2) == 16
    y = zeros(Float32, size(C, 2))
    C = permutedims(C, (2, 1))
    ccall(Libdl.dlsym(lib, :smpl_matrix_mult_transp_16_c), Cvoid, (Ptr{Float32}, Ptr{Float32}, Ptr{Float32}, Int, Int), C, x, y, length(y), length(x))
    return y
end

function matrix_mult_transp_16_avx(C::AbstractMatrix{Float32}, x::AbstractVector{Float32})
    @assert length(x) == size(C, 1) == size(C, 2) == 16
    y = zeros(Float32, size(C, 2))
    C = permutedims(C, (2, 1))
    ccall(Libdl.dlsym(lib, :smpl_matrix_mult_transp_16_Avx2), Cvoid, (Ptr{Float32}, Ptr{Float32}, Ptr{Float32}, Int, Int), C, x, y, length(y), length(x))
    return y
end

# function get_maxi_c(x::AbstractVector{Float32})
#     return ccall(Libdl.dlsym(lib, :smpl_get_maxi), Int, (Ptr{Float32}, Int), x, length(x)) + 1
# end

function get_maxi_c(x::AbstractVector{Float32}, K)
    result = zeros(Int32, K)
    ccall(Libdl.dlsym(lib, :smpl_get_maxi_K), Cvoid, (Ptr{Float32}, Ptr{Int32}, Int, Int), x, result, length(x), K)
    result .+ 1
end

function gen_rand_pulses_c(x, rnd_seed)
    ccall(Libdl.dlsym(lib, :smpl_gen_rand_pulses), Cvoid, (Ptr{Float32}, Int, Ptr{Int32}), x, length(x), rnd_seed)
    x
end

function get_env_c(exc, smth_coef, smth_state)
    env = similar(exc)
    ccall(Libdl.dlsym(lib, :smpl_get_env), Cvoid, (Ptr{Float32}, Int, Float32, Ptr{Float32}, Ptr{Float32}), exc, length(exc), smth_coef, smth_state, env)
    env
end

function filt_hp_FIX_c(x::AbstractVector{Int16}, b::Int32, aneg::Int32, state=Int32[0])
    out = similar(x)
    ccall(Libdl.dlsym(lib, :smpl_filt_hp_FIX), Cvoid, (Ptr{Int16}, Int32, Int32, Ptr{Int32}, Ptr{Int16}, Int32), 
        x, b, aneg, state, out, length(x))
    out
end


# Entropy encoding + decoding demo:
# sym_in = [rand(0:10) for _ = 1:100]
# cmf = UInt16.(0:3:100)
# buf_bytes = 100
# ec_enc = create_ec_encoder(buf_bytes)
# map(s -> ec_encode(ec_enc, s, cmf), sym_in)
# println("bits used = ", ec_tell(ec_enc))
# payload = ec_enc_done(ec_enc)
# ec_dec = create_ec_decoder(payload)
# sym_out = [ec_decode(ec_dec, cmf) for _ = 1:length(sym_in)]
# @assert sym_in == sym_out
