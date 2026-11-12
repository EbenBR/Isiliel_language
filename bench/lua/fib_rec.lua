-- Isiliel — scripting language interpreter/compiler.
-- Embedded: sandbox, embedding API, OOP, 2D graphics, and REPL.
-- Authors: Eben (AI-assisted) | Version: 1.0 | Date: 2026-11-08
local function fib(n)
    if n < 2 then
        return n
    end
    return fib(n - 1) + fib(n - 2)
end
return fib(27)
