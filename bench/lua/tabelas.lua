-- Isiliel — scripting language interpreter/compiler.
-- Embedded: sandbox, embedding API, OOP, 2D graphics, and REPL.
-- Authors: Eben (AI-assisted) | Version: 1.0 | Date: 2026-11-08
local function bench()
    local t = {}
    for i = 1, 500000 do
        t[#t + 1] = i
    end
    local n = #t
    local s = 0
    for i = 1, n do
        s = s + t[i]
    end
    return s
end
return bench()
