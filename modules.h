// Isiliel — scripting language interpreter/compiler.
// Embedded: sandbox, embedding API, OOP, 2D graphics and REPL.
// Authors: EEPS and MiMo-V2.6 | Version: 1.0 | Date: 2026-11-08

#ifndef MODULES_H
#define MODULES_H

// Native `.mod` modules (item 16)
//
// `import("path.mod");` loads a JSON manifest that points to a shared
// library (.so/.dylib): functions enter as calls (fallback of
// OP_CALL_BUILTIN) and variables become symbols with direct access
// to the library memory (native OP_LOAD/OP_STORE).
//
// The FFI is manual (no libffi): scalar types are normalized to
// (int64_t | float | double | const char*) on the call; out-params receive
// pointers to temporaries and the result becomes an array [ret, out0, ...].

#include "defs.h"
#include <string>

// Scalar types accepted in the manifest (functions/variables)
enum NativeTypeId {
    NT_INT8, NT_INT16, NT_INT32, NT_INT64,
    NT_UINT8, NT_UINT16, NT_UINT32, NT_UINT64,
    NT_FLOAT32, NT_FLOAT64, NT_BOOL, NT_STRING, NT_VOID
};

// Native variable: address inside the loaded library. The module is the
// owner (unique_ptr) — symbols merely point here.
struct NativeVar {
    void* addr = nullptr;
    int type = NT_INT32;      // NativeTypeId
    bool readonly = false;
    int size = 256;           // buffer (NT_STRING only; read/write cap)
    std::string name;         // name in the interpreter (error messages)
};

// Reading/writing native variables (used by OP_LOAD/OP_STORE).
// Throw std::runtime_error (the VM appends " (line N)").
void native_read(const NativeVar* nv, Value* out);
void native_write(const NativeVar* nv, const Value& v);

// Imports the `path` manifest. Returns true if loaded now; false if the
// module was already loaded (only rebinds the symbols in the current sym table).
// Any problem throws std::runtime_error with the prefix "import: ".
bool modules_import(const std::string& path);

// Fallback of compile_call/OP_CALL_BUILTIN: module function with this
// arity (number of input arguments; out-params do not count)?
bool modules_is_callable(const std::string& name, int argc);
// Does the name exist in any module (any arity) — arity error.
bool modules_known_function(const std::string& name);
// Module function arity (number of input arguments); -1 if it does not exist.
int modules_arity(const std::string& name);

// Calls a native function. Returns false if `name` is not from a module (then the VM
// continues the normal error path). With out-params, `res` becomes an array
// [return, out0, out1, ...]; without out-params, a scalar.
bool modules_call(const std::string& name, Value* args, int argc, Value* res);

// Calls the shutdown_function of every module and dlclose (idempotent).
// Only at process end: tails of run_pipeline (script mode) + run.cpp.
void modules_shutdown();

#endif
