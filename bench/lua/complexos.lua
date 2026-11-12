-- Isiliel — scripting language interpreter/compiler.
-- Embedded: sandbox, embedding API, OOP, 2D graphics, and REPL.
-- Authors: Eben (AI-assisted) | Version: 1.0 | Date: 2026-11-08
-- Complex numbers: Mandelbrot (z = z^2 + c). Lua has no native complex type:
-- the idiomatic way is two scalars (re/im) and math.sqrt for |z|.
local sqrt = math.sqrt
local function bench()
    local total = 0
    local width = 200
    local height = 200
    local maxit = 50
    for py = 0, height - 1 do
        local ci = (py / height) * 2.0 - 1.0
        for px = 0, width - 1 do
            local cr = (px / width) * 2.5 - 2.0
            local zr = 0.0
            local zi = 0.0
            local it = 0
            while it < maxit and sqrt(zr * zr + zi * zi) <= 2.0 do
                local zr2 = zr * zr - zi * zi + cr
                zi = 2.0 * zr * zi + ci
                zr = zr2
                it = it + 1
            end
            total = total + it
        end
    end
    return total
end
return bench()
