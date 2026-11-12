// Isiliel — scripting language interpreter/compiler.
// Embedded: sandbox, embedding API, OOP, 2D graphics and REPL.
// Authors: EEPS and MiMo-V2.6 | Version: 1.0 | Date: 2026-11-08

#include "vm.h"
#include "symbols.h"
#include "lexer.h"
#include "compiler.h"
#include "threads.h"
#include "modules.h"
#include "fileio.h"
#include <iostream>
#include <mutex>
#include <cmath>
#include <cctype>
#include <ctime>
#include <chrono>
#include <vector>
#include <algorithm>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <cstdio>
#include <fstream>
#include <cerrno>
#include <cstdlib>
#include <limits>

// E11: dispatch via computed goto (GCC/Clang extension, used on PC and on
// embedded targets with g++) and a 100% portable switch fallback for other
// compilers. The case source code is single (macros below).
#if (defined(__GNUC__) || defined(__clang__)) && !defined(ISILIEL_NO_CGOTO)
#define ISILIEL_CGOTO 1
#endif
#ifdef ISILIEL_CGOTO
#define VM_CASE(op) L_##op
#define VM_BREAK goto vm_continue
#define VM_DEFAULT L_default
#else
#define VM_CASE(op) case op
#define VM_BREAK break
#define VM_DEFAULT default
#endif
#ifdef ISILIEL_CGOTO
#pragma GCC diagnostic ignored "-Wpedantic" // &&label and goto* are GNU extensions
#endif
#include <stack>

// L1: draw callback (setDraw). Stored as a Value (VAL_FUNC) to keep
// the FuncRef alive; entry is copied into the frame in OP_CALL_DRAW.
static Value g_draw_callback;
static bool g_draw_callback_set = false;
void vm_set_draw_callback(const Value& fn) {
    g_draw_callback = fn;
    g_draw_callback_set = true;
}
int vm_draw_callback_entry() {
    if (!g_draw_callback_set || g_draw_callback.type != VAL_FUNC) return -1;
    FuncRef* f = g_draw_callback.fn();
    return f ? f->entry : -1;
}

#include <random>

// Shared output: println/printf can be called by threads (item 15)
// — the lock prevents interleaved lines on stdout
static std::mutex g_cout_mutex;

// CRITICAL FIX: Macro with stack overflow protection
#define PUSH(vm, val) do { \
    if ((vm)->sp >= STACK_MAX) throw std::runtime_error("Stack overflow - pilha cheia"); \
    (vm)->stack[(vm)->sp++] = val; \
} while(0)

#define POP(vm) ((vm)->sp <= 0 ? (throw std::runtime_error("Stack underflow - pilha vazia"), (vm)->stack[0]) : (vm)->stack[--(vm)->sp])

// L5: adjusts the top of the stack to the number of values the caller wants.
// n = values returned (on top); wanted = -1 keeps all.
static inline void vm_finish_return(VM* vm, int n, int wanted) {
    if (n < 0) n = vm->lastRet;
    if (n < 0) n = 0;
    if (n == wanted) { vm->lastRet = n; return; } // common case: 1 == 1
    if (wanted < 0) { vm->lastRet = n; return; }
    if (n > wanted) {
        // Keeps the FIRST `wanted` values (Lua: x = f() takes the 1st);
        // the rest are on top and are discarded.
        vm->sp -= (n - wanted);
    } else if (n < wanted) {
        for (int i = n; i < wanted; i++)
            PUSH(vm, make_nil());
    }
    vm->lastRet = wanted;
}

// L5: expansion of the last argument. The stack has [fixed args..., list];
// replaces the list with its elements and returns how many were pushed.
static inline int vm_expand_last_arg(VM* vm, int fixed, const char* what) {
    if (vm->sp <= fixed)
        throw std::runtime_error("Argumentos insuficientes em " + std::string(what));
    Value arr = POP(vm);
    if (arr.type != VAL_ARRAY || !arr.arr())
        throw std::runtime_error("Expansao de argumento esperava lista em " +
                                 std::string(what));
    int n = (int)arr.arr()->elements.size();
    if (vm->sp + n > STACK_MAX)
        throw std::runtime_error("Stack overflow - pilha cheia");
    for (int i = 0; i < n; i++) PUSH(vm, arr.arr()->elements[i]);
    return n;
}

// L5: pushes the result of a builtin respecting how many values the
// caller requested (wanted < 0 = all; VAL_MULTI from select/unpack).
static inline void vm_push_builtin_result(VM* vm, Value r, int wanted) {
    if (r.type == VAL_MULTI && r.mul()) {
        const std::vector<Value>& vals = r.mul()->values;
        int n = (int)vals.size();
        int want = wanted < 0 ? n : wanted;
        for (int i = 0; i < want; i++)
            PUSH(vm, i < n ? vals[i] : make_nil());
        vm->lastRet = want;
        return;
    }
    PUSH(vm, std::move(r));
    if (wanted > 1)
        for (int i = 1; i < wanted; i++) PUSH(vm, make_nil());
    vm->lastRet = wanted < 0 ? 1 : wanted;
}

// E16d: in-place binary operation. Both operands are in slots
// [sp-2] and [sp-1]; the result replaces the left slot and sp drops by 1 —
// avoids pop/pop/push (3 copies of 56 B) per arithmetic operation.
#define BIN2(vm, expr) do { \
    if ((vm)->sp < 2) throw std::runtime_error("Stack underflow - pilha vazia"); \
    Value& _a = (vm)->stack[(vm)->sp - 2]; \
    Value& _b = (vm)->stack[(vm)->sp - 1]; \
    (vm)->sp -= 2; /* consumes BEFORE: error sp == old pop/pop */ \
    _a = (expr); \
    (vm)->sp += 1; \
} while(0)

// E16d: operation with an immediate constant on top (sp unchanged on
// success; on exception, sp has already consumed 1 as in the old POP).
#define BIN1(vm, expr) do { \
    if ((vm)->sp < 1) throw std::runtime_error("Stack underflow - pilha vazia"); \
    Value& _a = (vm)->stack[(vm)->sp - 1]; \
    (vm)->sp -= 1; \
    _a = (expr); \
    (vm)->sp += 1; \
} while(0)

// E17b: pure numeric fast path — writes directly to `.f` (without building
// or moving a 56 B Value). The slow path preserves the sp semantics
// on exception (equivalent to the old POP).
#define BIN2_NUM(vm, fast, slow) do { \
    if ((vm)->sp < 2) throw std::runtime_error("Stack underflow - pilha vazia"); \
    Value& _a = (vm)->stack[(vm)->sp - 2]; \
    Value& _b = (vm)->stack[(vm)->sp - 1]; \
    if (_a.type == VAL_FLOAT && _b.type == VAL_FLOAT) { \
        _a.f = (fast); \
        (vm)->sp -= 1; \
    } else { \
        (vm)->sp -= 2; \
        _a = (slow); \
        (vm)->sp += 1; \
    } \
} while(0)

#define BIN1_NUM(vm, fast, slow) do { \
    if ((vm)->sp < 1) throw std::runtime_error("Stack underflow - pilha vazia"); \
    Value& _a = (vm)->stack[(vm)->sp - 1]; \
    if (_a.type == VAL_FLOAT) { \
        _a.f = (fast); \
    } else { \
        (vm)->sp -= 1; \
        _a = (slow); \
        (vm)->sp += 1; \
    } \
} while(0)

// --- COROUTINES (phase E5) ---
// YieldSignal does NOT inherit from std::exception: it passes through the pcall
// catch and the per-instruction catch (which only re-throws std::exception) and
// reaches the coroutine runner (builtin_coresume), which saves the state already
// resident in the VM.
struct YieldSignal {};
static Coroutine* g_active_coro = nullptr; // coroutine currently running
static CodeStore* g_current_code = nullptr; // current arena (vm_run registers it)
// L14: VM of the current execution (thread-local) — `dofile` must run a
// nested chunk on the same VM/stack; the guard in vm_run restores it on errors.
static thread_local VM* g_current_vm = nullptr;
static CodeStore g_code_arena;               // L14: stable bytecode
static int g_code_arena_gen = 0;             // changes on every append/reset

CodeStore& code_arena() { return g_code_arena; }
int code_arena_size() { return (int)g_code_arena.size(); }
int code_arena_gen() { return g_code_arena_gen; }
void code_arena_reset() {
    g_code_arena.clear();
    ++g_code_arena_gen;
}
int code_arena_append(const std::vector<Instruction>& chunk) {
    int base = (int)g_code_arena.size();
    g_code_arena.insert(g_code_arena.end(), chunk.begin(), chunk.end());
    ++g_code_arena_gen; // invalidates pointers cached in vm_run
    return base;
}

// --- NUMBER UTILITIES (REAL AND COMPLEX) ---
static inline bool is_complex(const Value& v) { return v.type == VAL_COMPLEX; }

static inline bool is_numeric(const Value& v) {
    return v.type == VAL_FLOAT || v.type == VAL_INT || v.type == VAL_COMPLEX;
}

static inline double v_re(const Value& v) {
    return v.type == VAL_INT ? (double)v.i : v.f;
}
static inline double v_im(const Value& v) { return v.type == VAL_COMPLEX ? v.im : 0.0f; }

// L7: int64 arithmetic with wrap-around (no UB). The result remains
// integer; division `/` always promotes to double.
static inline long long i64_wrap_add(long long a, long long b) {
    return (long long)((unsigned long long)a + (unsigned long long)b);
}
static inline long long i64_wrap_sub(long long a, long long b) {
    return (long long)((unsigned long long)a - (unsigned long long)b);
}
static inline long long i64_wrap_mul(long long a, long long b) {
    return (long long)((unsigned long long)a * (unsigned long long)b);
}
// Converts an immediate double constant to int64 only if exact.
static inline bool i64_exact_from_double(double d, long long& out) {
    const double LIM = 9223372036854775808.0; // 2^63
    if (!(d >= -LIM && d < LIM) || d != std::floor(d)) return false;
    out = (long long)d;
    return true;
}
static inline Value k_value(double n, int int_const) {
    long long iv;
    if (int_const && i64_exact_from_double(n, iv)) return make_int(iv);
    return make_float(n);
}
static inline Value k_value_infer(double n) {
    long long iv;
    if (i64_exact_from_double(n, iv)) return make_int(iv);
    return make_float(n);
}

// L7: exact int64 vs double comparison (without rounding the integer to 2^53).
static inline bool i64_eq_double(long long i, double d) {
    if (!std::isfinite(d)) return false;
    const double LIM = 9223372036854775808.0; // 2^63
    if (!(d >= -LIM && d < LIM) || d != std::floor(d)) return false;
    return (long long)d == i;
}
static inline bool i64_lt_double(long long i, double d) {
    if (std::isnan(d)) return false;
    const double LIM = 9223372036854775808.0; // 2^63
    if (d >= LIM) return true;
    if (d < -LIM) return false;
    long long fl = (long long)std::floor(d);
    if (i != fl) return i < fl;
    return d > (double)fl; // fractional part (only exists near 0)
}

// Result becomes complex if at least one operand is complex
static Value num_add(const Value& a, const Value& b) {
    if (a.type == VAL_NIL || b.type == VAL_NIL)
        throw std::runtime_error("Operacao aritmetica nao suportada para nil");
    double re = v_re(a) + v_re(b);
    double im = v_im(a) + v_im(b);
    return (is_complex(a) || is_complex(b)) ? make_complex(re, im) : make_float(re);
}

static Value num_sub(const Value& a, const Value& b) {
    if (a.type == VAL_NIL || b.type == VAL_NIL)
        throw std::runtime_error("Operacao aritmetica nao suportada para nil");
    double re = v_re(a) - v_re(b);
    double im = v_im(a) - v_im(b);
    return (is_complex(a) || is_complex(b)) ? make_complex(re, im) : make_float(re);
}

static Value num_mul(const Value& a, const Value& b) {
    if (a.type == VAL_NIL || b.type == VAL_NIL)
        throw std::runtime_error("Operacao aritmetica nao suportada para nil");
    double ar = v_re(a), ai = v_im(a), br = v_re(b), bi = v_im(b);
    double re = ar * br - ai * bi;
    double im = ar * bi + ai * br;
    return (is_complex(a) || is_complex(b)) ? make_complex(re, im) : make_float(re);
}

static Value num_div(const Value& a, const Value& b) {
    if (a.type == VAL_NIL || b.type == VAL_NIL)
        throw std::runtime_error("Operacao aritmetica nao suportada para nil");
    if (!is_complex(a) && !is_complex(b)) {
        if (std::abs(v_re(b)) < 1e-10f) throw std::runtime_error("Divisão por zero");
        return make_float(v_re(a) / v_re(b));
    }
    double ar = v_re(a), ai = v_im(a), br = v_re(b), bi = v_im(b);
    if (br == 0.0f && bi == 0.0f) throw std::runtime_error("Divisão por zero");
    double denom = br * br + bi * bi;
    return make_complex((ar * br + ai * bi) / denom, (ai * br - ar * bi) / denom);
}

static inline bool truthy(const Value& v) {
    if (v.type == VAL_NIL) return false;     // L6: nil is false
    if (v.type == VAL_STRUCT) return true;   // struct object is always true
    if (v.type == VAL_FUNC) return true;     // function as a value is true (E2)
    if (v.type == VAL_TABLE) return true;    // table (even empty) is true (E3)
    if (v.type == VAL_STRING) return true;   // any text is true
    if (v.type == VAL_COMPLEX) return v.f != 0.0f || v.im != 0.0f;
    if (v.type == VAL_INT) return v.i != 0;
    return v.f != 0.0f;
}

// L12: slot of a captured local (shared cell, if already created).
// Only the *_CELL opcodes use this function; the common path reads Symbol::value.
static inline Value* sym_slot(Symbol* s) {
    return s->cell ? s->cell.get() : &s->value;
}

// L12: per-frame environment stacks. They are kept only when the program
// has any closure (see g_program_has_closures); without closures, calls
// pay no push/pop.
static inline void vm_push_env(VM* vm, const std::shared_ptr<FrameEnv>& parent) {
    if (!g_program_has_closures) return;
    vm->frameEnvs.push_back(nullptr);
    vm->frameEnvParents.push_back(parent);
}
static inline void vm_pop_env(VM* vm) {
    if (!g_program_has_closures) return;
    if (!vm->frameEnvs.empty()) {
        vm->frameEnvs.pop_back();
        vm->frameEnvParents.pop_back();
    }
}
static inline void vm_reset_env(VM* vm, const std::shared_ptr<FrameEnv>& parent) {
    if (!g_program_has_closures) return;
    if (!vm->frameEnvs.empty()) {
        vm->frameEnvs.back() = nullptr;
        vm->frameEnvParents.back() = parent;
    }
}

// L12: looks up an upvalue by name in the captured environment chain.
// Starts at the environment where the current function was defined (parent),
// since names resolved as locals of the function itself use OP_LOAD/OP_STORE.
static Value* free_slot(VM* vm, const std::string& name) {
    if (vm->frameEnvParents.empty()) return nullptr;
    std::shared_ptr<FrameEnv> env = vm->frameEnvParents.back();
    while (env) {
        auto it = env->vars.find(name);
        if (it != env->vars.end()) return it->second.get();
        env = env->parent;
    }
    return nullptr;
}

// L12: ensures the current frame environment and migrates `owner`'s locals
// to cells. Called when evaluating an anonymous `fn(...)`.
static std::shared_ptr<FrameEnv> vm_capture_env(VM* vm, int owner) {
    if (vm->frameEnvs.empty()) return nullptr; // top of the script: no locals
    std::shared_ptr<FrameEnv>& env = vm->frameEnvs.back();
    if (!env) {
        env = std::make_shared<FrameEnv>();
        gc_register_obj(GC_OBJ_ENV, env);
        env->parent = vm->frameEnvParents.empty()
                          ? nullptr
                          : vm->frameEnvParents.back();
    }
    if (owner >= 0) sym_capture_owner(owner, *env);
    return env;
}

// L8: arbitrary table keys (number/text/boolean). The key is
// encoded into a canonical text ("s<text>" / "n<%.17g>") to keep the
// existing unordered_map<string,Value> and deterministic ordering.
static std::string table_key_encode(const Value& k) {
    switch (k.type) {
        case VAL_STRING:
            return "s" + k.s.str();
        case VAL_INT:
            return "n" + std::to_string(k.i); // L7: exact (without going through double)
        case VAL_FLOAT:
        case VAL_BOOL: {
            double d = k.f;
            if (std::isnan(d))
                throw std::runtime_error("Chave de tabela nao pode ser NaN");
            if (d == 0.0) d = 0.0; // -0 and +0 are the same key
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%.17g", d);
            return std::string("n") + buf;
        }
        default:
            throw std::runtime_error(
                "Chave de tabela invalida (use numero, texto ou booleano)");
    }
}
static std::string table_key_str(const std::string& s) { return "s" + s; }
static Value table_key_decode(const std::string& enc) {
    if (!enc.empty() && enc[0] == 's') return make_string(enc.substr(1));
    if (!enc.empty() && enc[0] == 'n') {
        std::string num = enc.substr(1);
        // L7: decimal integer (including > 2^53) returns as VAL_INT.
        bool decimal_int = !num.empty() &&
            num.find_first_of(".eE") == std::string::npos;
        if (decimal_int) {
            errno = 0;
            char* end = nullptr;
            long long iv = std::strtoll(num.c_str(), &end, 10);
            if (errno != ERANGE && end == num.c_str() + num.size())
                return make_int(iv);
        }
        try { return make_float(std::stod(num)); }
        catch (...) { return make_string(enc); }
    }
    return make_string(enc); // compatibility with old keys
}

// --- L11: metatables/metamethods -----------------------------------------
static Table* value_metatable(const Value& v) {
    if (v.type == VAL_TABLE && v.tb()) return v.tb()->metatable.get();
    if (v.type == VAL_STRUCT && v.st()) return v.st()->metatable.get();
    return nullptr;
}

static bool get_metamethod(const Value& v, const char* name, Value* out) {
    Table* mt = value_metatable(v);
    if (!mt) return false;
    auto it = mt->data.find(table_key_str(name));
    if (it == mt->data.end() || it->second.type == VAL_NIL) return false;
    *out = it->second;
    return true;
}

// Executes a script function synchronously within the same VM. The
// external call state is moved out (same as pcall) so that the
// metamethod RETURN does not pop the caller's frame.
static Value vm_call_mm(VM* vm, CodeStore* code, const Value& fn,
                        const Value* args, int argc, int line) {
    if (fn.type != VAL_FUNC || !fn.fn() || fn.fn()->entry < 0)
        throw std::runtime_error("metametodo nao e uma funcao");
    if (argc < 0) argc = 0;
    if (vm->sp + argc > STACK_MAX)
        throw std::runtime_error("Stack overflow - pilha cheia");
    int base = vm->sp;
    for (int i = 0; i < argc; i++) PUSH(vm, args[i]);

    auto extCall = std::move(vm->callStack);    vm->callStack = {};
    auto extResults = std::move(vm->callResults); vm->callResults = {};
    auto extMark = std::move(vm->frameMarkers); vm->frameMarkers = {};
    auto extSaves = std::move(vm->scopeSaves);  vm->scopeSaves = {};
    auto extFrames = std::move(vm->frames);     vm->frames = {};
    auto extEnvs = std::move(vm->frameEnvs);    vm->frameEnvs = {};
    auto extEnvParents = std::move(vm->frameEnvParents); vm->frameEnvParents = {};
    bool extRunning = vm->running;
    bool extClosures = g_program_has_closures;
    int extIp = vm->ip;
    int extArgc = vm->callArgc;
    int extLastRet = vm->lastRet;
    int extTopRet = vm->topRet;
    if (fn.fn()->env) g_program_has_closures = true;

    vm->callArgc = argc;
    vm->frames.push_back({InsStr(fn.fn()->name), line});
    vm_push_env(vm, fn.fn()->env);
    std::vector<SavedLocal> saved;
    sym_save_scope(fn.fn()->entry, saved);

    Value ret;
    try {
        vm_run(vm, code, fn.fn()->entry);
        int nret = vm->topRet;
        if (nret <= 0 || vm->sp <= base) ret = make_nil();
        else ret = vm->stack[vm->sp - nret];
    } catch (...) {
        vm->callStack = std::move(extCall);
        vm->callResults = std::move(extResults);
        vm->frameMarkers = std::move(extMark);
        vm->scopeSaves = std::move(extSaves);
        vm->frames = std::move(extFrames);
        vm->frameEnvs = std::move(extEnvs);
        vm->frameEnvParents = std::move(extEnvParents);
        vm->running = extRunning;
        g_program_has_closures = extClosures;
        vm->ip = extIp;
        vm->callArgc = extArgc;
        vm->lastRet = extLastRet;
        vm->topRet = extTopRet;
        sym_restore_scope(saved, 0);
        vm->sp = base;
        throw;
    }

    vm->callStack = std::move(extCall);
    vm->callResults = std::move(extResults);
    vm->frameMarkers = std::move(extMark);
    vm->scopeSaves = std::move(extSaves);
    vm->frames = std::move(extFrames);
    vm->frameEnvs = std::move(extEnvs);
    vm->frameEnvParents = std::move(extEnvParents);
    vm->running = extRunning;
    g_program_has_closures = extClosures;
    vm->ip = extIp;
    vm->callArgc = extArgc;
    vm->lastRet = extLastRet;
    vm->topRet = extTopRet;
    sym_restore_scope(saved, 0);
    vm->sp = base;
    return ret;
}

// __index: looks up the key in the table; if absent, follows the metatable
// chain (__index table) or calls __index(table, key).
static Value vm_index_get(VM* vm, CodeStore* code, const Value& obj,
                          const Value& idx, int line) {
    Value cur = obj;
    for (int depth = 0; depth < 100; depth++) {
        if (cur.type != VAL_TABLE || !cur.tb()) return make_nil();
        std::string key = table_key_encode(idx);
        auto it = cur.tb()->data.find(key);
        if (it != cur.tb()->data.end()) return it->second;
        Value mm;
        if (!get_metamethod(cur, "__index", &mm)) return make_nil();
        if (mm.type == VAL_TABLE) { cur = mm; continue; }
        if (mm.type == VAL_FUNC) {
            Value args[2] = {cur, idx};
            return vm_call_mm(vm, code, mm, args, 2, line);
        }
        throw std::runtime_error("__index deve ser tabela ou funcao");
    }
    throw std::runtime_error("__index: cadeia muito longa (ciclo?)");
}

// Field read: structs check their fields; if the field does not exist,
// __index (table or function) is used as a fallback.
static Value vm_field_get(VM* vm, CodeStore* code, const Value& obj,
                          const std::string& name, int line) {
    if (obj.type == VAL_TABLE) {
        // fast path: field present (avoids creating an extra Value/string)
        auto it = obj.tb()->data.find(table_key_str(name));
        if (it != obj.tb()->data.end()) return it->second;
        return vm_index_get(vm, code, obj, make_string(name), line);
    }
    if (obj.type == VAL_STRUCT && obj.st()) {
        int idx = struct_field_index(obj.st()->type_name, name);
        if (idx >= 0 && idx < (int)obj.st()->fields.size())
            return obj.st()->fields[idx];
        Value mm;
        if (get_metamethod(obj, "__index", &mm)) {
            Value key = make_string(name);
            if (mm.type == VAL_TABLE)
                return vm_index_get(vm, code, mm, key, line);
            if (mm.type == VAL_FUNC) {
                Value args[2] = {obj, key};
                return vm_call_mm(vm, code, mm, args, 2, line);
            }
            throw std::runtime_error("__index deve ser tabela ou funcao");
        }
        throw std::runtime_error("Campo '" + name + "' nao existe em '" +
                                 obj.st()->type_name + "'");
    }
    throw std::runtime_error("Acesso a campo em valor que nao e struct");
}

// __gc: registry of objects with finalizers. Since shared_ptr does not offer
// a mutable deleter after creation, finalizers run at the end of the
// script (before modules/threads are shut down). Cycles are left to
// L13; objects still alive at the end are also finalized.
struct GcEntry {
    std::weak_ptr<Table> table;
    std::weak_ptr<StructInstance> st;
};
static std::vector<GcEntry> g_gc_entries;
static std::mutex g_gc_mutex;

static void gc_register(const Value& v) {
    if (v.type == VAL_TABLE && v.tb()) {
        if (v.tb()->gc_registered) return;
        v.tb()->gc_registered = true;
        GcEntry e;
        e.table = std::static_pointer_cast<Table>(v.obj.p);
        std::lock_guard<std::mutex> lk(g_gc_mutex);
        g_gc_entries.push_back(e);
    } else if (v.type == VAL_STRUCT && v.st()) {
        if (v.st()->gc_registered) return;
        v.st()->gc_registered = true;
        GcEntry e;
        e.st = std::static_pointer_cast<StructInstance>(v.obj.p);
        std::lock_guard<std::mutex> lk(g_gc_mutex);
        g_gc_entries.push_back(e);
    }
}

void vm_clear_gc_finalizers() {
    std::lock_guard<std::mutex> lk(g_gc_mutex);
    g_gc_entries.clear();
}

void vm_run_gc_finalizers() {
    std::vector<GcEntry> entries;
    {
        std::lock_guard<std::mutex> lk(g_gc_mutex);
        entries.swap(g_gc_entries);
    }
    if (entries.empty()) return;

    VM vm;
    vm.sp = 0;
    for (int i = 0; i < STACK_MAX; i++) vm.stack[i] = make_float(0.0f);
    bool extClosures = g_program_has_closures;
    g_program_has_closures = true;

    for (const GcEntry& e : entries) {
        Value obj;
        if (auto t = e.table.lock()) {
            if (t->gc_finalized) continue;
            t->gc_finalized = true;
            obj.type = VAL_TABLE;
            obj.obj = t;
        } else if (auto s = e.st.lock()) {
            if (s->gc_finalized) continue;
            s->gc_finalized = true;
            obj.type = VAL_STRUCT;
            obj.obj = s;
        } else {
            continue;
        }
        Value mm;
        if (!get_metamethod(obj, "__gc", &mm) || mm.type != VAL_FUNC)
            continue;
        try {
            Value arg = obj;
            vm_call_mm(&vm, &code_arena(), mm, &arg, 1, 0);
        } catch (...) {
            // finalizers cannot bring down script shutdown
        }
    }
    g_program_has_closures = extClosures;
}


// ---------------------------------------------------------------------------
// L13: cycle collector by trial deletion.
//
// Every object that can form a cycle is registered when created (weak_ptr).
// Collection builds the graph, counts internal edges and subtracts them from use_count.
// Objects whose remaining use_count comes only from internal edges are
// candidates; starting from those with external references (globals, VM stack,
// local variables, etc.) everything still reachable is marked. The rest
// have their outgoing references cleared, breaking the cycle.
struct GcObjRef {
    GcObjKind kind;
    std::weak_ptr<void> wp;
};
static std::vector<GcObjRef> g_gc_objs;
static std::mutex g_gc_objs_mutex;

void gc_register_obj(GcObjKind kind, const std::shared_ptr<void>& obj) {
    if (!obj) return;
    // Common path (no script threads): direct push_back. With active
    // threads, the registry is shared and needs the mutex.
    if (!threads_any_active()) {
        g_gc_objs.push_back(GcObjRef{kind, obj});
        return;
    }
    std::lock_guard<std::mutex> lk(g_gc_objs_mutex);
    g_gc_objs.push_back(GcObjRef{kind, obj});
}

void vm_clear_gc_objects() {
    std::lock_guard<std::mutex> lk(g_gc_objs_mutex);
    g_gc_objs.clear();
}

struct GcInfo {
    GcObjKind kind;
    std::shared_ptr<void> strong;
    size_t internal = 0;
    bool live = false;
};

// Removes expired weak_ptrs from the registry. Without this the vector grows
// with every object created (REPL/long sessions) even when objects are released.
static void gc_prune_registry() {
    std::lock_guard<std::mutex> lk(g_gc_objs_mutex);
    size_t out = 0;
    for (size_t i = 0; i < g_gc_objs.size(); i++) {
        if (g_gc_objs[i].wp.expired()) continue;
        if (out != i) g_gc_objs[out] = std::move(g_gc_objs[i]);
        out++;
    }
    g_gc_objs.resize(out);
}

static void gc_build_infos(std::vector<GcInfo>& infos,
                           std::unordered_map<void*, size_t>& index) {
    std::vector<GcObjRef> raw;
    {
        std::lock_guard<std::mutex> lk(g_gc_objs_mutex);
        raw = g_gc_objs;
    }
    infos.clear();
    index.clear();
    infos.reserve(raw.size());
    for (const GcObjRef& e : raw) {
        std::shared_ptr<void> p = e.wp.lock();
        if (!p) continue;
        void* key = p.get();
        if (index.find(key) != index.end()) continue;
        index[key] = infos.size();
        GcInfo info;
        info.kind = e.kind;
        info.strong = std::move(p);
        infos.push_back(std::move(info));
    }
}

static void gc_collect_edges(std::vector<GcInfo>& infos,
                             const std::unordered_map<void*, size_t>& index,
                             std::vector<std::vector<size_t>>& children) {
    children.assign(infos.size(), {});
    auto add_value = [&](size_t from, const Value& v) {
        if (!v.obj.p) return;
        auto it = index.find(v.obj.p.get());
        if (it == index.end()) return;
        size_t to = it->second;
        infos[to].internal++;
        children[from].push_back(to);
    };
    auto add_ptr = [&](size_t from, const void* p) {
        if (!p) return;
        auto it = index.find(const_cast<void*>(p));
        if (it == index.end()) return;
        size_t to = it->second;
        infos[to].internal++;
        children[from].push_back(to);
    };

    for (size_t i = 0; i < infos.size(); i++) {
        GcInfo& info = infos[i];
        switch (info.kind) {
            case GC_OBJ_TABLE: {
                Table* t = static_cast<Table*>(info.strong.get());
                for (const auto& kv : t->data) add_value(i, kv.second);
                if (t->metatable) add_ptr(i, t->metatable.get());
                break;
            }
            case GC_OBJ_STRUCT: {
                StructInstance* st = static_cast<StructInstance*>(info.strong.get());
                for (const Value& v : st->fields) add_value(i, v);
                if (st->metatable) add_ptr(i, st->metatable.get());
                break;
            }
            case GC_OBJ_ARRAY: {
                ArrayData* a = static_cast<ArrayData*>(info.strong.get());
                for (const Value& v : a->elements) add_value(i, v);
                break;
            }
            case GC_OBJ_FUNC: {
                FuncRef* f = static_cast<FuncRef*>(info.strong.get());
                if (f->env) add_ptr(i, f->env.get());
                break;
            }
            case GC_OBJ_ENV: {
                FrameEnv* env = static_cast<FrameEnv*>(info.strong.get());
                for (const auto& kv : env->vars)
                    if (kv.second) add_value(i, *kv.second);
                if (env->parent) add_ptr(i, env->parent.get());
                break;
            }
            case GC_OBJ_MULTI: {
                MultiRet* m = static_cast<MultiRet*>(info.strong.get());
                for (const Value& v : m->values) add_value(i, v);
                break;
            }
        }
    }
}

static std::vector<size_t> gc_find_garbage(
    std::vector<GcInfo>& infos,
    const std::vector<std::vector<size_t>>& children) {
    std::vector<size_t> stack;
    for (size_t i = 0; i < infos.size(); i++) {
        long long ext = (long long)infos[i].strong.use_count() - 1 -
                        (long long)infos[i].internal;
        if (ext > 0) {
            infos[i].live = true;
            stack.push_back(i);
        }
    }
    while (!stack.empty()) {
        size_t i = stack.back();
        stack.pop_back();
        for (size_t j : children[i]) {
            if (!infos[j].live) {
                infos[j].live = true;
                stack.push_back(j);
            }
        }
    }
    std::vector<size_t> garbage;
    for (size_t i = 0; i < infos.size(); i++)
        if (!infos[i].live) garbage.push_back(i);
    return garbage;
}

static void gc_run_garbage_finalizers(const std::vector<GcInfo>& infos,
                                      const std::vector<size_t>& garbage) {
    if (!g_current_vm || !g_current_code) return;
    for (size_t idx : garbage) {
        const GcInfo& info = infos[idx];
        Value obj;
        Table* t = nullptr;
        StructInstance* st = nullptr;
        if (info.kind == GC_OBJ_TABLE) {
            t = static_cast<Table*>(info.strong.get());
            if (t->gc_finalized) continue;
            obj.type = VAL_TABLE;
            obj.obj = std::static_pointer_cast<Table>(info.strong);
        } else if (info.kind == GC_OBJ_STRUCT) {
            st = static_cast<StructInstance*>(info.strong.get());
            if (st->gc_finalized) continue;
            obj.type = VAL_STRUCT;
            obj.obj = std::static_pointer_cast<StructInstance>(info.strong);
        } else {
            continue;
        }
        Value mm;
        if (!get_metamethod(obj, "__gc", &mm) || mm.type != VAL_FUNC)
            continue;
        if (t) t->gc_finalized = true;
        if (st) st->gc_finalized = true;
        try {
            Value arg = obj;
            vm_call_mm(g_current_vm, g_current_code, mm, &arg, 1, 0);
        } catch (...) {
            // finalizers cannot bring down collectgarbage
        }
    }
}

static void gc_clear_garbage(std::vector<GcInfo>& infos,
                             const std::vector<size_t>& garbage) {
    for (size_t idx : garbage) {
        GcInfo& info = infos[idx];
        switch (info.kind) {
            case GC_OBJ_TABLE: {
                Table* t = static_cast<Table*>(info.strong.get());
                t->data.clear();
                t->metatable.reset();
                break;
            }
            case GC_OBJ_STRUCT: {
                StructInstance* st = static_cast<StructInstance*>(info.strong.get());
                st->fields.clear();
                st->metatable.reset();
                break;
            }
            case GC_OBJ_ARRAY: {
                ArrayData* a = static_cast<ArrayData*>(info.strong.get());
                a->elements.clear();
                break;
            }
            case GC_OBJ_FUNC: {
                FuncRef* f = static_cast<FuncRef*>(info.strong.get());
                f->env.reset();
                break;
            }
            case GC_OBJ_ENV: {
                FrameEnv* env = static_cast<FrameEnv*>(info.strong.get());
                env->vars.clear();
                env->parent.reset();
                break;
            }
            case GC_OBJ_MULTI: {
                MultiRet* m = static_cast<MultiRet*>(info.strong.get());
                m->values.clear();
                break;
            }
        }
    }
}

int gc_collect_cycles(bool run_finalizers) {
    // POP returns a copy of the value and leaves the old stack slot still
    // pointing to the object; slots above `sp` are unreachable, but
    // would count as external references. Clear them so cycles are
    // recognized.
    if (g_current_vm) {
        for (int i = g_current_vm->sp; i < STACK_MAX; i++)
            g_current_vm->stack[i] = make_float(0.0f);
    }
    gc_prune_registry();
    std::vector<GcInfo> infos;
    std::unordered_map<void*, size_t> index;
    gc_build_infos(infos, index);
    if (infos.empty()) return 0;

    std::vector<std::vector<size_t>> children;
    gc_collect_edges(infos, index, children);
    std::vector<size_t> garbage = gc_find_garbage(infos, children);

    if (run_finalizers && !garbage.empty()) {
        gc_run_garbage_finalizers(infos, garbage);
        // Finalizers can resurrect objects or create new cycles:
        // redo the analysis before breaking any reference.
        infos.clear();
        index.clear();
        gc_build_infos(infos, index);
        if (infos.empty()) return 0;
        gc_collect_edges(infos, index, children);
        garbage = gc_find_garbage(infos, children);
    }

    int collected = (int)garbage.size();
    gc_clear_garbage(infos, garbage);
    return collected;
}

static inline bool truthy_tol(const Value& v) {
    if (v.type == VAL_NIL) return false; // L6
    if (v.type == VAL_STRUCT || v.type == VAL_FUNC || v.type == VAL_TABLE ||
        v.type == VAL_COROUTINE) return true;
    if (v.type == VAL_STRING) return true; // L4/L6: any text is true
    if (v.type == VAL_COMPLEX)
        return !(std::abs(v.f) < 1e-6f && std::abs(v.im) < 1e-6f);
    if (v.type == VAL_INT) return v.i != 0;
    return !(std::abs(v.f) < 1e-6f);
}

static void require_real_pair(const Value& a, const Value& b, const char* op) {
    if (a.type == VAL_STRUCT || b.type == VAL_STRUCT)
        throw std::runtime_error(std::string("Operacao '") + op + "' nao suportada para structs");
    if (a.type == VAL_STRING || b.type == VAL_STRING)
        throw std::runtime_error(std::string("Operacao '") + op + "' nao suportada para texto");
    if (a.type == VAL_NIL || b.type == VAL_NIL)
        throw std::runtime_error(std::string("Operacao '") + op + "' nao suportada para nil");
    if (is_complex(a) || is_complex(b))
        throw std::runtime_error(std::string("Operacao '") + op + "' nao suportada para numeros complexos");
}

// Integer operand for '%' and bitwise operations (& | ^ ~ << >>):
// checks finiteness, int64 range and integrality (no silent truncation)
static long long req_int64(const Value& v, const char* op) {
    if (v.type == VAL_INT) return v.i;
    double d = v.f;
    const double LIMIT = 9223372036854775808.0; // 2^63 (exact in double)
    if (!(d >= -LIMIT && d < LIMIT))
        throw std::runtime_error(std::string("Operando fora da faixa inteira para '") + op + "'");
    if (d != std::floor(d))
        throw std::runtime_error(std::string("Operando nao inteiro para '") + op + "'");
    return (long long)d;
}

// Compares two values for < > <= >=:
//   string vs string  -> lexicographic order (bytes)
//   number vs number  -> numeric order
//   mixed/invalid     -> error
static bool cmp_lt(const Value& a, const Value& b, const char* op) {
    if (a.type == VAL_INT && b.type == VAL_INT) return a.i < b.i;
    if (a.type == VAL_FLOAT && b.type == VAL_FLOAT) return a.f < b.f;
    if (a.type == VAL_INT && b.type == VAL_FLOAT) return i64_lt_double(a.i, b.f);
    if (a.type == VAL_FLOAT && b.type == VAL_INT) {
        if (std::isnan(a.f)) return false;
        return !(i64_lt_double(b.i, a.f) || i64_eq_double(b.i, a.f));
    }
    if (a.type == VAL_STRING && b.type == VAL_STRING) return a.s < b.s;
    if (a.type == VAL_STRING || b.type == VAL_STRING)
        throw std::runtime_error(std::string("Comparacao invalida entre string e numero em '") + op + "'");
    require_real_pair(a, b, op);
    return v_re(a) < v_re(b);
}

static std::string fmt_float(double v) {
    std::ostringstream os;
    os << v;
    return os.str();
}

// Python style: str(1+2j) -> "1+2j", str(1+0j) -> "1", str(0j) -> "0j"
static std::string complex_to_string(const Value& v) {
    double re = v.f, im = v.im;
    if (im == 0.0f && re != 0.0f) return fmt_float(re);
    std::string s;
    if (re != 0.0f) s += fmt_float(re);
    if (re != 0.0f && im > 0.0f) s += "+";
    s += fmt_float(im);
    s += "j";
    return s;
}

// --- TEXT REPRESENTATION (repr/printf %s) ---
static std::string value_to_string(const Value& v);
static std::string struct_to_string(const Value& v);

// Compact list: [1, 2, 3] or [[1, 2], [3, 4]]
static std::string array_to_string(const Value& v) {
    if (!v.arr()) return "[]";
    int r = v.arr()->rows, c = v.arr()->cols;
    std::string s = "[";
    for (int i = 0; i < r; i++) {
        if (i) s += ", ";
        bool nested = (r > 1);
        if (nested) s += "[";
        for (int j = 0; j < c; j++) {
            if (j) s += ", ";
            s += value_to_string(v.arr()->elements[i * c + j]);
        }
        if (nested) s += "]";
    }
    return s + "]";
}

// Python dataclass style: Cars(brand="BMW", year=1999)
static std::string struct_to_string(const Value& v) {
    if (!v.st()) return "<null struct>";
    const StructType* t = struct_find(v.st()->type_name);
    std::string s = v.st()->type_name + "(";
    for (size_t i = 0; i < v.st()->fields.size(); i++) {
        if (i) s += ", ";
        if (t && i < t->fields.size()) s += t->fields[i] + "=";
        s += value_to_string(v.st()->fields[i]);
    }
    return s + ")";
}

static std::string table_to_string(const Value& v);

static std::string value_to_string(const Value& v) {
    switch (v.type) {
        case VAL_NIL:     return "nil";
        case VAL_STRING:  return "\"" + v.s + "\"";
        case VAL_COMPLEX: return complex_to_string(v);
        case VAL_STRUCT:  return struct_to_string(v);
        case VAL_ARRAY:   return array_to_string(v);
        case VAL_FUNC:    return "<funcao " + (v.fn() ? v.fn()->name : std::string("?")) + ">";
        case VAL_TABLE:   return table_to_string(v);
        case VAL_COROUTINE: return "<coroutine>";
        case VAL_INT:     return std::to_string(v.i);
        case VAL_MULTI:   return (v.mul() && !v.mul()->values.empty())
                                     ? value_to_string(v.mul()->values[0])
                                     : "nil";
        default:          return fmt_float(v.f);
    }
}

// Table (E3): {"key": value, ...} — sorted keys (determinism)
static std::string table_to_string(const Value& v) {
    if (!v.tb() || v.tb()->data.empty()) return "{}";
    std::vector<std::string> ks;
    ks.reserve(v.tb()->data.size());
    for (const auto& kv : v.tb()->data) ks.push_back(kv.first);
    std::sort(ks.begin(), ks.end());
    std::string out = "{";
    for (size_t i = 0; i < ks.size(); i++) {
        if (i) out += ", ";
        Value k = table_key_decode(ks[i]);
        if (k.type == VAL_STRING) out += "\"" + k.s.str() + "\"";
        else out += value_to_string(k);
        out += ": " + value_to_string(v.tb()->data.at(ks[i]));
    }
    return out + "}";
}

// Structural equality: strings, numbers (re+im), lists and structs
static bool value_eq(const Value& a, const Value& b) {
    if (a.type == VAL_NIL || b.type == VAL_NIL)
        return a.type == VAL_NIL && b.type == VAL_NIL;
    if (a.type == VAL_INT && b.type == VAL_INT) return a.i == b.i;
    if (a.type == VAL_INT && b.type == VAL_FLOAT) return i64_eq_double(a.i, b.f);
    if (a.type == VAL_FLOAT && b.type == VAL_INT) return i64_eq_double(b.i, a.f);
    if (a.type == VAL_FLOAT && b.type == VAL_FLOAT) return a.f == b.f;
    if (a.type == VAL_STRING || b.type == VAL_STRING)
        return a.type == b.type && a.s == b.s;
    if (a.type == VAL_STRUCT || b.type == VAL_STRUCT) {
        if (a.type != b.type || !a.st() || !b.st()) return false;
        if (a.st()->type_name != b.st()->type_name) return false;
        if (a.st()->fields.size() != b.st()->fields.size()) return false;
        for (size_t i = 0; i < a.st()->fields.size(); i++)
            if (!value_eq(a.st()->fields[i], b.st()->fields[i])) return false;
        return true;
    }
    if (a.type == VAL_ARRAY || b.type == VAL_ARRAY) {
        if (a.type != VAL_ARRAY || b.type != VAL_ARRAY || !a.arr() || !b.arr()) return false;
        if (a.arr()->rows != b.arr()->rows || a.arr()->cols != b.arr()->cols) return false;
        for (size_t i = 0; i < a.arr()->elements.size(); i++)
            if (!value_eq(a.arr()->elements[i], b.arr()->elements[i])) return false;
        return true;
    }
    if (a.type == VAL_FUNC || b.type == VAL_FUNC) {
        if (a.type != b.type || !a.fn() || !b.fn()) return false;
        return a.fn()->entry == b.fn()->entry;   // same function = equal (E2)
    }
    if (a.type == VAL_TABLE || b.type == VAL_TABLE) {
        if (a.type != b.type || !a.tb() || !b.tb()) return false;
        return a.tb() == b.tb();                 // structural equality? NO — identity (E3)
    }
    if (a.type == VAL_COROUTINE || b.type == VAL_COROUTINE) {
        if (a.type != b.type || !a.co() || !b.co()) return false;
        return a.co() == b.co();                 // identity (E5)
    }
    if (is_numeric(a) && is_numeric(b))
        return v_re(a) == v_re(b) && v_im(a) == v_im(b);
    return a.type == b.type && a.f == b.f;
}

// Unquoted text for str/tostring/println/printf %s. __tostring takes
// priority when the value has a metatable and the metamethod is a function.
static std::string value_to_text_raw(const Value& v) {
    if (v.type == VAL_NIL) return "nil";
    if (v.type == VAL_STRING) return v.s.str();
    if (v.type == VAL_COMPLEX) return complex_to_string(v);
    if (v.type == VAL_STRUCT) return struct_to_string(v);
    if (v.type == VAL_ARRAY) return array_to_string(v);
    if (v.type == VAL_FUNC || v.type == VAL_TABLE ||
        v.type == VAL_COROUTINE) return value_to_string(v);
    if (v.type == VAL_INT) return std::to_string(v.i);
    return fmt_float(v.f);
}

static std::string value_to_text(const Value& v) {
    Value mm;
    if (g_current_vm && g_current_code &&
        get_metamethod(v, "__tostring", &mm)) {
        if (mm.type != VAL_FUNC)
            throw std::runtime_error("__tostring deve ser funcao");
        Value arg = v;
        Value r = vm_call_mm(g_current_vm, g_current_code, mm, &arg, 1, 0);
        if (r.type != VAL_STRING)
            throw std::runtime_error("__tostring deve retornar string");
        return r.s.str();
    }
    return value_to_text_raw(v);
}

// E16d: comparisons as Value (used by the BIN2/BIN1 macros)
static inline Value do_eq(const Value& a, const Value& b) {
    return Value(VAL_FLOAT, value_eq(a, b) ? 1.0f : 0.0f);
}
static inline Value do_ne(const Value& a, const Value& b) {
    if (a.type == VAL_INT && b.type == VAL_INT)
        return Value(VAL_FLOAT, a.i != b.i ? 1.0f : 0.0f);
    if (a.type == VAL_INT && b.type == VAL_FLOAT)
        return Value(VAL_FLOAT, i64_eq_double(a.i, b.f) ? 0.0f : 1.0f);
    if (a.type == VAL_FLOAT && b.type == VAL_INT)
        return Value(VAL_FLOAT, i64_eq_double(b.i, a.f) ? 0.0f : 1.0f);
    if (is_numeric(a) && is_numeric(b))
        return Value(VAL_FLOAT,
                     (std::abs(v_re(a) - v_re(b)) >= 1e-6f ||
                      std::abs(v_im(a) - v_im(b)) >= 1e-6f) ? 1.0f : 0.0f);
    return Value(VAL_FLOAT, value_eq(a, b) ? 0.0f : 1.0f);
}
static inline Value do_lt(const Value& a, const Value& b) {
    return Value(VAL_FLOAT, cmp_lt(a, b, "<") ? 1.0f : 0.0f);
}
static inline Value do_gt(const Value& a, const Value& b) {
    return Value(VAL_FLOAT, cmp_lt(b, a, ">") ? 1.0f : 0.0f);
}
static inline Value do_le(const Value& a, const Value& b) {
    return Value(VAL_FLOAT, !cmp_lt(b, a, "<=") ? 1.0f : 0.0f);
}
static inline Value do_ge(const Value& a, const Value& b) {
    return Value(VAL_FLOAT, !cmp_lt(a, b, ">=") ? 1.0f : 0.0f);
}

// --- MATRIX UTILITIES ---
Value v_matrix(int r, int c) {
    Value v;
    v.type = VAL_ARRAY;
    v.obj = std::make_shared<ArrayData>();
    gc_register_obj(GC_OBJ_ARRAY, v.obj.p);
    v.arr()->rows = r;
    v.arr()->cols = c;
    v.arr()->elements.resize(r * c, {VAL_FLOAT, 0.0f});
    return v;
}

// --- LINEAR ALGEBRA ALGORITHMS (real and complex) ---
void mat_lu_exec(Value matrix, Value* res) {
    if (matrix.type != VAL_ARRAY || matrix.arr()->rows != matrix.arr()->cols) {
        throw std::runtime_error("LU requer matriz quadrada");
    }
    int n = matrix.arr()->rows;
    Value L = v_matrix(n, n);
    Value U = v_matrix(n, n);
    
    for (int i = 0; i < n; i++) {
        for (int k = i; k < n; k++) {
            Value sum = make_float(0.0f);
            for (int j = 0; j < i; j++) 
                sum = num_add(sum, num_mul(L.arr()->elements[i * n + j], U.arr()->elements[j * n + k]));
            U.arr()->elements[i * n + k] = num_sub(matrix.arr()->elements[i * n + k], sum);
        }
        for (int k = i; k < n; k++) {
            if (i == k) {
                L.arr()->elements[i * n + i] = make_float(1.0f);
            } else {
                Value sum = make_float(0.0f);
                for (int j = 0; j < i; j++) 
                    sum = num_add(sum, num_mul(L.arr()->elements[k * n + j], U.arr()->elements[j * n + i]));
                
                Value pivot = U.arr()->elements[i * n + i];
                double pr = v_re(pivot), pi = v_im(pivot);
                if (std::sqrt(pr * pr + pi * pi) < 1e-10f) {
                    throw std::runtime_error("Matriz singular na decomposição LU");
                }
                L.arr()->elements[k * n + i] = num_div(num_sub(matrix.arr()->elements[k * n + i], sum), pivot);
            }
        }
    }
    
    Value out = {VAL_ARRAY};
    out.obj = std::make_shared<ArrayData>();
    gc_register_obj(GC_OBJ_ARRAY, out.obj.p);
    out.arr()->elements.push_back(L);
    out.arr()->elements.push_back(U);
    out.arr()->rows = 2; 
    out.arr()->cols = 1;
    *res = out;
}

// --- BUILT-IN FUNCTIONS ---
// The actual map (name -> overloads; one per arity)
std::unordered_map<std::string, std::vector<Builtin>> builtinRegistry;
// E7/neg13: flat append-only — source of the id dispatch in OP_CALL_BUILTIN
std::vector<Builtin> g_builtins;

// Sandbox (E8/neg18) — see vm.h
long long g_step_limit = 0;
long long g_code_limit = 0;   // E9/neg22: REPL ceiling (0 = no limit)
long long g_symbol_limit = 0; // E9/neg22: REPL ceiling (0 = no limit)
long long g_steps = 0;
bool g_allow_fileio = true;
bool g_allow_modules = true;

void register_builtin(std::string name, int arity, BuiltinHandler handler) {
    std::vector<Builtin>& overloads = builtinRegistry[name];
    for (Builtin& b : overloads) {
        if (b.arity == arity) { // idempotent: keeps the id stable
            b.handler = handler;
            if (b.id >= 0 && b.id < (int)g_builtins.size())
                g_builtins[b.id].handler = handler;
            return;
        }
    }
    Builtin b{name, arity, handler, (int)g_builtins.size()};
    g_builtins.push_back(b);
    overloads.push_back(b);
}

const Builtin* builtin_lookup(const std::string& name, int argc) {
    auto it = builtinRegistry.find(name);
    if (it == builtinRegistry.end()) return nullptr;
    const Builtin* variadic = nullptr;
    for (const Builtin& b : it->second) {
        if (b.arity == argc) return &b;
        if (b.arity == -1) variadic = &b;
    }
    return variadic;
}

std::string builtin_arities(const std::string& name) {
    auto it = builtinRegistry.find(name);
    if (it == builtinRegistry.end()) return "?";
    std::string out;
    for (const Builtin& b : it->second) {
        if (!out.empty()) out += ", ";
        out += (b.arity == -1) ? std::string("variadico")
                               : std::to_string(b.arity);
    }
    return out;
}
void builtin_println(Value* args, int count, Value* res) {
    std::lock_guard<std::mutex> out_lk(g_cout_mutex);
    for (int i = 0; i < count; i++) {
        if (args[i].type == VAL_NIL) std::cout << "nil";
        else if (args[i].type == VAL_STRING) std::cout << args[i].s;
        else if (args[i].type == VAL_STRUCT) std::cout << value_to_text(args[i]);
        else if (args[i].type == VAL_COMPLEX) std::cout << complex_to_string(args[i]);
        else if (args[i].type == VAL_INT) std::cout << args[i].i;
        else if (args[i].type == VAL_FLOAT) std::cout << args[i].f;
        else if (args[i].type == VAL_FUNC) std::cout << value_to_string(args[i]);
        else if (args[i].type == VAL_TABLE) std::cout << value_to_text(args[i]);
        else if (args[i].type == VAL_COROUTINE) std::cout << value_to_string(args[i]);
        else if (args[i].type == VAL_ARRAY) {
            // Preserves cout flags so as not to affect subsequent number formatting
            std::ios::fmtflags savedFlags = std::cout.flags();
            std::streamsize savedPrec = std::cout.precision();
            std::cout << "\nMatriz [" << args[i].arr()->rows << "x" << args[i].arr()->cols << "]:\n";
            for (int i_r = 0; i_r < args[i].arr()->rows; i_r++) {
                std::cout << "  [ ";
                for (int i_c = 0; i_c < args[i].arr()->cols; i_c++) {
                    const Value& e = args[i].arr()->elements[i_r * args[i].arr()->cols + i_c];
                    if (e.type == VAL_COMPLEX) {
                        std::cout << std::setw(8) << complex_to_string(e) << " ";
                    } else if (e.type == VAL_STRING || e.type == VAL_FUNC ||
                               e.type == VAL_TABLE || e.type == VAL_STRUCT ||
                               e.type == VAL_COROUTINE) {
                        std::cout << std::setw(8) << value_to_string(e) << " ";
                    } else {
                        // Stable printing with 2 decimals: any value that
                        // would round to +/-0.00 becomes +0.00 (no "-0.00")
                        double cell = e.f;
                        if (cell > -0.005 && cell < 0.005) cell = 0.0;
                        std::cout << std::fixed << std::setprecision(2) << std::setw(8) << cell << " ";
                    }
                }
                std::cout << "]\n";
            }
            std::cout.flags(savedFlags);
            std::cout.precision(savedPrec);
        }
    }
    std::cout << std::endl;
    *res = {VAL_FLOAT, 1.0f};
}
void builtin_pow(Value* args, int count, Value* res) {
    if (!is_complex(args[0]) && !is_complex(args[1])) {
        *res = make_float(powf(args[0].f, args[1].f));
        return;
    }
    double ar = v_re(args[0]), ai = v_im(args[0]);
    double br = v_re(args[1]), bi = v_im(args[1]);

    // Integer power: binary exponentiation (same exactness as Python)
    if (bi == 0.0f && br == std::floor(br) && std::fabs(br) <= 1000000.0f) {
        int n = (int)br;
        Value base = make_complex(ar, ai);
        Value acc = make_complex(1.0f, 0.0f);
        int e = n < 0 ? -n : n;
        while (e > 0) {
            if (e & 1) acc = num_mul(acc, base);
            base = num_mul(base, base);
            e >>= 1;
        }
        if (n < 0) acc = num_div(make_complex(1.0f, 0.0f), acc);
        *res = acc;
        return;
    }

    // General case: z^w = exp(w * log(z))
    double r = std::sqrt(ar * ar + ai * ai);
    if (r == 0.0f) {
        if (br < 0.0f) throw std::runtime_error("Divisão por zero");
        *res = make_complex(0.0f, 0.0f);
        return;
    }
    double theta = std::atan2(ai, ar);
    double ln_r = std::log(r);
    double xr = br * ln_r - bi * theta;
    double xi = br * theta + bi * ln_r;
    double mag = std::exp(xr);
    *res = make_complex(mag * std::cos(xi), mag * std::sin(xi));
}
void builtin_sin(Value* args, int count, Value* res) {
    if (is_complex(args[0])) {
        double x = args[0].f, y = args[0].im;
        *res = make_complex(std::sin(x) * std::cosh(y), std::cos(x) * std::sinh(y));
    } else {
        *res = make_float(sinf(args[0].f));
    }
}
void builtin_cos(Value* args, int count, Value* res) {
    if (is_complex(args[0])) {
        double x = args[0].f, y = args[0].im;
        *res = make_complex(std::cos(x) * std::cosh(y), -std::sin(x) * std::sinh(y));
    } else {
        *res = make_float(cosf(args[0].f));
    }
}
void builtin_tan(Value* args, int count, Value* res) {
    if (is_complex(args[0])) {
        double x = args[0].f, y = args[0].im;
        Value s = make_complex(std::sin(x) * std::cosh(y), std::cos(x) * std::sinh(y));
        Value c = make_complex(std::cos(x) * std::cosh(y), -std::sin(x) * std::sinh(y));
        *res = num_div(s, c);
    } else {
        *res = make_float(tanf(args[0].f));
    }
}
void builtin_mat_index(Value* args, int count, Value* res) {
    // args[0]: Matrix (ArrayData object)
    // args[1]: Row (f)
    // args[2]: Column (f)
    if (args[0].type != VAL_ARRAY) throw std::runtime_error("mat_index: O primeiro argumento deve ser uma matriz");
    
    int r = (int)args[1].f-1;
    int c = (int)args[2].f-1;
    int total_cols = args[0].arr()->cols;

    // Bounds check
    if (r < 0 || r >= args[0].arr()->rows || c < 0 || c >= total_cols) {
        throw std::runtime_error("mat_index: Coordenadas fora dos limites da matriz");
    }

    *res = {VAL_FLOAT, (double)(r * total_cols + c)};
}

// L2: input([prompt]) -> string read from stdin (without the trailing newline).
// EOF/empty line -> "". The prompt, if given, is printed without a newline.
void builtin_input(Value* args, int count, Value* res) {
    if (count >= 1 && args[0].type == VAL_STRING) {
        std::lock_guard<std::mutex> out_lk(g_cout_mutex);
        std::cout << args[0].s << std::flush;
    }
    std::string line;
    std::getline(std::cin, line);
    *res = Value{VAL_STRING, 0, line};
}
REGISTER_BUILTIN("println", -1, builtin_println);
REGISTER_BUILTIN("pow", 2, builtin_pow);
REGISTER_BUILTIN("sin", 1, builtin_sin);
REGISTER_BUILTIN("cos", 1, builtin_cos);    
REGISTER_BUILTIN("tan", 1, builtin_tan);
REGISTER_BUILTIN("mat_index", 3, builtin_mat_index);

// len(x): string length (characters) or array length (total elements)
void builtin_len(Value* args, int count, Value* res) {
    if (args[0].type == VAL_STRING) {
        *res = make_float((double)args[0].s.size());
    } else if (args[0].type == VAL_ARRAY) {
        *res = make_float((double)args[0].arr()->elements.size());
    } else if (args[0].type == VAL_TABLE) {
        *res = make_float((double)args[0].tb()->data.size()); // E3: number of keys
    } else {
        throw std::runtime_error("len: tipo nao suportado (use string, lista ou tabela)");
    }
}
REGISTER_BUILTIN("len", 1, builtin_len);

// L5: select(n, ...) / select("#", ...) and unpack(list[, i[, j]]).
// Return MultiRet; OP_CALL_BUILTIN decides how many values the caller wants.
void builtin_select(Value* args, int count, Value* res) {
    if (count < 1)
        throw std::runtime_error("select: informe o indice");
    int n = count - 1;
    if (args[0].type == VAL_STRING && args[0].s == "#") {
        *res = make_float((double)n);
        return;
    }
    if (args[0].type != VAL_FLOAT && args[0].type != VAL_INT)
        throw std::runtime_error("select: indice deve ser numero ou \"#\"");
    int i = (int)(args[0].type == VAL_INT ? args[0].i : (long long)args[0].f);
    if (i < 0) i = n + i + 1;
    if (i < 1)
        throw std::runtime_error("select: indice fora da faixa");
    auto m = std::make_shared<MultiRet>();
    gc_register_obj(GC_OBJ_MULTI, m);
    for (int k = i; k <= n; k++) m->values.push_back(args[k]);
    res->type = VAL_MULTI;
    res->obj = std::move(m);
}
REGISTER_BUILTIN("select", -1, builtin_select);

void builtin_unpack(Value* args, int count, Value* res) {
    if (count < 1)
        throw std::runtime_error("unpack: informe a lista");
    if (args[0].type != VAL_ARRAY || !args[0].arr())
        throw std::runtime_error("unpack: esperava uma lista");
    int len = (int)args[0].arr()->elements.size();
    int i = count >= 2 ? (int)args[1].f : 0;
    int j = count >= 3 ? (int)args[2].f : len - 1;
    if (i < 0 || j >= len || i > j) {
        if (i > j) { // empty range: zero values
            auto m = std::make_shared<MultiRet>();
            gc_register_obj(GC_OBJ_MULTI, m);
            res->type = VAL_MULTI;
            res->obj = std::move(m);
            return;
        }
        throw std::runtime_error("unpack: indice fora da faixa");
    }
    auto m = std::make_shared<MultiRet>();
    gc_register_obj(GC_OBJ_MULTI, m);
    for (int k = i; k <= j; k++) m->values.push_back(args[0].arr()->elements[k]);
    res->type = VAL_MULTI;
    res->obj = std::move(m);
}
REGISTER_BUILTIN("unpack", -1, builtin_unpack);

// --- TABLES (phase E3) ---
// keys(t): keys in a 1D list, SORTED (deterministic printing/output)
void builtin_keys(Value* args, int count, Value* res) {
    (void)count;
    if (args[0].type != VAL_TABLE || !args[0].tb())
        throw std::runtime_error("keys: esperava uma tabela");
    std::vector<std::string> ks;
    ks.reserve(args[0].tb()->data.size());
    for (const auto& kv : args[0].tb()->data) ks.push_back(kv.first);
    std::sort(ks.begin(), ks.end());
    Value out = v_matrix(1, (int)ks.size());
    for (size_t i = 0; i < ks.size(); i++)
        out.arr()->elements[i] = table_key_decode(ks[i]);
    *res = out;
}
REGISTER_BUILTIN("keys", 1, builtin_keys);

// has(t, key): 1 if the key exists, 0 otherwise
void builtin_has(Value* args, int count, Value* res) {
    (void)count;
    if (args[0].type != VAL_TABLE || !args[0].tb())
        throw std::runtime_error("has: esperava uma tabela");
    *res = make_float(args[0].tb()->data.count(table_key_encode(args[1])) ? 1.0 : 0.0);
}
REGISTER_BUILTIN("has", 2, builtin_has);

// remove(t, key) | remove(s, index[, count])
// Table: deletes the key; returns 1 if it existed, 0 otherwise.
// Text (Arduino style): returns a NEW string without the characters.
void builtin_remove(Value* args, int count, Value* res) {
    if (args[0].type == VAL_STRING) {
        long long idx = req_int64(args[1], "remove");
        long long len = (long long)args[0].s.size();
        long long n = (count >= 3) ? req_int64(args[2], "remove") : len - idx;
        if (idx < 0) idx = 0;
        if (idx >= len || n <= 0) { *res = args[0]; return; }
        if (n > len - idx) n = len - idx;
        std::string s = args[0].s.str();
        s.erase((size_t)idx, (size_t)n);
        *res = make_string(s);
        return;
    }
    if (count != 2)
        throw std::runtime_error("remove: use remove(tabela, chave) ou remove(texto, indice[, quantidade])");
    if (args[0].type != VAL_TABLE || !args[0].tb())
        throw std::runtime_error("remove: esperava uma tabela ou texto");
    *res = make_float(args[0].tb()->data.erase(table_key_encode(args[1])) ? 1.0 : 0.0);
}
REGISTER_BUILTIN("remove", 2, builtin_remove);
REGISTER_BUILTIN("remove", 3, builtin_remove);

// append(list, x): appends x to the end of the 1D list (mutates and returns the list)
void builtin_append(Value* args, int count, Value* res) {
    if (args[0].type != VAL_ARRAY)
        throw std::runtime_error("append: o primeiro argumento deve ser uma lista");
    if (args[0].arr()->rows != 1)
        throw std::runtime_error("append: apenas em listas 1D");
    args[0].arr()->elements.push_back(args[1]);
    args[0].arr()->cols = (int)args[0].arr()->elements.size();
    *res = args[0];
}
REGISTER_BUILTIN("append", 2, builtin_append);

// --- LIST/TABLE LIBRARY (phase E6, negative 10) ---
// concat(list [, sep]): joins 1D elements into text (default sep "")
void builtin_concat(Value* args, int count, Value* res) {
    if (count < 1)
        throw std::runtime_error("concat: informe o texto ou a lista");
    // Text (Arduino style): concat(s, ...) returns a new string.
    if (args[0].type == VAL_STRING) {
        std::string out = args[0].s.str();
        for (int i = 1; i < count; i++) out += value_to_text(args[i]);
        *res = make_string(out);
        return;
    }
    if (args[0].type != VAL_ARRAY || args[0].arr()->rows != 1)
        throw std::runtime_error("concat: o primeiro argumento deve ser uma lista 1D");
    std::string sep = "";
    if (count >= 2) {
        if (args[1].type != VAL_STRING)
            throw std::runtime_error("concat: o separador deve ser texto");
        sep = args[1].s;
    }
    const auto& els = args[0].arr()->elements;
    std::string out;
    for (size_t i = 0; i < els.size(); i++) {
        if (i) out += sep;
        // text is inserted raw (quoted repr is only for println/table)
        if (els[i].type == VAL_STRING) out += els[i].s;
        else out += value_to_string(els[i]);
    }
    *res = make_string(out);
}
REGISTER_BUILTIN("concat", -1, builtin_concat);

// sort(list): sorts numbers in ascending order (mutates; returns the list)
void builtin_sort(Value* args, int count, Value* res) {
    (void)count;
    if (args[0].type != VAL_ARRAY || args[0].arr()->rows != 1)
        throw std::runtime_error("sort: o argumento deve ser uma lista 1D");
    for (const Value& v : args[0].arr()->elements)
        if (v.type != VAL_FLOAT && v.type != VAL_INT)
            throw std::runtime_error("sort: a lista deve conter apenas numeros");
    std::sort(args[0].arr()->elements.begin(), args[0].arr()->elements.end(),
              [](const Value& a, const Value& b) {
                  if (a.type == VAL_INT && b.type == VAL_INT) return a.i < b.i;
                  return v_re(a) < v_re(b);
              });
    *res = args[0];
}
REGISTER_BUILTIN("sort", 1, builtin_sort);

// --- COMPLEX NUMBER BUILT-INS ---
void builtin_abs(Value* args, int count, Value* res) {
    if (is_complex(args[0])) {
        *res = make_float(std::sqrt(args[0].f * args[0].f + args[0].im * args[0].im));
    } else if (args[0].type == VAL_INT) {
        long long i = args[0].i;
        *res = make_int(i < 0 ? i64_wrap_sub(0, i) : i);
    } else {
        *res = make_float(std::fabs(v_re(args[0])));
    }
}

// Principal root (cmath style): accepts negatives and returns complex
void builtin_sqrt(Value* args, int count, Value* res) {
    double re = args[0].f;
    double im = v_im(args[0]);
    if (!is_complex(args[0]) && re >= 0.0f) {
        *res = make_float(std::sqrt(re));
        return;
    }
    double r = std::sqrt(re * re + im * im);
    double sr = std::sqrt(std::max(0.0, (r + re) / 2.0f));
    double si = std::sqrt(std::max(0.0, (r - re) / 2.0f));
    if (im < 0.0f) si = -si;
    *res = make_complex(sr, si);
}

void builtin_conj(Value* args, int count, Value* res) {
    if (is_complex(args[0])) *res = make_complex(args[0].f, -args[0].im);
    else *res = make_float(args[0].f);
}

void builtin_real(Value* args, int count, Value* res) { *res = make_float(v_re(args[0])); }
void builtin_imag(Value* args, int count, Value* res) { *res = make_float(v_im(args[0])); }

// rect(a, b) -> a + b*j (rectangular components)
void builtin_rect(Value* args, int count, Value* res) {
    *res = make_complex(v_re(args[0]), v_re(args[1]));
}

// polar(modulus, angle_rad) -> modulus*(cos + j*sin) (polar components)
void builtin_polar(Value* args, int count, Value* res) {
    double mod = v_re(args[0]);
    double ang = v_re(args[1]);
    *res = make_complex(mod * std::cos(ang), mod * std::sin(ang));
}

REGISTER_BUILTIN("abs", 1, builtin_abs);
REGISTER_BUILTIN("sqrt", 1, builtin_sqrt);
REGISTER_BUILTIN("conj", 1, builtin_conj);
REGISTER_BUILTIN("real", 1, builtin_real);
REGISTER_BUILTIN("imag", 1, builtin_imag);
REGISTER_BUILTIN("rect", 2, builtin_rect);
REGISTER_BUILTIN("polar", 2, builtin_polar);

// --- EXTRA NUMERIC BUILT-INS (roadmap item 6) ---

// log(x): natural; real > 0 -> double; real < 0 -> complex (cmath style)
void builtin_log(Value* args, int count, Value* res) {
    if (is_complex(args[0])) {
        double re = v_re(args[0]), im = v_im(args[0]);
        double r = std::sqrt(re * re + im * im);
        if (r == 0.0f) throw std::runtime_error("log: logaritmo de zero");
        *res = make_complex(std::log(r), std::atan2(im, re));
        return;
    }
    double x = args[0].f;
    if (x == 0.0f) throw std::runtime_error("log: logaritmo de zero");
    if (x < 0.0f) *res = make_complex(std::log(-x), (double)M_PI);
    else *res = make_float(std::log(x));
}

// log10(x): base 10
void builtin_log10(Value* args, int count, Value* res) {
    if (is_complex(args[0])) {
        builtin_log(args, count, res);
        *res = num_div(*res, make_float(std::log(10.0f)));
        return;
    }
    double x = args[0].f;
    if (x == 0.0f) throw std::runtime_error("log10: logaritmo de zero");
    if (x < 0.0f) {
        builtin_log(args, count, res);
        *res = num_div(*res, make_float(std::log(10.0f)));
    } else *res = make_float(std::log10(x));
}

// exp(x): e^x; complex: e^re * (cos im + j sin im)
void builtin_exp(Value* args, int count, Value* res) {
    if (is_complex(args[0])) {
        double re = v_re(args[0]), im = v_im(args[0]);
        double m = std::exp(re);
        *res = make_complex(m * std::cos(im), m * std::sin(im));
        return;
    }
    *res = make_float(std::exp(args[0].f));
}

// atan2(y, x) (real only)
void builtin_atan2(Value* args, int count, Value* res) {
    if (is_complex(args[0]) || is_complex(args[1]))
        throw std::runtime_error("atan2: nao suportado para numeros complexos");
    *res = make_float(std::atan2(args[0].f, args[1].f));
}

static void require_real_value(const Value& v, const char* fn) {
    if (is_complex(v))
        throw std::runtime_error(std::string(fn) + ": nao suportado para numeros complexos");
}

void builtin_floor(Value* args, int count, Value* res) {
    require_real_value(args[0], "floor");
    if (args[0].type == VAL_INT) { *res = args[0]; return; }
    *res = make_float(std::floor(v_re(args[0])));
}
void builtin_ceil(Value* args, int count, Value* res) {
    require_real_value(args[0], "ceil");
    if (args[0].type == VAL_INT) { *res = args[0]; return; }
    *res = make_float(std::ceil(v_re(args[0])));
}
void builtin_round(Value* args, int count, Value* res) {
    require_real_value(args[0], "round");
    if (args[0].type == VAL_INT) { *res = args[0]; return; }
    *res = make_float(std::round(v_re(args[0])));
}

// floordiv(a, b): division rounded down (floor(a/b)).
// We do not use `//` as an operator: the lexer treats // as a line comment
// (didactic decision/E6 — see roadmap negative 9).
void builtin_floordiv(Value* args, int count, Value* res) {
    (void)count;
    require_real_value(args[0], "floordiv");
    require_real_value(args[1], "floordiv");
    if (args[1].f == 0.0)
        throw std::runtime_error("floordiv: divisao por zero");
    *res = make_float(std::floor(args[0].f / args[1].f));
}
REGISTER_BUILTIN("floordiv", 2, builtin_floordiv);

// variadic min/max: min(1, 5, 2) -> 1
void builtin_min(Value* args, int count, Value* res) {
    if (count < 1) throw std::runtime_error("min: requer ao menos 1 argumento");
    Value best = args[0];
    bool all_int = best.type == VAL_INT;
    for (int i = 0; i < count; i++) {
        require_real_value(args[i], "min");
        if (args[i].type != VAL_INT) all_int = false;
        if (i > 0 && cmp_lt(args[i], best, "min")) best = args[i];
    }
    *res = all_int ? best : make_float(v_re(best));
}
void builtin_max(Value* args, int count, Value* res) {
    if (count < 1) throw std::runtime_error("max: requer ao menos 1 argumento");
    Value best = args[0];
    bool all_int = best.type == VAL_INT;
    for (int i = 0; i < count; i++) {
        require_real_value(args[i], "max");
        if (args[i].type != VAL_INT) all_int = false;
        if (i > 0 && cmp_lt(best, args[i], "max")) best = args[i];
    }
    *res = all_int ? best : make_float(v_re(best));
}

// Random number generator (fixed per execution; seeded once)
static std::mt19937& rng() {
    static std::mt19937 gen(std::random_device{}());
    return gen;
}

// random() -> double in [0, 1)
void builtin_random(Value* args, int count, Value* res) {
    std::uniform_real_distribution<float> dist(0.0f, 1.0f);
    *res = make_float(dist(rng()));
}

// random_int(a, b) -> uniform integer in [a, b] (inclusive)
void builtin_random_int(Value* args, int count, Value* res) {
    require_real_value(args[0], "random_int");
    require_real_value(args[1], "random_int");
    int a = (int)args[0].f, b = (int)args[1].f;
    if (a > b) throw std::runtime_error("random_int: a deve ser <= b");
    std::uniform_int_distribution<int> dist(a, b);
    *res = make_float((double)dist(rng()));
}

REGISTER_BUILTIN("log", 1, builtin_log);
REGISTER_BUILTIN("log10", 1, builtin_log10);
REGISTER_BUILTIN("exp", 1, builtin_exp);
REGISTER_BUILTIN("atan2", 2, builtin_atan2);
REGISTER_BUILTIN("floor", 1, builtin_floor);
REGISTER_BUILTIN("ceil", 1, builtin_ceil);
REGISTER_BUILTIN("round", 1, builtin_round);
REGISTER_BUILTIN("min", -1, builtin_min);
REGISTER_BUILTIN("max", -1, builtin_max);
REGISTER_BUILTIN("random", 0, builtin_random);
REGISTER_BUILTIN("random_int", 2, builtin_random_int);

// --- OS LIBRARY (phase E6, negative 10) ---
// time(): seconds since the epoch (wall clock; -1 if unavailable)
void builtin_time(Value* args, int count, Value* res) {
    (void)args; (void)count;
    std::time_t t = std::time(nullptr);
    *res = make_float(t == (std::time_t)-1 ? -1.0 : (double)t);
}
REGISTER_BUILTIN("time", 0, builtin_time);

// clock(): milliseconds since process start (monotonic clock)
void builtin_clock(Value* args, int count, Value* res) {
    (void)args; (void)count;
    static const auto start = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
    *res = make_float(ms);
}
REGISTER_BUILTIN("clock", 0, builtin_clock);

// --- IMPLEMENTATION OF LINEAR ALGEBRA BUILT-INS ---

void builtin_det(Value* args, int count, Value* res) {
    if (args[0].type != VAL_ARRAY) throw std::runtime_error("det: Requer matriz");
    ArrayData* m = args[0].arr();
    if (m->rows == 2 && m->cols == 2) {
        *res = num_sub(num_mul(m->elements[0], m->elements[3]),
                       num_mul(m->elements[1], m->elements[2]));
    } else if (m->rows == 3 && m->cols == 3) {
        auto g = [&](int r, int c) -> Value { return m->elements[r * 3 + c]; };
        Value t1 = num_mul(g(0,0), num_sub(num_mul(g(1,1), g(2,2)), num_mul(g(1,2), g(2,1))));
        Value t2 = num_mul(g(0,1), num_sub(num_mul(g(1,0), g(2,2)), num_mul(g(1,2), g(2,0))));
        Value t3 = num_mul(g(0,2), num_sub(num_mul(g(1,0), g(2,1)), num_mul(g(1,1), g(2,0))));
        *res = num_add(num_sub(t1, t2), t3);
    } else throw std::runtime_error("det: Suporta apenas 2x2 ou 3x3");
}

void builtin_transpose(Value* args, int count, Value* res) {
    if (args[0].type != VAL_ARRAY) throw std::runtime_error("transpose: Requer matriz");
    ArrayData* m = args[0].arr();
    Value out = v_matrix(m->cols, m->rows);
    for (int r = 0; r < m->rows; r++) {
        for (int c = 0; c < m->cols; c++) {
            out.arr()->elements[c * m->rows + r] = m->elements[r * m->cols + c];
        }
    }
    *res = out;
}

void builtin_trace(Value* args, int count, Value* res) {
    if (args[0].type != VAL_ARRAY || args[0].arr()->rows != args[0].arr()->cols)
        throw std::runtime_error("trace: Requer matriz quadrada");
    Value sum = make_float(0.0f);
    for (int i = 0; i < args[0].arr()->rows; i++)
        sum = num_add(sum, args[0].arr()->elements[i * args[0].arr()->cols + i]);
    *res = sum;
}

void builtin_identity(Value* args, int count, Value* res) {
    int n = (int)args[0].f;
    if (n <= 0) throw std::runtime_error("identity: Tamanho deve ser > 0");
    Value out = v_matrix(n, n);
    for (int i = 0; i < n; i++) out.arr()->elements[i * n + i].f = 1.0f;
    *res = out;
}

void builtin_lu(Value* args, int count, Value* res) {
    mat_lu_exec(args[0], res);
}
void builtin_inv(Value* args, int count, Value* res) {
    if (args[0].type != VAL_ARRAY) throw std::runtime_error("inv: Requer matriz");
    ArrayData* m = args[0].arr();

    if (m->rows != m->cols) throw std::runtime_error("inv: Requer matriz quadrada");

    // We compute the determinant using the function we already have
    Value det_val;
    builtin_det(args, 1, &det_val);
    double dr = v_re(det_val), di = v_im(det_val);
    if (std::sqrt(dr * dr + di * di) < 1e-10f)
        throw std::runtime_error("inv: Matriz singular (det=0) nao possui inversa");

    if (m->rows == 2) {
        // Formula: (1/det) * [ d  -b ]
        //                   [ -c  a ]
        Value out = v_matrix(2, 2);
        Value a = m->elements[0];
        Value b = m->elements[1];
        Value c = m->elements[2];
        Value d = m->elements[3];
        Value zero = make_float(0.0f);

        out.arr()->elements[0] = num_div(d, det_val);
        out.arr()->elements[1] = num_div(num_sub(zero, b), det_val);
        out.arr()->elements[2] = num_div(num_sub(zero, c), det_val);
        out.arr()->elements[3] = num_div(a, det_val);
        *res = out;
    } 
    else {
        // For 3x3 or larger, you could implement Gauss-Jordan or cofactors
        throw std::runtime_error("inv: Atualmente suporta apenas matrizes 2x2");
    }
}

// --- FUNCTION REGISTRATION ---
REGISTER_BUILTIN("det", 1, builtin_det);
REGISTER_BUILTIN("transpose", 1, builtin_transpose);
REGISTER_BUILTIN("trace", 1, builtin_trace);
REGISTER_BUILTIN("identity", 1, builtin_identity);
REGISTER_BUILTIN("lu", 1, builtin_lu);
REGISTER_BUILTIN("inv", 1, builtin_inv);

// --- EIGENVALUES / EIGENVECTORS (roadmap item 9) ---

// magnitude of a value (real or complex)
static double c_absv(const Value& v) {
    if (is_complex(v)) return std::sqrt(v.f * v.f + v.im * v.im);
    return std::fabs(v.f);
}

// cube root (negative real -> real; complex -> principal branch)
static Value c_cbrt(const Value& v) {
    if (!is_complex(v)) return make_float(std::cbrt(v.f));
    Value e = make_float(1.0f / 3.0f);
    Value args[2] = {v, e};
    Value r;
    builtin_pow(args, 2, &r);
    return r;
}

// eigvals(M): eigenvalues of 2x2 or 3x3 (real or complex).
// Deterministic order, but not sorted.
void builtin_eigvals(Value* args, int count, Value* res) {
    if (args[0].type != VAL_ARRAY) throw std::runtime_error("eigvals: Requer matriz");
    ArrayData* m = args[0].arr();
    int n = m->rows;
    if (n != m->cols) throw std::runtime_error("eigvals: Requer matriz quadrada");
    if (n != 2 && n != 3) throw std::runtime_error("eigvals: Suporta apenas 2x2 ou 3x3");

    if (n == 2) {
        Value a = m->elements[0], b = m->elements[1];
        Value c = m->elements[2], d = m->elements[3];
        Value tr = num_add(a, d);
        Value det = num_sub(num_mul(a, d), num_mul(b, c));
        Value disc;
        {
            Value tmp = num_sub(num_mul(tr, tr), num_mul(make_float(4.0f), det));
            builtin_sqrt(&tmp, 1, &disc);
        }
        Value two = make_float(2.0f);
        Value out = v_matrix(1, 2);
        out.arr()->elements[0] = num_div(num_add(tr, disc), two);
        out.arr()->elements[1] = num_div(num_sub(tr, disc), two);
        // Removes numeric noise: im ~ 0 becomes pure real
        for (Value& v : out.arr()->elements)
            if (is_complex(v) && std::fabs(v.im) < 1e-4f) v = make_float(v.f);
        *res = out;
        return;
    }

    // --- 3x3: Cardano on the characteristic polynomial ---
    auto g = [&](int r, int c) -> Value { return m->elements[r * 3 + c]; };
    Value a = g(0,0), b = g(0,1), c = g(0,2);
    Value d = g(1,0), e = g(1,1), f = g(1,2);
    Value h = g(2,0), i = g(2,1), j = g(2,2);

    Value three = make_float(3.0f);
    Value c2 = num_add(num_add(a, e), j);                       // trace
    Value m00 = num_sub(num_mul(e, j), num_mul(f, i));          // 2x2 minors
    Value m11 = num_sub(num_mul(a, j), num_mul(c, h));
    Value m22 = num_sub(num_mul(a, e), num_mul(b, d));
    Value c1 = num_add(num_add(m00, m11), m22);
    Value c0;
    builtin_det(args, 1, &c0);

    // Depressed form: lambda = t + c2/3  =>  t^3 + p t + q = 0
    Value p = num_sub(c1, num_div(num_mul(c2, c2), three));
    Value c2_3 = num_div(c2, three);
    Value q = num_sub(
        num_add(num_sub(make_float(0.0f), c0), num_div(num_mul(c2, c1), three)),
        num_div(num_mul(make_float(2.0f), num_mul(c2, num_mul(c2, c2))), make_float(27.0f)));

    Value half_q = num_div(q, make_float(2.0f));
    Value p3 = num_div(num_mul(p, num_mul(p, p)), make_float(27.0f));
    Value disc;
    {
        Value tmp = num_add(num_mul(half_q, half_q), p3);
        builtin_sqrt(&tmp, 1, &disc);
    }

    Value u = c_cbrt(num_sub(disc, half_q));                    // u^3 = -q/2 + sqrt(...)
    Value v;
    if (c_absv(u) < 1e-12f) {
        v = c_cbrt(num_sub(num_sub(make_float(0.0f), half_q), disc));
    } else {
        v = num_div(num_sub(make_float(0.0f), p), num_mul(three, u));
    }

    // w = e^{2pi i/3}; t_k = u w^k + v w^{-k}
    double w3 = std::sqrt(3.0f) / 2.0f;
    Value w1 = make_complex(-0.5f, w3);
    Value w2 = make_complex(-0.5f, -w3);
    Value t0 = num_add(u, v);
    Value t1 = num_add(num_mul(u, w1), num_mul(v, w2));
    Value t2 = num_add(num_mul(u, w2), num_mul(v, w1));

    Value out = v_matrix(1, 3);
    out.arr()->elements[0] = num_add(t0, c2_3);
    out.arr()->elements[1] = num_add(t1, c2_3);
    out.arr()->elements[2] = num_add(t2, c2_3);
    // Removes numeric noise: im ~ 0 becomes pure real
    for (Value& v : out.arr()->elements)
        if (is_complex(v) && std::fabs(v.im) < 1e-4f) v = make_float(v.f);
    *res = out;
}

// eigvecs(M): eigenvectors as COLUMNS of the returned matrix (n x n).
// Each column corresponds to eigvals(M)[col]; defined up to a scalar.
void builtin_eigvecs(Value* args, int count, Value* res) {
    if (args[0].type != VAL_ARRAY) throw std::runtime_error("eigvecs: Requer matriz");
    ArrayData* m = args[0].arr();
    int n = m->rows;
    if (n != m->cols) throw std::runtime_error("eigvecs: Requer matriz quadrada");
    if (n != 2 && n != 3) throw std::runtime_error("eigvecs: Suporta apenas 2x2 ou 3x3");

    Value lams;
    builtin_eigvals(args, 1, &lams);

    Value out = v_matrix(n, n); // zeroed
    const double EPS = 1e-5f;

    for (int k = 0; k < n; k++) {
        Value lam = lams.arr()->elements[k];

        if (n == 2) {
            Value r00 = num_sub(m->elements[0], lam), r01 = m->elements[1];
            Value r10 = m->elements[2], r11 = num_sub(m->elements[3], lam);
            Value v0, v1;
            if (c_absv(r00) + c_absv(r01) > EPS) {
                v0 = r01;  v1 = num_sub(make_float(0.0f), r00);   // [r01, -r00]
            } else if (c_absv(r10) + c_absv(r11) > EPS) {
                v0 = r11;  v1 = num_sub(make_float(0.0f), r10);   // [r11, -r10]
            } else {
                v0 = make_float(1.0f); v1 = make_float(0.0f);      // A = lambda I
            }
            out.arr()->elements[0 * n + k] = v0;
            out.arr()->elements[1 * n + k] = v1;
        } else {
            // rows of (A - lambda I)
            Value r[3][3];
            for (int row = 0; row < 3; row++)
                for (int col = 0; col < 3; col++) {
                    Value base = m->elements[row * 3 + col];
                    r[row][col] = (row == col) ? num_sub(base, lam) : base;
                }
            auto cross = [&](const Value* p, const Value* q, Value* o) {
                o[0] = num_sub(num_mul(p[1], q[2]), num_mul(p[2], q[1]));
                o[1] = num_sub(num_mul(p[2], q[0]), num_mul(p[0], q[2]));
                o[2] = num_sub(num_mul(p[0], q[1]), num_mul(p[1], q[0]));
            };
            int pairs[3][2] = {{0, 1}, {0, 2}, {1, 2}};
            Value vec[3] = {make_float(1.0f), make_float(0.0f), make_float(0.0f)};
            for (int t = 0; t < 3; t++) {
                Value cand[3];
                cross(r[pairs[t][0]], r[pairs[t][1]], cand);
                if (c_absv(cand[0]) + c_absv(cand[1]) + c_absv(cand[2]) > EPS) {
                    for (int z = 0; z < 3; z++) vec[z] = cand[z];
                    break;
                }
            }
            for (int row = 0; row < 3; row++)
                out.arr()->elements[row * n + k] = vec[row];
        }
    }
    // Eliminates residual -0.0 (cosmetic)
    for (Value& v : out.arr()->elements) {
        if (v.type == VAL_COMPLEX) {
            if (v.f == 0.0f) v.f = 0.0f;
            if (v.im == 0.0f) v.im = 0.0f;
        } else if (v.type == VAL_FLOAT && v.f == 0.0f) {
            v.f = 0.0f;
        }
    }
    *res = out;
}

REGISTER_BUILTIN("eigvals", 1, builtin_eigvals);
REGISTER_BUILTIN("eigvecs", 1, builtin_eigvecs);

// printf("fmt", ...) — conversions: %s %d %i %f %g %j %c %%
// Formats WITHOUT a newline (reused by the file module's fprintf).
// fn = builtin name for error messages ("printf", "fprintf").
std::string format_values(const std::string& fmt, Value* args, int argc,
                          const char* fn) {
    std::ostringstream os;
    int ai = 0;
    const std::string prefix = std::string(fn) + ": ";
    for (size_t i = 0; i < fmt.size(); i++) {
        char c = fmt[i];
        if (c != '%') { os << c; continue; }
        if (i + 1 >= fmt.size())
            throw std::runtime_error(prefix + "'%' sem conversao de formato");
        char spec = fmt[++i];
        if (spec == '%') { os << '%'; continue; }
        if (ai >= argc)
            throw std::runtime_error(prefix + std::string("argumento insuficiente para '%") + spec + "'");
        const Value& v = args[ai++];
        bool numeric = (v.type == VAL_FLOAT || v.type == VAL_INT || v.type == VAL_COMPLEX);
        switch (spec) {
            case 's':
                if (v.type == VAL_NIL) os << "nil";
                else if (v.type == VAL_STRING) os << v.s;
                else if (v.type == VAL_STRUCT) os << value_to_text(v);
                else if (v.type == VAL_ARRAY) os << array_to_string(v);
                else if (v.type == VAL_COMPLEX) os << complex_to_string(v);
                else if (v.type == VAL_FUNC) os << value_to_string(v);
                else if (v.type == VAL_TABLE) os << value_to_text(v);
                else if (v.type == VAL_COROUTINE) os << value_to_string(v);
                else if (v.type == VAL_INT) os << v.i;
                else os << fmt_float(v.f);
                break;
            case 'd':
            case 'i':
                if (!numeric)
                    throw std::runtime_error(prefix + std::string("'") + spec + "' invalido para este tipo");
                os << (v.type == VAL_INT ? v.i : (long long)v.f);
                break;
            case 'f':
                if (!numeric)
                    throw std::runtime_error(prefix + "'%f' invalido para este tipo");
                os << std::fixed << std::setprecision(6) << v_re(v);
                break;
            case 'g':
                if (!numeric)
                    throw std::runtime_error(prefix + "'%g' invalido para este tipo");
                os << (v.type == VAL_INT ? std::to_string(v.i) : fmt_float(v.f));
                break;
            case 'j':
                if (!numeric)
                    throw std::runtime_error(prefix + "'%j' invalido para este tipo");
                if (v.type == VAL_COMPLEX) os << complex_to_string(v);
                else if (v.type == VAL_INT) os << v.i;
                else os << fmt_float(v.f);
                break;
            case 'c':
                if (!numeric)
                    throw std::runtime_error(prefix + "'%c' invalido para este tipo");
                os << (char)(v.type == VAL_INT ? v.i : (long long)v.f);
                break;
            default:
                throw std::runtime_error(prefix + std::string("conversao desconhecida '%") + spec + "'");
        }
    }
    return os.str();
}

// printf: formats, prints WITHOUT a newline and returns the character count.
static void builtin_printf(Value* args, int count, Value* res) {
    if (count < 1)
        throw std::runtime_error("printf: requer uma string de formato");
    if (args[0].type != VAL_STRING)
        throw std::runtime_error("printf: o primeiro argumento deve ser uma string");

    std::string out = format_values(args[0].s, args + 1, count - 1, "printf");
    {
        std::lock_guard<std::mutex> out_lk(g_cout_mutex);
        std::cout << out;
    }
    *res = make_float((double)out.size());
}
REGISTER_BUILTIN("printf", -1, builtin_printf);

// str(x) — converts a value to a string without quotes (concatenation and text())
static void builtin_str(Value* args, int count, Value* res) {
    (void)count;
    std::string out = value_to_text(args[0]);
    Value v;
    v.type = VAL_STRING;
    v.s = out;
    *res = v;
}
REGISTER_BUILTIN("str", 1, builtin_str);

// --- TEXT LIBRARY (phase E6, negative 10) ---
static void require_string_arg(const Value& v, const char* fn) {
    if (v.type != VAL_STRING)
        throw std::runtime_error(std::string(fn) + ": esperava texto");
}

void builtin_upper(Value* args, int count, Value* res) {
    (void)count;
    require_string_arg(args[0], "upper");
    std::string s = args[0].s;
    for (char& c : s) c = (char)std::toupper((unsigned char)c);
    *res = make_string(s);
}
REGISTER_BUILTIN("upper", 1, builtin_upper);

void builtin_lower(Value* args, int count, Value* res) {
    (void)count;
    require_string_arg(args[0], "lower");
    std::string s = args[0].s;
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    *res = make_string(s);
}
REGISTER_BUILTIN("lower", 1, builtin_lower);

// rep(s, n): repeats the text n times (integer n >= 0)
void builtin_rep(Value* args, int count, Value* res) {
    (void)count;
    require_string_arg(args[0], "rep");
    require_real_value(args[1], "rep");
    long long n = req_int64(args[1], "rep");
    if (n < 0) throw std::runtime_error("rep: n deve ser >= 0");
    if (n > 1000000) throw std::runtime_error("rep: n muito grande (max 1000000)");
    std::string out;
    out.reserve(args[0].s.size() * (size_t)n);
    for (long long i = 0; i < n; i++) out += args[0].s;
    *res = make_string(out);
}
REGISTER_BUILTIN("rep", 2, builtin_rep);

// find(s, sub): index (0-based) of the first occurrence or -1
void builtin_find(Value* args, int count, Value* res) {
    (void)count;
    require_string_arg(args[0], "find");
    require_string_arg(args[1], "find");
    size_t p = args[0].s.find(args[1].s);
    *res = make_float(p == std::string::npos ? -1.0 : (double)p);
}
REGISTER_BUILTIN("find", 2, builtin_find);

// trim(s): removes spaces/tabs/newlines at both ends
void builtin_trim(Value* args, int count, Value* res) {
    (void)count;
    require_string_arg(args[0], "trim");
    const std::string& s = args[0].s;
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) { *res = make_string(""); return; }
    size_t b = s.find_last_not_of(" \t\r\n");
    *res = make_string(s.substr(a, b - a + 1));
}
REGISTER_BUILTIN("trim", 1, builtin_trim);

// --- COROUTINES (phase E5, negative 7) ---
// cocreate(f): new coroutine (own VM, fixed stack) pointing to f
void builtin_cocreate(Value* args, int count, Value* res) {
    (void)count;
    if (args[0].type != VAL_FUNC || !args[0].fn())
        throw std::runtime_error("cocreate: esperava uma funcao");
    Value v;
    v.type = VAL_COROUTINE;
    v.obj = std::make_shared<Coroutine>();
    v.co()->entry = args[0].fn()->entry;
    v.co()->env = args[0].fn()->env; // L12: closure coroutine
    *res = v;
}
REGISTER_BUILTIN("cocreate", 1, builtin_cocreate);

// coresume(co, ...): runs until coyield or end; returns a 1x2 matrix {ok, value}
//   ok=1 -> value = yielded value or return value (end)
//   ok=0 -> value = error message + traceback | "dead coroutine"
void builtin_coresume(Value* args, int count, Value* res) {
    if (count < 1)
        throw std::runtime_error("coresume: informe a coroutine");
    if (args[0].type != VAL_COROUTINE || !args[0].co())
        throw std::runtime_error("coresume: esperava uma coroutine");
    Coroutine* c = args[0].co();
    Value out = v_matrix(1, 2);
    if (c->status == "morto") {
        out.arr()->elements[0] = make_float(0.0);
        out.arr()->elements[1] = make_string("coroutine morta");
        *res = out;
        return;
    }
    if (c->status == "rodando")
        throw std::runtime_error("coresume: coroutine ja em execucao");

    if (c->started) {
        // Resumption: coyield consumed 1 value; returns 1 (or "0"; extra
        // call arguments are discarded — yield has only 1 value)
        Value rv = (count >= 2) ? args[1] : make_float(0.0);
        if (c->vm.sp >= STACK_MAX)
            throw std::runtime_error("coresume: pilha da coroutine cheia");
        c->vm.stack[c->vm.sp++] = rv;
    } else {
        // 1st execution: args become the parameters of the target function
        for (int i = 1; i < count; i++) {
            if (c->vm.sp >= STACK_MAX)
                throw std::runtime_error("coresume: pilha da coroutine cheia");
            c->vm.stack[c->vm.sp++] = args[i];
        }
    }

    Coroutine* prev = g_active_coro;
    g_active_coro = c;
    c->status = "rodando";
    c->yielded = false;
    bool first = !c->started;
    c->started = true;
    if (first) {
        c->vm.callArgc = count > 0 ? count - 1 : 0;
        vm_push_env(&c->vm, c->env); // L12
        sym_save_scope(c->entry, c->vm.scopeSaves); // clears locals
    }
    try {
        vm_run(&c->vm, g_current_code, first ? c->entry : -1); // -1 = continue
        c->status = "morto";
        c->finished = true;
        int nret = c->vm.topRet;
        c->returnVal = (nret > 0 && c->vm.sp >= nret)
                           ? c->vm.stack[c->vm.sp - nret] // L5: 1st value
                           : make_float(0.0);
        out.arr()->elements[0] = make_float(1.0);
        out.arr()->elements[1] = c->returnVal;
    } catch (YieldSignal&) {
        c->status = "suspenso";
        out.arr()->elements[0] = make_float(1.0);
        out.arr()->elements[1] = c->yieldVal;
    } catch (const std::exception& e) {
        c->status = "morto";
        c->errorMsg = e.what();
        for (const auto& fr : c->vm.frames)
            c->errorMsg += "\n  em " + fr.first.str() +
                           " (linha " + std::to_string(fr.second) + ")";
        out.arr()->elements[0] = make_float(0.0);
        out.arr()->elements[1] = make_string(c->errorMsg);
    }
    g_active_coro = prev;
    *res = out;
}
REGISTER_BUILTIN("coresume", -1, builtin_coresume);

// coyield(v): suspends the coroutine returning v to coresume (E5)
void builtin_coyield(Value* args, int count, Value* res) {
    (void)count; (void)res;
    if (!g_active_coro)
        throw std::runtime_error("coyield: fora de coroutine");
    g_active_coro->yielded = true;
    g_active_coro->yieldVal = args[0];
    throw YieldSignal();
}
REGISTER_BUILTIN("coyield", 1, builtin_coyield);

// costatus(co): "new" | "running" | "suspended" | "dead"
void builtin_costatus(Value* args, int count, Value* res) {
    (void)count;
    if (args[0].type != VAL_COROUTINE || !args[0].co())
        throw std::runtime_error("costatus: esperava uma coroutine");
    Value v;
    v.type = VAL_STRING;
    v.s = args[0].co()->status;
    *res = v;
}
REGISTER_BUILTIN("costatus", 1, builtin_costatus);

// L2: registered LAST so as not to shift the IDs of existing builtins
// (the disasm prints the id in A: the goldens stay stable).
REGISTER_BUILTIN("input", -1, builtin_input);

// ---- L4: basic library (type/tostring/tonumber/assert/error/raw*) ----
void builtin_type(Value* args, int, Value* res) {
    const char* t = "?";
    switch (args[0].type) {
        case VAL_INT:
        case VAL_FLOAT: t = "number"; break;
        case VAL_STRING: t = "string"; break;
        case VAL_BOOL: t = "boolean"; break;
        case VAL_ARRAY: t = "array"; break;
        case VAL_STRUCT: t = "struct"; break;
        case VAL_FUNC: t = "function"; break;
        case VAL_TABLE: t = "table"; break;
        case VAL_COROUTINE: t = "thread"; break;
        case VAL_COMPLEX: t = "complex"; break;
        case VAL_NIL: t = "nil"; break;
        case VAL_MULTI: t = "multi"; break;
    }
    *res = Value{VAL_STRING, 0, std::string(t)};
}
REGISTER_BUILTIN("type", 1, builtin_type);

void builtin_tostring(Value* args, int count, Value* res) {
    builtin_str(args, count, res); // same format as str()
}
REGISTER_BUILTIN("tostring", 1, builtin_tostring);

// tonumber(v[, base]) — base 10 uses strtod (accepts 0x/1e3); other bases
// 2..36 are parsed manually. Failure returns nil (L6).
void builtin_tonumber(Value* args, int count, Value* res) {
    if (args[0].type == VAL_INT) {
        *res = make_int(args[0].i);
        return;
    }
    if (args[0].type == VAL_FLOAT) {
        *res = make_float(args[0].f);
        return;
    }
    if (args[0].type == VAL_BOOL) {
        *res = make_int(args[0].f != 0.0 ? 1 : 0);
        return;
    }
    if (args[0].type != VAL_STRING) { *res = make_nil(); return; }
    std::string t = args[0].s.str();
    size_t b = 0, e = t.size();
    while (b < e && std::isspace((unsigned char)t[b])) b++;
    while (e > b && std::isspace((unsigned char)t[e - 1])) e--;
    t = t.substr(b, e - b);
    if (t.empty()) { *res = make_nil(); return; }
    int base = (count >= 2) ? (int)args[1].f : 10;
    if (base < 2 || base > 36)
        throw std::runtime_error("tonumber: base deve ser 2..36");
    if (base == 10) {
        // L7: decimal integer text fits in int64 -> exact VAL_INT.
        if (t.find_first_of(".eE") == std::string::npos) {
            errno = 0;
            char* iend = nullptr;
            long long iv = std::strtoll(t.c_str(), &iend, 10);
            if (errno != ERANGE && iend != t.c_str() && iend && *iend == '\0') {
                *res = make_int(iv);
                return;
            }
        }
        char* end = nullptr;
        double v = std::strtod(t.c_str(), &end);
        if (end != t.c_str() && end && *end == '\0') { *res = make_float(v); return; }
        *res = make_nil();
        return;
    }
    size_t i = 0;
    bool neg = false;
    if (t[i] == '+' || t[i] == '-') { neg = (t[i] == '-'); i++; }
    if (base == 16 && i + 1 < t.size() && t[i] == '0' &&
        (t[i + 1] == 'x' || t[i + 1] == 'X')) i += 2;
    if (i >= t.size()) { *res = make_nil(); return; }
    double v = 0.0;
    unsigned long long acc = 0;
    bool exact = true;
    for (; i < t.size(); i++) {
        char c = t[i];
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'z') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'Z') d = c - 'A' + 10;
        else { *res = make_nil(); return; }
        if (d >= base) { *res = make_nil(); return; }
        v = v * base + d;
        if (exact) {
            if (acc > (~0ULL - (unsigned)d) / (unsigned)base) exact = false;
            else acc = acc * (unsigned)base + (unsigned)d;
        }
    }
    // L7: bases 2..36 return exact int64 when it fits.
    if (exact && (neg ? acc <= 9223372036854775808ULL
                       : acc <= 9223372036854775807ULL)) {
        *res = make_int(neg ? (long long)(0ULL - acc) : (long long)acc);
        return;
    }
    *res = make_float(neg ? -v : v);
}
REGISTER_BUILTIN("tonumber", -1, builtin_tonumber);

void builtin_assert(Value* args, int count, Value* res) {
    if (count >= 1 && truthy_tol(args[0])) { *res = args[0]; return; }
    std::string msg = (count >= 2) ? value_to_text(args[1]) : "assert falhou";
    throw std::runtime_error(msg);
}
REGISTER_BUILTIN("assert", -1, builtin_assert);

void builtin_error(Value* args, int count, Value* res) {
    (void)res;
    std::string msg = (count >= 1) ? value_to_text(args[0]) : "error";
    throw std::runtime_error(msg);
}
REGISTER_BUILTIN("error", -1, builtin_error);

void builtin_rawget(Value* args, int count, Value* res) {
    if (args[0].type != VAL_TABLE)
        throw std::runtime_error("rawget: espera uma tabela");
    if (count < 2)
        throw std::runtime_error("rawget: informe a chave");
    std::string key = table_key_encode(args[1]);
    auto it = args[0].tb()->data.find(key);
    *res = (it == args[0].tb()->data.end()) ? make_nil() : it->second;
}
REGISTER_BUILTIN("rawget", 2, builtin_rawget);

void builtin_rawset(Value* args, int count, Value* res) {
    if (args[0].type != VAL_TABLE)
        throw std::runtime_error("rawset: espera uma tabela");
    if (count < 3)
        throw std::runtime_error("rawset: informe chave e valor");
    std::string key = table_key_encode(args[1]);
    if (args[2].type == VAL_NIL) args[0].tb()->data.erase(key);
    else args[0].tb()->data[key] = args[2];
    *res = args[0];
}
REGISTER_BUILTIN("rawset", 3, builtin_rawset);

// --- L11: metatables ---
void builtin_setmetatable(Value* args, int count, Value* res) {
    (void)count;
    if (args[0].type != VAL_TABLE && args[0].type != VAL_STRUCT)
        throw std::runtime_error("setmetatable: espera uma tabela ou struct");
    if (args[1].type == VAL_NIL) {
        if (args[0].type == VAL_TABLE) args[0].tb()->metatable.reset();
        else args[0].st()->metatable.reset();
        *res = args[0];
        return;
    }
    if (args[1].type != VAL_TABLE || !args[1].tb())
        throw std::runtime_error("setmetatable: metatable deve ser tabela ou nil");
    std::shared_ptr<Table> mt =
        std::static_pointer_cast<Table>(args[1].obj.p);
    if (args[0].type == VAL_TABLE) args[0].tb()->metatable = mt;
    else args[0].st()->metatable = mt;
    Value mm;
    if (get_metamethod(args[0], "__gc", &mm)) gc_register(args[0]);
    *res = args[0];
}
REGISTER_BUILTIN("setmetatable", 2, builtin_setmetatable);

void builtin_getmetatable(Value* args, int count, Value* res) {
    (void)count;
    if (args[0].type != VAL_TABLE && args[0].type != VAL_STRUCT)
        throw std::runtime_error("getmetatable: espera uma tabela ou struct");
    Table* mt = value_metatable(args[0]);
    if (!mt) { *res = make_nil(); return; }
    Value out;
    out.type = VAL_TABLE;
    if (args[0].type == VAL_TABLE)
        out.obj = args[0].tb()->metatable;
    else
        out.obj = args[0].st()->metatable;
    *res = out;
}
REGISTER_BUILTIN("getmetatable", 1, builtin_getmetatable);

// L13: manual cycle collection. Returns how many objects were released.
void builtin_collectgarbage(Value* args, int count, Value* res) {
    (void)args;
    (void)count;
    *res = make_int(gc_collect_cycles(true));
}
REGISTER_BUILTIN("collectgarbage", -1, builtin_collectgarbage);

// L8: generic table iteration (deterministic canonical order).
// No multiple returns yet: next returns [k, v] or [] at the end; pairs
// returns a list of [k, v] pairs; ipairs returns the values from 0..n-1.
static std::vector<std::string> sorted_table_keys(const Value& t) {
    std::vector<std::string> ks;
    ks.reserve(t.tb()->data.size());
    for (const auto& kv : t.tb()->data) ks.push_back(kv.first);
    std::sort(ks.begin(), ks.end());
    return ks;
}

void builtin_next(Value* args, int count, Value* res) {
    if (args[0].type != VAL_TABLE || !args[0].tb())
        throw std::runtime_error("next: espera uma tabela");
    std::vector<std::string> ks = sorted_table_keys(args[0]);
    size_t pos = 0;
    if (count >= 2) {
        std::string key = table_key_encode(args[1]);
        auto it = std::find(ks.begin(), ks.end(), key);
        if (it == ks.end())
            throw std::runtime_error("next: chave invalida");
        pos = (size_t)(it - ks.begin()) + 1;
    }
    if (pos >= ks.size()) { *res = v_matrix(1, 0); return; }
    Value out = v_matrix(1, 2);
    out.arr()->elements[0] = table_key_decode(ks[pos]);
    out.arr()->elements[1] = args[0].tb()->data.at(ks[pos]);
    *res = out;
}
REGISTER_BUILTIN("next", -1, builtin_next);

void builtin_pairs(Value* args, int, Value* res) {
    if (args[0].type != VAL_TABLE || !args[0].tb())
        throw std::runtime_error("pairs: espera uma tabela");
    std::vector<std::string> ks = sorted_table_keys(args[0]);
    Value out = v_matrix(1, (int)ks.size());
    for (size_t i = 0; i < ks.size(); i++) {
        Value pair = v_matrix(1, 2);
        pair.arr()->elements[0] = table_key_decode(ks[i]);
        pair.arr()->elements[1] = args[0].tb()->data.at(ks[i]);
        out.arr()->elements[i] = pair;
    }
    *res = out;
}
REGISTER_BUILTIN("pairs", 1, builtin_pairs);

void builtin_ipairs(Value* args, int, Value* res) {
    if (args[0].type != VAL_TABLE || !args[0].tb())
        throw std::runtime_error("ipairs: espera uma tabela");
    std::vector<Value> vals;
    for (int i = 0;; i++) {
        auto it = args[0].tb()->data.find(table_key_encode(make_float(i)));
        if (it == args[0].tb()->data.end()) break;
        vals.push_back(it->second);
    }
    Value out = v_matrix(1, (int)vals.size());
    for (size_t i = 0; i < vals.size(); i++)
        out.arr()->elements[i] = vals[i];
    *res = out;
}
REGISTER_BUILTIN("ipairs", 1, builtin_ipairs);

// ---- L15: subsets of string/math/os/utf8 ---------------------------
// sub(s, i[, j]) — 0-based inclusive; negative indices count from the end.
void builtin_sub(Value* args, int count, Value* res) {
    require_string_arg(args[0], "sub");
    int len = (int)args[0].s.size();
    int i = (count >= 2) ? (int)args[1].f : 0;
    int j = (count >= 3) ? (int)args[2].f : len - 1;
    if (i < 0) i += len;
    if (j < 0) j += len;
    if (i < 0) i = 0;
    if (j >= len) j = len - 1;
    if (i > j || i >= len || j < 0) { *res = make_string(""); return; }
    *res = make_string(args[0].s.substr(i, j - i + 1));
}
REGISTER_BUILTIN("sub", -1, builtin_sub);

// byte(s[, i]) — code of byte i (0-based); 0 if out of bounds.
void builtin_byte(Value* args, int count, Value* res) {
    require_string_arg(args[0], "byte");
    int i = (count >= 2) ? (int)args[1].f : 0;
    if (i < 0 || i >= (int)args[0].s.size()) { *res = make_float(0); return; }
    *res = make_float((unsigned char)args[0].s[i]);
}
REGISTER_BUILTIN("byte", -1, builtin_byte);

// char(b1, b2, ...) — string with the given bytes (0..255).
void builtin_char(Value* args, int count, Value* res) {
    std::string out;
    out.reserve((size_t)count);
    for (int i = 0; i < count; i++) {
        int b = (int)args[i].f;
        if (b < 0 || b > 255)
            throw std::runtime_error("char: byte fora de 0..255");
        out += (char)(unsigned char)b;
    }
    *res = make_string(out);
}
REGISTER_BUILTIN("char", -1, builtin_char);

// format(fmt, ...) — like printf, but returns the string (does not print).
void builtin_format(Value* args, int count, Value* res) {
    if (count < 1 || args[0].type != VAL_STRING)
        throw std::runtime_error("format: o primeiro argumento deve ser texto");
    *res = make_string(format_values(args[0].s, args + 1, count - 1, "format"));
}
REGISTER_BUILTIN("format", -1, builtin_format);

void builtin_reverse(Value* args, int, Value* res) {
    require_string_arg(args[0], "reverse");
    std::string s = args[0].s;
    std::reverse(s.begin(), s.end());
    *res = make_string(s);
}
REGISTER_BUILTIN("reverse", 1, builtin_reverse);

void builtin_fmod(Value* args, int, Value* res) {
    require_real_pair(args[0], args[1], "fmod");
    double b = v_re(args[1]);
    if (b == 0.0) throw std::runtime_error("fmod: divisao por zero");
    *res = make_float(std::fmod(v_re(args[0]), b));
}
REGISTER_BUILTIN("fmod", 2, builtin_fmod);

// ---- Arduino String (free functions; strings are immutable) -------------
static char arduino_char_arg(const Value& v, const char* fn) {
    if (v.type == VAL_STRING) {
        if (v.s.size() != 1)
            throw std::runtime_error(std::string(fn) + ": esperava um caractere");
        return v.s[0];
    }
    require_real_value(v, fn);
    long long b = req_int64(v, fn);
    if (b < 0 || b > 255)
        throw std::runtime_error(std::string(fn) + ": byte fora de 0..255");
    return (char)(unsigned char)b;
}

static long long arduino_index_arg(const Value& v, const char* fn) {
    require_real_value(v, fn);
    return req_int64(v, fn);
}

static std::string arduino_int_base(long long n, int base) {
    if (base < 2 || base > 36)
        throw std::runtime_error("String: base deve estar em 2..36");
    static const char* digits = "0123456789abcdefghijklmnopqrstuvwxyz";
    bool neg = n < 0;
    unsigned long long u = neg ? (unsigned long long)(-(n + 1)) + 1ULL
                               : (unsigned long long)n;
    std::string out;
    do {
        out += digits[u % (unsigned)base];
        u /= (unsigned)base;
    } while (u != 0);
    if (neg) out += '-';
    std::reverse(out.begin(), out.end());
    return out;
}

static std::string arduino_float_decimals(double v, long long dec) {
    if (dec < 0) throw std::runtime_error("String: numero de casas deve ser >= 0");
    if (dec > 20) throw std::runtime_error("String: numero de casas muito grande (max 20)");
    std::ostringstream os;
    os << std::fixed << std::setprecision((int)dec) << v;
    return os.str();
}

void builtin_string_ctor(Value* args, int count, Value* res) {
    if (count == 0) { *res = make_string(""); return; }
    if (count == 1) {
        if (args[0].type == VAL_STRING) { *res = args[0]; return; }
        if (args[0].type == VAL_INT) {
            *res = make_string(std::to_string(args[0].i));
            return;
        }
        if (args[0].type == VAL_FLOAT) {
            *res = make_string(arduino_float_decimals(v_re(args[0]), 2));
            return;
        }
        *res = make_string(value_to_text(args[0]));
        return;
    }
    if (args[0].type == VAL_STRING)
        throw std::runtime_error("String: com dois argumentos use numero e base/casas");
    if (args[0].type == VAL_INT) {
        long long base = arduino_index_arg(args[1], "String");
        *res = make_string(arduino_int_base(args[0].i, (int)base));
        return;
    }
    require_real_value(args[0], "String");
    long long dec = arduino_index_arg(args[1], "String");
    *res = make_string(arduino_float_decimals(v_re(args[0]), dec));
}
REGISTER_BUILTIN("String", 0, builtin_string_ctor);
REGISTER_BUILTIN("String", 1, builtin_string_ctor);
REGISTER_BUILTIN("String", 2, builtin_string_ctor);

void builtin_charAt(Value* args, int, Value* res) {
    require_string_arg(args[0], "charAt");
    long long i = arduino_index_arg(args[1], "charAt");
    if (i < 0 || i >= (long long)args[0].s.size()) { *res = make_string(""); return; }
    *res = make_string(args[0].s.substr((size_t)i, 1));
}
REGISTER_BUILTIN("charAt", 2, builtin_charAt);

void builtin_compareTo(Value* args, int, Value* res) {
    require_string_arg(args[0], "compareTo");
    require_string_arg(args[1], "compareTo");
    *res = make_int((long long)args[0].s.str().compare(args[1].s.str()));
}
REGISTER_BUILTIN("compareTo", 2, builtin_compareTo);

void builtin_c_str(Value* args, int, Value* res) {
    require_string_arg(args[0], "c_str");
    *res = args[0];
}
REGISTER_BUILTIN("c_str", 1, builtin_c_str);

void builtin_endsWith(Value* args, int, Value* res) {
    require_string_arg(args[0], "endsWith");
    require_string_arg(args[1], "endsWith");
    const std::string& s = args[0].s.str();
    const std::string& suf = args[1].s.str();
    bool ok = s.size() >= suf.size() &&
              s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
    *res = make_float(ok ? 1.0 : 0.0);
}
REGISTER_BUILTIN("endsWith", 2, builtin_endsWith);

void builtin_equals(Value* args, int, Value* res) {
    require_string_arg(args[0], "equals");
    require_string_arg(args[1], "equals");
    *res = make_float(args[0].s == args[1].s ? 1.0 : 0.0);
}
REGISTER_BUILTIN("equals", 2, builtin_equals);

void builtin_equalsIgnoreCase(Value* args, int, Value* res) {
    require_string_arg(args[0], "equalsIgnoreCase");
    require_string_arg(args[1], "equalsIgnoreCase");
    const std::string& a = args[0].s.str();
    const std::string& b = args[1].s.str();
    bool ok = a.size() == b.size();
    for (size_t i = 0; ok && i < a.size(); i++)
        ok = std::tolower((unsigned char)a[i]) == std::tolower((unsigned char)b[i]);
    *res = make_float(ok ? 1.0 : 0.0);
}
REGISTER_BUILTIN("equalsIgnoreCase", 2, builtin_equalsIgnoreCase);

void builtin_getBytes(Value* args, int, Value* res) {
    require_string_arg(args[0], "getBytes");
    const std::string& s = args[0].s.str();
    Value out = v_matrix(1, (int)s.size());
    for (size_t i = 0; i < s.size(); i++)
        out.arr()->elements[i] = make_int((long long)(unsigned char)s[i]);
    *res = out;
}
REGISTER_BUILTIN("getBytes", 1, builtin_getBytes);

static long long fill_byte_list(const Value& s, const Value& buf,
                                long long limit, bool terminate, const char* fn) {
    if (buf.type != VAL_ARRAY || !buf.arr() || buf.arr()->rows != 1)
        throw std::runtime_error(std::string(fn) + ": o buffer deve ser uma lista 1D");
    const std::string& str = s.s.str();
    long long n = (long long)str.size();
    if (limit >= 0 && limit < n) n = limit;
    if (n > (long long)buf.arr()->elements.size())
        n = (long long)buf.arr()->elements.size();
    for (long long i = 0; i < n; i++)
        buf.arr()->elements[(size_t)i] = make_int((long long)(unsigned char)str[(size_t)i]);
    if (terminate && n < (long long)buf.arr()->elements.size()
        && (limit < 0 || n < limit))
        buf.arr()->elements[(size_t)n] = make_int(0);
    return n;
}

void builtin_getBytes_buf(Value* args, int count, Value* res) {
    require_string_arg(args[0], "getBytes");
    long long limit = (count >= 3) ? arduino_index_arg(args[2], "getBytes") : -1;
    if (count >= 3 && limit < 0) throw std::runtime_error("getBytes: tamanho deve ser >= 0");
    *res = make_int(fill_byte_list(args[0], args[1], limit, false, "getBytes"));
}
REGISTER_BUILTIN("getBytes", 2, builtin_getBytes_buf);
REGISTER_BUILTIN("getBytes", 3, builtin_getBytes_buf);

void builtin_indexOf(Value* args, int count, Value* res) {
    require_string_arg(args[0], "indexOf");
    require_string_arg(args[1], "indexOf");
    long long from = (count >= 3) ? arduino_index_arg(args[2], "indexOf") : 0;
    if (from < 0) from = 0;
    const std::string& s = args[0].s.str();
    size_t p = std::string::npos;
    if (from <= (long long)s.size())
        p = s.find(args[1].s.str(), (size_t)from);
    *res = make_float(p == std::string::npos ? -1.0 : (double)p);
}
REGISTER_BUILTIN("indexOf", 2, builtin_indexOf);
REGISTER_BUILTIN("indexOf", 3, builtin_indexOf);

void builtin_lastIndexOf(Value* args, int count, Value* res) {
    require_string_arg(args[0], "lastIndexOf");
    require_string_arg(args[1], "lastIndexOf");
    const std::string& s = args[0].s.str();
    size_t p = std::string::npos;
    if (count >= 3) {
        long long from = arduino_index_arg(args[2], "lastIndexOf");
        if (from >= 0) p = s.rfind(args[1].s.str(), (size_t)from);
    } else {
        p = s.rfind(args[1].s.str());
    }
    *res = make_float(p == std::string::npos ? -1.0 : (double)p);
}
REGISTER_BUILTIN("lastIndexOf", 2, builtin_lastIndexOf);
REGISTER_BUILTIN("lastIndexOf", 3, builtin_lastIndexOf);

void builtin_length(Value* args, int, Value* res) {
    require_string_arg(args[0], "length");
    *res = make_float((double)args[0].s.size());
}
REGISTER_BUILTIN("length", 1, builtin_length);

void builtin_replace(Value* args, int, Value* res) {
    require_string_arg(args[0], "replace");
    require_string_arg(args[1], "replace");
    require_string_arg(args[2], "replace");
    const std::string& s = args[0].s.str();
    const std::string& from = args[1].s.str();
    const std::string& to = args[2].s.str();
    if (from.empty()) { *res = args[0]; return; }
    std::string out;
    size_t pos = 0;
    while (true) {
        size_t hit = s.find(from, pos);
        if (hit == std::string::npos) { out.append(s, pos, std::string::npos); break; }
        out.append(s, pos, hit - pos);
        out += to;
        pos = hit + from.size();
    }
    *res = make_string(out);
}
REGISTER_BUILTIN("replace", 3, builtin_replace);

void builtin_reserve(Value* args, int, Value* res) {
    require_string_arg(args[0], "reserve");
    long long n = arduino_index_arg(args[1], "reserve");
    if (n < 0) throw std::runtime_error("reserve: tamanho deve ser >= 0");
    *res = args[0];
}
REGISTER_BUILTIN("reserve", 2, builtin_reserve);

void builtin_setCharAt(Value* args, int, Value* res) {
    require_string_arg(args[0], "setCharAt");
    long long i = arduino_index_arg(args[1], "setCharAt");
    char c = arduino_char_arg(args[2], "setCharAt");
    std::string s = args[0].s.str();
    if (i >= 0 && i < (long long)s.size()) s[(size_t)i] = c;
    *res = make_string(s);
}
REGISTER_BUILTIN("setCharAt", 3, builtin_setCharAt);

void builtin_startsWith(Value* args, int, Value* res) {
    require_string_arg(args[0], "startsWith");
    require_string_arg(args[1], "startsWith");
    const std::string& s = args[0].s.str();
    const std::string& pre = args[1].s.str();
    *res = make_float(s.compare(0, pre.size(), pre) == 0 ? 1.0 : 0.0);
}
REGISTER_BUILTIN("startsWith", 2, builtin_startsWith);

void builtin_substring(Value* args, int count, Value* res) {
    require_string_arg(args[0], "substring");
    const std::string& s = args[0].s.str();
    long long len = (long long)s.size();
    long long begin = arduino_index_arg(args[1], "substring");
    long long end = (count >= 3) ? arduino_index_arg(args[2], "substring") : len;
    if (begin > end) std::swap(begin, end);
    if (begin < 0) begin = 0;
    if (end < 0) end = 0;
    if (begin > len) begin = len;
    if (end > len) end = len;
    *res = make_string(s.substr((size_t)begin, (size_t)(end - begin)));
}
REGISTER_BUILTIN("substring", 2, builtin_substring);
REGISTER_BUILTIN("substring", 3, builtin_substring);

void builtin_toCharArray(Value* args, int count, Value* res) {
    builtin_getBytes(args, count, res);
}
REGISTER_BUILTIN("toCharArray", 1, builtin_toCharArray);

void builtin_toCharArray_buf(Value* args, int count, Value* res) {
    require_string_arg(args[0], "toCharArray");
    long long limit = (count >= 3) ? arduino_index_arg(args[2], "toCharArray") : -1;
    if (count >= 3 && limit < 0) throw std::runtime_error("toCharArray: tamanho deve ser >= 0");
    *res = make_int(fill_byte_list(args[0], args[1], limit, true, "toCharArray"));
}
REGISTER_BUILTIN("toCharArray", 2, builtin_toCharArray_buf);
REGISTER_BUILTIN("toCharArray", 3, builtin_toCharArray_buf);

void builtin_toDouble(Value* args, int, Value* res) {
    require_string_arg(args[0], "toDouble");
    const char* p = args[0].s.c_str();
    char* end = nullptr;
    errno = 0;
    double v = std::strtod(p, &end);
    if (end == p) v = 0.0;
    *res = make_float(v);
}
REGISTER_BUILTIN("toDouble", 1, builtin_toDouble);

void builtin_toFloat(Value* args, int count, Value* res) {
    builtin_toDouble(args, count, res);
}
REGISTER_BUILTIN("toFloat", 1, builtin_toFloat);

void builtin_toInt(Value* args, int, Value* res) {
    require_string_arg(args[0], "toInt");
    const char* p = args[0].s.c_str();
    char* end = nullptr;
    errno = 0;
    long long v = std::strtoll(p, &end, 10);
    if (end == p) v = 0;
    *res = make_int(v);
}
REGISTER_BUILTIN("toInt", 1, builtin_toInt);

void builtin_toLowerCase(Value* args, int count, Value* res) {
    builtin_lower(args, count, res);
}
REGISTER_BUILTIN("toLowerCase", 1, builtin_toLowerCase);

void builtin_toUpperCase(Value* args, int count, Value* res) {
    builtin_upper(args, count, res);
}
REGISTER_BUILTIN("toUpperCase", 1, builtin_toUpperCase);

// ---- math.h (C99, double; complex only where support already existed) ----
#define DEFINE_MATH_UNARY(fn, expr) \
    void builtin_math_##fn(Value* args, int, Value* res) { \
        require_real_value(args[0], #fn); \
        double x = v_re(args[0]); \
        *res = make_float(expr); \
    } \
    REGISTER_BUILTIN(#fn, 1, builtin_math_##fn)

#define DEFINE_MATH_BINARY(fn, expr) \
    void builtin_math_##fn(Value* args, int, Value* res) { \
        require_real_pair(args[0], args[1], #fn); \
        double a = v_re(args[0]), b = v_re(args[1]); \
        *res = make_float(expr); \
    } \
    REGISTER_BUILTIN(#fn, 2, builtin_math_##fn)

#define DEFINE_MATH_PRED(fn, expr) \
    void builtin_math_##fn(Value* args, int, Value* res) { \
        require_real_value(args[0], #fn); \
        double x = v_re(args[0]); \
        *res = make_float((expr) ? 1.0 : 0.0); \
    } \
    REGISTER_BUILTIN(#fn, 1, builtin_math_##fn)

#define DEFINE_MATH_PRED2(fn, expr) \
    void builtin_math_##fn(Value* args, int, Value* res) { \
        require_real_pair(args[0], args[1], #fn); \
        double a = v_re(args[0]), b = v_re(args[1]); \
        *res = make_float((expr) ? 1.0 : 0.0); \
    } \
    REGISTER_BUILTIN(#fn, 2, builtin_math_##fn)

DEFINE_MATH_UNARY(acos, std::acos(x));
DEFINE_MATH_UNARY(asin, std::asin(x));
DEFINE_MATH_UNARY(atan, std::atan(x));
DEFINE_MATH_UNARY(cosh, std::cosh(x));
DEFINE_MATH_UNARY(sinh, std::sinh(x));
DEFINE_MATH_UNARY(tanh, std::tanh(x));
DEFINE_MATH_UNARY(acosh, std::acosh(x));
DEFINE_MATH_UNARY(asinh, std::asinh(x));
DEFINE_MATH_UNARY(atanh, std::atanh(x));
DEFINE_MATH_UNARY(exp2, std::exp2(x));
DEFINE_MATH_UNARY(expm1, std::expm1(x));
DEFINE_MATH_UNARY(log2, std::log2(x));
DEFINE_MATH_UNARY(log1p, std::log1p(x));
DEFINE_MATH_UNARY(cbrt, std::cbrt(x));
DEFINE_MATH_UNARY(erf, std::erf(x));
DEFINE_MATH_UNARY(erfc, std::erfc(x));
DEFINE_MATH_UNARY(tgamma, std::tgamma(x));
DEFINE_MATH_UNARY(lgamma, std::lgamma(x));
DEFINE_MATH_UNARY(trunc, std::trunc(x));
DEFINE_MATH_UNARY(rint, std::rint(x));
DEFINE_MATH_UNARY(nearbyint, std::nearbyint(x));
DEFINE_MATH_UNARY(fabs, std::fabs(x));

DEFINE_MATH_BINARY(hypot, std::hypot(a, b));
DEFINE_MATH_BINARY(fdim, std::fdim(a, b));
DEFINE_MATH_BINARY(fmax, std::fmax(a, b));
DEFINE_MATH_BINARY(fmin, std::fmin(a, b));
DEFINE_MATH_BINARY(copysign, std::copysign(a, b));
DEFINE_MATH_BINARY(nextafter, std::nextafter(a, b));
DEFINE_MATH_BINARY(remainder, std::remainder(a, b));

DEFINE_MATH_PRED(isfinite, std::isfinite(x));
DEFINE_MATH_PRED(isinf, std::isinf(x));
DEFINE_MATH_PRED(isnan, std::isnan(x));
DEFINE_MATH_PRED(isnormal, std::isnormal(x));
DEFINE_MATH_PRED(signbit, std::signbit(x));

DEFINE_MATH_PRED2(isgreater, std::isgreater(a, b));
DEFINE_MATH_PRED2(isgreaterequal, std::isgreaterequal(a, b));
DEFINE_MATH_PRED2(isless, std::isless(a, b));
DEFINE_MATH_PRED2(islessequal, std::islessequal(a, b));
DEFINE_MATH_PRED2(islessgreater, std::islessgreater(a, b));
DEFINE_MATH_PRED2(isunordered, std::isunordered(a, b));

void builtin_lround(Value* args, int, Value* res) {
    require_real_value(args[0], "lround");
    *res = make_int((long long)std::lround(v_re(args[0])));
}
REGISTER_BUILTIN("lround", 1, builtin_lround);

void builtin_llround(Value* args, int, Value* res) {
    require_real_value(args[0], "llround");
    *res = make_int((long long)std::llround(v_re(args[0])));
}
REGISTER_BUILTIN("llround", 1, builtin_llround);

void builtin_lrint(Value* args, int, Value* res) {
    require_real_value(args[0], "lrint");
    *res = make_int((long long)std::lrint(v_re(args[0])));
}
REGISTER_BUILTIN("lrint", 1, builtin_lrint);

void builtin_llrint(Value* args, int, Value* res) {
    require_real_value(args[0], "llrint");
    *res = make_int((long long)std::llrint(v_re(args[0])));
}
REGISTER_BUILTIN("llrint", 1, builtin_llrint);

void builtin_ilogb(Value* args, int, Value* res) {
    require_real_value(args[0], "ilogb");
    *res = make_int((long long)std::ilogb(v_re(args[0])));
}
REGISTER_BUILTIN("ilogb", 1, builtin_ilogb);

void builtin_logb(Value* args, int, Value* res) {
    require_real_value(args[0], "logb");
    *res = make_float(std::logb(v_re(args[0])));
}
REGISTER_BUILTIN("logb", 1, builtin_logb);

void builtin_ldexp(Value* args, int, Value* res) {
    require_real_value(args[0], "ldexp");
    long long e = arduino_index_arg(args[1], "ldexp");
    *res = make_float(std::ldexp(v_re(args[0]), (int)e));
}
REGISTER_BUILTIN("ldexp", 2, builtin_ldexp);

void builtin_scalbn(Value* args, int, Value* res) {
    require_real_value(args[0], "scalbn");
    long long e = arduino_index_arg(args[1], "scalbn");
    *res = make_float(std::scalbn(v_re(args[0]), (int)e));
}
REGISTER_BUILTIN("scalbn", 2, builtin_scalbn);

void builtin_scalbln(Value* args, int, Value* res) {
    require_real_value(args[0], "scalbln");
    long long e = arduino_index_arg(args[1], "scalbln");
    *res = make_float(std::scalbln(v_re(args[0]), (long)e));
}
REGISTER_BUILTIN("scalbln", 2, builtin_scalbln);

void builtin_fma(Value* args, int, Value* res) {
    require_real_value(args[0], "fma");
    require_real_value(args[1], "fma");
    require_real_value(args[2], "fma");
    *res = make_float(std::fma(v_re(args[0]), v_re(args[1]), v_re(args[2])));
}
REGISTER_BUILTIN("fma", 3, builtin_fma);

void builtin_frexp(Value* args, int, Value* res) {
    require_real_value(args[0], "frexp");
    int e = 0;
    double f = std::frexp(v_re(args[0]), &e);
    auto m = std::make_shared<MultiRet>();
    gc_register_obj(GC_OBJ_MULTI, m);
    m->values.push_back(make_float(f));
    m->values.push_back(make_int((long long)e));
    res->type = VAL_MULTI;
    res->obj = std::move(m);
}
REGISTER_BUILTIN("frexp", 1, builtin_frexp);

void builtin_modf(Value* args, int, Value* res) {
    require_real_value(args[0], "modf");
    double ip = 0.0;
    double fp = std::modf(v_re(args[0]), &ip);
    auto m = std::make_shared<MultiRet>();
    gc_register_obj(GC_OBJ_MULTI, m);
    m->values.push_back(make_float(fp));
    m->values.push_back(make_float(ip));
    res->type = VAL_MULTI;
    res->obj = std::move(m);
}
REGISTER_BUILTIN("modf", 1, builtin_modf);

void builtin_remquo(Value* args, int, Value* res) {
    require_real_pair(args[0], args[1], "remquo");
    int q = 0;
    double r = std::remquo(v_re(args[0]), v_re(args[1]), &q);
    auto m = std::make_shared<MultiRet>();
    gc_register_obj(GC_OBJ_MULTI, m);
    m->values.push_back(make_float(r));
    m->values.push_back(make_int((long long)q));
    res->type = VAL_MULTI;
    res->obj = std::move(m);
}
REGISTER_BUILTIN("remquo", 2, builtin_remquo);

void builtin_nan(Value* args, int, Value* res) {
    (void)args;
    *res = make_float(std::numeric_limits<double>::quiet_NaN());
}
REGISTER_BUILTIN("nan", -1, builtin_nan);

void builtin_fpclassify(Value* args, int, Value* res) {
    require_real_value(args[0], "fpclassify");
    double x = v_re(args[0]);
    int cls = std::isnan(x) ? 0 : std::isinf(x) ? 1 : (x == 0.0) ? 2
              : (std::fpclassify(x) == FP_SUBNORMAL) ? 3 : 4;
    *res = make_int((long long)cls);
}
REGISTER_BUILTIN("fpclassify", 1, builtin_fpclassify);

// os_date([fmt]) — formatted local time (strftime); ISO default.
void builtin_os_date(Value* args, int count, Value* res) {
    std::string fmt = (count >= 1 && args[0].type == VAL_STRING)
                          ? args[0].s.str() : "%Y-%m-%d %H:%M:%S";
    std::time_t t = std::time(nullptr);
    std::tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char buf[256];
    size_t n = std::strftime(buf, sizeof(buf), fmt.c_str(), &tmv);
    *res = make_string(std::string(buf, n));
}
REGISTER_BUILTIN("os_date", -1, builtin_os_date);

void builtin_os_time(Value* args, int count, Value* res) {
    builtin_time(args, count, res);
}
REGISTER_BUILTIN("os_time", 0, builtin_os_time);
void builtin_os_clock(Value* args, int count, Value* res) {
    builtin_clock(args, count, res);
}
REGISTER_BUILTIN("os_clock", 0, builtin_os_clock);

// utf8_len(s) — number of UTF-8 code points (continuation bytes do not count)
void builtin_utf8_len(Value* args, int, Value* res) {
    require_string_arg(args[0], "utf8_len");
    const std::string& s = args[0].s;
    int n = 0;
    for (unsigned char c : s)
        if ((c & 0xC0) != 0x80) n++;
    *res = make_float(n);
}
REGISTER_BUILTIN("utf8_len", 1, builtin_utf8_len);

static int utf8_decode_at(const std::string& s, size_t i) {
    if (i >= s.size()) return -1;
    unsigned char c = (unsigned char)s[i];
    if (c < 0x80) return c;
    int n, cp;
    if ((c & 0xE0) == 0xC0) { n = 1; cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { n = 2; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { n = 3; cp = c & 0x07; }
    else return -1;
    if (i + (size_t)n >= s.size()) return -1;
    for (int k = 1; k <= n; k++) {
        unsigned char cc = (unsigned char)s[i + (size_t)k];
        if ((cc & 0xC0) != 0x80) return -1;
        cp = (cp << 6) | (cc & 0x3F);
    }
    return cp;
}

// utf8_codepoint(s[, i]) — code point starting at byte i (0-based); -1 if invalid
void builtin_utf8_codepoint(Value* args, int count, Value* res) {
    require_string_arg(args[0], "utf8_codepoint");
    int i = (count >= 2) ? (int)args[1].f : 0;
    *res = make_float(utf8_decode_at(args[0].s.str(), (size_t)(i < 0 ? 0 : i)));
}
REGISTER_BUILTIN("utf8_codepoint", -1, builtin_utf8_codepoint);

// utf8_char(cp) — encodes a code point (0..0x10FFFF, no surrogates)
void builtin_utf8_char(Value* args, int, Value* res) {
    long cp = (long)args[0].f;
    if (cp < 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
        throw std::runtime_error("utf8_char: code point invalido");
    std::string out;
    if (cp < 0x80) {
        out += (char)cp;
    } else if (cp < 0x800) {
        out += (char)(0xC0 | (cp >> 6));
        out += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    } else {
        out += (char)(0xF0 | (cp >> 18));
        out += (char)(0x80 | ((cp >> 12) & 0x3F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
    *res = make_string(out);
}
REGISTER_BUILTIN("utf8_char", 1, builtin_utf8_char);

// ---- L14: load/dofile (code at runtime) --------------------------------
// Compiles `source` as a new chunk, appends it to the stable arena and returns
// the global entry (base). The final HALT becomes RETURN so the chunk can be
// called (load) or executed in isolation (dofile) without terminating the VM.
static int compile_runtime_chunk(const std::string& source, const char* what,
                                 std::string* err) {
    std::vector<Token> toks;
    try {
        Lexer lexer(source);
        toks = lexer.scan_tokens();
    } catch (const std::exception& e) {
        if (err) *err = e.what();
        return -1;
    }
    Compiler compiler;
    int base = code_arena_size();
    compiler.init(toks, base);
    compiler.set_draw_loop(false);
    compiler.set_setup_first(false);
    compiler.quiet_errors = true;
    if (!compiler.run()) {
        if (err) *err = compiler.last_error.empty()
                            ? std::string("erro de compilacao")
                            : compiler.last_error;
        return -1;
    }
    std::vector<Instruction> chunk = compiler.get_bytecode();
    // Final HALT -> `PUSH 0; RETURN`: a chunk without an explicit `return`
    // returns 0 (instead of leaving the function value on top of the stack).
    // If there is an explicit `return`, this section is unreachable.
    if (!chunk.empty() && chunk.back().op == OP_HALT) {
        Instruction ret;
        ret.op = OP_RETURN;
        ret.line = chunk.back().line;
        chunk.back().op = OP_PUSH;
        chunk.back().a = 0;
        chunk.back().b = 0;
        chunk.back().n = 0.0;
        chunk.push_back(ret);
    }
    int appended = code_arena_append(chunk);
    if (appended != base) {
        if (err) *err = "arena de codigo inconsistente";
        return -1;
    }
    (void)what;
    return base;
}

// Executes a chunk on the current VM, with isolated call/frame stacks
// (same pattern as pcall). Returns the value left by `return` (or 0).
static Value run_isolated_chunk(int entry, const char* what) {
    VM* vm = g_current_vm;
    if (!vm) throw std::runtime_error(std::string(what) + ": sem VM ativa");
    auto extCall = std::move(vm->callStack);    vm->callStack = {};
    auto extResults = std::move(vm->callResults); vm->callResults = {};
    auto extMark = std::move(vm->frameMarkers); vm->frameMarkers = {};
    auto extSaves = std::move(vm->scopeSaves);  vm->scopeSaves = {};
    auto extFrames = std::move(vm->frames);     vm->frames = {};
    auto extEnvs = std::move(vm->frameEnvs);    vm->frameEnvs = {};
    auto extEnvParents = std::move(vm->frameEnvParents); vm->frameEnvParents = {};
    bool extRunning = vm->running;
    int extIp = vm->ip;
    int sp0 = vm->sp;
    vm->running = true;
    vm->callArgc = 0;
    vm->frames.push_back({what, 0});
    vm_push_env(vm, nullptr); // L12
    Value ret = make_float(0);
    try {
        vm_run(vm, &code_arena(), entry);
        int nret = vm->topRet;
        if (nret <= 0 || vm->sp <= sp0) ret = make_float(0);
        else {
            ret = vm->stack[vm->sp - nret]; // L5: 1st value
            vm->sp = sp0;
        }
    } catch (...) {
        vm->callStack = std::move(extCall);
        vm->callResults = std::move(extResults);
        vm->frameMarkers = std::move(extMark);
        vm->scopeSaves = std::move(extSaves);
        vm->frames = std::move(extFrames);
        vm->frameEnvs = std::move(extEnvs);
        vm->frameEnvParents = std::move(extEnvParents);
        vm->running = extRunning;
        vm->ip = extIp;
        vm->sp = sp0;
        throw;
    }
    vm->callStack = std::move(extCall);
    vm->callResults = std::move(extResults);
    vm->frameMarkers = std::move(extMark);
    vm->scopeSaves = std::move(extSaves);
    vm->frames = std::move(extFrames);
    vm->frameEnvs = std::move(extEnvs);
    vm->frameEnvParents = std::move(extEnvParents);
    vm->running = extRunning;
    vm->ip = extIp;
    vm->sp = sp0;
    return ret;
}

// load(code[, name]) -> function; syntax error throws (no nil yet, L6).
void builtin_load(Value* args, int count, Value* res) {
    if (count < 1 || args[0].type != VAL_STRING)
        throw std::runtime_error("load: espera o codigo como texto");
    if (!g_allow_modules)
        throw std::runtime_error(
            "codigo dinamico desabilitado (allow_modules=false): load");
    if (!g_allow_fileio)
        throw std::runtime_error(
            "codigo dinamico desabilitado (allow_fileio=false): load");
    std::string name = (count >= 2 && args[1].type == VAL_STRING)
                           ? args[1].s.str() : std::string("=(load)");
    std::string err;
    int entry = compile_runtime_chunk(args[0].s.str(), "load", &err);
    if (entry < 0)
        throw std::runtime_error("load: " + err);
    Value fv;
    fv.type = VAL_FUNC;
    fv.obj = std::make_shared<FuncRef>();
    gc_register_obj(GC_OBJ_FUNC, fv.obj.p);
    fv.fn()->entry = entry;
    fv.fn()->name = name;
    *res = fv;
}
REGISTER_BUILTIN("load", -1, builtin_load);

// dofile(path) -> chunk's `return` value (or 0).
void builtin_dofile(Value* args, int count, Value* res) {
    if (count < 1 || args[0].type != VAL_STRING)
        throw std::runtime_error("dofile: espera o caminho do arquivo");
    if (!g_allow_fileio)
        throw std::runtime_error(
            "fileio desabilitado (allow_fileio=false): dofile");
    if (!g_allow_modules)
        throw std::runtime_error(
            "codigo dinamico desabilitado (allow_modules=false): dofile");
    std::ifstream f(args[0].s.str());
    if (!f.is_open())
        throw std::runtime_error("dofile: nao foi possivel abrir " +
                                 args[0].s.str());
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string err;
    int entry = compile_runtime_chunk(ss.str(), "dofile", &err);
    if (entry < 0)
        throw std::runtime_error("dofile: " + err);
    *res = run_isolated_chunk(entry, "dofile");
}
REGISTER_BUILTIN("dofile", 1, builtin_dofile);

// --- VM EXECUTION LOOP ---
// E15: arithmetic bodies extracted for reuse by the _K variants.
// E16: numeric fast path (avoids the type-test chain)
static Value do_add(const Value& a, const Value& b) {
    if (a.type == VAL_FLOAT && b.type == VAL_FLOAT)
        return Value{VAL_FLOAT, a.f + b.f};
    // L7: two integers remain integers (64-bit wrap-around).
    if (a.type == VAL_INT && b.type == VAL_INT)
        return make_int(i64_wrap_add(a.i, b.i));
    if (a.type == VAL_NIL || b.type == VAL_NIL)
        throw std::runtime_error("Operacao aritmetica nao suportada para nil");

if (a.type == VAL_STRUCT || b.type == VAL_STRUCT)
                    throw std::runtime_error("Operacao aritmetica nao suportada para structs");
                if (a.type == VAL_ARRAY && b.type == VAL_ARRAY) {
                    if (a.arr()->rows != b.arr()->rows || a.arr()->cols != b.arr()->cols)
                        throw std::runtime_error("Soma requer matrizes de mesmas dimensoes");
                    
                    Value res = v_matrix(a.arr()->rows, a.arr()->cols);
                    for (size_t i = 0; i < a.arr()->elements.size(); i++) 
                        res.arr()->elements[i] = num_add(a.arr()->elements[i], b.arr()->elements[i]);
                    return res;
                }
                // If both are strings, concatenate
                else if (a.type == VAL_STRING && b.type == VAL_STRING) {
                    Value v;
                    v.type = VAL_STRING;
                    v.s = a.s + b.s; // Safe concatenation
                    return v;
                }
                // Matrix + scalar (real or complex), numpy style
                else if (a.type == VAL_ARRAY && is_numeric(b)) {
                    Value res = v_matrix(a.arr()->rows, a.arr()->cols);
                    for (size_t i = 0; i < a.arr()->elements.size(); i++)
                        res.arr()->elements[i] = num_add(a.arr()->elements[i], b);
                    return res;
                }
                else if (is_numeric(a) && b.type == VAL_ARRAY) {
                    Value res = v_matrix(b.arr()->rows, b.arr()->cols);
                    for (size_t i = 0; i < b.arr()->elements.size(); i++)
                        res.arr()->elements[i] = num_add(a, b.arr()->elements[i]);
                    return res;
                }
                else if (is_complex(a) || is_complex(b)) {
                    if (a.type == VAL_STRING || b.type == VAL_STRING)
                        throw std::runtime_error("Soma entre complexo e texto nao suportada");
                    return num_add(a, b);
                }
                else if (a.type == VAL_STRING || b.type == VAL_STRING) {
                    throw std::runtime_error("Soma entre texto e numero nao suportada");
                }
                else return (Value{VAL_FLOAT, a.f + b.f});
}

static Value do_sub(const Value& a, const Value& b) {
    // E16: numeric fast path (avoids the type-test chain)
    if (a.type == VAL_FLOAT && b.type == VAL_FLOAT)
        return Value{VAL_FLOAT, a.f - b.f};
    if (a.type == VAL_INT && b.type == VAL_INT)
        return make_int(i64_wrap_sub(a.i, b.i));
    if (a.type == VAL_NIL || b.type == VAL_NIL)
        throw std::runtime_error("Operacao aritmetica nao suportada para nil");

if (a.type == VAL_STRUCT || b.type == VAL_STRUCT)
                    throw std::runtime_error("Operacao aritmetica nao suportada para structs");
                if (a.type == VAL_ARRAY && b.type == VAL_ARRAY) {
                    if (a.arr()->rows != b.arr()->rows || a.arr()->cols != b.arr()->cols)
                        throw std::runtime_error("Subtracao requer matrizes de mesmas dimensoes");
                    Value res = v_matrix(a.arr()->rows, a.arr()->cols);
                    for (size_t i = 0; i < a.arr()->elements.size(); i++)
                        res.arr()->elements[i] = num_sub(a.arr()->elements[i], b.arr()->elements[i]);
                    return res;
                }
                else if (a.type == VAL_ARRAY && is_numeric(b)) {
                    Value res = v_matrix(a.arr()->rows, a.arr()->cols);
                    for (size_t i = 0; i < a.arr()->elements.size(); i++)
                        res.arr()->elements[i] = num_sub(a.arr()->elements[i], b);
                    return res;
                }
                else if (is_numeric(a) && b.type == VAL_ARRAY) {
                    Value res = v_matrix(b.arr()->rows, b.arr()->cols);
                    for (size_t i = 0; i < b.arr()->elements.size(); i++)
                        res.arr()->elements[i] = num_sub(a, b.arr()->elements[i]);
                    return res;
                }
                else if (is_complex(a) || is_complex(b)) {
                    if (a.type == VAL_STRING || b.type == VAL_STRING)
                        throw std::runtime_error("Subtracao entre complexo e texto nao suportada");
                    return num_sub(a, b);
                }
                else if (a.type == VAL_STRING || b.type == VAL_STRING) {
                    throw std::runtime_error("Subtracao entre texto e numero nao suportada");
                }
                else return (Value{VAL_FLOAT, a.f - b.f});
}

static Value do_mul(const Value& a, const Value& b) {
    // E16: numeric fast path (avoids the type-test chain)
    if (a.type == VAL_FLOAT && b.type == VAL_FLOAT)
        return Value{VAL_FLOAT, a.f * b.f};
    if (a.type == VAL_INT && b.type == VAL_INT)
        return make_int(i64_wrap_mul(a.i, b.i));
    if (a.type == VAL_NIL || b.type == VAL_NIL)
        throw std::runtime_error("Operacao aritmetica nao suportada para nil");

if (a.type == VAL_STRUCT || b.type == VAL_STRUCT)
                    throw std::runtime_error("Operacao aritmetica nao suportada para structs");
                
                // Matrix * Matrix (matrix product, real and complex)
                if (a.type == VAL_ARRAY && b.type == VAL_ARRAY) {
                    if (a.arr()->cols != b.arr()->rows) 
                        throw std::runtime_error("Erro: Colunas de A (" + std::to_string(a.arr()->cols) + 
                                                ") != Linhas de B (" + std::to_string(b.arr()->rows) + ")");
                    
                    Value res = v_matrix(a.arr()->rows, b.arr()->cols);
                    for (int i = 0; i < a.arr()->rows; i++) {
                        for (int j = 0; j < b.arr()->cols; j++) {
                            Value sum = make_float(0.0f);
                            for (int k = 0; k < a.arr()->cols; k++) {
                                sum = num_add(sum, num_mul(a.arr()->elements[i * a.arr()->cols + k],
                                                          b.arr()->elements[k * b.arr()->cols + j]));
                            }
                            res.arr()->elements[i * res.arr()->cols + j] = sum;
                        }
                    }
                    return res;
                } 
                // Matrix * Scalar (real or complex)
                else if (a.type == VAL_ARRAY && is_numeric(b)) {
                    Value res = v_matrix(a.arr()->rows, a.arr()->cols);
                    for (size_t i = 0; i < a.arr()->elements.size(); i++) 
                        res.arr()->elements[i] = num_mul(a.arr()->elements[i], b);
                    return res;
                }
                // Scalar * Matrix (real or complex)
                else if (is_numeric(a) && b.type == VAL_ARRAY) {
                    Value res = v_matrix(b.arr()->rows, b.arr()->cols);
                    for (size_t i = 0; i < b.arr()->elements.size(); i++) 
                        res.arr()->elements[i] = num_mul(a, b.arr()->elements[i]);
                    return res;
                }
                // Scalar * Scalar (real or complex)
                else if (is_complex(a) || is_complex(b)) {
                    if (a.type == VAL_STRING || b.type == VAL_STRING)
                        throw std::runtime_error("Multiplicacao entre complexo e texto nao suportada");
                    return num_mul(a, b);
                }
                else if (a.type == VAL_STRING || b.type == VAL_STRING) {
                    throw std::runtime_error("Multiplicacao entre texto e numero nao suportada");
                }
                else return (Value{VAL_FLOAT, a.f * b.f});
}

static Value do_div(const Value& a, const Value& b) {
    // E16: numeric fast path
    if (a.type == VAL_FLOAT && b.type == VAL_FLOAT) {
        if (std::abs(b.f) < 1e-10f) throw std::runtime_error("Divisão por zero");
        return Value{VAL_FLOAT, a.f / b.f};
    }
    if (a.type == VAL_NIL || b.type == VAL_NIL)
        throw std::runtime_error("Operacao aritmetica nao suportada para nil");

if (a.type == VAL_STRUCT || b.type == VAL_STRUCT)
                    throw std::runtime_error("Operacao aritmetica nao suportada para structs");
                if (a.type == VAL_STRING || b.type == VAL_STRING)
                    throw std::runtime_error("Divisao entre texto e numero nao suportada");
                return num_div(a, b);
}

static Value do_mod(const Value& a, const Value& b) {
    if (a.type == VAL_NIL || b.type == VAL_NIL)
        throw std::runtime_error("Operacao aritmetica nao suportada para nil");
    // L7: modulo of two integers returns an integer (LLONG_MIN % -1 = 0).
    if (a.type == VAL_INT && b.type == VAL_INT) {
        if (b.i == 0) throw std::runtime_error("Módulo por zero");
        return make_int(b.i == -1 ? 0LL : a.i % b.i);
    }
    // E16: integer fast path
    if (a.type == VAL_FLOAT && b.type == VAL_FLOAT) {
        // same validation as req_int64 (identical messages)
        const double LIMIT = 9223372036854775808.0;
        if (!(a.f >= -LIMIT && a.f < LIMIT))
            throw std::runtime_error("Operando fora da faixa inteira para '%'");
        if (a.f != std::floor(a.f))
            throw std::runtime_error("Operando nao inteiro para '%'");
        if (!(b.f >= -LIMIT && b.f < LIMIT))
            throw std::runtime_error("Operando fora da faixa inteira para '%'");
        if (b.f != std::floor(b.f))
            throw std::runtime_error("Operando nao inteiro para '%'");
        long long ia = (long long)a.f, ib = (long long)b.f;
        if (ib == 0) throw std::runtime_error("Módulo por zero");
        return make_float((double)(ib == -1 ? 0LL : ia % ib));
    }

require_real_pair(a, b, "%");
                long long ia = req_int64(a, "%"), ib = req_int64(b, "%");
                if (ib == 0) throw std::runtime_error("Módulo por zero");
                return make_float((double)(ib == -1 ? 0LL : ia % ib));
}

// E16d: variants with a double scalar (avoids building a temporary Value).
// L7: k_int=1 marks an immediate integer constant (from OP_PUSH b=2).
static inline Value do_add_num(const Value& a, double k, bool k_int = false) {
    if (a.type == VAL_FLOAT) return Value(VAL_FLOAT, a.f + k);
    if (a.type == VAL_INT && k_int) {
        long long iv;
        if (i64_exact_from_double(k, iv)) return make_int(i64_wrap_add(a.i, iv));
    }
    return do_add(a, k_value(k, k_int));
}
static inline Value do_sub_num(const Value& a, double k, bool k_int = false) {
    if (a.type == VAL_FLOAT) return Value(VAL_FLOAT, a.f - k);
    if (a.type == VAL_INT && k_int) {
        long long iv;
        if (i64_exact_from_double(k, iv)) return make_int(i64_wrap_sub(a.i, iv));
    }
    return do_sub(a, k_value(k, k_int));
}
static inline Value do_mul_num(const Value& a, double k, bool k_int = false) {
    if (a.type == VAL_FLOAT) return Value(VAL_FLOAT, a.f * k);
    if (a.type == VAL_INT && k_int) {
        long long iv;
        if (i64_exact_from_double(k, iv)) return make_int(i64_wrap_mul(a.i, iv));
    }
    return do_mul(a, k_value(k, k_int));
}
static inline Value do_div_num(const Value& a, double k, bool k_int = false) {
    if (a.type == VAL_FLOAT) {
        if (std::abs(k) < 1e-10f) throw std::runtime_error("Divisão por zero");
        return Value(VAL_FLOAT, a.f / k);
    }
    return do_div(a, k_value(k, k_int));
}
static inline Value do_mod_num(const Value& a, double k, bool k_int = false) {
    if (a.type == VAL_FLOAT) {
        const double LIMIT = 9223372036854775808.0;
        if (!(a.f >= -LIMIT && a.f < LIMIT))
            throw std::runtime_error("Operando fora da faixa inteira para '%'");
        if (a.f != std::floor(a.f))
            throw std::runtime_error("Operando nao inteiro para '%'");
        if (!(k >= -LIMIT && k < LIMIT))
            throw std::runtime_error("Operando fora da faixa inteira para '%'");
        if (k != std::floor(k))
            throw std::runtime_error("Operando nao inteiro para '%'");
        long long ia = (long long)a.f, ib = (long long)k;
        if (ib == 0) throw std::runtime_error("Módulo por zero");
        return make_float((double)(ib == -1 ? 0LL : ia % ib));
    }
    return do_mod(a, k_value(k, k_int));
}
static inline Value do_eq_num(const Value& a, double k, bool k_int = false) {
    if (a.type == VAL_FLOAT) return Value(VAL_FLOAT, a.f == k ? 1.0f : 0.0f);
    return do_eq(a, k_value(k, k_int));
}
static inline Value do_ne_num(const Value& a, double k, bool k_int = false) {
    if (a.type == VAL_FLOAT)
        return Value(VAL_FLOAT, std::abs(a.f - k) >= 1e-6f ? 1.0f : 0.0f);
    return do_ne(a, k_value(k, k_int));
}
static inline Value do_lt_num(const Value& a, double k, bool k_int = false) {
    if (a.type == VAL_FLOAT) return Value(VAL_FLOAT, a.f < k ? 1.0f : 0.0f);
    return do_lt(a, k_value(k, k_int));
}
static inline Value do_le_num(const Value& a, double k, bool k_int = false) {
    if (a.type == VAL_FLOAT) return Value(VAL_FLOAT, a.f <= k ? 1.0f : 0.0f);
    return do_le(a, k_value(k, k_int));
}
static inline Value do_gt_num(const Value& a, double k, bool k_int = false) {
    if (a.type == VAL_FLOAT) return Value(VAL_FLOAT, a.f > k ? 1.0f : 0.0f);
    return do_gt(a, k_value(k, k_int));
}
static inline Value do_ge_num(const Value& a, double k, bool k_int = false) {
    if (a.type == VAL_FLOAT) return Value(VAL_FLOAT, a.f >= k ? 1.0f : 0.0f);
    return do_ge(a, k_value(k, k_int));
}

// E16g: builtin argument buffer without building 16 Values per
// call (896 B + 16 destructors). Builds only the `count` used.
struct ArgBuf {
    alignas(Value) unsigned char raw[16 * sizeof(Value)];
    int n = 0;
    Value* p() { return reinterpret_cast<Value*>(raw); }
    Value& at(int i, Value&& v) {
        new (p() + i) Value(std::move(v));
        if (i >= n) n = i + 1;
        return p()[i];
    }
    ~ArgBuf() { for (int i = 0; i < n; i++) p()[i].~Value(); }
};

void vm_run(VM* vm, CodeStore* code, int start_ip) {
    struct CurrentVmGuard {
        VM* prev;
        explicit CurrentVmGuard(VM* v) : prev(g_current_vm) { g_current_vm = v; }
        ~CurrentVmGuard() { g_current_vm = prev; }
    } currentVm(vm);
    // E5: callStack/frameMarkers/scopeSaves/ip/running live in the VM so that
    // coyield can suspend and coresume resume without losing state.
    // start_ip >= 0 starts from zero; -1 continues from vm->ip (resumption).
    if (start_ip >= 0) vm->ip = start_ip;
    if (start_ip >= 0) vm->topRet = 1;
    // L5: avoids reallocations of the count stack in common shallow calls
    if (vm->callResults.capacity() < 64) vm->callResults.reserve(64);
    vm->running = true;
    g_current_code = code;
    std::vector<int>& callStack = vm->callStack;
    std::vector<int>& callResults = vm->callResults;
    std::vector<size_t>& frameMarkers = vm->frameMarkers;
    std::vector<SavedLocal>& scopeSaves = vm->scopeSaves;
    int& ip = vm->ip;
    bool& running = vm->running;
    // L14: cache of the vector start. Renewed only when the arena grows
    // (load/dofile), keeping the hot fetch identical to before.
    Instruction* code_data = code->data();
    int code_gen = code_arena_gen();
    // Renews the cache only when the arena has grown (load/dofile). Inline (no
    // lambda) so as not to force code_data/code_gen onto the stack in the hot path.
    #define REFRESH_CODE() \
        do { if (code_gen != g_code_arena_gen) { \
                 code_gen = g_code_arena_gen; code_data = code->data(); } \
        } while (0)

    // Thread checkpoint (item 15): on the main thread g_thread_ctl is
    // nullptr and the branches below are never taken (cost ~zero)
    ThreadCtl* tctl = g_thread_ctl;

#ifdef ISILIEL_CGOTO
    // OpCode -> label table, in the exact order of the enum in defs.h.
    void* vm_dispatch_table[] = {
        &&L_OP_HALT, &&L_OP_PUSH, &&L_OP_PUSH_STR, &&L_OP_POP,
        &&L_OP_ADD, &&L_OP_SUB, &&L_OP_MUL, &&L_OP_DIV, &&L_OP_MOD,
        &&L_OP_EQ, &&L_OP_NE, &&L_OP_LT, &&L_OP_GT, &&L_OP_LE, &&L_OP_GE,
        &&L_OP_AND, &&L_OP_OR, &&L_OP_BIT_AND, &&L_OP_BIT_OR, &&L_OP_BIT_XOR,
        &&L_OP_BIT_NOT, &&L_OP_SHL, &&L_OP_SHR,
        &&L_OP_LOAD, &&L_OP_STORE, &&L_OP_JUMP, &&L_OP_JUMP_IF_FALSE,
        &&L_OP_CALL, &&L_OP_RETURN, &&L_OP_CALL_BUILTIN,
        &&L_OP_BUILD_ARRAY, &&L_OP_INDEX_LOAD, &&L_OP_INDEX_STORE,
        &&L_OP_INDEX_SLICE, &&L_OP_BUILD_STRUCT, &&L_OP_FIELD_LOAD,
        &&L_OP_FIELD_STORE, &&L_OP_CALL_METHOD, &&L_OP_PUSH_FUNC,
        &&L_OP_CALL_INDIRECT, &&L_OP_CALL_PCALL, &&L_OP_BUILD_TABLE,
        &&L_OP_NEW_INSTANCE, &&L_OP_DUP, &&L_OP_CALL_SUPER,
        &&L_OP_ADD_K, &&L_OP_SUB_K, &&L_OP_MUL_K, &&L_OP_DIV_K, &&L_OP_MOD_K,
        &&L_OP_EQ_K, &&L_OP_NE_K, &&L_OP_LT_K, &&L_OP_LE_K, &&L_OP_GT_K,
        &&L_OP_GE_K, &&L_OP_INC_LOCAL,
        &&L_OP_LT_JIF_K, &&L_OP_LE_JIF_K, &&L_OP_GT_JIF_K, &&L_OP_GE_JIF_K,
        &&L_OP_EQ_JIF_K, &&L_OP_NE_JIF_K,
        &&L_OP_ADD_STORE, &&L_OP_SUB_STORE,
        &&L_OP_ADD_STORE_K, &&L_OP_SUB_STORE_K,
        &&L_OP_LT_LOOP, &&L_OP_LE_LOOP, &&L_OP_GT_LOOP, &&L_OP_GE_LOOP,
        &&L_OP_INC_JUMP, &&L_OP_CALL_DRAW, &&L_OP_TAILCALL,
        &&L_OP_PUSH_NIL, &&L_OP_RETURN_N, &&L_OP_COLLECT_VARARGS,
        &&L_OP_RETURN_SPREAD, &&L_OP_PACK_LAST, &&L_OP_VARARG_FIRST,
        &&L_OP_BIND_PARAMS, &&L_OP_PUSH_CLOSURE, &&L_OP_LOAD_FREE,
        &&L_OP_STORE_FREE,
        &&L_OP_LOAD_CELL, &&L_OP_STORE_CELL, &&L_OP_INC_CELL,
        &&L_OP_ADD_STORE_CELL, &&L_OP_SUB_STORE_CELL,
        &&L_OP_ADD_STORE_K_CELL, &&L_OP_SUB_STORE_K_CELL,
        &&L_OP_LT_JIF_K_CELL, &&L_OP_LE_JIF_K_CELL, &&L_OP_GT_JIF_K_CELL,
        &&L_OP_GE_JIF_K_CELL, &&L_OP_EQ_JIF_K_CELL, &&L_OP_NE_JIF_K_CELL,
        &&L_OP_LT_LOOP_CELL, &&L_OP_LE_LOOP_CELL, &&L_OP_GT_LOOP_CELL,
        &&L_OP_GE_LOOP_CELL, &&L_OP_INC_JUMP_CELL
    };
#endif

    while (running) {
        if (tctl) {
            if (tctl->stop.load(std::memory_order_relaxed)) { // stop/shutdown
                running = false;
                break;
            }
            if (tctl->pause_requested.load(std::memory_order_relaxed))
                tctl->wait_paused(); // blocks until resume/stop
        }
        // Sandbox (E8/neg18): global instruction counter (covers nested vm_run
        // calls via pcall/coroutines). Disabled (0) = short-circuit
        // on the first condition, ~zero cost in the hot path.
        if (g_step_limit > 0 && ++g_steps > g_step_limit)
            throw std::runtime_error("step_limit atingido (" +
                                     std::to_string(g_step_limit) +
                                     " instrucoes)");
        // E7/neg12: REFERENCE — no VM command modifies `code[ip]`
        // (bytecode is immutable at runtime; require preloads at compile time),
        // so there is no Instruction copy (with std::string) per instruction.
        const Instruction& in = code_data[ip++];
        const int curLine = in.line; // safe even if `code` reallocates
        try {
#ifdef ISILIEL_CGOTO
        if ((unsigned)in.op < (unsigned)OP_COUNT)
            goto *vm_dispatch_table[in.op];
        goto L_default;
#else
        switch (in.op) {
#endif
            VM_CASE(OP_CALL): { // OP_CALL (script function: saves/clears locals for recursion)
                int argc = in.b;
                if (argc < 0) {
                    int fixed = -argc - 1;
                    argc = fixed + vm_expand_last_arg(vm, fixed, in.s.str().c_str());
                }
                vm->callArgc = argc;
                callStack.push_back(ip);
                callResults.push_back(in.c == 0 ? 1 : in.c); // L5
                frameMarkers.push_back(scopeSaves.size());
                vm->frames.push_back({in.s.empty() ? InsStr("?") : in.s, in.line});
                vm_push_env(vm, nullptr); // L12
                sym_save_scope(in.a, scopeSaves); // in.a = entry_point = owner
                ip = in.a;
                VM_BREAK;
            }

            VM_CASE(OP_TAILCALL): { // L10: reuses the frame (return f(args))
                int argc = in.b;
                if (argc < 0) {
                    int fixed = -argc - 1;
                    argc = fixed + vm_expand_last_arg(vm, fixed, in.s.str().c_str());
                }
                vm->callArgc = argc;
                if (callStack.empty()) { // top of the script: normal call
                    callStack.push_back(ip);
                    callResults.push_back(in.c == 0 ? 1 : in.c); // L5
                    frameMarkers.push_back(scopeSaves.size());
                    vm->frames.push_back({in.s.empty() ? InsStr("?") : in.s, in.line});
                    vm_push_env(vm, nullptr); // L12
                    sym_save_scope(in.a, scopeSaves);
                    ip = in.a;
                    VM_BREAK;
                }
                // Keeps the caller's return/marker; discards the current function's
                // locals and installs the callee frame (without push).
                size_t marker = frameMarkers.back();
                sym_restore_scope(scopeSaves, marker);
                scopeSaves.resize(marker);
                if (!vm->frames.empty())
                    vm->frames.back() = {in.s.empty() ? InsStr("?") : in.s, in.line};
                vm_reset_env(vm, nullptr); // L12: named callee
                sym_save_scope(in.a, scopeSaves);
                ip = in.a;
                VM_BREAK;
            }

            VM_CASE(OP_CALL_DRAW): { // L1: setDraw(fn) callback, or global draw() (in.a)
                int entry = vm_draw_callback_entry();
                if (entry < 0) entry = in.a;
                if (entry < 0) { // no callback: push a dummy for the epilogue POP
                    PUSH(vm, make_float(0));
                    VM_BREAK;
                }
                vm->callArgc = 0;
                callStack.push_back(ip);
                callResults.push_back(1); // L5: draw does not use the return value
                frameMarkers.push_back(scopeSaves.size());
                vm->frames.push_back({"draw", in.line});
                vm_push_env(vm,
                    (g_draw_callback_set && g_draw_callback.type == VAL_FUNC &&
                     g_draw_callback.fn())
                        ? g_draw_callback.fn()->env
                        : nullptr); // L12: setDraw can receive a closure
                sym_save_scope(entry, scopeSaves);
                ip = entry;
                VM_BREAK;
            }

            VM_CASE(OP_RETURN): {
                if (callStack.empty()) {
                    // If the call stack is empty, ends execution (halt)
                    vm->topRet = 1;
                    running = false; 
                } else {
                    ip = callStack.back(); // Recovers the address where it stopped
                    callStack.pop_back();
                    if (!vm->frames.empty()) vm->frames.pop_back();
                    vm_pop_env(vm);
                    // Restores the locals of the frame being returned
                    size_t marker = frameMarkers.back();
                    frameMarkers.pop_back();
                    sym_restore_scope(scopeSaves, marker);
                    scopeSaves.resize(marker);
                    int wanted = callResults.empty() ? 1 : callResults.back();
                    if (!callResults.empty()) callResults.pop_back();
                    vm_finish_return(vm, 1, wanted); // L5: 1 value
                }
                VM_BREAK;
            }

            VM_CASE(OP_RETURN_N): { // L5: return with N values (in.a)
                int n = in.a;
                if (callStack.empty()) {
                    if (n < 0) n = vm->lastRet;
                    vm->topRet = n < 0 ? 0 : n;
                    running = false;
                } else {
                    ip = callStack.back();
                    callStack.pop_back();
                    if (!vm->frames.empty()) vm->frames.pop_back();
                    vm_pop_env(vm);
                    size_t marker = frameMarkers.back();
                    frameMarkers.pop_back();
                    sym_restore_scope(scopeSaves, marker);
                    scopeSaves.resize(marker);
                    int wanted = callResults.empty() ? 1 : callResults.back();
                    if (!callResults.empty()) callResults.pop_back();
                    vm_finish_return(vm, n, wanted);
                }
                VM_BREAK;
            }

            VM_CASE(OP_COLLECT_VARARGS): { // L5: vararg function prologue
                int nparams = in.a;
                int extra = vm->callArgc - nparams;
                if (extra < 0) extra = 0;
                Value arr = v_matrix(1, extra);
                for (int i = 0; i < extra; i++)
                    arr.arr()->elements[i] = vm->stack[vm->sp - extra + i];
                vm->sp -= extra;
                // Missing arguments become nil (as in Lua).
                int deficit = nparams - vm->callArgc;
                for (int i = 0; i < deficit; i++) PUSH(vm, make_nil());
                Symbol* vs = sym_get(in.b);
                if (!vs) throw std::runtime_error("Variavel __varargs_l5 nao encontrada");
                vs->value = std::move(arr);
                VM_BREAK;
            }

            VM_CASE(OP_RETURN_SPREAD): { // L5: return a, ... (array -> N values)
                int fixed = in.a;
                Value arr = POP(vm);
                if (arr.type != VAL_ARRAY || !arr.arr())
                    throw std::runtime_error("return ... esperava lista vararg");
                int n = fixed + (int)arr.arr()->elements.size();
                for (int i = 0; i < (int)arr.arr()->elements.size(); i++)
                    PUSH(vm, arr.arr()->elements[i]);
                if (callStack.empty()) {
                    vm->topRet = n;
                    running = false;
                } else {
                    ip = callStack.back();
                    callStack.pop_back();
                    if (!vm->frames.empty()) vm->frames.pop_back();
                    vm_pop_env(vm);
                    size_t marker = frameMarkers.back();
                    frameMarkers.pop_back();
                    sym_restore_scope(scopeSaves, marker);
                    scopeSaves.resize(marker);
                    int wanted = callResults.empty() ? 1 : callResults.back();
                    if (!callResults.empty()) callResults.pop_back();
                    vm_finish_return(vm, n, wanted);
                }
                VM_BREAK;
            }

            VM_CASE(OP_PACK_LAST): { // L5: packs lastRet values (args)
                int n = vm->lastRet;
                if (n < 0) n = 0;
                if (vm->sp < n)
                    throw std::runtime_error("Stack vazia ao expandir retorno");
                Value arr = v_matrix(1, n);
                for (int i = n - 1; i >= 0; i--)
                    arr.arr()->elements[i] = POP(vm);
                PUSH(vm, std::move(arr));
                vm->lastRet = 1;
                VM_BREAK;
            }

            VM_CASE(OP_VARARG_FIRST): { // L5: ... in an expression = 1st extra
                Symbol* vs = sym_get(in.a);
                if (!vs) throw std::runtime_error("Variavel __varargs_l5 nao encontrada");
                const Value& v = vs->value;
                if (v.type == VAL_ARRAY && v.arr() && !v.arr()->elements.empty())
                    PUSH(vm, v.arr()->elements[0]);
                else
                    PUSH(vm, make_nil());
                VM_BREAK;
            }

            VM_CASE(OP_BIND_PARAMS): { // L5: normal function prologue
                int n = in.a;
                int argc = vm->callArgc;
                if (argc != n) {
                    if (argc > n) {
                        vm->sp -= argc - n; // discards extra arguments
                    } else {
                        for (int i = argc; i < n; i++)
                            PUSH(vm, make_nil()); // missing parameters = nil
                    }
                }
                VM_BREAK;
            }

            VM_CASE(OP_HALT): 
                running = false; 
                VM_BREAK;

            VM_CASE(OP_PUSH): {
                // E10: constant pre-parsed at compile time (no stod here).
                // in.b == 1 => imaginary literal; in.b == 2 => exact int64
                // (low/high 32 bits in a/c); otherwise, double.
                if (in.b == 1) {
                    PUSH(vm, make_complex(0.0f, in.n));
                } else if (in.b == 2) {
                    unsigned long long u =
                        ((unsigned long long)(unsigned int)in.c << 32) |
                        (unsigned long long)(unsigned int)in.a;
                    PUSH(vm, make_int((long long)u));
                } else {
                    PUSH(vm, make_float(in.n));
                }
                VM_BREAK;
            }

            VM_CASE(OP_PUSH_STR): {
                // E16e: interned literal (in.a) — refcount only, no allocation
                if (vm->sp >= STACK_MAX)
                    throw std::runtime_error("Stack overflow - pilha cheia");
                Value& v = vm->stack[vm->sp++];
                v.type = VAL_STRING;
                v.f = 0.0; v.im = 0.0;
                v.s = str_pool_get(in.a);
                v.obj = RefObj();
                VM_BREAK;
            }

            VM_CASE(OP_PUSH_NIL): {
                PUSH(vm, make_nil());
                VM_BREAK;
            }

            VM_CASE(OP_POP):
                if (vm->sp > 0) vm->sp--; 
                VM_BREAK;            
            
            VM_CASE(OP_LOAD): {
                Symbol* sym = sym_get(in.a);
                if (!sym) throw std::runtime_error("Variável não encontrada no LOAD");
                if (sym->native) {
                    // Sandbox (E8/neg18): natives come from .mod modules
                    if (!g_allow_modules)
                        throw std::runtime_error(
                            "modulos desabilitados (allow_modules=false): " +
                            sym->name);
                    // Native variable of a `.mod` module (item 16): reads directly
                    // from library memory (the value is never cached)
                    Value v;
                    native_read(sym->native, &v);
                    PUSH(vm, std::move(v));
                    VM_BREAK;
                }
                // Script function used as a value (first-class, E2)
                if (sym->is_function) {
                    Value fv;
                    fv.type = VAL_FUNC;
                    fv.obj = std::make_shared<FuncRef>(FuncRef{sym->entry_point, sym->name, nullptr});
                    gc_register_obj(GC_OBJ_FUNC, fv.obj.p);
                    PUSH(vm, std::move(fv));
                    VM_BREAK;
                }
                PUSH(vm, sym->value);
                VM_BREAK;
            }
            
            VM_CASE(OP_STORE): {
                Symbol* sym = sym_get(in.a);
                if (!sym) throw std::runtime_error("Variável não encontrada");
                if (vm->sp <= 0) throw std::runtime_error("Stack vazia no STORE");
                // E16d: common path (ordinary local/global) writes directly
                if (!sym->native && sym->scalar == SC_NONE) {
                    sym->value = std::move(vm->stack[--vm->sp]);
                    VM_BREAK;
                }
                Value val = std::move(vm->stack[--vm->sp]);
                if (sym->native) {
                    // Sandbox (E8/neg18): natives come from .mod modules
                    if (!g_allow_modules)
                        throw std::runtime_error(
                            "modulos desabilitados (allow_modules=false): " +
                            sym->name);
                    // Direct memory write (type/read-only errors
                    // come from here; the line is appended by the loop catch)
                    native_write(sym->native, val);
                    VM_BREAK;
                }
                // Optional scalar declaration (E1c): `i32 x = ...` — every
                // write goes through coercion (truncate/range/0-1/text)
                if (sym->scalar != SC_NONE)
                    val = value_coerce_scalar(sym->scalar, val);
                sym->value = std::move(val);
                VM_BREAK;
            }
            
            VM_CASE(OP_INDEX_LOAD): {
                Value idx = POP(vm); 
                Value arr = POP(vm);
                // List (common path): tested first (table bench)
                if (arr.type == VAL_ARRAY) {
                    int i = (int)idx.f;
                    int n = (int)arr.arr()->elements.size();
                    if (i < 0) i += n;
                    if (i < 0 || i >= n) throw std::runtime_error("Índice fora dos limites");
                    PUSH(vm, arr.arr()->elements[i]);
                    VM_BREAK;
                }
                // String: s[i] returns 1 character (negative indices = from the end)
                if (arr.type == VAL_STRING) {
                    int i = (int)idx.f;
                    int n = (int)arr.s.size();
                    if (i < 0) i += n;
                    if (i < 0 || i >= n) throw std::runtime_error("Índice fora dos limites");
                    PUSH(vm, (Value{VAL_STRING, 0, std::string(1, arr.s[i])}));
                    VM_BREAK;
                }
                // Table (E3/L6): t["key"] — missing returns nil;
                // L11: __index (table or function) when the key is missing.
                if (arr.type == VAL_TABLE) {
                    Value r = vm_index_get(vm, code, arr, idx, in.line);
                    REFRESH_CODE();
                    PUSH(vm, std::move(r));
                    VM_BREAK;
                }
                throw std::runtime_error("Tentativa de indexar algo que não é array");
            }
            
            VM_CASE(OP_INDEX_STORE): {
                Value val = POP(vm); 
                Value idx = POP(vm); 
                Value arr = POP(vm);
                if (arr.type == VAL_ARRAY) {
                    int i = (int)idx.f;
                    int n = (int)arr.arr()->elements.size();
                    if (i < 0) i += n;
                    if (i < 0 || i >= n) throw std::runtime_error("Índice fora dos limites");
                    arr.arr()->elements[i] = val;
                    VM_BREAK;
                }
                if (arr.type == VAL_STRING) throw std::runtime_error("Strings sao imutaveis");
                // Table (E3/L6): t["key"] = v — creates; nil deletes the key
                if (arr.type == VAL_TABLE) {
                    std::string key = table_key_encode(idx);
                    if (val.type == VAL_NIL) arr.tb()->data.erase(key);
                    else arr.tb()->data[key] = val;
                    VM_BREAK;
                }
                throw std::runtime_error("Tentativa de indexar algo que não é array");
            }

            // Slicing: v[a:b] (start/end may be omitted; negatives = from the end)
            VM_CASE(OP_INDEX_SLICE): {
                Value endv = POP(vm);
                Value startv = POP(vm);
                Value src = POP(vm);
                int flags = in.a; // bit1 = start present, bit0 = end present

                if (src.type != VAL_ARRAY && src.type != VAL_STRING)
                    throw std::runtime_error("Fatiamento requer lista ou string");

                int n = (src.type == VAL_STRING) ? (int)src.s.size()
                                                 : (int)src.arr()->elements.size();
                int a = (flags & 2) ? (int)startv.f : 0;
                int b = (flags & 1) ? (int)endv.f   : n;
                if (a < 0) a += n;
                if (b < 0) b += n;
                if (a < 0) a = 0;
                if (b > n) b = n;
                if (a > n) a = n;
                if (b < a) b = a;

                if (src.type == VAL_STRING) {
                    PUSH(vm, (Value{VAL_STRING, 0, src.s.substr(a, b - a)}));
                } else {
                    Value out = v_matrix(1, b - a);
                    for (int i = a; i < b; i++)
                        out.arr()->elements[i - a] = src.arr()->elements[i];
                    PUSH(vm, std::move(out));
                }
                VM_BREAK;
            }

            // --- CLASSES (phase E4) ---
            VM_CASE(OP_NEW_INSTANCE): {
                // new T(...) — instance with ALL fields zeroed
                // (root -> leaf; defaults are VAL_FLOAT 0)
                if (!struct_find(in.s))
                    throw std::runtime_error("Tipo desconhecido: " + in.s);
                Value v;
                v.type = VAL_STRUCT;
                v.obj = std::make_shared<StructInstance>();
                gc_register_obj(GC_OBJ_STRUCT, v.obj.p);
                v.st()->type_name = in.s;
                v.st()->fields.resize(struct_total_fields(in.s));
                PUSH(vm, std::move(v));
                VM_BREAK;
            }

            VM_CASE(OP_DUP): {
                if (vm->sp <= 0)
                    throw std::runtime_error("Stack vazia no OP_DUP");
                PUSH(vm, vm->stack[vm->sp - 1]);
                VM_BREAK;
            }

            VM_CASE(OP_CALL_SUPER): {
                // super.m(args): [this, args...] with the PARENT entry (resolved
                // at compile time — ignores the current override)
                int n = in.b;
                if (n < 0) {
                    int fixed = -n - 1;
                    n = fixed + vm_expand_last_arg(vm, fixed, in.s.str().c_str());
                }
                if (n > 16)
                    throw std::runtime_error("Numero de argumentos invalido em super");
                Value tmp[16];
                for (int i = n - 1; i >= 0; i--) tmp[i] = POP(vm);
                Value self = POP(vm);
                if (self.type != VAL_STRUCT || !self.st())
                    throw std::runtime_error("'super' chamado em valor que nao e struct");
                PUSH(vm, std::move(self));
                for (int i = 0; i < n; i++) PUSH(vm, std::move(tmp[i]));
                vm->callArgc = n + 1; // self + args
                callStack.push_back(ip);
                callResults.push_back(in.c == 0 ? 1 : in.c); // L5
                frameMarkers.push_back(scopeSaves.size());
                vm->frames.push_back({in.s, in.line});
                vm_push_env(vm, nullptr); // L12
                sym_save_scope(in.a, scopeSaves); // in.a = GLOBAL entry of the parent
                ip = in.a;
                VM_BREAK;
            }

            // Table literal (E3): stack [k1, v1, ..., kn, vn], a = n pairs
            VM_CASE(OP_BUILD_TABLE): {
                int n = in.a;
                Value v;
                v.type = VAL_TABLE;
                v.obj = std::make_shared<Table>();
                gc_register_obj(GC_OBJ_TABLE, v.obj.p);
                for (int i = 0; i < n; i++) { // LIFO: vn..v1 come out first
                    Value val = POP(vm);
                    Value key = POP(vm);
                    std::string enc = table_key_encode(key);
                    if (val.type == VAL_NIL) v.tb()->data.erase(enc); // L6
                    else v.tb()->data[enc] = val; // repeated: last wins
                }
                PUSH(vm, std::move(v));
                VM_BREAK;
            }

            // --- STRUCTS ---
            // Literal {..} -> instance; in.s = type name, in.a = number of fields
            VM_CASE(OP_BUILD_STRUCT): {
                const StructType* t = struct_find(in.s);
                if (!t) throw std::runtime_error("Tipo desconhecido: " + in.s);
                if (struct_total_fields(in.s) != in.a)
                    throw std::runtime_error("Literal de '" + in.s + "' com numero de campos errado");
                Value v;
                v.type = VAL_STRUCT;
                v.obj = std::make_shared<StructInstance>();
                gc_register_obj(GC_OBJ_STRUCT, v.obj.p);
                v.st()->type_name = in.s;
                v.st()->fields.resize(in.a > 0 ? in.a : 0); // total (inherited+own)
                for (int i = in.a - 1; i >= 0; i--) v.st()->fields[i] = POP(vm);
                PUSH(vm, std::move(v));
                VM_BREAK;
            }

            VM_CASE(OP_FIELD_LOAD): {
                Value obj = POP(vm);
                // Table/struct: t.key. L11: __index when missing.
                Value r = vm_field_get(vm, code, obj, in.s.str(), in.line);
                REFRESH_CODE();
                PUSH(vm, std::move(r));
                VM_BREAK;
            }

            VM_CASE(OP_FIELD_STORE): {
                Value val = POP(vm);
                Value obj = POP(vm);
                // Table (E3/L6): t.key = v — creates; nil deletes the key
                if (obj.type == VAL_TABLE) {
                    std::string key = table_key_str(in.s.str());
                    if (val.type == VAL_NIL) obj.tb()->data.erase(key);
                    else obj.tb()->data[key] = val;
                    VM_BREAK;
                }
                if (obj.type != VAL_STRUCT || !obj.st())
                    throw std::runtime_error("Atribuicao a campo em valor que nao e struct");
                int idx = struct_field_index(obj.st()->type_name, in.s);
                if (idx < 0 || idx >= (int)obj.st()->fields.size())
                    throw std::runtime_error("Campo '" + in.s + "' nao existe em '" + obj.st()->type_name + "'");
                obj.st()->fields[idx] = val; // st shares the instance
                VM_BREAK;
            }

            // Method call: stack [this, arg1..argN], in.s = method, in.b = n
            VM_CASE(OP_CALL_METHOD): {
                int n = in.b;
                if (n < 0) {
                    int fixed = -n - 1;
                    n = fixed + vm_expand_last_arg(vm, fixed, in.s.str().c_str());
                }
                if (n > 16)
                    throw std::runtime_error("Numero de argumentos invalido no metodo");
                Value tmp[16];
                for (int i = n - 1; i >= 0; i--) tmp[i] = POP(vm);
                Value self = POP(vm);
                if (self.type != VAL_STRUCT || !self.st())
                    throw std::runtime_error("'" + in.s + "' chamado em valor que nao e struct");
                int msid = struct_method_sid(self.st()->type_name, in.s);
                if (msid == -1)
                    throw std::runtime_error("Metodo '" + in.s + "' nao existe em '" + self.st()->type_name + "'");
                Symbol* m = sym_get(msid);
                if (!m || !m->is_function)
                    throw std::runtime_error("Metodo invalido: " + in.s);
                // Returns [this, args...] — the callee captures params and __this
                // E7: type_name BEFORE the move (PUSH empties self's shared_ptr)
                std::string typeName = self.st()->type_name;
                PUSH(vm, std::move(self));
                for (int i = 0; i < n; i++) PUSH(vm, std::move(tmp[i]));
                vm->callArgc = n + 1; // self + args
                callStack.push_back(ip);
                callResults.push_back(in.c == 0 ? 1 : in.c); // L5
                frameMarkers.push_back(scopeSaves.size());
                vm->frames.push_back({InsStr(typeName + "::" + in.s), in.line});
                vm_push_env(vm, nullptr); // L12
                sym_save_scope(m->entry_point, scopeSaves);
                ip = m->entry_point;
                VM_BREAK;
            }

            // --- E2: first-class functions ---
            VM_CASE(OP_PUSH_FUNC): {
                Value fv;
                fv.type = VAL_FUNC;
                fv.obj = std::make_shared<FuncRef>();
                gc_register_obj(GC_OBJ_FUNC, fv.obj.p);
                fv.fn()->entry = in.a;
                fv.fn()->name = in.s.empty() ? std::string("anonima") : std::string(in.s);
                PUSH(vm, std::move(fv));
                VM_BREAK;
            }

            // L12: creates a closure and captures (by cell) the locals of the
            // frame where the `fn(...)` was evaluated (owner = in.b; -1 at the top).
            VM_CASE(OP_PUSH_CLOSURE): {
                Value fv;
                fv.type = VAL_FUNC;
                fv.obj = std::make_shared<FuncRef>();
                gc_register_obj(GC_OBJ_FUNC, fv.obj.p);
                fv.fn()->entry = in.a;
                fv.fn()->name = in.s.empty() ? std::string("anonima") : std::string(in.s);
                fv.fn()->env = vm_capture_env(vm, in.b);
                PUSH(vm, std::move(fv));
                VM_BREAK;
            }

            // L12: upvalue access by name in the captured lexical environment.
            VM_CASE(OP_LOAD_FREE): {
                Value* slot = free_slot(vm, in.s.str());
                if (!slot)
                    throw std::runtime_error("Variavel '" + in.s.str() +
                                             "' nao declarada (closure)");
                PUSH(vm, *slot);
                VM_BREAK;
            }
            VM_CASE(OP_STORE_FREE): {
                Value* slot = free_slot(vm, in.s.str());
                if (!slot)
                    throw std::runtime_error("Variavel '" + in.s.str() +
                                             "' nao declarada (closure)");
                if (vm->sp <= 0) throw std::runtime_error("Stack vazia no STORE_FREE");
                *slot = std::move(vm->stack[--vm->sp]);
                VM_BREAK;
            }

            VM_CASE(OP_CALL_INDIRECT): {
                int argc = in.a;
                if (argc < 0) {
                    int fixed = -argc - 1;
                    argc = fixed + vm_expand_last_arg(vm, fixed, "chamada indireta");
                }
                if (argc > 16)
                    throw std::runtime_error("Numero de argumentos invalido na chamada indireta");
                if (vm->sp < argc + 1)
                    throw std::runtime_error("Stack vazia na chamada indireta");
                int base = vm->sp - argc - 1;
                Value fv = vm->stack[base];
                if (fv.type != VAL_FUNC || !fv.fn() || fv.fn()->entry < 0) {
                    Value mm;
                    if ((fv.type == VAL_TABLE || fv.type == VAL_STRUCT) &&
                        get_metamethod(fv, "__call", &mm) &&
                        mm.type == VAL_FUNC && mm.fn() && mm.fn()->entry >= 0) {
                        if (vm->sp >= STACK_MAX)
                            throw std::runtime_error("Stack overflow - pilha cheia");
                        for (int i = vm->sp; i > base; --i)
                            vm->stack[i] = std::move(vm->stack[i - 1]);
                        vm->stack[base] = mm;
                        vm->sp += 1;
                        argc += 1; // the object enters as the 1st argument
                        fv = mm;
                    } else {
                        throw std::runtime_error("Valor nao e uma funcao");
                    }
                }
                // Consumes the function value: the callee receives only [args...].
                for (int i = 0; i < argc; i++)
                    vm->stack[base + i] = std::move(vm->stack[base + i + 1]);
                vm->sp -= 1;
                vm->callArgc = argc;
                callStack.push_back(ip);
                callResults.push_back(in.c == 0 ? 1 : in.c); // L5
                frameMarkers.push_back(scopeSaves.size());
                vm->frames.push_back({InsStr(fv.fn()->name), in.line});
                vm_push_env(vm, fv.fn()->env); // L12: closure
                sym_save_scope(fv.fn()->entry, scopeSaves);
                ip = fv.fn()->entry;
                VM_BREAK;
            }

            VM_CASE(OP_CALL_PCALL): {
                // Protected call (E2): pushes [func, args...], runs the
                // function in a nested VM and returns {ok, payload} (1x2):
                //   ok=1 -> payload = return value
                //   ok=0 -> payload = error message + traceback with frames
                int argc = in.a; // includes the function (1st arg): [f, a, b...]
                if (argc < 0) {
                    int fixed = -argc - 1;
                    argc = fixed + vm_expand_last_arg(vm, fixed, "pcall");
                }
                if (argc < 1)
                    throw std::runtime_error("pcall: funcao nao informada");
                if (vm->sp < argc)
                    throw std::runtime_error("Stack vazia no pcall");
                int base = vm->sp - argc;
                Value fv = vm->stack[base];
                Value result;
                if (fv.type != VAL_FUNC || !fv.fn() || fv.fn()->entry < 0) {
                    Value mm;
                    if ((fv.type == VAL_TABLE || fv.type == VAL_STRUCT) &&
                        get_metamethod(fv, "__call", &mm) &&
                        mm.type == VAL_FUNC && mm.fn() && mm.fn()->entry >= 0) {
                        if (vm->sp >= STACK_MAX)
                            throw std::runtime_error("Stack overflow - pilha cheia");
                        for (int i = vm->sp; i > base; --i)
                            vm->stack[i] = std::move(vm->stack[i - 1]);
                        vm->stack[base] = mm;
                        vm->sp += 1;
                        argc += 1; // called object becomes the 1st argument
                        fv = mm;
                    } else {
                        vm->sp = base;
                        result = v_matrix(1, 2);
                        result.arr()->elements[0] = make_float(0.0);
                        result.arr()->elements[1] = make_string("pcall: valor nao e uma funcao");
                        PUSH(vm, std::move(result));
                        VM_BREAK;
                    }
                }
                int entry = fv.fn()->entry;
                // E5: the nested run needs FRESH callStack/frames/scopeSaves
                // — vm_run references the VM members, so the external
                // state is moved out before and restored afterwards
                // (without this the pcall function's RETURN would pop the
                // caller's frame).
                auto extCall = std::move(vm->callStack);    vm->callStack = {};
                auto extResults = std::move(vm->callResults); vm->callResults = {};
                auto extMark = std::move(vm->frameMarkers); vm->frameMarkers = {};
                auto extSaves = std::move(vm->scopeSaves);  vm->scopeSaves = {};
                auto extFrames = std::move(vm->frames);     vm->frames = {};
                auto extEnvs = std::move(vm->frameEnvs);    vm->frameEnvs = {};
                auto extEnvParents = std::move(vm->frameEnvParents); vm->frameEnvParents = {};
                bool extRunning = vm->running;
                int extIp = vm->ip;
                vm->callArgc = argc - 1;
                vm->frames.push_back({InsStr(fv.fn()->name), in.line});
                vm_push_env(vm, fv.fn()->env); // L12
                std::vector<SavedLocal> saved;
                sym_save_scope(entry, saved);
                bool ok = false, yieldedOut = false;
                Value ret;
                std::string errMsg;
                try {
                    vm_run(vm, code, entry);
                    ok = true;
                    int nret = vm->topRet;
                    if (nret <= 0 || vm->sp <= 0) ret = make_nil();
                    else ret = vm->stack[vm->sp - nret]; // L5: 1st value
                } catch (YieldSignal&) {
                    yieldedOut = true; // becomes an error below (out of context)
                } catch (const std::exception& e) {
                    errMsg = e.what();
                    for (size_t i = 0; i < vm->frames.size(); i++)
                        errMsg += "\n  em " + vm->frames[i].first.str() +
                                 " (linha " + std::to_string(vm->frames[i].second) + ")";
                }
                vm->callStack = std::move(extCall);
                vm->callResults = std::move(extResults);
                vm->frameMarkers = std::move(extMark);
                vm->scopeSaves = std::move(extSaves);
                vm->frames = std::move(extFrames);
                vm->frameEnvs = std::move(extEnvs);
                vm->frameEnvParents = std::move(extEnvParents);
                vm->running = extRunning;
                vm->ip = extIp;
                REFRESH_CODE(); // pcall may have called dofile/load
                sym_restore_scope(saved, 0);
                vm->sp = base;
                if (yieldedOut)
                    throw std::runtime_error("coyield: nao suportado dentro de pcall");
                result = v_matrix(1, 2);
                result.arr()->elements[0] = make_float(ok ? 1.0 : 0.0);
                result.arr()->elements[1] = ok ? ret : make_string(errMsg);
                PUSH(vm, std::move(result));
                VM_BREAK;
            }

            // ARITHMETIC (E17b: in-place numeric fast path)
            VM_CASE(OP_ADD): {
                if (vm->sp < 2) throw std::runtime_error("Stack underflow - pilha vazia");
                Value& a = vm->stack[vm->sp - 2];
                Value& b = vm->stack[vm->sp - 1];
                if (a.type == VAL_FLOAT && b.type == VAL_FLOAT) {
                    a.f += b.f;
                    vm->sp -= 1;
                } else if (a.type == VAL_INT && b.type == VAL_INT) {
                    a.i = i64_wrap_add(a.i, b.i);
                    a.f = (double)a.i;
                    vm->sp -= 1;
                } else {
                    Value mm;
                    if (get_metamethod(a, "__add", &mm) ||
                        get_metamethod(b, "__add", &mm)) {
                        Value args[2] = {a, b};
                        Value r = vm_call_mm(vm, code, mm, args, 2, in.line);
                        REFRESH_CODE();
                        vm->sp -= 2;
                        PUSH(vm, std::move(r));
                    } else {
                        vm->sp -= 2;
                        a = do_add(a, b);
                        vm->sp += 1;
                    }
                }
                VM_BREAK;
            }
            VM_CASE(OP_SUB): {
                if (vm->sp < 2) throw std::runtime_error("Stack underflow - pilha vazia");
                Value& a = vm->stack[vm->sp - 2];
                Value& b = vm->stack[vm->sp - 1];
                if (a.type == VAL_FLOAT && b.type == VAL_FLOAT) {
                    a.f -= b.f;
                    vm->sp -= 1;
                } else if (a.type == VAL_INT && b.type == VAL_INT) {
                    a.i = i64_wrap_sub(a.i, b.i);
                    a.f = (double)a.i;
                    vm->sp -= 1;
                } else {
                    vm->sp -= 2;
                    a = do_sub(a, b);
                    vm->sp += 1;
                }
                VM_BREAK;
            }
            VM_CASE(OP_MUL): {
                if (vm->sp < 2) throw std::runtime_error("Stack underflow - pilha vazia");
                Value& a = vm->stack[vm->sp - 2];
                Value& b = vm->stack[vm->sp - 1];
                if (a.type == VAL_FLOAT && b.type == VAL_FLOAT) {
                    a.f *= b.f;
                    vm->sp -= 1;
                } else if (a.type == VAL_INT && b.type == VAL_INT) {
                    a.i = i64_wrap_mul(a.i, b.i);
                    a.f = (double)a.i;
                    vm->sp -= 1;
                } else {
                    vm->sp -= 2;
                    a = do_mul(a, b);
                    vm->sp += 1;
                }
                VM_BREAK;
            }
            VM_CASE(OP_DIV): {
                if (vm->sp < 2) throw std::runtime_error("Stack underflow - pilha vazia");
                Value& a = vm->stack[vm->sp - 2];
                Value& b = vm->stack[vm->sp - 1];
                if (a.type == VAL_FLOAT && b.type == VAL_FLOAT) {
                    if (std::abs(b.f) < 1e-10f) {
                        vm->sp -= 2;
                        throw std::runtime_error("Divisão por zero");
                    }
                    a.f /= b.f;
                    vm->sp -= 1;
                } else {
                    vm->sp -= 2;
                    a = do_div(a, b);
                    vm->sp += 1;
                }
                VM_BREAK;
            }
            VM_CASE(OP_MOD): {
                if (vm->sp < 2) throw std::runtime_error("Stack underflow - pilha vazia");
                Value& a = vm->stack[vm->sp - 2];
                Value& b = vm->stack[vm->sp - 1];
                if (a.type == VAL_FLOAT && b.type == VAL_FLOAT) {
                    const double LIMIT = 9223372036854775808.0;
                    if (a.f >= -LIMIT && a.f < LIMIT && a.f == std::floor(a.f) &&
                        b.f >= -LIMIT && b.f < LIMIT && b.f == std::floor(b.f)) {
                        long long ib = (long long)b.f;
                        if (ib == 0) {
                            vm->sp -= 2;
                            throw std::runtime_error("Módulo por zero");
                        }
                        long long ia = (long long)a.f;
                        a.f = (double)(ib == -1 ? 0LL : ia % ib);
                        vm->sp -= 1;
                    } else {
                        vm->sp -= 2;
                        a = do_mod(a, b);
                        vm->sp += 1;
                    }
                } else if (a.type == VAL_INT && b.type == VAL_INT) {
                    if (b.i == 0) {
                        vm->sp -= 2;
                        throw std::runtime_error("Módulo por zero");
                    }
                    a.i = (b.i == -1) ? 0LL : (a.i % b.i);
                    a.f = (double)a.i;
                    vm->sp -= 1;
                } else {
                    vm->sp -= 2;
                    a = do_mod(a, b);
                    vm->sp += 1;
                }
                VM_BREAK;
            }

            // Covers strings, numbers, lists and structs (structural value)
            VM_CASE(OP_EQ): {
                if (vm->sp < 2) throw std::runtime_error("Stack underflow - pilha vazia");
                Value& a = vm->stack[vm->sp - 2];
                Value& b = vm->stack[vm->sp - 1];
                if (a.type == VAL_FLOAT && b.type == VAL_FLOAT) {
                    a.f = (a.f == b.f) ? 1.0 : 0.0;
                    vm->sp -= 1;
                } else {
                    Value mm;
                    bool meta = (a.type == b.type &&
                                 (a.type == VAL_TABLE || a.type == VAL_STRUCT)) &&
                                (get_metamethod(a, "__eq", &mm) ||
                                 get_metamethod(b, "__eq", &mm));
                    if (meta) {
                        Value args[2] = {a, b};
                        Value r = vm_call_mm(vm, code, mm, args, 2, in.line);
                        REFRESH_CODE();
                        vm->sp -= 2;
                        PUSH(vm, make_float(truthy(r) ? 1.0 : 0.0));
                    } else {
                        vm->sp -= 2;
                        a = do_eq(a, b);
                        vm->sp += 1;
                    }
                }
                VM_BREAK;
            }
            VM_CASE(OP_NE): {
                if (vm->sp < 2) throw std::runtime_error("Stack underflow - pilha vazia");
                Value& a = vm->stack[vm->sp - 2];
                Value& b = vm->stack[vm->sp - 1];
                if (a.type == VAL_FLOAT && b.type == VAL_FLOAT) {
                    a.f = (std::abs(a.f - b.f) >= 1e-6f) ? 1.0 : 0.0;
                    vm->sp -= 1;
                } else {
                    Value mm;
                    bool meta = (a.type == b.type &&
                                 (a.type == VAL_TABLE || a.type == VAL_STRUCT)) &&
                                (get_metamethod(a, "__eq", &mm) ||
                                 get_metamethod(b, "__eq", &mm));
                    if (meta) {
                        Value args[2] = {a, b};
                        Value r = vm_call_mm(vm, code, mm, args, 2, in.line);
                        REFRESH_CODE();
                        vm->sp -= 2;
                        PUSH(vm, make_float(truthy(r) ? 0.0 : 1.0));
                    } else {
                        vm->sp -= 2;
                        a = do_ne(a, b);
                        vm->sp += 1;
                    }
                }
                VM_BREAK;
            }
            VM_CASE(OP_LT): { BIN2_NUM(vm, _a.f < _b.f ? 1.0 : 0.0, do_lt(_a, _b)); VM_BREAK; }
            VM_CASE(OP_GT): { BIN2_NUM(vm, _b.f < _a.f ? 1.0 : 0.0, do_gt(_a, _b)); VM_BREAK; }
            VM_CASE(OP_LE): { BIN2_NUM(vm, !(_b.f < _a.f) ? 1.0 : 0.0, do_le(_a, _b)); VM_BREAK; }
            VM_CASE(OP_GE): { BIN2_NUM(vm, !(_a.f < _b.f) ? 1.0 : 0.0, do_ge(_a, _b)); VM_BREAK; }

            // --- E15: variants with an immediate constant (in.n) ----------
            VM_CASE(OP_ADD_K): {
                if (vm->sp < 1) throw std::runtime_error("Stack underflow - pilha vazia");
                Value& a = vm->stack[vm->sp - 1];
                if (a.type == VAL_FLOAT) {
                    a.f += in.n;
                } else if (a.type == VAL_INT && in.c) {
                    a.i = i64_wrap_add(a.i, (long long)in.n);
                    a.f = (double)a.i;
                } else {
                    Value mm;
                    if (get_metamethod(a, "__add", &mm)) {
                        Value args[2] = {a, k_value(in.n, in.c)};
                        Value r = vm_call_mm(vm, code, mm, args, 2, in.line);
                        REFRESH_CODE();
                        vm->sp -= 1;
                        PUSH(vm, std::move(r));
                    } else {
                        vm->sp -= 1;
                        a = do_add_num(a, in.n, in.c != 0);
                        vm->sp += 1;
                    }
                }
                VM_BREAK;
            }
            VM_CASE(OP_SUB_K): {
                if (vm->sp < 1) throw std::runtime_error("Stack underflow - pilha vazia");
                Value& a = vm->stack[vm->sp - 1];
                if (a.type == VAL_FLOAT) a.f -= in.n;
                else if (a.type == VAL_INT && in.c) {
                    a.i = i64_wrap_sub(a.i, (long long)in.n);
                    a.f = (double)a.i;
                } else {
                    vm->sp -= 1;
                    a = do_sub_num(a, in.n, in.c != 0);
                    vm->sp += 1;
                }
                VM_BREAK;
            }
            VM_CASE(OP_MUL_K): {
                if (vm->sp < 1) throw std::runtime_error("Stack underflow - pilha vazia");
                Value& a = vm->stack[vm->sp - 1];
                if (a.type == VAL_FLOAT) a.f *= in.n;
                else if (a.type == VAL_INT && in.c) {
                    a.i = i64_wrap_mul(a.i, (long long)in.n);
                    a.f = (double)a.i;
                } else {
                    vm->sp -= 1;
                    a = do_mul_num(a, in.n, in.c != 0);
                    vm->sp += 1;
                }
                VM_BREAK;
            }
            VM_CASE(OP_DIV_K): {
                if (vm->sp < 1) throw std::runtime_error("Stack underflow - pilha vazia");
                Value& a = vm->stack[vm->sp - 1];
                if (a.type == VAL_FLOAT) {
                    if (std::abs(in.n) < 1e-10f) {
                        vm->sp -= 1;
                        throw std::runtime_error("Divisão por zero");
                    }
                    a.f /= in.n;
                } else {
                    vm->sp -= 1;
                    a = do_div_num(a, in.n, in.c != 0);
                    vm->sp += 1;
                }
                VM_BREAK;
            }
            VM_CASE(OP_MOD_K): {
                if (vm->sp < 1) throw std::runtime_error("Stack underflow - pilha vazia");
                Value& a = vm->stack[vm->sp - 1];
                if (a.type == VAL_FLOAT) {
                    const double LIMIT = 9223372036854775808.0;
                    if (a.f >= -LIMIT && a.f < LIMIT && a.f == std::floor(a.f) &&
                        in.n >= -LIMIT && in.n < LIMIT && in.n == std::floor(in.n)) {
                        long long ib = (long long)in.n;
                        if (ib == 0) {
                            vm->sp -= 1;
                            throw std::runtime_error("Módulo por zero");
                        }
                        long long ia = (long long)a.f;
                        a.f = (double)(ib == -1 ? 0LL : ia % ib);
                    } else {
                        vm->sp -= 1;
                        a = do_mod_num(a, in.n, in.c != 0);
                        vm->sp += 1;
                    }
                } else if (a.type == VAL_INT && in.c) {
                    long long ib = (long long)in.n;
                    if (ib == 0) {
                        vm->sp -= 1;
                        throw std::runtime_error("Módulo por zero");
                    }
                    a.i = (ib == -1) ? 0LL : (a.i % ib);
                    a.f = (double)a.i;
                } else {
                    vm->sp -= 1;
                    a = do_mod_num(a, in.n, in.c != 0);
                    vm->sp += 1;
                }
                VM_BREAK;
            }
            VM_CASE(OP_EQ_K): { BIN1_NUM(vm, _a.f == in.n ? 1.0 : 0.0, do_eq_num(_a, in.n, in.c != 0)); VM_BREAK; }
            VM_CASE(OP_NE_K): { BIN1_NUM(vm, std::abs(_a.f - in.n) >= 1e-6f ? 1.0 : 0.0, do_ne_num(_a, in.n, in.c != 0)); VM_BREAK; }
            VM_CASE(OP_LT_K): { BIN1_NUM(vm, _a.f < in.n ? 1.0 : 0.0, do_lt_num(_a, in.n, in.c != 0)); VM_BREAK; }
            VM_CASE(OP_LE_K): { BIN1_NUM(vm, _a.f <= in.n ? 1.0 : 0.0, do_le_num(_a, in.n, in.c != 0)); VM_BREAK; }
            VM_CASE(OP_GT_K): { BIN1_NUM(vm, _a.f > in.n ? 1.0 : 0.0, do_gt_num(_a, in.n, in.c != 0)); VM_BREAK; }
            VM_CASE(OP_GE_K): { BIN1_NUM(vm, _a.f >= in.n ? 1.0 : 0.0, do_ge_num(_a, in.n, in.c != 0)); VM_BREAK; }

            // x = x + k  (assignment/auto-increment without going through the stack)
            VM_CASE(OP_INC_LOCAL): {
                Symbol* sym = sym_get(in.a);
                if (!sym) throw std::runtime_error("Variavel nao encontrada no INC_LOCAL");
                Value* slot = &sym->value;
                if (slot->type == VAL_FLOAT && !sym->native &&
                    sym->scalar == SC_NONE) {
                    slot->f += in.n;
                    VM_BREAK;
                }
                if (slot->type == VAL_INT && !sym->native &&
                    sym->scalar == SC_NONE && in.c) {
                    slot->i = i64_wrap_add(slot->i, (long long)in.n);
                    slot->f = (double)slot->i;
                    VM_BREAK;
                }
                Value nv;
                Value mm;
                if (get_metamethod(*slot, "__add", &mm)) {
                    Value args[2] = {*slot, k_value(in.n, in.c)};
                    nv = vm_call_mm(vm, code, mm, args, 2, in.line);
                    REFRESH_CODE();
                } else {
                    nv = do_add(*slot, k_value(in.n, in.c));
                }
                if (sym->native) {
                    if (!g_allow_modules)
                        throw std::runtime_error(
                            "modulos desabilitados (allow_modules=false): " +
                            sym->name);
                    native_write(sym->native, nv);
                } else {
                    if (sym->scalar != SC_NONE)
                        nv = value_coerce_scalar(sym->scalar, nv);
                    *slot = std::move(nv);
                }
                VM_BREAK;
            }

            // [LOAD sid, CMP_K c] JUMP_IF_FALSE fused (a=target, b=sid)
            VM_CASE(OP_LT_JIF_K): {
                Symbol* sym = sym_get(in.b);
                if (!sym) throw std::runtime_error("Variavel nao encontrada no JIF");
                const Value& v = sym->value;
                if (v.type == VAL_FLOAT) { if (!(v.f < in.n)) ip = in.a; }
                else if (!cmp_lt(v, k_value(in.n, in.c), "<")) ip = in.a;
                VM_BREAK;
            }
            VM_CASE(OP_LE_JIF_K): {
                Symbol* sym = sym_get(in.b);
                if (!sym) throw std::runtime_error("Variavel nao encontrada no JIF");
                const Value& v = sym->value;
                if (v.type == VAL_FLOAT) { if (in.n < v.f) ip = in.a; }
                else if (cmp_lt(k_value(in.n, in.c), v, "<=")) ip = in.a;
                VM_BREAK;
            }
            VM_CASE(OP_GT_JIF_K): {
                Symbol* sym = sym_get(in.b);
                if (!sym) throw std::runtime_error("Variavel nao encontrada no JIF");
                const Value& v = sym->value;
                if (v.type == VAL_FLOAT) { if (!(in.n < v.f)) ip = in.a; }
                else if (!cmp_lt(k_value(in.n, in.c), v, ">")) ip = in.a;
                VM_BREAK;
            }
            VM_CASE(OP_GE_JIF_K): {
                Symbol* sym = sym_get(in.b);
                if (!sym) throw std::runtime_error("Variavel nao encontrada no JIF");
                const Value& v = sym->value;
                if (v.type == VAL_FLOAT) { if (v.f < in.n) ip = in.a; }
                else if (cmp_lt(v, k_value(in.n, in.c), ">=")) ip = in.a;
                VM_BREAK;
            }
            VM_CASE(OP_EQ_JIF_K): {
                Symbol* sym = sym_get(in.b);
                if (!sym) throw std::runtime_error("Variavel nao encontrada no JIF");
                const Value& v = sym->value;
                if (v.type == VAL_FLOAT) { if (v.f != in.n) ip = in.a; }
                else if (!value_eq(v, k_value(in.n, in.c))) ip = in.a;
                VM_BREAK;
            }
            VM_CASE(OP_NE_JIF_K): {
                Symbol* sym = sym_get(in.b);
                if (!sym) throw std::runtime_error("Variavel nao encontrada no JIF");
                const Value& v = sym->value;
                if (v.type == VAL_FLOAT) { if (v.f == in.n) ip = in.a; }
                else if (value_eq(v, k_value(in.n, in.c))) ip = in.a;
                VM_BREAK;
            }

            VM_CASE(OP_AND): { Value b = POP(vm), a = POP(vm); PUSH(vm, (Value{VAL_FLOAT, (truthy(a) && truthy(b)) ? 1.0f : 0.0f})); VM_BREAK; }
            VM_CASE(OP_OR):  { Value b = POP(vm), a = POP(vm); PUSH(vm, (Value{VAL_FLOAT, (truthy(a) || truthy(b)) ? 1.0f : 0.0f})); VM_BREAK; }
            VM_CASE(OP_BIT_AND): { Value b = POP(vm), a = POP(vm); require_real_pair(a, b, "&"); PUSH(vm, make_int(req_int64(a, "&") & req_int64(b, "&"))); VM_BREAK; }
            VM_CASE(OP_BIT_OR):  { Value b = POP(vm), a = POP(vm); require_real_pair(a, b, "|"); PUSH(vm, make_int(req_int64(a, "|") | req_int64(b, "|"))); VM_BREAK; }
            VM_CASE(OP_BIT_XOR): { Value b = POP(vm), a = POP(vm); require_real_pair(a, b, "^"); PUSH(vm, make_int(req_int64(a, "^") ^ req_int64(b, "^"))); VM_BREAK; }
            VM_CASE(OP_BIT_NOT): { Value a = POP(vm); require_real_pair(a, a, "~"); PUSH(vm, make_int(~req_int64(a, "~"))); VM_BREAK; }
            VM_CASE(OP_SHL): {
                Value b = POP(vm), a = POP(vm);
                require_real_pair(a, b, "<<");
                long long sh = req_int64(b, "<<");
                if (sh < 0 || sh > 63)
                    throw std::runtime_error("Deslocamento fora da faixa 0..63");
                unsigned long long ua = (unsigned long long)req_int64(a, "<<");
                PUSH(vm, make_int((long long)(ua << sh)));
                VM_BREAK;
            }
            VM_CASE(OP_SHR): {
                Value b = POP(vm), a = POP(vm);
                require_real_pair(a, b, ">>");
                long long sh = req_int64(b, ">>");
                if (sh < 0 || sh > 63)
                    throw std::runtime_error("Deslocamento fora da faixa 0..63");
                PUSH(vm, make_int(req_int64(a, ">>") >> sh));
                VM_BREAK;
            }

            VM_CASE(OP_JUMP): ip = in.a; VM_BREAK;
            VM_CASE(OP_JUMP_IF_FALSE): {
                Value cond = POP(vm);
                if (!truthy_tol(cond)) ip = in.a; 
                VM_BREAK;
            }

            VM_CASE(OP_BUILD_ARRAY): {
                Value arr = v_matrix(in.a, in.b);
                for (int i = (in.a * in.b) - 1; i >= 0; i--) arr.arr()->elements[i] = POP(vm);
                PUSH(vm, std::move(arr)); VM_BREAK;
            }
            
            VM_CASE(OP_CALL_BUILTIN): {
                int count = in.b;
                if (count < 0) {
                    int fixed = -count - 1;
                    count = fixed + vm_expand_last_arg(vm, fixed, in.s.str().c_str());
                }
                if (count > 16)
                    throw std::runtime_error("Numero de argumentos invalido em " + in.s);
                int wanted = in.c == 0 ? 1 : in.c;
                // `in` points into the arena: a builtin (dofile/load) may
                // reallocate the vector and invalidate the reference. Saves what
                // will be used after the call BEFORE the handler.
                bool want_default = (in.c == 0);

                // E7/neg13: O(1) dispatch by id (a >= 0) — resolved at
                // COMPILE time, without a string hash per call. a < 0 (or
                // invalid id/divergent arity) falls back to the name path.
                if (in.a >= 0 && in.a < (int)g_builtins.size()) {
                    const Builtin& bi = g_builtins[in.a];
                    if (bi.arity == -1 || bi.arity == count) {
                        // Sandbox (E8/neg18): gate only evaluated if disabled
                        if (!g_allow_fileio && fileio_is_restricted(bi.name))
                            throw std::runtime_error(
                                "fileio desabilitado (allow_fileio=false): " +
                                bi.name);
                        ArgBuf ab;
                        for (int i = count - 1; i >= 0; i--)
                            ab.at(i, std::move(POP(vm)));
                        Value r;
                        bi.handler(ab.p(), count, &r);
                        REFRESH_CODE();
                        if (want_default && r.type != VAL_MULTI) {
                            PUSH(vm, std::move(r));
                            vm->lastRet = 1;
                        } else {
                            vm_push_builtin_result(vm, std::move(r), wanted);
                        }
                        VM_BREAK;
                    }
                }

                // Name path: `.mod` module function (item 16) or
                // arity error with the full message.
                std::string funcName = in.s;
                // Sandbox (E8/neg18): blocks calls to already-imported modules
                if (!g_allow_modules &&
                    (modules_is_callable(funcName, count) ||
                     modules_known_function(funcName)))
                    throw std::runtime_error(
                        "modulos desabilitados (allow_modules=false): " +
                        funcName);
                const Builtin* bi = builtin_lookup(funcName, count);
                if (!bi) {
                    // Fallback: `.mod` module function (item 16). The stack
                    // is only unwound on the path that actually calls.
                    if (modules_is_callable(funcName, count)) {
                        ArgBuf ab;
                        for (int i = count - 1; i >= 0; i--)
                            ab.at(i, std::move(POP(vm)));
                        Value r;
                        modules_call(funcName, ab.p(), count, &r);
                        REFRESH_CODE();
                        if (want_default && r.type != VAL_MULTI) {
                            PUSH(vm, std::move(r));
                            vm->lastRet = 1;
                        } else {
                            vm_push_builtin_result(vm, std::move(r), wanted);
                        }
                        VM_BREAK;
                    }
                    if (builtinRegistry.count(funcName))
                        throw std::runtime_error(funcName + ": aridade errada ("
                            + std::to_string(count) + " args; aceita: " +
                            builtin_arities(funcName) + ")");
                    if (modules_known_function(funcName))
                        throw std::runtime_error(funcName + ": aridade errada ("
                            + std::to_string(count) + " args; aceita: " +
                            std::to_string(modules_arity(funcName)) + ")");
                    throw std::runtime_error("Funcao builtin nao encontrada: " + funcName);
                }

                ArgBuf ab;
                for (int i = count - 1; i >= 0; i--)
                    ab.at(i, std::move(POP(vm)));
                
                // Sandbox (E8/neg18): fileio gate on the name path
                if (!g_allow_fileio && fileio_is_restricted(funcName))
                    throw std::runtime_error(
                        "fileio desabilitado (allow_fileio=false): " + funcName);
                Value r;
                bi->handler(ab.p(), count, &r);
                REFRESH_CODE();
                if (want_default && r.type != VAL_MULTI) {
                    PUSH(vm, std::move(r));
                    vm->lastRet = 1;
                } else {
                    vm_push_builtin_result(vm, std::move(r), wanted);
                }
                VM_BREAK;
            }            
                        
            // --- E17a: superopcodes (compiler fusion) ---------------
            // sid = sid OP top (the compiler only fuses when there are no
            // calls on the RHS, preserving evaluation order).
            VM_CASE(OP_ADD_STORE): {
                Symbol* sym = sym_get(in.a);
                if (!sym) throw std::runtime_error("Variavel nao encontrada no ADD_STORE");
                if (vm->sp <= 0) throw std::runtime_error("Stack vazia no ADD_STORE");
                Value& v = sym->value;
                Value& b = vm->stack[vm->sp - 1];
                if (v.type == VAL_FLOAT && b.type == VAL_FLOAT) {
                    v.f += b.f;
                    vm->sp -= 1;
                } else if (v.type == VAL_INT && b.type == VAL_INT) {
                    v.i = i64_wrap_add(v.i, b.i);
                    v.f = (double)v.i;
                    vm->sp -= 1;
                } else {
                    Value mm;
                    if (get_metamethod(v, "__add", &mm) ||
                        get_metamethod(b, "__add", &mm)) {
                        Value args[2] = {v, b};
                        Value r = vm_call_mm(vm, code, mm, args, 2, in.line);
                        REFRESH_CODE();
                        vm->sp -= 1;
                        v = std::move(r);
                    } else {
                        vm->sp -= 1;
                        v = do_add(v, b);
                    }
                }
                VM_BREAK;
            }
            VM_CASE(OP_SUB_STORE): {
                Symbol* sym = sym_get(in.a);
                if (!sym) throw std::runtime_error("Variavel nao encontrada no SUB_STORE");
                if (vm->sp <= 0) throw std::runtime_error("Stack vazia no SUB_STORE");
                Value& v = sym->value;
                Value& b = vm->stack[vm->sp - 1];
                if (v.type == VAL_FLOAT && b.type == VAL_FLOAT) {
                    v.f -= b.f;
                    vm->sp -= 1;
                } else if (v.type == VAL_INT && b.type == VAL_INT) {
                    v.i = i64_wrap_sub(v.i, b.i);
                    v.f = (double)v.i;
                    vm->sp -= 1;
                } else {
                    vm->sp -= 1;
                    v = do_sub(v, b);
                }
                VM_BREAK;
            }
            VM_CASE(OP_ADD_STORE_K): {
                Symbol* sym = sym_get(in.a);
                if (!sym) throw std::runtime_error("Variavel nao encontrada no ADD_STORE_K");
                Value& v = sym->value;
                if (v.type == VAL_FLOAT) {
                    v.f += in.n;
                } else if (v.type == VAL_INT && in.c) {
                    v.i = i64_wrap_add(v.i, (long long)in.n);
                    v.f = (double)v.i;
                } else {
                    Value mm;
                    if (get_metamethod(v, "__add", &mm)) {
                        Value args[2] = {v, k_value(in.n, in.c)};
                        Value r = vm_call_mm(vm, code, mm, args, 2, in.line);
                        REFRESH_CODE();
                        v = std::move(r);
                    } else {
                        v = do_add_num(v, in.n, in.c != 0);
                    }
                }
                VM_BREAK;
            }
            VM_CASE(OP_SUB_STORE_K): {
                Symbol* sym = sym_get(in.a);
                if (!sym) throw std::runtime_error("Variavel nao encontrada no SUB_STORE_K");
                Value& v = sym->value;
                if (v.type == VAL_FLOAT) v.f -= in.n;
                else if (v.type == VAL_INT && in.c) {
                    v.i = i64_wrap_sub(v.i, (long long)in.n);
                    v.f = (double)v.i;
                } else v = do_sub_num(v, in.n, in.c != 0);
                VM_BREAK;
            }

            // Loops: increments the counter and tests against the limit
            // (constant in n). a = body start (return jump).
            VM_CASE(OP_LT_LOOP): {
                Symbol* sym = sym_get(in.b);
                if (!sym) throw std::runtime_error("Variavel nao encontrada no LOOP");
                Value& v = sym->value;
                if (v.type == VAL_FLOAT) {
                    v.f += (double)in.c;
                    if (v.f < in.n) ip = in.a;
                } else if (v.type == VAL_INT) {
                    v.i = i64_wrap_add(v.i, (long long)in.c);
                    v.f = (double)v.i;
                    if (in.s.id == -2) {
                        if (v.i < (long long)in.n) ip = in.a;
                    } else if (cmp_lt(v, make_float(in.n), "<")) ip = in.a;
                } else {
                    v = do_add_num(v, (double)in.c, true);
                    if (cmp_lt(v, k_value_infer(in.n), "<")) ip = in.a;
                }
                VM_BREAK;
            }
            VM_CASE(OP_LE_LOOP): {
                Symbol* sym = sym_get(in.b);
                if (!sym) throw std::runtime_error("Variavel nao encontrada no LOOP");
                Value& v = sym->value;
                if (v.type == VAL_FLOAT) {
                    v.f += (double)in.c;
                    if (!(in.n < v.f)) ip = in.a;
                } else if (v.type == VAL_INT) {
                    v.i = i64_wrap_add(v.i, (long long)in.c);
                    v.f = (double)v.i;
                    if (in.s.id == -2) {
                        if (!((long long)in.n < v.i)) ip = in.a;
                    } else if (!cmp_lt(make_float(in.n), v, "<=")) ip = in.a;
                } else {
                    v = do_add_num(v, (double)in.c, true);
                    if (!cmp_lt(k_value_infer(in.n), v, "<=")) ip = in.a;
                }
                VM_BREAK;
            }
            VM_CASE(OP_GT_LOOP): {
                Symbol* sym = sym_get(in.b);
                if (!sym) throw std::runtime_error("Variavel nao encontrada no LOOP");
                Value& v = sym->value;
                if (v.type == VAL_FLOAT) {
                    v.f += (double)in.c;
                    if (in.n < v.f) ip = in.a;
                } else if (v.type == VAL_INT) {
                    v.i = i64_wrap_add(v.i, (long long)in.c);
                    v.f = (double)v.i;
                    if (in.s.id == -2) {
                        if ((long long)in.n < v.i) ip = in.a;
                    } else if (cmp_lt(make_float(in.n), v, ">")) ip = in.a;
                } else {
                    v = do_add_num(v, (double)in.c, true);
                    if (cmp_lt(k_value_infer(in.n), v, ">")) ip = in.a;
                }
                VM_BREAK;
            }
            VM_CASE(OP_GE_LOOP): {
                Symbol* sym = sym_get(in.b);
                if (!sym) throw std::runtime_error("Variavel nao encontrada no LOOP");
                Value& v = sym->value;
                if (v.type == VAL_FLOAT) {
                    v.f += (double)in.c;
                    if (!(v.f < in.n)) ip = in.a;
                } else if (v.type == VAL_INT) {
                    v.i = i64_wrap_add(v.i, (long long)in.c);
                    v.f = (double)v.i;
                    if (in.s.id == -2) {
                        if (!(v.i < (long long)in.n)) ip = in.a;
                    } else if (!cmp_lt(v, make_float(in.n), ">=")) ip = in.a;
                } else {
                    v = do_add_num(v, (double)in.c, true);
                    if (!cmp_lt(v, k_value_infer(in.n), ">=")) ip = in.a;
                }
                VM_BREAK;
            }
            // Increment + jump to the condition (any condition form)
            VM_CASE(OP_INC_JUMP): {
                Symbol* sym = sym_get(in.b);
                if (!sym) throw std::runtime_error("Variavel nao encontrada no INC_JUMP");
                Value& v = sym->value;
                if (v.type == VAL_FLOAT) v.f += in.n;
                else if (v.type == VAL_INT && in.c) {
                    v.i = i64_wrap_add(v.i, (long long)in.n);
                    v.f = (double)v.i;
                } else v = do_add_num(v, in.n, in.c != 0);
                ip = in.a;
                VM_BREAK;
            }

            // L12: variants for captured locals. The compiler swaps the
            // common opcodes for these only when the local is captured,
            // preserving the hot path of scripts without closures.
            VM_CASE(OP_LOAD_CELL): {
                Symbol* sym = sym_get(in.a);
                if (!sym) throw std::runtime_error("Variavel nao encontrada no LOAD_CELL");
                // Before the first capture the cell does not exist yet; a
                // local named function remains a VAL_FUNC value.
                if (!sym->cell && sym->is_function) {
                    Value fv;
                    fv.type = VAL_FUNC;
                    fv.obj = std::make_shared<FuncRef>(
                        FuncRef{sym->entry_point, sym->name, nullptr});
                    gc_register_obj(GC_OBJ_FUNC, fv.obj.p);
                    PUSH(vm, std::move(fv));
                    VM_BREAK;
                }
                PUSH(vm, *sym_slot(sym));
                VM_BREAK;
            }
            VM_CASE(OP_STORE_CELL): {
                Symbol* sym = sym_get(in.a);
                if (!sym) throw std::runtime_error("Variavel nao encontrada no STORE_CELL");
                if (vm->sp <= 0) throw std::runtime_error("Stack vazia no STORE_CELL");
                Value val = std::move(vm->stack[--vm->sp]);
                if (sym->scalar != SC_NONE)
                    val = value_coerce_scalar(sym->scalar, val);
                *sym_slot(sym) = std::move(val);
                VM_BREAK;
            }
            VM_CASE(OP_INC_CELL): {
                Symbol* sym = sym_get(in.a);
                if (!sym) throw std::runtime_error("Variavel nao encontrada no INC_CELL");
                Value* slot = sym_slot(sym);
                if (slot->type == VAL_FLOAT && sym->scalar == SC_NONE) {
                    slot->f += in.n;
                    VM_BREAK;
                }
                Value nv;
                Value mm;
                if (get_metamethod(*slot, "__add", &mm)) {
                    Value args[2] = {*slot, k_value(in.n, in.c)};
                    nv = vm_call_mm(vm, code, mm, args, 2, in.line);
                    REFRESH_CODE();
                } else {
                    nv = do_add(*slot, k_value(in.n, in.c));
                }
                if (sym->scalar != SC_NONE)
                    nv = value_coerce_scalar(sym->scalar, nv);
                *slot = std::move(nv);
                VM_BREAK;
            }
            VM_CASE(OP_ADD_STORE_CELL): {
                Symbol* sym = sym_get(in.a);
                if (!sym) throw std::runtime_error("Variavel nao encontrada no ADD_STORE_CELL");
                if (vm->sp <= 0) throw std::runtime_error("Stack vazia no ADD_STORE_CELL");
                Value& v = *sym_slot(sym);
                Value& b = vm->stack[vm->sp - 1];
                if (v.type == VAL_FLOAT && b.type == VAL_FLOAT) {
                    v.f += b.f;
                    vm->sp -= 1;
                } else {
                    Value mm;
                    if (get_metamethod(v, "__add", &mm) ||
                        get_metamethod(b, "__add", &mm)) {
                        Value args[2] = {v, b};
                        Value r = vm_call_mm(vm, code, mm, args, 2, in.line);
                        REFRESH_CODE();
                        vm->sp -= 1;
                        v = std::move(r);
                    } else {
                        vm->sp -= 1;
                        v = do_add(v, b);
                    }
                }
                VM_BREAK;
            }
            VM_CASE(OP_SUB_STORE_CELL): {
                Symbol* sym = sym_get(in.a);
                if (!sym) throw std::runtime_error("Variavel nao encontrada no SUB_STORE_CELL");
                if (vm->sp <= 0) throw std::runtime_error("Stack vazia no SUB_STORE_CELL");
                Value& v = *sym_slot(sym);
                Value& b = vm->stack[vm->sp - 1];
                if (v.type == VAL_FLOAT && b.type == VAL_FLOAT) {
                    v.f -= b.f;
                    vm->sp -= 1;
                } else {
                    vm->sp -= 1;
                    v = do_sub(v, b);
                }
                VM_BREAK;
            }
            VM_CASE(OP_ADD_STORE_K_CELL): {
                Symbol* sym = sym_get(in.a);
                if (!sym) throw std::runtime_error("Variavel nao encontrada no ADD_STORE_K_CELL");
                Value& v = *sym_slot(sym);
                if (v.type == VAL_FLOAT) {
                    v.f += in.n;
                } else {
                    Value mm;
                    if (get_metamethod(v, "__add", &mm)) {
                        Value args[2] = {v, k_value(in.n, in.c)};
                        Value r = vm_call_mm(vm, code, mm, args, 2, in.line);
                        REFRESH_CODE();
                        v = std::move(r);
                    } else {
                        v = do_add_num(v, in.n, in.c != 0);
                    }
                }
                VM_BREAK;
            }
            VM_CASE(OP_SUB_STORE_K_CELL): {
                Symbol* sym = sym_get(in.a);
                if (!sym) throw std::runtime_error("Variavel nao encontrada no SUB_STORE_K_CELL");
                Value& v = *sym_slot(sym);
                if (v.type == VAL_FLOAT) v.f -= in.n;
                else v = do_sub_num(v, in.n, in.c != 0);
                VM_BREAK;
            }
            VM_CASE(OP_LT_JIF_K_CELL): {
                Symbol* sym = sym_get(in.b);
                if (!sym) throw std::runtime_error("Variavel nao encontrada no JIF_CELL");
                const Value& v = *sym_slot(sym);
                if (v.type == VAL_FLOAT) { if (!(v.f < in.n)) ip = in.a; }
                else if (!cmp_lt(v, k_value(in.n, in.c), "<")) ip = in.a;
                VM_BREAK;
            }
            VM_CASE(OP_LE_JIF_K_CELL): {
                Symbol* sym = sym_get(in.b);
                if (!sym) throw std::runtime_error("Variavel nao encontrada no JIF_CELL");
                const Value& v = *sym_slot(sym);
                if (v.type == VAL_FLOAT) { if (in.n < v.f) ip = in.a; }
                else if (cmp_lt(k_value(in.n, in.c), v, "<=")) ip = in.a;
                VM_BREAK;
            }
            VM_CASE(OP_GT_JIF_K_CELL): {
                Symbol* sym = sym_get(in.b);
                if (!sym) throw std::runtime_error("Variavel nao encontrada no JIF_CELL");
                const Value& v = *sym_slot(sym);
                if (v.type == VAL_FLOAT) { if (!(in.n < v.f)) ip = in.a; }
                else if (!cmp_lt(k_value(in.n, in.c), v, ">")) ip = in.a;
                VM_BREAK;
            }
            VM_CASE(OP_GE_JIF_K_CELL): {
                Symbol* sym = sym_get(in.b);
                if (!sym) throw std::runtime_error("Variavel nao encontrada no JIF_CELL");
                const Value& v = *sym_slot(sym);
                if (v.type == VAL_FLOAT) { if (v.f < in.n) ip = in.a; }
                else if (cmp_lt(v, k_value(in.n, in.c), ">=")) ip = in.a;
                VM_BREAK;
            }
            VM_CASE(OP_EQ_JIF_K_CELL): {
                Symbol* sym = sym_get(in.b);
                if (!sym) throw std::runtime_error("Variavel nao encontrada no JIF_CELL");
                const Value& v = *sym_slot(sym);
                if (v.type == VAL_FLOAT) { if (v.f != in.n) ip = in.a; }
                else if (!value_eq(v, k_value(in.n, in.c))) ip = in.a;
                VM_BREAK;
            }
            VM_CASE(OP_NE_JIF_K_CELL): {
                Symbol* sym = sym_get(in.b);
                if (!sym) throw std::runtime_error("Variavel nao encontrada no JIF_CELL");
                const Value& v = *sym_slot(sym);
                if (v.type == VAL_FLOAT) { if (v.f == in.n) ip = in.a; }
                else if (value_eq(v, k_value(in.n, in.c))) ip = in.a;
                VM_BREAK;
            }
            VM_CASE(OP_LT_LOOP_CELL): {
                Symbol* sym = sym_get(in.b);
                if (!sym) throw std::runtime_error("Variavel nao encontrada no LOOP_CELL");
                Value& v = *sym_slot(sym);
                if (v.type == VAL_FLOAT) {
                    v.f += (double)in.c;
                    if (v.f < in.n) ip = in.a;
                } else {
                    v = do_add_num(v, (double)in.c, true);
                    if (cmp_lt(v, k_value_infer(in.n), "<")) ip = in.a;
                }
                VM_BREAK;
            }
            VM_CASE(OP_LE_LOOP_CELL): {
                Symbol* sym = sym_get(in.b);
                if (!sym) throw std::runtime_error("Variavel nao encontrada no LOOP_CELL");
                Value& v = *sym_slot(sym);
                if (v.type == VAL_FLOAT) {
                    v.f += (double)in.c;
                    if (!(in.n < v.f)) ip = in.a;
                } else {
                    v = do_add_num(v, (double)in.c, true);
                    if (!cmp_lt(k_value_infer(in.n), v, "<=")) ip = in.a;
                }
                VM_BREAK;
            }
            VM_CASE(OP_GT_LOOP_CELL): {
                Symbol* sym = sym_get(in.b);
                if (!sym) throw std::runtime_error("Variavel nao encontrada no LOOP_CELL");
                Value& v = *sym_slot(sym);
                if (v.type == VAL_FLOAT) {
                    v.f += (double)in.c;
                    if (in.n < v.f) ip = in.a;
                } else {
                    v = do_add_num(v, (double)in.c, true);
                    if (cmp_lt(k_value_infer(in.n), v, ">")) ip = in.a;
                }
                VM_BREAK;
            }
            VM_CASE(OP_GE_LOOP_CELL): {
                Symbol* sym = sym_get(in.b);
                if (!sym) throw std::runtime_error("Variavel nao encontrada no LOOP_CELL");
                Value& v = *sym_slot(sym);
                if (v.type == VAL_FLOAT) {
                    v.f += (double)in.c;
                    if (!(v.f < in.n)) ip = in.a;
                } else {
                    v = do_add_num(v, (double)in.c, true);
                    if (!cmp_lt(v, k_value_infer(in.n), ">=")) ip = in.a;
                }
                VM_BREAK;
            }
            VM_CASE(OP_INC_JUMP_CELL): {
                Symbol* sym = sym_get(in.b);
                if (!sym) throw std::runtime_error("Variavel nao encontrada no INC_JUMP_CELL");
                Value& v = *sym_slot(sym);
                if (v.type == VAL_FLOAT) v.f += in.n;
                else v = do_add_num(v, in.n, in.c != 0);
                ip = in.a;
                VM_BREAK;
            }

            VM_DEFAULT: throw std::runtime_error("OpCode desconhecido: " + std::to_string((int)in.op));
#ifndef ISILIEL_CGOTO
        }
#endif
        } catch (const std::exception& e) {
            // Appends the line of the failed instruction to the error message
            std::string msg = e.what();
            if (curLine > 0 && msg.find("linha ") == std::string::npos) {
                msg += " (linha " + std::to_string(curLine) + ")";
            }
            throw std::runtime_error(msg);
        }
#ifdef ISILIEL_CGOTO
        vm_continue: ;
#endif
    }
    #undef REFRESH_CODE
}