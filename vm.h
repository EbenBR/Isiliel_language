// Isiliel — scripting language interpreter/compiler.
// Embedded: sandbox, embedding API, OOP, 2D graphics and REPL.
// Authors: Eben (AI-assisted) | Version: 1.0 | Date: 2026-11-08

#ifndef VM_H
#define VM_H

#include <string>
#include <unordered_map>
#include <vector>

#include "defs.h"
#include "symbols.h"
#include <stack>

#define STACK_MAX 1024

// L1: frame callback registered by setDraw(fn) (builtin in graphics).
// The VM reads the entry in OP_CALL_DRAW; -1 = no callback registered.
void vm_set_draw_callback(const Value& fn);
int  vm_draw_callback_entry();

// Definition for native functions (Built-ins)
typedef void (*BuiltinImpl)(Value*, int, Value*);

struct BuiltinFunction {
    const char* name;
    int argCount; // -1 for variadic arguments
    BuiltinImpl implementation;
};

struct VM {
    // E13: hot fields first (sp/ip/running) and the value stack right
    // after — same cache line for the control scalars.
    int sp = 0;
    int ip = 0;
    bool running = false;
    Value stack[STACK_MAX];
    // Traceback (phase E2): frames {name, call line} — outer at
    // the start, inner at the end. Filled by OP_CALL/INDIRECT/METHOD/PCALL;
    // on an unhandled error the REPL/printer shows the stack.
    // E19: {name, line} with InsStr (4 B) instead of std::string: the push/pop
    // per call does not allocate/copy text (name already interned).
    std::vector<std::pair<InsStr, int>> frames;
    // Execution state MOVED into the VM (phase E5): vm_run uses
    // references to these members, allowing suspension (coyield) and
    // resumption (coresume) without losing the call/local stack.
    std::vector<int> callStack;
    std::vector<size_t> frameMarkers;
    std::vector<SavedLocal> scopeSaves;
    // L12: one environment per frame. `frameEnvs` is created on demand when a
    // `fn(...)` is evaluated; `frameEnvParents` stores the lexical environment in
    // which the current function was defined (for upvalue lookup by name).
    std::vector<std::shared_ptr<FrameEnv>> frameEnvs;
    std::vector<std::shared_ptr<FrameEnv>> frameEnvParents;
    // L5: return values. callResults stores how many values each
    // caller wants to receive (1 by default; -1 = all); lastRet is the number
    // returned by the last call; topRet is the number left on top of the
    // stack when a chunk/thread finishes.
    std::vector<int> callResults;
    int lastRet = 1;
    int topRet = 1;
    // L5: number of arguments of the call that just entered (read by the
    // OP_COLLECT_VARARGS prologue). No stack needed: the prologue runs
    // immediately after the branch and makes no other calls.
    int callArgc = 0;
};

// Coroutine (phase E5): own fixed-stack VM + state metadata.
// status: "new" | "running" | "suspended" | "dead"
struct Coroutine {
    VM vm;
    int entry = -1;          // GLOBAL entry of the target function
    std::shared_ptr<FrameEnv> env; // L12: captured environment of the target closure
    bool started = false;
    bool yielded = false;    // coyield happened in this run
    Value yieldVal;          // value returned to coresume
    bool finished = false;
    std::string status = "novo";
    Value returnVal;         // final value (dead status)
    std::string errorMsg;    // final error + traceback (if it died with an error)
};

// E14: typed coroutine accessor (Coroutine complete only here).
inline Coroutine* Value::co() const { return static_cast<Coroutine*>(obj.get()); }

// start_ip: initial execution index (the REPL accumulates bytecode and runs
// only the new section from it)
void vm_run(VM* vm, CodeStore* code, int start_ip = 0);

// L11: runs registered __gc finalizers. Called at the end of a script
// (outside the REPL), before shutting down threads/modules.
void vm_run_gc_finalizers();
void vm_clear_gc_finalizers(); // discards records when resetting the state

// L13: cycle collector (trial deletion). Returns how many objects were
// released. `run_finalizers` runs __gc on unreachable objects before
// breaking the cycle (used by the collectgarbage builtin).
int gc_collect_cycles(bool run_finalizers = false);
void vm_clear_gc_objects(); // discards the object registry when resetting

// L14: global bytecode arena. All chunks (main script, REPL and
// `load`/`dofile`) live in the same address space; FuncRef.entry is an
// absolute index into this arena. The arena is only cleared between whole executions.
CodeStore& code_arena();
int code_arena_size();
int code_arena_append(const std::vector<Instruction>& chunk);
void code_arena_reset();
// Arena generation: changes on every append/reset. vm_run caches the
// `data()` pointer and only renews it when the generation changes (after builtins that
// may call load/dofile), keeping the fetch at the same cost as before.
int code_arena_gen();
// Helper functions to create Values in an ISO C++-compatible way
inline Value make_float(double f) {
    Value v; v.type = VAL_FLOAT; v.f = f; return v;
}

// L7: native int64 integer. `f` receives the approximate conversion for the
// legacy paths (math/graphics); integer operations use `i`.
inline Value make_int(long long i) {
    Value v; v.type = VAL_INT; v.i = i; v.f = (double)i; return v;
}

inline Value make_nil() {
    Value v; v.type = VAL_NIL; return v;
}

inline Value make_string(std::string s) {
    Value v; v.type = VAL_STRING; v.s = s; return v;
}

inline Value make_complex(double re, double im) {
    Value v; v.type = VAL_COMPLEX; v.f = re; v.im = im; return v;
}

//--- Native function registry
typedef void (*BuiltinHandler)(Value* args, int count, Value* res);

struct Builtin {
    std::string name;
    int arity;
    BuiltinHandler handler;
    int id = -1; // E7/neg13: stable index in g_builtins (O(1) dispatch)
};

// The global function map (a name may have SEVERAL arities — overload)
extern std::unordered_map<std::string, std::vector<Builtin>> builtinRegistry;

// E7/neg13: FLAT append-only registry — `Builtin.id` indexes this vector.
// OP_CALL_BUILTIN stores the id in `a` (compile-time) and calls the handler by
// index in O(1), with no string hash per instruction. `-1` = path by
// name (`.mod` module functions, registered at runtime).
extern std::vector<Builtin> g_builtins;

// --- Sandbox (E8/neg18) — per-process globals -------------------------------
// Defaults = no sandbox (CLI unchanged). interp_run() configures per
// execution and restores at the end. `g_steps` is shared by nested vm_run
// calls (pcall/coroutines) and by threads (limit = process total).
extern long long g_step_limit; // 0 = no instruction limit
extern long long g_steps;      // accumulated counter (reset per interp_run)
extern bool g_allow_fileio;    // false: fileio/include/require → error
extern bool g_allow_modules;   // false: import(...)/native → error

// --- REPL growth limits (E9/neg22) ------------------------------------------
// 0 = no limit (scripts/API are unaffected). The REPL configures from
// the environment (MYINTERP_REPL_CODE_LIMIT / MYINTERP_REPL_SYMBOLS_LIMIT) per
// entry; the compiler refuses to emit beyond the ceiling with a compilation error.
extern long long g_code_limit;   // accumulated instructions (including code_base)
extern long long g_symbol_limit; // symbols (globals + locals + functions)

// Registration function (used by the macro)
void register_builtin(std::string name, int arity, BuiltinHandler handler);

// Chooses the `name` overload compatible with `argc` (exact arity
// preferred; otherwise variadic). nullptr = does not exist or wrong arity.
const Builtin* builtin_lookup(const std::string& name, int argc);

// Lists the registered arities of `name` (for error messages)
std::string builtin_arities(const std::string& name);

// Macro for auto-registration
#define REGISTER_BUILTIN_CAT2(a, b) a##b
#define REGISTER_BUILTIN_CAT(a, b) REGISTER_BUILTIN_CAT2(a, b)
#define REGISTER_BUILTIN(name, arity, func) \
    static bool REGISTER_BUILTIN_CAT(_reg_##func##_, __LINE__) = []() { \
        register_builtin(name, arity, func); \
        return true; \
    }()

// Formats `fmt` with `argc` values (%s %d %i %f %g %j %c %% conversions).
// `fn` = builtin name for error messages ("printf", "fprintf").
// No implicit newline; throws std::runtime_error (same rules as printf).
std::string format_values(const std::string& fmt, Value* args, int argc,
                          const char* fn);

#endif