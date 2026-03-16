# make sure
# - to have cmake and MinGW 64 bit installed

using Libdl

@isdefined(lib) && (lib isa Ptr{Nothing}) && Libdl.dlclose(lib)

cmake_gen_cmd = `cmake -S . -B buildDLL`
if Sys.iswindows()
    libprefix = "lib"
    libextension = ".dll"
    # Need to use MinGW instead of MSVS in order to export all functions & tables in dll
    cmake_gen_cmd = `cmake -S . -B buildDLL -G "MinGW Makefiles"`
elseif Sys.isapple()
    libprefix = "lib"
    libextension = ".dylib"
elseif Sys.islinux()
    libprefix = "lib"
    libextension = ".so"
else
    @assert false "Operating system not supported"
end

repldir_makelib_jl = pwd()
thisdir_makelib_jl = @__DIR__
libfile = thisdir_makelib_jl * "/$(libprefix)SmplCodecDLL" * libextension

cd(thisdir_makelib_jl)
run(cmake_gen_cmd)
run(`cmake --build buildDLL --target SmplCodecDLL --config Release`)
run(`cmake --install buildDLL --component SmplCodecDLL`)
cd(repldir_makelib_jl)

lib = Libdl.dlopen(libfile)
