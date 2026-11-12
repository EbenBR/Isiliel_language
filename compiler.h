// Isiliel — scripting language interpreter/compiler.
// Embedded: sandbox, embedding API, OOP, 2D graphics and REPL.
// Authors: Eben (AI-assisted) | Version: 1.0 | Date: 2026-11-08

#ifndef COMPILER_H
#define COMPILER_H

#include "defs.h"
#include "symbols.h"
#include <vector>
#include <string>
#include <set>
#include <map>

class Compiler {
public:
    // code_base: initial global address of this section in the accumulated bytecode
    // (REPL). Local jumps are relocated at the end of run(); functions are already born
    // with a global entry_point.
    void init(const std::vector<Token>& tokens, int code_base = 0);
    bool run();
    void debug_dump_bytecode();
    std::vector<Instruction> get_bytecode() { return bytecode; }

    // Processing model (item 13): when enabled and the script defines
    // draw(), run() appends the loop: setup(); while (show()) { draw(); }.
    // The REPL keeps it disabled (defining draw() does not start an animation).
    void set_draw_loop(bool v) { drawLoopEnabled = v; }
    bool has_draw_loop() const { return drawEmitted; }
    // L0: setup() in the core — when the script defines setup() and this flag is
    // on, run() emits an initial jump to a stub that calls setup()
    // BEFORE any body instruction (even without graphics). Disabled
    // in disasm (goldens) and in the REPL (code_base != 0).
    void set_setup_first(bool v) { setupFirstEnabled = v; }

    // E8/neg16: embedding API — on run() error, fills last_error
    // (full message, without printing) when quiet_errors = true.
    std::string last_error;
    bool quiet_errors = false;

private:
    std::vector<Token> tokens;
    std::vector<Instruction> bytecode;
    int pos; // 'pos' kept to stay in sync with compiler.cpp
    int current_line = 1; // Line of the token being processed (errors)
    int code_base = 0;    // Global offset of this section (REPL)

    // Files already included in this compilation unit (cycle-safe/idempotent)
    std::set<std::string> includedFiles;
    std::vector<Token> expand_includes(const std::vector<Token>& toks, int depth);

    // require() of a script module (E6, negative 10): the module is compiled
    // BEFORE the main script (body with a single-execution guard), inside
    // the SAME bytecode — no runtime append (no reallocation while the VM
    // runs) and with the module functions valid forever.
    struct RequiredModule { int entry; int flagSid; int valSid; }; // entry LOCAL
    std::map<std::string, RequiredModule> requiredModules;
    std::set<std::string> loadingModules; // cycle detection (A->B->A)
    int module_val_sid = -1;  // >=0: `return` in the module body stores in val
    std::vector<std::pair<std::string,int>> find_requires(); // path, line
    void preload_requires();
    void load_module(const std::string& path, int line);

    // break/continue: stack of pending jumps per loop (nesting support)
    int loop_depth = 0;
    std::vector<int> breakJumps;
    std::vector<int> continueJumps;

    // Function scope being compiled: -1 = top level (globals); otherwise entry_point
    int current_func = -1;
    // L5: sid of the hidden __varargs local of the vararg function being compiled
    // (-1 = current function is not vararg). `...` and `return ...` use this sid.
    int current_vararg_sid = -1;

    // L12: stack of function contexts (to resolve lexical upvalues).
    // `anonymous` enables capture of locals from outer functions.
    struct FuncCtx { int owner; bool anonymous; };
    std::vector<FuncCtx> funcStack;
    // True if `name` is a local of an outer function and the current function is
    // anonymous (closure). In that case the read/write becomes *_FREE.
    bool free_var(const std::string& name);
    void patch_captured_locals();
    void push_func_ctx(int owner, bool anonymous);
    void pop_func_ctx();

    // Processing-style render loop (see set_draw_loop)
    bool drawLoopEnabled = false;
    bool drawEmitted = false;
    void emit_draw_epilogue();

    // L10: goto/labels resolved at the end of run() (per function: current_func)
    struct PendingGoto { int func; std::string name; int instr; int line; };
    std::map<int, std::map<std::string, int>> labels;
    std::vector<PendingGoto> pendingGotos;
    void resolve_gotos();

    // L0: setup() before everything (see set_setup_first)
    bool setupFirstEnabled = true;
    bool has_setup_definition();
    bool uses_set_draw();
    void emit_setup_boot(int& bootJump, int& mainStart);

    // Structs (`type Name { ... };`): context of the method being compiled
    std::string current_struct_type; // "" = outside a method
    int current_this_sid = -1;       // local sid of the implicit __this parameter

    // Function Detection and Compilation
    bool is_function_definition();
    void save_function_address(std::string name, int address);
    void compile_function_definition();
    void function_declaration(); 

    // Structs (type definition, literal and method)
    void compile_type_definition(bool isClass = false); // E4: class inherits from type
    void compile_method_definition(const std::string& typeName, Token nameTok);
    void compile_struct_literal(const std::string& typeName);
    bool in_struct_field(const std::string& name);

    // Optional scalar declaration (E1c): `i32 x = 5;`, `i32 x;`,
    // chain `i32 a = 1, b = 2;` — annotates Symbol::scalar
    void compile_scalar_declaration(ScalarType sc);

    // Built-ins and Calls
    int builtin_find(const std::string& name); 
    int check_builtin(const std::string& name); 
    void compile_call(std::string name, int argCount, bool spread = false);
    // L5: call arguments; detects `...`/final call (expansion) and
    // returns the syntactic argCount + spread (count encoded as -argCount).
    void parse_call_args(int& argCount, bool& spread, const char* what);
    // Stack discipline of `for`: only emits OP_POP if the init/incr
    // expression left a value (assignment already ends in OP_STORE, which pops)
    void pop_if_left_value();

    // Grammar and Precedence
    void statement();
    void expression();
    void assignment();
    void logical_or();
    void logical_and();
    void bitwise_or();
    void bitwise_xor();
    void bitwise_and();
    void equality();
    void comparison();
    void shift();
    void term();
    void factor();
    void unary();
    void primary();

    // Navigation Helpers (Implemented in the .cpp)
    int emit(OpCode op, int a = 0, int b = 0, std::string s = "");
    // E17: fuses `sid = sid OP top` into OP_ADD_STORE/OP_SUB_STORE[_K]
    // when the RHS contains no jumps or calls (evaluation order
    // preserved). Returns true if fused (the STORE must not be emitted).
    bool try_fuse_store(int sid, int load_idx);
    void patch(int index, int value);
    bool match(TokenType type);
    bool check(TokenType type); // Only a declaration here
    Token advance();
    Token previous();
    Token consume(TokenType type, std::string message);
    bool is_at_end();

    // Lookahead utility (inline for performance)
    Token peek_n(int n) {
        if (pos + n >= (int)tokens.size()) return tokens.back();
        return tokens[pos + n];
    }
};

#endif