-- Isiliel — scripting language interpreter/compiler.
-- Embedded: sandbox, embedding API, OOP, 2D graphics, and REPL.
-- Authors: Eben (AI-assisted) | Version: 1.0 | Date: 2026-11-08
local function bench()
    local s = 0
    for i = 1, 3000000 do
        s = s + (i % 1000) * 2
    end
    return s
end
return bench()
