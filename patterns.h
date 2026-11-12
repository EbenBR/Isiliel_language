// Isiliel — L9: Lua-style string patterns (subset).
// Builtins: match(s, pat[, init]) -> string ("" if no match; no nil yet)
//           gmatch(s, pat) -> 1D list with ALL matches (no iterator)
//           gsub(s, pat, repl) -> new string (text repl with %0..%9 and %%)
// Supports: . %a %c %d %g %l %p %s %u %w %x %z (and uppercase), [...] with
// ranges/^, quantifiers * + - ?, anchors ^ $, captures (...) and %bxy/%f.
#ifndef PATTERNS_H
#define PATTERNS_H

void register_pattern_builtins();

#endif
