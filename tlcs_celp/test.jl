include("wrapper.jl")
include("../fftw_cached.jl")

# pffft
r = randn(Float32, 256)
pffftr(r) ≈ rfft(r)
# oourafftr(r) ≈ rfft(r)

# fft benchmarking
using BenchmarkTools
println()
for L = sort([2 .^ (7:11); 3 .* 2 .^ (6:9)])
    #for L = 2 .^ (7:11)  # for ooura, which can only handle power-of-2 length
    println("Real input data length = ", L)
    r64 = randn(L)
    r32 = Float32.(r64)
    print("    FFTW: ")
    @btime rfft($r32)
    print("   PFFFT: ")
    @btime pffftr($r32)
    # print("   OOURA: ")
    # @btime oourafftr($r32);
end
