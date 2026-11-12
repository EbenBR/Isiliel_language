-- Isiliel — scripting language interpreter/compiler.
-- Embedded: sandbox, embedding API, OOP, 2D graphics, and REPL.
-- Authors: Eben (AI-assisted) | Version: 1.0 | Date: 2026-11-08
local function bench()
    local t = {}
    for i = 1, 100000 do
        t[#t + 1] = "abc"
    end
    return #table.concat(t)
end
return bench()
