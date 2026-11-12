// Isiliel — scripting language interpreter/compiler.
// Embedded: sandbox, embedding API, OOP, 2D graphics and REPL.
// Authors: EEPS and MiMo-V2.6 | Version: 1.0 | Date: 2026-11-08

#ifndef DEFS_H
#define DEFS_H

#include <string>
#include <unordered_map>
#include <vector>
#include <memory>
#include <ostream>

enum ValueType { VAL_INT, VAL_FLOAT, VAL_STRING, VAL_ARRAY, VAL_BOOL, VAL_COMPLEX, VAL_STRUCT, VAL_FUNC, VAL_TABLE, VAL_COROUTINE, VAL_NIL, VAL_MULTI };

// L13: types that can participate in reference cycles. The collector
// records every object created and, at the end of the script, performs trial
// deletion over this graph (tables, structs, lists, functions/closures and environments).
enum GcObjKind {
    GC_OBJ_TABLE,
    GC_OBJ_STRUCT,
    GC_OBJ_ARRAY,
    GC_OBJ_FUNC,
    GC_OBJ_ENV,
    GC_OBJ_MULTI
};
void gc_register_obj(GcObjKind kind, const std::shared_ptr<void>& obj);

// E9/neg22: REPL growth limits (0 = no limit). Defined in
// vm.cpp and queried by the compiler/symbols during compilation.
extern long long g_code_limit;
extern long long g_symbol_limit;

// Reference to a script function as a first-class value (phase E2):
// copyable, comparable (same entry = equal), printable (<function name>).
// L12: `env` holds the captured cells from the scope where the anonymous
// function was created (closures). Top-level named functions use an empty env.
struct FrameEnv;
struct FuncRef {
    int entry = -1;   // GLOBAL bytecode address
    std::string name; // for traceback/printing ("anonymous" in `fn(...)`)
    std::shared_ptr<FrameEnv> env; // L12: captured environment (may be null)
};

struct Value;
struct StructInstance;
struct Table;
struct Coroutine;
struct MultiRet;

// E14: shared immutable string (16 B). Previously `Value` embedded a
// std::string (32 B) copied on every PUSH/POP; now the text is shared
// by pointer (1 atomic inc on copy, 0 allocations for numeric values).
// The interface mimics std::string in existing uses (size/empty/c_str/...).
struct Str {
    std::shared_ptr<const std::string> p;
    Str() = default;
    Str(const std::string& x) : p(std::make_shared<const std::string>(x)) {}
    Str(std::string&& x) : p(std::make_shared<const std::string>(std::move(x))) {}
    Str(const char* x) : p(std::make_shared<const std::string>(x)) {}
    Str& operator=(const std::string& x) {
        p = std::make_shared<const std::string>(x); return *this;
    }
    Str& operator=(const char* x) {
        p = std::make_shared<const std::string>(x); return *this;
    }
    Str& assign(const char* x, size_t n) {
        p = std::make_shared<const std::string>(x, n); return *this;
    }
    void clear() { p.reset(); }
    const std::string& str() const {
        static const std::string empty;
        return p ? *p : empty;
    }
    operator const std::string&() const { return str(); }
    const std::string& operator*() const { return str(); }
    const std::string* operator->() const { return &str(); }
    size_t size() const { return str().size(); }
    bool empty() const { return str().empty(); }
    const char* c_str() const { return str().c_str(); }
    const char* data() const { return str().data(); }
    char back() const { return str().back(); }
    char operator[](size_t i) const { return str()[i]; }
    std::string substr(size_t pos, size_t n = std::string::npos) const {
        return str().substr(pos, n);
    }
    size_t find(const std::string& x, size_t pos = 0) const { return str().find(x, pos); }
    size_t find(const char* x, size_t pos = 0) const { return str().find(x, pos); }
};

inline std::string operator+(const Str& a, const Str& b) { return a.str() + b.str(); }
inline std::string operator+(const Str& a, const std::string& b) { return a.str() + b; }
inline std::string operator+(const std::string& a, const Str& b) { return a + b.str(); }
inline std::string operator+(const Str& a, const char* b) { return a.str() + b; }
inline std::string operator+(const char* a, const Str& b) { return a + b.str(); }
inline bool operator==(const Str& a, const Str& b) { return a.str() == b.str(); }
inline bool operator==(const Str& a, const std::string& b) { return a.str() == b; }
inline bool operator==(const std::string& a, const Str& b) { return a == b.str(); }
inline bool operator==(const Str& a, const char* b) { return a.str() == b; }
inline bool operator!=(const Str& a, const Str& b) { return !(a == b); }
inline bool operator!=(const Str& a, const char* b) { return !(a == b); }
inline bool operator<(const Str& a, const Str& b) { return a.str() < b.str(); }
inline bool operator<(const Str& a, const std::string& b) { return a.str() < b; }
inline bool operator<(const std::string& a, const Str& b) { return a < b.str(); }
inline std::ostream& operator<<(std::ostream& os, const Str& a) { return os << a.str(); }

// E14: type-erased reference (16 B) for types with identity (array,
// struct, function, table, coroutine). A single shared_ptr<void> preserves the
// original deleter; typed accessors live in Value (arr()/st()/...).
struct RefObj {
    std::shared_ptr<void> p;
    RefObj() = default;
    template <class T>
    RefObj(std::shared_ptr<T> q) : p(std::move(q)) {}
    template <class T>
    RefObj& operator=(std::shared_ptr<T> q) { p = std::move(q); return *this; }
    void* get() const { return p.get(); }
    explicit operator bool() const { return (bool)p; }
};

struct ArrayData {
    std::vector<Value> elements;
    int rows = 0;
    int cols = 0;
};

// E14: lean Value (56 B, was ~136 B): numbers no longer carry
// std::string nor 4 shared_ptr; strings are shared (Str) and types
// with identity live in a single type-erased pointer (RefObj).
struct Value {
    ValueType type = VAL_FLOAT;
    double f = 0.0;      // real part (or numeric value) — double precision
    // L7: VAL_INT stores the native integer in `i`; VAL_COMPLEX uses `im`.
    // Both fields share the same 8 bytes (Value remains 56 B).
    union { double im = 0.0; long long i; };
    Str s;               // only VAL_STRING (empty for other types)
    RefObj obj;          // arr/st/fn/tb/co (only the type indicated by `type`)

    Value() = default;
    Value(ValueType t, double fv = 0.0) : type(t), f(fv) {}
    Value(ValueType t, double fv, const std::string& sv) : type(t), f(fv), s(sv) {}
    Value(ValueType t, double fv, std::string&& sv) : type(t), f(fv), s(std::move(sv)) {}

    ArrayData* arr() const;
    StructInstance* st() const;
    FuncRef* fn() const;
    Table* tb() const;
    Coroutine* co() const;
    MultiRet* mul() const;
};

// L5: multiple return from builtins (`select`, `unpack`). Never escapes to
// variables: OP_CALL_BUILTIN unpacks according to the number of requested values.
struct MultiRet {
    std::vector<Value> values;
};

// L12: per-frame environment for closures. Each captured local becomes a
// shared cell (shared_ptr<Value>); `parent` chains the outer
// lexical scopes. The environment is only created when a `fn(...)` is evaluated.
struct FrameEnv {
    std::unordered_map<std::string, std::shared_ptr<Value>> vars;
    std::shared_ptr<FrameEnv> parent;
};

// L12: enabled at compile time when emitting OP_PUSH_CLOSURE. Programs
// without closures do not keep per-frame environment stacks (the hot call
// path, e.g. recursive fib, stays cost-free).
inline bool g_program_has_closures = false;

// Table/dictionary (phase E3): text key -> Value. shared_ptr (reference,
// like array/struct); printing and keys() sorted by key (determinism).
struct Table {
    std::unordered_map<std::string, Value> data;
    // L11: metatable (nil = no metatable). shared_ptr preserves identity
    // and keeps the method table alive for as long as the object exists.
    std::shared_ptr<Table> metatable;
    bool gc_registered = false;
    bool gc_finalized = false;
};

// Struct instance (`type Name { ... };`): values in declared field order
// (names live in the type registry, symbols.h)
struct StructInstance {
    std::string type_name;
    std::vector<Value> fields;
    std::shared_ptr<Table> metatable; // L11
    bool gc_registered = false;
    bool gc_finalized = false;
};

// E14: typed accessors for the type-erased pointer (types already complete here;
// co() lives in vm.h, after the Coroutine definition).
inline ArrayData* Value::arr() const { return static_cast<ArrayData*>(obj.get()); }
inline StructInstance* Value::st() const { return static_cast<StructInstance*>(obj.get()); }
inline FuncRef* Value::fn() const { return static_cast<FuncRef*>(obj.get()); }
inline Table* Value::tb() const { return static_cast<Table*>(obj.get()); }
inline MultiRet* Value::mul() const { return static_cast<MultiRet*>(obj.get()); }


enum TokenType {
    TK_ID, 
    TK_NUM, 
    TK_STRING, 
    TK_EOF,
    TK_LPAREN, 
    TK_RPAREN, 
    TK_LBRACE, 
    TK_RBRACE, 
    TK_LBRACKET, 
    TK_RBRACKET,
    TK_COMMA, 
    TK_PUNCT,
    TK_COLON,
    TK_ASSIGN, 
    TK_PLUS, 
    TK_MINUS, 
    TK_MUL, 
    TK_DIV, 
    TK_MOD,
    TK_PLUS_ASSIGN, 
    TK_MIN_ASSIGN, 
    TK_MUL_ASSIGN, 
    TK_DIV_ASSIGN,
    TK_EQ, 
    TK_NE, 
    TK_LT, 
    TK_GT, 
    TK_LE, 
    TK_GE,
    TK_AND, 
    TK_OR,
    TK_BIT_AND, 
    TK_BIT_OR, 
    TK_BIT_XOR, 
    TK_BIT_NOT, 
    TK_SHL, 
    TK_SHR,
    TK_IF, 
    TK_ELSE, 
    TK_WHILE, 
    TK_FOR,
    TK_VOID, 
    TK_INT, 
    TK_FLOAT,
    TK_IDENTIFIER,
    TK_RETURN, // More token types
    TK_BREAK,
    TK_CONTINUE,
    TK_INCLUDE,
    TK_IMPORT, // import "file.mod"; — native module (.mod) via dlopen (item 16)
    TK_TYPE,
    TK_DOT,
    TK_ELLIPSIS, // ... (L5 varargs)
    TK_GLOBAL, // global a, b; — variables shared with callbacks (draw)
    TK_THREAD, // thread f() {...} — function running on its own thread (item 15)
    TK_START,  // start f();  — starts (or resumes if paused)
    TK_STOP,   // stop f();   — stops permanently (join)
    TK_PAUSE,  // pause f();  — pauses at the next checkpoint
    TK_RESUME, // resume f(); — resumes a paused thread
    TK_FN,     // fn(x) { ... } — anonymous function without capture (phase E2)
    TK_CLASS,  // class Name { ... } — class (struct with inheritance, E4)
    TK_EXTENDS,// class B extends A { ... }
    TK_NEW,    // new T(args) — instance with init (E4)
    TK_SUPER,  // super.m(args) — parent class method (E4)
    TK_ELSEIF, // elseif (E6) — if/elseif/else chain in a single keyword
    TK_REPEAT, TK_UNTIL, // L10: repeat { ... } until (cond);
    TK_GOTO,   // L10: goto label;
    TK_DCOLON, // L10: ::label::
    TK_NIL     // L6: literal nil
};

struct Token {
    TokenType type;
    std::string lexeme;
    int line = 1; // Source line in the source file (used in errors)
};

enum OpCode {
    OP_HALT,           // 0
    OP_PUSH,           // 1
    OP_PUSH_STR,       // 2
    OP_POP,            // 3
    OP_ADD,            // 4
    OP_SUB,            // 5
    OP_MUL,            // 6
    OP_DIV,            // 7
    OP_MOD,            // 8
    OP_EQ,             // 9
    OP_NE,             // 10
    OP_LT,             // 11
    OP_GT,             // 12
    OP_LE,             // 13
    OP_GE,             // 14
    OP_AND,            // 15
    OP_OR,             // 16
    OP_BIT_AND,        // 17
    OP_BIT_OR,         // 18
    OP_BIT_XOR,        // 19
    OP_BIT_NOT,        // 20
    OP_SHL,            // 21
    OP_SHR,            // 22
    OP_LOAD,           // 23
    OP_STORE,          // 24
    OP_JUMP,           // 25
    OP_JUMP_IF_FALSE,  // 26
    OP_CALL,           // 27 <-- matches case 27
    OP_RETURN,         // 28 <-- matches case 28
    OP_CALL_BUILTIN,   // 29
    OP_BUILD_ARRAY,    // 30
    OP_INDEX_LOAD,     // 31
    OP_INDEX_STORE,    // 32
    OP_INDEX_SLICE,    // 33: slicing v[a:b] (a/b may be omitted)
    OP_BUILD_STRUCT,   // 34: literal {..} -> instance (s = type name)
    OP_FIELD_LOAD,     // 35: obj.field (s = field name)
    OP_FIELD_STORE,    // 36: obj.field = value (s = field name)
    OP_CALL_METHOD,    // 37: obj.method(args) (s = method, b = argCount)
    OP_PUSH_FUNC,      // 38: pushes function value (a = global entry, s = name)
    OP_CALL_INDIRECT,  // 39: calls function value from the stack (a = argCount)
    OP_CALL_PCALL,     // 40: protected call pcall (a = argCount) — E2
    OP_BUILD_TABLE,    // 41: literal {key = value, ...} (a = n pairs) — E3
    OP_NEW_INSTANCE,   // 42: new instance with zeroed fields (s = type) — E4
    OP_DUP,            // 43: duplicates top of stack (new: keeps instance) — E4
    OP_CALL_SUPER,     // 44: super.m(args) (a = parent entry, b = nArgs) — E4
    // --- E15: opcode fusion/specialization -----------------------------
    // _K: right operand is a pre-parsed numeric constant (n).
    // Reduces 2 dispatches (PUSH + op) to 1 and one Value copy.
    OP_ADD_K, OP_SUB_K, OP_MUL_K, OP_DIV_K, OP_MOD_K,
    OP_EQ_K, OP_NE_K, OP_LT_K, OP_LE_K, OP_GT_K, OP_GE_K,
    OP_INC_LOCAL,      // a = local sid, n = delta (x = x + k) — no stack
    // CMP+JUMP_IF_FALSE fused over a local: a = target, b = sid, n = const
    OP_LT_JIF_K, OP_LE_JIF_K, OP_GT_JIF_K, OP_GE_JIF_K, OP_EQ_JIF_K, OP_NE_JIF_K,
    // E17a: superopcodes (compiler fusion)
    OP_ADD_STORE,      // a=sid: sid = sid + top (top consumed)
    OP_SUB_STORE,      // a=sid: sid = sid - top
    OP_ADD_STORE_K,    // a=sid, n=k: sid = sid + k
    OP_SUB_STORE_K,    // a=sid, n=k: sid = sid - k
    OP_LT_LOOP,        // a=body, b=sid, c=incr, n=limit: sid+=incr; if sid<limit -> body
    OP_LE_LOOP,        // same, <=
    OP_GT_LOOP,        // same, >
    OP_GE_LOOP,        // same, >=
    OP_INC_JUMP,       // a=target, b=sid, n=incr: sid+=incr; ip=target
    OP_CALL_DRAW,      // L1: calls the setDraw callback (a=fallback entry)
    OP_TAILCALL,       // L10: return f(args) reuses the current frame (a=entry, b=nargs)
    OP_PUSH_NIL,       // L6: pushes the nil value
    OP_RETURN_N,       // L5: return with N values (a = N; -1 = dynamic)
    OP_COLLECT_VARARGS,// L5: vararg prologue (a = nparams, b = __varargs sid)
    OP_RETURN_SPREAD,  // L5: return ... (array on top becomes N values)
    OP_PACK_LAST,      // L5: packs lastRet values from the top into a list (args)
    OP_VARARG_FIRST,   // L5: pushes the 1st vararg (or nil) — expression use
    OP_BIND_PARAMS,    // L5: normal prologue (a=nparams): trims extras/fills nil
    OP_PUSH_CLOSURE,   // L12: pushes closure (a=entry, b=owner for capture, s=name)
    OP_LOAD_FREE,      // L12: reads upvalue by name in the captured environment
    OP_STORE_FREE,     // L12: writes upvalue by name in the captured environment
    // L12: variants for captured locals (direct cell access, without
    // checks in the common opcodes). The compiler swaps the opcodes at the end.
    OP_LOAD_CELL, OP_STORE_CELL, OP_INC_CELL,
    OP_ADD_STORE_CELL, OP_SUB_STORE_CELL,
    OP_ADD_STORE_K_CELL, OP_SUB_STORE_K_CELL,
    OP_LT_JIF_K_CELL, OP_LE_JIF_K_CELL, OP_GT_JIF_K_CELL,
    OP_GE_JIF_K_CELL, OP_EQ_JIF_K_CELL, OP_NE_JIF_K_CELL,
    OP_LT_LOOP_CELL, OP_LE_LOOP_CELL, OP_GT_LOOP_CELL, OP_GE_LOOP_CELL,
    OP_INC_JUMP_CELL,
    OP_COUNT           // sentinel (not an instruction)
};

// E7/neg15: opcode names for the disassembler (--disasm) and debug dumps
inline const char* op_name(OpCode op) {
    static const char* names[] = {
        "HALT", "PUSH", "PUSH_STR", "POP", "ADD", "SUB", "MUL", "DIV",
        "MOD", "EQ", "NE", "LT", "GT", "LE", "GE", "AND", "OR", "BIT_AND",
        "BIT_OR", "BIT_XOR", "BIT_NOT", "SHL", "SHR", "LOAD", "STORE",
        "JUMP", "JUMP_IF_FALSE", "CALL", "RETURN", "CALL_BUILTIN",
        "BUILD_ARRAY", "INDEX_LOAD", "INDEX_STORE", "INDEX_SLICE",
        "BUILD_STRUCT", "FIELD_LOAD", "FIELD_STORE", "CALL_METHOD",
        "PUSH_FUNC", "CALL_INDIRECT", "CALL_PCALL", "BUILD_TABLE",
        "NEW_INSTANCE", "DUP", "CALL_SUPER",
        "ADD_K", "SUB_K", "MUL_K", "DIV_K", "MOD_K",
        "EQ_K", "NE_K", "LT_K", "LE_K", "GT_K", "GE_K",
        "INC_LOCAL", "LT_JIF_K", "LE_JIF_K", "GT_JIF_K", "GE_JIF_K",
        "EQ_JIF_K", "NE_JIF_K",
        "ADD_STORE", "SUB_STORE", "ADD_STORE_K", "SUB_STORE_K",
        "LT_LOOP", "LE_LOOP", "GT_LOOP", "GE_LOOP", "INC_JUMP", "CALL_DRAW",
        "TAILCALL", "PUSH_NIL", "RETURN_N", "COLLECT_VARARGS",
        "RETURN_SPREAD", "PACK_LAST", "VARARG_FIRST", "BIND_PARAMS",
        "PUSH_CLOSURE", "LOAD_FREE", "STORE_FREE",
        "LOAD_CELL", "STORE_CELL", "INC_CELL",
        "ADD_STORE_CELL", "SUB_STORE_CELL", "ADD_STORE_K_CELL",
        "SUB_STORE_K_CELL", "LT_JIF_K_CELL", "LE_JIF_K_CELL",
        "GT_JIF_K_CELL", "GE_JIF_K_CELL", "EQ_JIF_K_CELL", "NE_JIF_K_CELL",
        "LT_LOOP_CELL", "LE_LOOP_CELL", "GT_LOOP_CELL", "GE_LOOP_CELL",
        "INC_JUMP_CELL", "COUNT"
    };
    int i = (int)op;
    if (i < 0 || i >= (int)(sizeof(names) / sizeof(names[0]))) return "?";
    return names[i];
}

// E16f: global string pool (literals + instruction names). The handle
// below stores only the index (4 B), keeping the `.s` syntax of existing
// uses through implicit conversion. Instruction shrinks from 64 B to 32 B
// (2 per cache line; measured ~10% on the dispatch loop).
int str_intern(const std::string& s);
const Str& str_pool_get(int id);

struct InsStr {
    int id = -1;
    InsStr() = default;
    InsStr(const std::string& x) : id(x.empty() ? -1 : str_intern(x)) {}
    InsStr(const char* x) : id((!x || !*x) ? -1 : str_intern(x)) {}
    InsStr& operator=(const std::string& x) {
        id = x.empty() ? -1 : str_intern(x); return *this;
    }
    InsStr& operator=(const char* x) {
        id = (!x || !*x) ? -1 : str_intern(x); return *this;
    }
    bool empty() const { return id < 0; }
    const std::string& str() const {
        static const std::string emptyStr;
        return id < 0 ? emptyStr : str_pool_get(id).str();
    }
    operator const std::string&() const { return str(); }
};

struct Instruction {
    OpCode op = OP_HALT;
    int a = 0;
    int b = 0;
    int line = 0; // Source line (runtime errors report this line)
    int c = 0;    // E17a: third integer operand (secondary sid / increment)
    InsStr s;     // E16f: index into the string pool (4 B)
    // E10: numeric constant pre-parsed at compile time (OP_PUSH).
    // Eliminates std::stod per execution. b==1 marks an imaginary literal (3j):
    // n stores the imaginary part.
    double n = 0.0;
};

// L14: bytecode storage. `FuncRef.entry` is an index (not a
// pointer), so `load`/`dofile` can append chunks at runtime; the VM
// re-fetches `(*code)[ip]` on every instruction and the catch block uses the copied line
// (`curLine`), never a reference to `Instruction` that could be invalidated
// by vector reallocation during a builtin.
using CodeStore = std::vector<Instruction>;

#endif