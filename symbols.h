// Isiliel — scripting language interpreter/compiler.
// Embedded: sandbox, embedding API, OOP, 2D graphics and REPL.
// Authors: Eben (AI-assisted) | Version: 1.0 | Date: 2026-11-08

#ifndef SYMBOLS_H
#define SYMBOLS_H

#include "defs.h"
#include <string>
#include <vector>
#include <map>
#include <utility>

// Native .mod module variable (defined in modules.h)
struct NativeVar;

// --- Optional scalar types (phase E1c) ---
// C/C++-style declarations with Rust-style names:
//   i32 x = 5;  u8 n = 200;  f32 p = 3.14;  bool on = 1;  str s = "hi";
// Aliases: int -> i32, float -> f64, String -> str.
// The annotation defines coercion on EVERY write to `x` (OP_STORE):
// integers truncate toward zero + range-check; f32 rounds to float32;
// bool normalizes 0/1; str requires text. Without a declaration, everything stays
// dynamic (current model).
enum ScalarType {
    SC_NONE = 0,
    SC_I8, SC_I16, SC_I32, SC_I64, SC_ISIZE,
    SC_U8, SC_U16, SC_U32, SC_U64, SC_USIZE,
    SC_F32, SC_F64, SC_BOOL, SC_STR
};

// Type name for error messages ("i32", "u8", "str"...)
const char* scalar_type_name(ScalarType sc);
// "i32"/"f64"/... if `name` is a scalar type; otherwise SC_NONE
ScalarType scalar_type_from_name(const std::string& name);
// Coercion on write: truncate/range/0-1/text. Throws std::runtime_error
// (the VM appends " (line N)").
Value value_coerce_scalar(ScalarType sc, const Value& v);

struct Symbol {
    std::string name;
    Value value;
    bool is_function = false;
    int entry_point = -1; // Bytecode address where the function starts
    int owner = -1;       // -1 = global; otherwise entry_point of the owning function (local)
    std::string type_name; // Typed declaration annotation: "Cars car1 = ..." (optional)
    // Scalar type annotation (E1c): i32/u8/... — every write goes through
    // value_coerce_scalar(); SC_NONE = dynamic (current model)
    ScalarType scalar = SC_NONE;
    // `global x;` — assignments/reads inside functions use this global
    // (essential in Processing-style draw()/setup() callbacks)
    bool force_global = false;
    // `thread f() {...}` — start/stop/pause/resume only accept these;
    // erased together with sym_clear() (no parallel state)
    bool is_thread = false;
    // Native `.mod` module variable (item 16): reads/writes go straight
    // to the loaded library memory (never uses `value`).
    // Library-owned pointer (does not belong to sym_clear()).
    const NativeVar* native = nullptr;
    // L12: when the local is captured by a closure, reads/writes
    // go through this shared cell (may outlive the frame).
    std::shared_ptr<Value> cell;
    // L12: marked at compile time by free_var(); the compiler uses it
    // to swap the access opcodes for *_CELL variants at the end of run().
    bool captured = false;
};

// --- Type registry (structs) ---
// `type Name { field; ... void m() {} };` — fields in declared order;
// methods are normal functions in the symbol table ("Name::method").
struct StructType {
    std::string name;
    std::vector<std::string> fields;             // ONLY the own fields
    std::map<std::string, int> methods;          // name -> sid (own)
    std::string parent;                          // parent class ("" = root) — E4
};

StructType* struct_find(const std::string& name);            // nullptr if it does not exist
StructType* struct_create(const std::string& name);           // creates (or recreates empty in the REPL)
bool struct_add_field(const std::string& type, const std::string& field);
bool struct_add_method(const std::string& type, const std::string& method, int sid);
int struct_field_index(const std::string& type, const std::string& field); // -1 if it does not exist (includes inherited, flattened root->leaf index)
int struct_total_fields(const std::string& type);   // sum along the parent->leaf chain
int struct_set_parent(const std::string& type, const std::string& parent); // 0 ok, -1 error
int struct_method_sid(const std::string& type, const std::string& method); // -1 if it does not exist
void struct_clear();

// Searches only GLOBAL symbols (owner == -1)
int sym_find(const char* name);
// Local lookup belonging to the function `owner`
int sym_find_local(const char* name, int owner);

int sym_create(const char* name, Value val);                       // global
int sym_create_local(const char* name, Value val, int owner);      // local
int sym_create_func(const char* name, int entry_point);

// L12: saved state of a local when entering/leaving a frame (value + cell).
struct SavedLocal {
    int sid;
    Value value;
    std::shared_ptr<Value> cell;
};

// Execution scopes (recursion): saves/clears locals of `owner` and restores
void sym_save_scope(int owner, std::vector<SavedLocal>& saved);
void sym_restore_scope(const std::vector<SavedLocal>& saved, size_t from);

// L12: migrates all locals of `owner` into cells of the `env` environment.
// Called when an anonymous `fn(...)` is evaluated in the current frame.
void sym_capture_owner(int owner, FrameEnv& env);

Symbol* sym_get(int id);
void sym_dump();
void sym_clear();

#endif
