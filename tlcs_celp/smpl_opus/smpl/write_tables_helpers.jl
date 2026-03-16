function float_to_int16(x::Float32, Q::Int)
    ret = round(Int, x * 2^Q)
    @assert(abs(ret) < 2^15)
    return Int16(ret)
end

function write_matrix(io, M, typesym, last=true)
    for j = 1 : size(M)[2]-1
        print(io, "{")
        for i = 1 : size(M)[1]-1    
            print(io, M[i, j],"$typesym, ")
        end
        print(io, M[end, j],"$typesym },\n    ")
    end
    print(io, "{")
    for i = 1 : size(M)[1]-1    
        print(io, M[i, end],"$typesym, ")
    end
    if last
        print(io, M[end, end],"$typesym }\n};\n")
    else
        print(io, M[end, end],"$typesym }\n},\n")
    end        
end

function write_matrix(io, M, name::String, type::String)
    print(io, "const $type $name = {\n    ")
    write_matrix(io, M, startswith(type, "u") ? "u" : (startswith(type, "f") ? "f" : ""), true)
end

function write_float_matrix(io, M, last=true)
    for j = 1 : size(M)[2]-1
        print(io, "{")
        for i = 1 : size(M)[1]-1    
            print(io, M[i, j],"f, ")
        end
        print(io, M[end, j],"f },\n    ")
    end
    print(io, "{")
    for i = 1 : size(M)[1]-1    
        print(io, M[i, end],"f, ")
    end
    if last
        print(io, M[end, end],"f }\n}; \n")
    else
        print(io, M[end, end],"f }\n}, \n")
    end        
end

function write_float_matrix(io, M, name::String)
    print(io, "const float $name = {\n    ")
    write_float_matrix(io, M)
end

function write_vector(io, vec, per_line=Inf, typesym="", last=true)
    print(io, "{")
    for (i, v) in enumerate(vec)
        (i-1) % per_line == 0 && per_line < Inf && print(io, "\n    ")
        print(io, "$(v)$(typesym)")
        i == length(vec) || print(io, ", ")
    end
    per_line < Inf && print(io, "\n")
    print(io, last ? "};\n" : "},\n")
end
