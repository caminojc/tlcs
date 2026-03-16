include("write_tables_helpers.jl")

n_pulses_pdf_dict = load(string(@__DIR__, "/../../../codebooks/n_pulses_pdf.jld2"), "n_pulses_pdf")

cfg = config(16000)

io = open(string(@__DIR__, "/smpl_pulse_tables.h"),"w")
print(io, "#include <stdint.h> \n\n")
print(io, "#ifdef __cplusplus\n")
print(io, """extern "C" {\n""")
print(io, "#endif\n\n")
tab = n_pulses_pdf_dict[(; voiced = false, voiced_active = false)]
print(io, "extern const uint8_t smpl_n_pulses_dcmf_bgn[$(length(tab))];\n")
tab = n_pulses_pdf_dict[(; voiced = false, voiced_active = true)]
print(io, "extern const uint8_t smpl_n_pulses_dcmf_uv[$(length(tab))];\n")
tab = n_pulses_pdf_dict[(; voiced = true, voiced_active = true)]
print(io, "extern const uint8_t smpl_n_pulses_dcmf_v[$(length(tab))];\n")
print(io, "\n#ifdef __cplusplus\n")
print(io, "}\n")
print(io, "#endif\n")
close(io)


io = open(string(@__DIR__, "/smpl_pulse_tables.c"),"w")
print(io, "#include \"smpl_pulse_tables.h\" \n\n")

tab = n_pulses_pdf_dict[(; voiced = false, voiced_active = false)]
print(io, "const uint8_t smpl_n_pulses_dcmf_bgn[$(length(tab))] = ")
write_vector(io, tab)
tab = n_pulses_pdf_dict[(; voiced = false, voiced_active = true)]
print(io, "const uint8_t smpl_n_pulses_dcmf_uv[$(length(tab))] = ")
write_vector(io, tab)
tab = n_pulses_pdf_dict[(; voiced = true, voiced_active = true)]
print(io, "const uint8_t smpl_n_pulses_dcmf_v[$(length(tab))] = ")
write_vector(io, tab)

close(io)
