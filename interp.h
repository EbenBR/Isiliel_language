// Isiliel — scripting language interpreter/compiler.
// Embedded: sandbox, embedding API, OOP, 2D graphics and REPL.
// Authors: Eben (AI-assisted) | Version: 1.0 | Date: 2026-11-08

#ifndef INTERP_H
#define INTERP_H

#include <iosfwd>
#include <string>

#include "vm.h" // BuiltinHandler (interp_register_fn)

// Runs the full pipeline (lexer -> compiler -> VM) over the source.
//   resetSymbols: false keeps the symbol table (used by the REPL).
//   verbose:      false suppresses the execution banners (used by the REPL).
// Returns true on success, false if there was a compilation/runtime error.
bool run_pipeline(const std::string& source,
                  bool resetSymbols = true,
                  bool verbose = true);

// REPL variant: keeps symbols AND accumulates bytecode between entries
// (functions defined stay valid on later lines).
bool run_pipeline_repl(const std::string& source);

// Reads a script file and runs the pipeline.
// Returns true on success, false if the file did not open or there was an error.
bool run_file(const std::string& path);

// E7/neg15: disassembler — compiles the script and prints the bytecode
// (without executing). true on success; false if it did not open or a compilation error.
bool disasm_file(const std::string& path);

// ===========================================================================
// Embedding API (E8/neg16–18) — to host the interpreter in C++
// without going through the CLI (banners/output/sandbox configurable per call).
// ===========================================================================

struct InterpOptions {
    bool verbose = false;       // banners (">>> STARTING...", etc.)
    bool capture = false;       // redirects stdout → InterpResult.output
    long long step_limit = 0;   // 0 = no VM instruction limit
    bool allow_fileio = true;   // false: fileio/include/require → error
    bool allow_modules = true;  // false: import(...) and natives → error
};

struct InterpResult {
    bool ok = true;
    std::string error;  // empty on success; message without printing
    std::string output; // captured stdout (when opts.capture)
};

// Compiles and runs `source` (same pipeline as the CLI, without banners by
// default). Always restores the state (threads/files/screen/modules) and the
// sandbox to defaults on exit — including on error.
InterpResult interp_run(const std::string& source,
                        const InterpOptions& opts = InterpOptions());

// Clean state for the next execution: terminates threads/files/screen,
// unloads modules, clears symbols AND the REPL accumulated bytecode,
// resets the sandbox. Call between executions of single-state embedders
// (neg17). Returns true (always; idempotent operations).
bool interp_reset();

// Registers a host function (same signature as the builtins) as a builtin
// — receives a stable id and O(1) dispatch like the others. Call BEFORE
// interp_run/run_pipeline; overrides an overload with the same arity.
void interp_register_fn(const char* name, int arity, BuiltinHandler fn);

// Output hook: redirects std::cout (where println/printf write) to
// `os`. nullptr restores the original std::cout. Valid until the next call.
void interp_set_output(std::ostream* os);

#endif // INTERP_H
