// Isiliel — scripting language interpreter/compiler.
// Embedded: sandbox, embedding API, OOP, 2D graphics and REPL.
// Authors: Eben (AI-assisted) | Version: 1.0 | Date: 2026-11-08

#include "symbols.h"
#include <iostream>
#include <vector>
#include <iomanip>
#include <cmath>
#include <stdexcept>
#include <unordered_map>

static std::vector<Symbol> symbolTable;
static std::vector<StructType> structRegistry;
// E12: owner index -> ids of the locals. Avoids scanning the whole table in
// sym_save_scope/sym_find_local (OP_CALL hot path). Kept in sync
// in sym_create_local and cleared in sym_clear.
static std::unordered_map<int, std::vector<int>> ownerIndex;
// E19: single-entry cache for the OP_CALL hot path. Recursion (fib)
// repeats the same owner; avoids the find() on the unordered_map on every call.
// References to unordered_map elements are stable (rehash only invalidates
// iterators), so the pointer to the vector<int> remains valid while
// the entry exists; sym_clear() invalidates the cache.
static int lastOwner = -2;
static const std::vector<int>* lastLocals = nullptr;

// --- Optional scalar types (phase E1c) ---
const char* scalar_type_name(ScalarType sc) {
    switch (sc) {
        case SC_I8: return "i8";     case SC_I16: return "i16";
        case SC_I32: return "i32";   case SC_I64: return "i64";
        case SC_ISIZE: return "isize";
        case SC_U8: return "u8";     case SC_U16: return "u16";
        case SC_U32: return "u32";   case SC_U64: return "u64";
        case SC_USIZE: return "usize";
        case SC_F32: return "f32";   case SC_F64: return "f64";
        case SC_BOOL: return "bool"; case SC_STR: return "str";
        default: return "?";
    }
}

ScalarType scalar_type_from_name(const std::string& name) {
    static const std::map<std::string, ScalarType> m = {
        {"i8", SC_I8}, {"i16", SC_I16}, {"i32", SC_I32}, {"i64", SC_I64},
        {"isize", SC_ISIZE},
        {"u8", SC_U8}, {"u16", SC_U16}, {"u32", SC_U32}, {"u64", SC_U64},
        {"usize", SC_USIZE},
        {"f32", SC_F32}, {"f64", SC_F64}, {"bool", SC_BOOL},
        {"str", SC_STR}, {"String", SC_STR},
        {"int", SC_I32}, {"float", SC_F64}   // C-style aliases
    };
    auto it = m.find(name);
    return it == m.end() ? SC_NONE : it->second;
}

// Coercion applied on EVERY write to a symbol with scalar != SC_NONE.
// Rules (see docs/linguagem.md): integers truncate toward zero and range-check;
// f32 rounds to float32 precision; bool normalizes to 0/1; str requires text.
Value value_coerce_scalar(ScalarType sc, const Value& v) {
    if (sc == SC_NONE) return v;
    const std::string tn = scalar_type_name(sc);

    if (sc == SC_STR) {
        if (v.type != VAL_STRING)
            throw std::runtime_error("Espera uma string para '" + tn + "'");
        return v;
    }
    // Everything else is numeric (text/matrix/struct/complex/function do not coerce)
    if (v.type == VAL_STRING || v.type == VAL_ARRAY || v.type == VAL_STRUCT ||
        v.type == VAL_COMPLEX || v.type == VAL_FUNC || v.type == VAL_TABLE ||
        v.type == VAL_COROUTINE)
        throw std::runtime_error("Espera um numero para '" + tn + "'");

    // L7: i64/isize (and non-negative u64/usize) preserve the exact native
    // integer when the source is already VAL_INT.
    if (v.type == VAL_INT &&
        (sc == SC_I64 || sc == SC_ISIZE ||
         ((sc == SC_U64 || sc == SC_USIZE) && v.i >= 0))) {
        Value r = v;
        r.type = VAL_INT;
        r.i = v.i;
        r.f = (double)v.i;
        return r;
    }

    double d = v.f;
    Value r = v;
    r.type = VAL_FLOAT;

    if (sc == SC_BOOL) { r.f = (d != 0.0) ? 1.0 : 0.0; return r; }
    if (sc == SC_F64)  { r.f = d; return r; }
    if (sc == SC_F32)  { r.f = (double)(float)d; return r; } // rounds to float32

    // Integers: truncate toward zero (C/Rust `as` style) and range-check
    double t = std::trunc(d);
    double lo = 0.0, hi = 0.0;
    bool hi_excl = false;
    switch (sc) {
        case SC_I8:  lo = -128;           hi = 127;            break;
        case SC_I16: lo = -32768;         hi = 32767;          break;
        case SC_I32: lo = -2147483648.0;  hi = 2147483647.0;   break;
        case SC_I64: case SC_ISIZE: {
            static const double LIM = 9223372036854775808.0;  // 2^63
            lo = -LIM; hi = LIM; hi_excl = true;              // exclusive: 2^63 does not fit in int64
            break;
        }
        case SC_U8:  lo = 0; hi = 255;        break;
        case SC_U16: lo = 0; hi = 65535;      break;
        case SC_U32: lo = 0; hi = 4294967295.0; break;
        case SC_U64: case SC_USIZE: {
            static const double LIM = 18446744073709551616.0; // 2^64
            lo = 0; hi = LIM; hi_excl = true;
            break;
        }
        default: return v;
    }
    bool ok = (t >= lo) && (hi_excl ? (t < hi) : (t <= hi));
    if (!ok) throw std::runtime_error("Valor fora da faixa para '" + tn + "'");
    r.f = t;
    return r;
}

void sym_clear() {
    symbolTable.clear();
    ownerIndex.clear();
    struct_clear();
    lastOwner = -2;
    lastLocals = nullptr;
}

// --- Type registry (structs) ---
StructType* struct_find(const std::string& name) {
    for (auto& t : structRegistry)
        if (t.name == name) return &t;
    return nullptr;
}

StructType* struct_create(const std::string& name) {
    StructType* t = struct_find(name);
    if (t) { // redefinition (REPL): clears fields/methods/parent and rebuilds
        t->fields.clear();
        t->methods.clear();
        t->parent.clear();
        return t;
    }
    structRegistry.push_back({name, {}, {}, ""});
    return &structRegistry.back();
}

bool struct_add_field(const std::string& type, const std::string& field) {
    StructType* t = struct_find(type);
    if (!t) return false;
    if (struct_field_index(type, field) != -1) return false; // own or inherited
    t->fields.push_back(field);
    return true;
}

bool struct_add_method(const std::string& type, const std::string& method, int sid) {
    StructType* t = struct_find(type);
    if (!t) return false;
    if (t->methods.count(method)) return false; // duplicate method
    t->methods[method] = sid;
    return true;
}

// Type chain root -> leaf (for flattened field indices)
static std::vector<const StructType*> struct_chain(const std::string& type) {
    std::vector<const StructType*> chain;
    std::string cur = type;
    int guard = 0;
    while (!cur.empty() && guard++ < 64) {
        const StructType* t = struct_find(cur);
        if (!t) break;
        chain.push_back(t);   // [leaf, parent, ... root]
        cur = t->parent;
    }
    return chain;
}

int struct_field_index(const std::string& type, const std::string& field) {
    auto chain = struct_chain(type);
    int idx = 0; // flattened layout: root fields first (E4)
    for (int i = (int)chain.size() - 1; i >= 0; i--)
        for (const auto& f : chain[i]->fields) {
            if (f == field) return idx;
            idx++;
        }
    return -1;
}

int struct_total_fields(const std::string& type) {
    auto chain = struct_chain(type);
    int n = 0;
    for (const StructType* t : chain) n += (int)t->fields.size();
    return n;
}

int struct_set_parent(const std::string& type, const std::string& parent) {
    StructType* t = struct_find(type);
    StructType* p = struct_find(parent);
    if (!t || !p || type == parent) return -1;
    // circular inheritance: walk up from the parent root until `type` is found
    std::string cur = p->parent;
    int guard = 0;
    while (!cur.empty() && guard++ < 64) {
        if (cur == type) return -1;
        StructType* pt = struct_find(cur);
        if (!pt) break;
        cur = pt->parent;
    }
    t->parent = parent;
    return 0;
}

int struct_method_sid(const std::string& type, const std::string& method) {
    auto chain = struct_chain(type); // resolves in the hierarchy (parent -> grandparent)
    for (const StructType* t : chain) {
        auto it = t->methods.find(method);
        if (it != t->methods.end()) return it->second;
    }
    return -1;
}

void struct_clear() { structRegistry.clear(); }

int sym_find(const char* name) {
    for (int i = 0; i < (int)symbolTable.size(); i++) {
        if (symbolTable[i].owner == -1 && symbolTable[i].name == name) return i;
    }
    return -1;
}

int sym_find_local(const char* name, int owner) {
    if (owner == -1) return -1;
    auto it = ownerIndex.find(owner);
    if (it != ownerIndex.end()) {
        for (int i : it->second)
            if (symbolTable[i].name == name) return i;
    }
    return -1;
}

// E9/neg22: the REPL limits the total number of symbols so it does not grow forever.
static void sym_check_limit() {
    if (g_symbol_limit > 0 &&
        (long long)symbolTable.size() >= g_symbol_limit)
        throw std::runtime_error(
            "limite de simbolos do REPL atingido (" +
            std::to_string(g_symbol_limit) +
            "; ajuste MYINTERP_REPL_SYMBOLS_LIMIT)");
}

int sym_create(const char* name, Value val) {
    int id = sym_find(name);
    if (id != -1) return id;
    sym_check_limit();
    symbolTable.push_back(Symbol{name, val, false, -1, -1, {}, SC_NONE, false, false, nullptr, nullptr});
    return (int)symbolTable.size() - 1;
}

int sym_create_local(const char* name, Value val, int owner) {
    int id = sym_find_local(name, owner);
    if (id != -1) return id;
    sym_check_limit();
    symbolTable.push_back(Symbol{name, val, false, -1, owner, {}, SC_NONE, false, false, nullptr, nullptr});
    id = (int)symbolTable.size() - 1;
    ownerIndex[owner].push_back(id);
    return id;
}

int sym_create_func(const char* name, int entry_point) {
    int id = sym_find(name);
    if (id != -1) {
        symbolTable[id].is_function = true;
        symbolTable[id].entry_point = entry_point;
        return id;
    }
    sym_check_limit();
    symbolTable.push_back(Symbol{name, {VAL_INT}, true, entry_point, -1, {}, SC_NONE, false, false, nullptr, nullptr});
    return (int)symbolTable.size() - 1;
}

// Saves the values of `owner`'s locals and clears them (clean initial state)
void sym_save_scope(int owner, std::vector<SavedLocal>& saved) {
    // E12: iterates only `owner`'s locals (O(1) index), not the whole table.
    // E19: single-entry cache — recursive calls repeat the same owner.
    const std::vector<int>* locals;
    if (owner == lastOwner) {
        locals = lastLocals;
    } else {
        auto it = ownerIndex.find(owner);
        lastOwner = owner;
        lastLocals = (it != ownerIndex.end()) ? &it->second : nullptr;
        locals = lastLocals;
    }
    if (!locals) return;
    saved.reserve(saved.size() + locals->size());
    for (int i : *locals) {
        Symbol& s = symbolTable[i];
        if (g_program_has_closures) {
            // L12: the previous frame's cell is preserved for the
            // closures; the new frame starts without a cell (created on capture).
            saved.push_back({i, s.value, s.cell});
            s.cell.reset();
        } else {
            // Hot path without closures: does not touch shared_ptr.
            saved.push_back({i, s.value, {}});
        }
        s.value = Value{VAL_FLOAT, 0.0f};
    }
}

// Restores the saved values starting at index `from`
void sym_restore_scope(const std::vector<SavedLocal>& saved, size_t from) {
    for (size_t i = from; i < saved.size(); i++) {
        Symbol* s = sym_get(saved[i].sid);
        if (s) {
            s->value = saved[i].value;
            if (g_program_has_closures) s->cell = saved[i].cell;
        }
    }
}

// L12: turns `owner`'s locals into shared cells and registers them
// in the environment. If the cell already exists (another closure of the same frame),
// it is reused — two closures share the same variable.
void sym_capture_owner(int owner, FrameEnv& env) {
    if (owner < 0) return;
    auto it = ownerIndex.find(owner);
    if (it == ownerIndex.end()) return;
    for (int i : it->second) {
        Symbol& s = symbolTable[i];
        if (!s.cell) {
            Value v = s.value;
            // Local named function used as a value by a closure: the
            // cell must hold the FuncRef, not the default value.
            if (s.is_function && s.entry_point >= 0) {
                v = Value{};
                v.type = VAL_FUNC;
                v.obj = std::make_shared<FuncRef>(
                    FuncRef{s.entry_point, s.name, nullptr});
                gc_register_obj(GC_OBJ_FUNC, v.obj.p);
            }
            s.cell = std::make_shared<Value>(std::move(v));
        }
        env.vars[s.name] = s.cell;
    }
}

Symbol* sym_get(int id) {
    if (id < 0 || id >= (int)symbolTable.size()) return nullptr;
    return &symbolTable[id];
}

void sym_dump() {
    std::cout << "\n=== DEBUG: TABELA DE SIMBOLOS ===\n";
    std::cout << std::left << std::setw(15) << "NOME" << " | " << "TIPO"
              << " | " << "OWNER" << " | " << "ENTRY\n";
    for (const auto& sym : symbolTable) {
        std::cout << std::left << std::setw(15) << sym.name << " | "
                  << (sym.is_function ? "FUNC" : "VAR") << " | "
                  << sym.owner << " | " << sym.entry_point << "\n";
    }
}

// --- E16e: string literal pool ---------------------------------------------
static std::vector<Str> g_strPool;
static std::unordered_map<std::string, int> g_strIndex;

int str_intern(const std::string& s) {
    auto it = g_strIndex.find(s);
    if (it != g_strIndex.end()) return it->second;
    int id = (int)g_strPool.size();
    g_strPool.emplace_back(s);
    g_strIndex.emplace(s, id);
    return id;
}

const Str& str_pool_get(int id) {
    if (id < 0 || (size_t)id >= g_strPool.size())
        throw std::runtime_error("literal de string invalido no bytecode");
    return g_strPool[(size_t)id];
}
