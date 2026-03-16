using Libdl

# close lib and rebuild
@isdefined(lib) && (lib isa Ptr{Nothing}) && Libdl.dlclose(lib)
include("makelib.jl")  # defines libfile
lib = Libdl.dlopen(libfile)

# pffft
println("PFFFT SIMD size = ", ccall(Libdl.dlsym(lib, :pffft_simd_size), Int32, ()))
pffft_configs = if @isdefined pffft_configs
    for cfg in values(pffft_configs)
        ccall(Libdl.dlsym(lib, :pffft_destroy_setup), Cvoid, (Ptr{Cvoid},), cfg)  # destroy cfg
    end
    empty!(pffft_configs)
else
    Dict{UInt64,Ptr{Cvoid}}()
end
function pffftr(x::Array{Float32})
    nfft = length(x)
    @assert nfft >= 32
    transform = 0  # 0 -> real; 1 -> complex
    direction = 0  # 0 -> forward; 1 -> backward
    key = hash(nfft, UInt64(transform))
    cfg = if haskey(pffft_configs, key)
        pffft_configs[key]
    else
        pffft_configs[key] = ccall(Libdl.dlsym(lib, :pffft_new_setup), Ptr{Cvoid}, (Int32, Int32), Int32(nfft), Int32(transform))
    end
    y = similar(x)
    ccall(Libdl.dlsym(lib, :pffft_transform_ordered), Cvoid, (Ptr{Cvoid}, Ptr{Float32}, Ptr{Float32}, Ptr{Float32}, Int32), cfg, x, y, C_NULL, Int32(direction))
    # pffft stores DC and Nyquist in 1st and 2nd coefs
    # [view(y, 1:2:nfft); y[2]] .+ im * [0; view(y, 4:2:nfft); 0]
    out = ComplexF32[view(y, 1:2:nfft); y[2]]
    out[2:end-1] .+= im * view(y, 4:2:nfft)
    out
end

# ooura_configs = Dict{Int, Tuple}()
# function oourafftr(x::Array{T}) where T # watch out: overwrites input!
#     nfft = length(x)
#     direction = 1  # 1 -> forward; -1 -> backward
#     cfg = if haskey(ooura_configs, nfft)
#         ooura_configs[nfft]
#     else
#         ooura_configs[nfft] = (zeros(Int, 4 + isqrt(nfft ÷ 2)), zeros(Float64, nfft ÷ 2))
#     end
#     ccall(Libdl.dlsym(lib, :rdft), Cvoid, (Int, Int, Ptr{Float64}, Ptr{Int}, Ptr{Float64}), nfft, direction, x, cfg[1], cfg[2])
#     #[view(x, 1:2:nfft); x[2]] .+ im * [0; view(x, 4:2:nfft); 0]
#     out = ComplexF32[view(x, 1:2:nfft); x[2]]
#     out[2:end-1] .+= im * view(x, 4:2:nfft)
#     out
# end

# dct
function dctr(x::Array{Float32}, outlen=length(x))
    cfg = ccall(Libdl.dlsym(lib, :create_dct), Ptr{Cvoid}, (Int32, Int32), Int32(length(x)), Int32(outlen))
    out = Array{Float32}(undef, outlen)
    ccall(Libdl.dlsym(lib, :dct), Cvoid, (Ptr{Cvoid}, Ptr{Float32}, Ptr{Float32}), cfg, x, out)
    ccall(Libdl.dlsym(lib, :destroy_dct), Cvoid, (Ptr{Cvoid},), cfg)
    out
end
