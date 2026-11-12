// Isiliel — scripting language interpreter/compiler.
// Embedded: sandbox, embedding API, OOP, 2D graphics and REPL.
// Authors: Eben (AI-assisted) | Version: 1.0 | Date: 2026-11-08

#ifndef GRAPHICS_BUILTINS_H
#define GRAPHICS_BUILTINS_H

// Registers the graphics builtins in the VM global registry (idempotent).
// Called by the pipeline before compiling (builtins = callbacks registered
// via register_builtin).
void register_graphics_builtins();

#endif
