
cdir = @__DIR__ 
srcfiles = cdir * "/bench_mult_div.cpp"

run(`gcc $srcfiles -o bench_mult_div -lstdc++`)

run(`bench_mult_div`)
