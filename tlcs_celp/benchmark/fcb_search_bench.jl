
cdir = @__DIR__ 
srcfiles = cdir * "/bench_fcb_search.cpp"
srcfiles = [srcfiles; cdir[1:end-length("benchmark")] * "codec/celp.cpp"]
incDir =  cdir[1:end-length("benchmark")] * "codec"

run(`gcc $srcfiles -I$incDir -o bench_fcb_search -Ofast -DDUMP_INPUT=0 -lstdc++`)

# build codec with DUMP_INPUT=1 to get input to fcb search written to file

fs = 16000
target_bitrate = 8000 # To get 10 ms frames
cfg = config(fs; target_bitrate)
xin, _ = readwav("test_signal_vctk.wav"; fs_out=fs)
xin = Float32.(xin)
enc_results = encode_main(xin, cfg)

run(`bench_fcb_search fcb_search_in.dat`)

using DelimitedFiles
m = readdlm("timing_results.txt")
num_surv = unique(m[:,3])

p = plot(m[m[:,3] .== num_surv[1],2], m[m[:,3] .== num_surv[1],5], title="FCB search 10 ms subframe", label=string("Toeplz nsurv: ", string(Int(num_surv[1]))), xlabel="nPulses max", ylabel="CPU load %",legend_position=:topleft)
plot!(p,m[m[:,3] .== num_surv[1],2], m[m[:,3] .== num_surv[1],6], label=string("!Toeplz nsurv: ", string(Int(num_surv[1]))), xlabel="nPulses max", ylabel="CPU load %",legend_position=:topleft)
for i = 2 : length(num_surv)
    plot!(p,m[m[:,3] .== num_surv[i],2], m[m[:,3] .== num_surv[i],5], label=string("Toeplz nsurv: ", string(Int(num_surv[i]))))
    plot!(p,m[m[:,3] .== num_surv[i],2], m[m[:,3] .== num_surv[i],6],label=string("!Toeplz nsurv: ", string(Int(num_surv[i]))))
end
plot!([1, 10],[0.2, 0.2],label="Opus NSQ C5")

display(plot(p))
png(plot(p), "fcb_search_smpl_10ms")

target_bitrate = 10000 # To get 5 ms frames
cfg = config(fs; target_bitrate)
cfg = change_tuple(cfg, :pitch_sharp_coef, Dict(true => 0.95f0, false => 0.95f0))
xin, _ = readwav("test_signal_vctk.wav"; fs_out=fs)
xin = Float32.(xin)
enc_results = encode_main(xin, cfg)

run(`bench_fcb_search fcb_search_in.dat`)

using DelimitedFiles
m = readdlm("timing_results.txt")
num_surv = unique(m[:,3])

p = plot(m[m[:,3] .== num_surv[1],2], m[m[:,3] .== num_surv[1],5], title="FCB search 5 ms subframe", label=string("Toeplz nsurv: ", string(Int(num_surv[1]))), xlabel="nPulses max", ylabel="CPU load %",legend_position=:topleft)
plot!(p,m[m[:,3] .== num_surv[1],2], m[m[:,3] .== num_surv[1],6], label=string("!Toeplz nsurv: ", string(Int(num_surv[1]))), xlabel="nPulses max", ylabel="CPU load %",legend_position=:topleft)
for i = 2 : length(num_surv)
    plot!(p,m[m[:,3] .== num_surv[i],2], m[m[:,3] .== num_surv[i],5], label=string("Toeplz nsurv: ", string(Int(num_surv[i]))))
    plot!(p,m[m[:,3] .== num_surv[i],2], m[m[:,3] .== num_surv[i],6],label=string("!Toeplz nsurv: ", string(Int(num_surv[i]))))
end
plot!([1, 10],[0.2, 0.2],label="Opus NSQ C5")

display(plot(p))
png(plot(p), "fcb_search_smpl_5ms")
