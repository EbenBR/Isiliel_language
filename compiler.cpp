// Isiliel — scripting language interpreter/compiler.
// Embedded: sandbox, embedding API, OOP, 2D graphics and REPL.
// Authors: EEPS and MiMo-V2.6 | Version: 1.0 | Date: 2026-11-08

#include "compiler.h"
#include "symbols.h"
#include "vm.h"
#include "lexer.h"
#include "modules.h"
#include <stdexcept>
#include <cmath>
#include <iostream>
#include <iomanip>
#include <fstream>
#include <iterator>
#include <cerrno>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <limits>

// L7: decimal literal that fits in int64 (no dot/exponent).
static bool parse_i64_literal(const std::string& s, long long& out) {
    if (s.empty()) return false;
    for (char c : s)
        if (c < '0' || c > '9') return false;
    errno = 0;
    char* end = nullptr;
    long long v = std::strtoll(s.c_str(), &end, 10);
    if (errno == ERANGE || end != s.c_str() + s.size()) return false;
    out = v;
    return true;
}

// L7: rebuilds the immediate integer of OP_PUSH (low/high in a/c).
static bool push_i64_value(const Instruction& in, long long& out) {
    if (in.b != 2) return false;
    unsigned long long u =
        ((unsigned long long)(unsigned int)in.c << 32) |
        (unsigned long long)(unsigned int)in.a;
    out = (long long)u;
    return true;
}

static bool i64_exact_in_double(long long v) {
    return v >= -9007199254740992LL && v <= 9007199254740992LL; // +-2^53
}

// math.h constants resolved as literals in the compiler. They do not enter
// the symbol table: disasm IDs and the REPL symbol limit stay
// stable, and the assignment `PI = ...` remains an ordinary variable.
struct MathConstant { const char* name; double value; };

static bool math_constant_value(const std::string& name, double& out) {
    static const double pi  = 3.141592653589793238462643383279502884;
    static const double e   = 2.718281828459045235360287471352662498;
    static const double phi = 1.618033988749894848204586834365638118;
    static const double inf = std::numeric_limits<double>::infinity();
    static const double nan = std::numeric_limits<double>::quiet_NaN();
    static const MathConstant consts[] = {
        {"M_E", e}, {"M_LOG2E", 1.442695040888963407359924681001892137},
        {"M_LOG10E", 0.434294481903251827651128918916605082},
        {"M_LN2", 0.693147180559945309417232121458176568},
        {"M_LN10", 2.302585092994045684017991454684364208},
        {"M_PI", pi}, {"M_PI_2", pi / 2.0}, {"M_PI_4", pi / 4.0},
        {"M_1_PI", 1.0 / pi}, {"M_2_PI", 2.0 / pi},
        {"M_2_SQRTPI", 2.0 / std::sqrt(pi)},
        {"M_SQRT2", std::sqrt(2.0)}, {"M_SQRT1_2", std::sqrt(0.5)},
        {"M_PHI", phi},
        {"PI", pi}, {"E", e}, {"TAU", 2.0 * pi}, {"TWO_PI", 2.0 * pi},
        {"PI_2", pi / 2.0}, {"PI_4", pi / 4.0}, {"INV_PI", 1.0 / pi},
        {"TWO_OVER_PI", 2.0 / pi}, {"TWO_OVER_SQRTPI", 2.0 / std::sqrt(pi)},
        {"SQRT2", std::sqrt(2.0)}, {"SQRT1_2", std::sqrt(0.5)},
        {"LN2", std::log(2.0)}, {"LN10", std::log(10.0)},
        {"LOG2E", 1.0 / std::log(2.0)}, {"LOG10E", 1.0 / std::log(10.0)},
        {"PHI", phi}, {"DEG_TO_RAD", pi / 180.0}, {"RAD_TO_DEG", 180.0 / pi},
        {"HUGE_VAL", inf}, {"HUGE_VALF", inf}, {"HUGE_VALL", inf},
        {"INFINITY", inf}, {"INF", inf}, {"NAN", nan},
        {"MAXFLOAT", 3.402823466385288598117041834845169255e38},
        {"FLT_MAX", 3.402823466385288598117041834845169255e38},
        {"DBL_MAX", 1.797693134862315708145274237317043568e308},
        {"FLT_EPSILON", 1.1920928955078125e-7},
        {"DBL_EPSILON", 2.220446049250313080847263336181640625e-16},
        {"FP_NAN", 0.0}, {"FP_INFINITE", 1.0}, {"FP_ZERO", 2.0},
        {"FP_SUBNORMAL", 3.0}, {"FP_NORMAL", 4.0},
        {"MATH_ERRNO", 1.0}, {"MATH_ERREXCEPT", 2.0},
        {"math_errhandling", 3.0},
    };
    for (const MathConstant& c : consts) {
        if (name == c.name) { out = c.value; return true; }
    }
    return false;
}

static std::string math_constant_literal(double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.17g", v);
    return std::string(buf);
}

// L7: integral double representable in int64 (loop limits).
static bool double_exact_i64(double d, long long& out) {
    const double LIM = 9223372036854775808.0; // 2^63
    if (!(d >= -LIM && d < LIM) || d != std::floor(d)) return false;
    out = (long long)d;
    return true;
}

// E7/neg13: stable builtin id (O(1) dispatch at runtime); -1 = no id
// (path by name — `.mod` module functions or not registered yet).
static int builtin_id_for(const std::string& name, int argc) {
    const Builtin* bi = builtin_lookup(name, argc);
    return bi ? bi->id : -1;
}

void Compiler::init(const std::vector<Token>& tokens, int code_base) {
    this->tokens = tokens;
    this->pos = 0;
    this->bytecode.clear();
    this->current_line = tokens.empty() ? 1 : tokens[0].line;
    this->includedFiles.clear();
    this->code_base = code_base;
}

// Recursively expands 'include("file");' in place.
// Paths are resolved relative to the working directory.
// A file is included at most once per compilation unit
// (this also cuts cycles a -> b -> a).
std::vector<Token> Compiler::expand_includes(const std::vector<Token>& toks, int depth) {
    if (depth > 16) {
        throw std::runtime_error("include: profundidade maxima excedida (ciclo?)");
    }
    std::vector<Token> out;
    out.reserve(toks.size());
    for (size_t i = 0; i < toks.size(); i++) {
        if (toks[i].type != TK_INCLUDE) {
            out.push_back(toks[i]);
            continue;
        }
        if (i + 3 >= toks.size() ||
            toks[i + 1].type != TK_LPAREN ||
            toks[i + 2].type != TK_STRING ||
            toks[i + 3].type != TK_RPAREN) {
            throw std::runtime_error("include: use include(\"arquivo\"); (linha " +
                                     std::to_string(toks[i].line) + ")");
        }
        std::string path = toks[i + 2].lexeme;
        int iline = toks[i].line;
        size_t j = i + 4;
        if (j < toks.size() && toks[j].type == TK_PUNCT) j++; // consumes ';'
        i = j - 1; // the for loop increments i

        // Sandbox (E8/neg18): include reads a file at compile time
        if (!g_allow_fileio)
            throw std::runtime_error("include desabilitado (allow_fileio=false) (linha " +
                                     std::to_string(iline) + ")");

        if (includedFiles.count(path)) continue; // already included
        includedFiles.insert(path);

        std::ifstream f(path);
        if (!f) {
            throw std::runtime_error("include: nao foi possivel abrir '" + path +
                                     "' (linha " + std::to_string(iline) + ")");
        }
        std::string content((std::istreambuf_iterator<char>(f)),
                            std::istreambuf_iterator<char>());
        Lexer lex(content);
        std::vector<Token> sub = lex.scan_tokens();
        if (!sub.empty() && sub.back().type == TK_EOF) sub.pop_back();

        std::vector<Token> expanded = expand_includes(sub, depth + 1);
        for (const Token& t : expanded) out.push_back(t);
    }
    return out;
}

// --- require() of a script module (E6, negative 10) ---
// Scans the current tokens for `require("path")` not yet loaded.
std::vector<std::pair<std::string,int>> Compiler::find_requires() {
    std::vector<std::pair<std::string,int>> found;
    std::set<std::string> seen;
    for (size_t i = 0; i + 2 < tokens.size(); i++) {
        if (tokens[i].type != TK_ID || tokens[i].lexeme != "require") continue;
        if (tokens[i+1].type != TK_LPAREN || tokens[i+2].type != TK_STRING)
            continue;
        const std::string& p = tokens[i+2].lexeme;
        if (requiredModules.count(p) || seen.count(p)) continue;
        seen.insert(p);
        found.push_back({p, tokens[i].line});
    }
    return found;
}

// Compiles the modules required by the MAIN script before compiling the
// script itself: a single JUMP over all bodies (the script lands
// directly on its first command; the bodies only run via require).
void Compiler::preload_requires() {
    auto found = find_requires();
    if (found.empty()) return;
    int jumpToMain = emit(OP_JUMP, 0);
    for (auto& f : found) load_module(f.first, f.second);
    patch(jumpToMain, (int)bytecode.size());
}

// Compiles a script module as a body with a single-execution guard:
//   entry: LOAD flag; JIF run; LOAD val; JUMP ret;
//   run:   PUSH 1; STORE flag; <body>; PUSH 0; STORE val;
//   ret:   LOAD val; RETURN
// `return X` in the body (module top level) becomes STORE val; LOAD val; RETURN.
// The body is compiled into the SAME bytecode (no runtime append): module
// functions become global symbols and stay valid forever.
void Compiler::load_module(const std::string& path, int line) {
    // Sandbox (E8/neg18): require reads a file at compile time
    if (!g_allow_fileio)
        throw std::runtime_error("require desabilitado (allow_fileio=false) (linha " +
                                 std::to_string(line) + ")");
    if (loadingModules.count(path))
        throw std::runtime_error("require: ciclo ao carregar '" + path +
                                 "' (linha " + std::to_string(line) + ")");
    loadingModules.insert(path);

    std::ifstream f(path);
    if (!f)
        throw std::runtime_error("require: nao foi possivel abrir '" + path +
                                 "' (linha " + std::to_string(line) + ")");
    std::string content((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
    Lexer lex(content);
    std::vector<Token> sub = lex.scan_tokens();
    if (!sub.empty() && sub.back().type == TK_EOF) sub.pop_back();
    sub = expand_includes(sub, 0);

    // saves the context of the script that called require
    std::vector<Token> savedTokens = std::move(tokens);
    int savedPos = pos;
    int savedLine = current_line;
    int savedValSid = module_val_sid;

    tokens = std::move(sub);
    pos = 0;
    current_line = tokens.empty() ? 1 : tokens[0].line;

    // require INSIDE the module: loads the dependencies BEFORE the body
    // (the body entry comes after them; the main script JUMP
    // already covers all bodies emitted so far)
    for (auto& f2 : find_requires())
        load_module(f2.first, f2.second);

    // guard/cache symbols (reused between REPL and recompilations)
    std::string flagName = "__req_flag_" + path;
    std::string valName  = "__req_val_" + path;
    int flagSid = sym_find(flagName.c_str());
    if (flagSid == -1) flagSid = sym_create(flagName.c_str(), {VAL_FLOAT});
    int valSid = sym_find(valName.c_str());
    if (valSid == -1) valSid = sym_create(valName.c_str(), {VAL_FLOAT});

    int entry = (int)bytecode.size();
    emit(OP_LOAD, flagSid); // flag=0 (false) → JIF falls into the body
    int jf = emit(OP_JUMP_IF_FALSE, 0);
    emit(OP_LOAD, valSid);            // already ran: returns the cached value
    int jo = emit(OP_JUMP, 0);
    patch(jf, (int)bytecode.size());
    emit(OP_PUSH, 0, 0, "1");
    emit(OP_STORE, flagSid);          // marks BEFORE the body (cycle-friendly)
    module_val_sid = valSid;          // module `return` stores in val
    while (!is_at_end()) statement();
    module_val_sid = savedValSid;
    emit(OP_PUSH, 0, 0, "0");         // body without return → returns 0
    emit(OP_STORE, valSid);
    patch(jo, (int)bytecode.size());
    emit(OP_LOAD, valSid);
    emit(OP_RETURN);

    requiredModules[path] = RequiredModule{entry, flagSid, valSid};

    // restores the caller context
    tokens = std::move(savedTokens);
    pos = savedPos;
    current_line = savedLine;
    loadingModules.erase(path);
}

bool Compiler::run() {
    try {
        // Expands include("file"); before compiling (may throw with line)
        tokens = expand_includes(tokens, 0);

        // Resets the position pointer to guarantee safety
        pos = 0;
        current_line = tokens.empty() ? 1 : tokens[0].line;

        // L0: setup() in the core — if the script defines setup(), the first
        // executed instruction is a jump to the stub that calls it (closed
        // after compiling everything, when the entry_point is known).
        // Disabled in disasm and in the REPL (code_base != 0).
        int bootJump = -1, mainStart = -1;
        bool setupFirst = setupFirstEnabled && code_base == 0 &&
                          has_setup_definition();
        if (setupFirst) bootJump = emit(OP_JUMP, 0);

        // require("...") (E6): compiles the script modules BEFORE the main one
        preload_requires();
        if (setupFirst) mainStart = (int)bytecode.size();

        // The main loop walks all tokens until the end of the file (EOF)
        while (!is_at_end()) {
            // Each iteration tries to process a "declaration" (complete statement)
            statement();
        }

        // Processing-style render loop: if the script defines draw(),
        // appends  setup(); while (show()) { draw(); }  before the halt.
        if (drawLoopEnabled)
            emit_draw_epilogue();

        // After processing all tokens, we emit OP_HALT
        // This ensures the VM stops executing when it reaches the end of the global code
        emit(OP_HALT);

        // L0: closes the boot jump (stub with setup() at the end of the bytecode)
        if (setupFirst) emit_setup_boot(bootJump, mainStart);

        // Relocates local jumps to the global address (the REPL accumulates bytecode).
        // L10: gotos -> labels (local positions; code_base is added below)
        resolve_gotos();

        // OP_CALL is untouched: it is already born with a global entry_point via the symbol.
        if (code_base != 0) {
            for (Instruction& in : bytecode) {
                if (in.op == OP_JUMP || in.op == OP_JUMP_IF_FALSE ||
                    in.op == OP_LT_JIF_K || in.op == OP_LE_JIF_K ||
                    in.op == OP_GT_JIF_K || in.op == OP_GE_JIF_K ||
                    in.op == OP_EQ_JIF_K || in.op == OP_NE_JIF_K)
                    in.a += code_base;
            }
        }

        patch_captured_locals();

        return true; // Compilation succeeded
    } catch (const std::exception& e) {
        // Catches syntax errors thrown by consume() or manual throws
        std::string msg = e.what();
        std::string full = "ERRO COMPILACAO: " + msg;
        // Errors from expand_includes already carry "(line N)" in the message
        if (msg.find("(linha") == std::string::npos && pos < (int)tokens.size()) {
            full += " no lexema: '" + tokens[pos].lexeme + "'";
            int line = tokens[pos].line;
            if (line > 0) full += " (linha " + std::to_string(line) + ")";
        }
        last_error = full;
        if (!quiet_errors) std::cerr << full << std::endl;
        return false; // Compilation failed
    }
}

// Processing-style epilogue: if the script defines the global function draw(),
// generates:  setup();  L: draw(); if (show()) goto L;   (show = frame/events)
// Without draw() the script runs once (classic behavior).
void Compiler::emit_draw_epilogue() {
    int sidDraw = sym_find("draw");
    Symbol* drawSym = (sidDraw >= 0) ? sym_get(sidDraw) : nullptr;
    bool hasDraw = drawSym && drawSym->is_function && drawSym->owner == -1;
    // L1: if the script uses setDraw(fn)/onDraw(fn), the loop calls the
    // registered callback (OP_CALL_DRAW); global draw() keeps working as a
    // fallback and the old auto-detection is preserved when there is no setDraw.
    bool callbackMode = uses_set_draw();
    if (!hasDraw && !callbackMode) return;

    // L0: setup() is NO LONGER called here — it runs at boot (emit_setup_boot),
    // before any body instruction, even without graphics.

    // Processing-style loop, with the frame test BEFORE each draw():
    //   setup();
    //   while (show()) { draw(); }   // show = presents + events/close
    // This way the window really closes (nothing is drawn after closing) and the
    // headless backend runs exactly 1 frame.
    int loopStart = (int)bytecode.size();
    emit(OP_CALL_BUILTIN, builtin_id_for("show", 0), 0, "show"); // 1 = window open, 0 = closed
    int exitJump = emit(OP_JUMP_IF_FALSE, 0);
    if (callbackMode) {
        emit(OP_CALL_DRAW, hasDraw ? drawSym->entry_point : -1, 0, "draw");
    } else {
        emit(OP_CALL, drawSym->entry_point, 0, "draw");
    }
    emit(OP_POP);
    emit(OP_JUMP, loopStart);
    patch(exitJump, (int)bytecode.size());

    drawEmitted = true;
}

// L0: scans the tokens for a `setup(...) {` definition at level 0 (top level).
// Used to decide whether the boot jump is needed (does not touch the disasm
// of scripts without setup).
// L10: resolves pending gotos to labels of the SAME function.
void Compiler::resolve_gotos() {
    for (const PendingGoto& g : pendingGotos) {
        auto fit = labels.find(g.func);
        if (fit == labels.end() || !fit->second.count(g.name))
            throw std::runtime_error("Label nao encontrado: " + g.name +
                                     " (linha " + std::to_string(g.line) + ")");
        patch(g.instr, fit->second[g.name]);
    }
    pendingGotos.clear();
    labels.clear();
}

// L1: looks for `setDraw`/`onDraw` at any level (callback builtin).
bool Compiler::uses_set_draw() {
    for (const Token& t : tokens) {
        if (t.type == TK_ID && (t.lexeme == "setDraw" || t.lexeme == "onDraw"))
            return true;
    }
    return false;
}

bool Compiler::has_setup_definition() {
    int depth = 0;
    for (size_t i = 0; i + 1 < tokens.size(); i++) {
        if (tokens[i].type == TK_LBRACE) { depth++; continue; }
        if (tokens[i].type == TK_RBRACE) { if (depth > 0) depth--; continue; }
        if (depth == 0 && tokens[i].type == TK_ID &&
            tokens[i].lexeme == "setup" && tokens[i+1].type == TK_LPAREN)
            return true;
    }
    return false;
}

// L0: closes the boot jump emitted at the start of run(). If setup() is a
// global function, emits a stub `CALL setup; POP; JUMP mainStart` at the end and
// points the jump to it; otherwise points straight to the body.
void Compiler::emit_setup_boot(int& bootJump, int& mainStart) {
    if (bootJump < 0) return;
    int sid = sym_find("setup");
    Symbol* sym = (sid >= 0) ? sym_get(sid) : nullptr;
    if (sym && sym->is_function && sym->owner == -1) {
        int stub = (int)bytecode.size();
        emit(OP_CALL, sym->entry_point, 0, "setup");
        emit(OP_POP);
        emit(OP_JUMP, mainStart);
        patch(bootJump, stub);
    } else {
        patch(bootJump, mainStart);
    }
}

bool Compiler::is_function_definition() {
    // Optional prefixes in any order: return type (`void setup()`,
    // `int f() {}`, scalar types `i32 f() {}` — E1c)
    // and/or `thread` (`thread f() {...}` — item 15)
    int base = pos;
    while (base < (int)tokens.size()) {
        TokenType bt = tokens[base].type;
        if (bt == TK_VOID || bt == TK_INT || bt == TK_FLOAT || bt == TK_THREAD) base++;
        else if (bt == TK_FN && base + 1 < (int)tokens.size() &&
                 tokens[base + 1].type == TK_ID) base++; // named fn (E2)
        else if (bt == TK_ID &&
                 scalar_type_from_name(tokens[base].lexeme) != SC_NONE) base++;
        else break;
    }

    if (base >= (int)tokens.size() || tokens[base].type != TK_ID) return false;

    // Cast to int resolves the comparison warning
    if (base + 1 >= (int)tokens.size() || tokens[base + 1].type != TK_LPAREN) return false;

    int i = 2;
    int paren_count = 1;

    while (paren_count > 0 && (base + i) < (int)tokens.size()) {
        if (tokens[base + i].type == TK_LPAREN) paren_count++;
        else if (tokens[base + i].type == TK_RPAREN) paren_count--;
        i++;
    }

    return (base + i < (int)tokens.size()) && tokens[base + i].type == TK_LBRACE;
}

void Compiler::function_declaration() {
    advance(); 
    Token name = consume(TK_ID, "Esperado nome da funcao.");
    consume(TK_LPAREN, "Esperado '(' apos nome da funcao.");
    consume(TK_RPAREN, "Esperado ')' apos parametros.");

    int jump_over = emit(OP_JUMP, 0);
    int entry_point = (int)bytecode.size();
    
    sym_create_func(name.lexeme.c_str(), entry_point);
    statement(); 
    
    emit(OP_RETURN_N, 0); // L5
    patch(jump_over, (int)bytecode.size());
}

// L12: function context to resolve lexical captures.
void Compiler::push_func_ctx(int owner, bool anonymous) {
    funcStack.push_back({owner, anonymous});
}

void Compiler::pop_func_ctx() {
    if (!funcStack.empty()) funcStack.pop_back();
}

bool Compiler::free_var(const std::string& name) {
    if (funcStack.empty() || !funcStack.back().anonymous) return false;
    // Searches from the immediately outer scope to the outermost one.
    for (int i = (int)funcStack.size() - 2; i >= 0; i--) {
        int sid = sym_find_local(name.c_str(), funcStack[i].owner);
        if (sid != -1) {
            Symbol* s = sym_get(sid);
            if (s) s->captured = true; // access now uses a cell
            return true;
        }
    }
    return false;
}

// L12: swaps, at the end of compilation, the access opcodes of captured
// locals for *_CELL variants. Thus the common opcodes (the hot path
// of scripts without closures) pay no cell check.
void Compiler::patch_captured_locals() {
    for (Instruction& in : bytecode) {
        int sid = -1;
        switch (in.op) {
            case OP_LOAD: case OP_STORE: case OP_INC_LOCAL:
            case OP_ADD_STORE: case OP_SUB_STORE:
            case OP_ADD_STORE_K: case OP_SUB_STORE_K:
                sid = in.a; break;
            case OP_LT_JIF_K: case OP_LE_JIF_K: case OP_GT_JIF_K:
            case OP_GE_JIF_K: case OP_EQ_JIF_K: case OP_NE_JIF_K:
            case OP_LT_LOOP: case OP_LE_LOOP: case OP_GT_LOOP:
            case OP_GE_LOOP: case OP_INC_JUMP:
                sid = in.b; break;
            default: continue;
        }
        Symbol* s = (sid >= 0) ? sym_get(sid) : nullptr;
        if (!s || !s->captured) continue;
        switch (in.op) {
            case OP_LOAD: in.op = OP_LOAD_CELL; break;
            case OP_STORE: in.op = OP_STORE_CELL; break;
            case OP_INC_LOCAL: in.op = OP_INC_CELL; break;
            case OP_ADD_STORE: in.op = OP_ADD_STORE_CELL; break;
            case OP_SUB_STORE: in.op = OP_SUB_STORE_CELL; break;
            case OP_ADD_STORE_K: in.op = OP_ADD_STORE_K_CELL; break;
            case OP_SUB_STORE_K: in.op = OP_SUB_STORE_K_CELL; break;
            case OP_LT_JIF_K: in.op = OP_LT_JIF_K_CELL; break;
            case OP_LE_JIF_K: in.op = OP_LE_JIF_K_CELL; break;
            case OP_GT_JIF_K: in.op = OP_GT_JIF_K_CELL; break;
            case OP_GE_JIF_K: in.op = OP_GE_JIF_K_CELL; break;
            case OP_EQ_JIF_K: in.op = OP_EQ_JIF_K_CELL; break;
            case OP_NE_JIF_K: in.op = OP_NE_JIF_K_CELL; break;
            case OP_LT_LOOP: in.op = OP_LT_LOOP_CELL; break;
            case OP_LE_LOOP: in.op = OP_LE_LOOP_CELL; break;
            case OP_GT_LOOP: in.op = OP_GT_LOOP_CELL; break;
            case OP_GE_LOOP: in.op = OP_GE_LOOP_CELL; break;
            case OP_INC_JUMP: in.op = OP_INC_JUMP_CELL; break;
            default: break;
        }
    }
}

void Compiler::save_function_address(std::string name, int address) {
    // GLOBAL address in the accumulated bytecode (REPL: code_base > 0)
    int global_addr = address + code_base;

    // 1. Checks whether the symbol already exists
    int sid = sym_find(name.c_str());
    
    // 2. If it does not exist, creates it using its function-specific helper
    if (sid == -1) {
        sid = sym_create_func(name.c_str(), global_addr);
    }
    
    // 3. Gets the symbol pointer to configure the details
    Symbol* s = sym_get(sid);
    
    // 4. Explicitly marks it as a function and sets the entry point (IP)
    s->is_function = true;
    s->entry_point = global_addr; 
}

void Compiler::compile_function_definition() {
    // Optional prefixes in any order: type (void/int/float, discarded;
    // scalar types i32/f64/... — E1c, also discarded) and/or `thread`
    // (function run by start/stop/pause/resume — item 15)
    bool isThread = false;
    while (true) {
        if (check(TK_VOID) || check(TK_INT) || check(TK_FLOAT)) advance();
        else if (check(TK_THREAD)) { advance(); isThread = true; }
        else if (check(TK_FN) && peek_n(1).type == TK_ID) advance(); // fn f() (E2)
        else if (check(TK_ID) && scalar_type_from_name(peek_n(0).lexeme) != SC_NONE) advance();
        else break;
    }
    Token nameToken = consume(TK_ID, "Esperado nome da função.");
    std::string funcName = nameToken.lexeme;

    int jump_over = emit(OP_JUMP, 0);
    int entry_point = (int)bytecode.size();
    save_function_address(funcName, entry_point);

    // Marks (or unmarks, when redefining in the REPL) as a thread function
    int tsid = sym_find(funcName.c_str());
    if (tsid >= 0) sym_get(tsid)->is_thread = isThread;

    // Parameters and body belong to this function. The owner of the locals is the
    // GLOBAL address (same as the VM uses in sym_save_scope via OP_CALL).
    int prev_func = current_func;
    int prev_vararg = current_vararg_sid;
    current_func = entry_point + code_base;
    current_vararg_sid = -1;
    push_func_ctx(current_func, false); // L12: named function does not capture

    consume(TK_LPAREN, "Esperado '('");
    std::vector<int> params;
    bool is_vararg = false;
    int vararg_sid = -1;
    if (!check(TK_RPAREN)) {
        do {
            if (check(TK_ELLIPSIS)) { // L5: ... (last parameter)
                advance();
                is_vararg = true;
                vararg_sid = sym_find_local("__varargs_l5", current_func);
                if (vararg_sid == -1)
                    vararg_sid = sym_create_local("__varargs_l5", {VAL_FLOAT}, current_func);
                break;
            }
            Token p = consume(TK_ID, "Esperado nome do parâmetro.");
            int sid = sym_find_local(p.lexeme.c_str(), current_func);
            if (sid == -1) sid = sym_create_local(p.lexeme.c_str(), {VAL_FLOAT}, current_func);
            params.push_back(sid);
        } while (match(TK_COMMA));
    }
    consume(TK_RPAREN, "Esperado ')'");

    // start f() has no argument list: a thread receives no parameters
    if (isThread && (is_vararg || !params.empty()))
        throw std::runtime_error("funcao 'thread' nao aceita parametros: " + funcName);

    // L5: collects the extra arguments BEFORE binding the parameters
    // (they are on top of the stack; afterwards exactly nparams remain).
    if (is_vararg) emit(OP_COLLECT_VARARGS, (int)params.size(), vararg_sid);
    else emit(OP_BIND_PARAMS, (int)params.size());
    current_vararg_sid = vararg_sid;

    // Binds parameters (LIFO)
    for (int i = (int)params.size() - 1; i >= 0; i--) {
        emit(OP_STORE, params[i]);
    }

    // A function defined inside a loop does not inherit the break/continue context
    int saved_loop_depth = loop_depth;
    size_t saved_breaks = breakJumps.size(), saved_continues = continueJumps.size();
    loop_depth = 0;

    // Compiles the body. If it is '{', statement() handles the block.
    statement(); 

    pop_func_ctx(); // L12
    current_func = prev_func;
    current_vararg_sid = prev_vararg;
    loop_depth = saved_loop_depth;
    breakJumps.resize(saved_breaks);
    continueJumps.resize(saved_continues);

    // L5: without an explicit `return` the function returns ZERO values (the caller
    // fills with nil when it asks for 1).
    emit(OP_RETURN_N, 0);
    
    patch(jump_over, (int)bytecode.size());
}

// --- OPTIONAL SCALAR DECLARATION (E1c) ---
// `i32 x = 5;`, `i32 x;` (zero default), chain `i32 a = 1, b = 2;`
// Annotates Symbol::scalar; every later write coerces (OP_STORE in the VM).
void Compiler::compile_scalar_declaration(ScalarType sc) {
    Token typeTok = advance(); // type name (TK_ID, TK_INT or TK_FLOAT)
    std::string lastName;
    do {
        Token nameTok = consume(TK_ID, "Esperado nome apos '" + typeTok.lexeme + "'");
        lastName = nameTok.lexeme;
        if (match(TK_ASSIGN)) {
            expression();
        } else if (sc == SC_STR) {
            emit(OP_PUSH_STR, str_intern("")); // default: empty text
        } else {
            emit(OP_PUSH, 0, 0, "0");    // default: zero
        }

        int sid;
        if (current_func != -1) {
            sid = sym_find_local(nameTok.lexeme.c_str(), current_func);
            if (sid == -1) sid = sym_create_local(nameTok.lexeme.c_str(), {VAL_FLOAT}, current_func);
        } else {
            sid = sym_find(nameTok.lexeme.c_str());
            if (sid == -1) sid = sym_create(nameTok.lexeme.c_str(), {VAL_FLOAT});
        }
        Symbol* s = sym_get(sid);
        if (s->is_function)
            throw std::runtime_error("'" + nameTok.lexeme + "' ja e uma funcao");
        if (s->scalar == SC_NONE) {
            s->scalar = sc;
            s->type_name.clear(); // scalar annotation replaces the struct one
        } else if (s->scalar != sc) {
            throw std::runtime_error("'" + nameTok.lexeme +
                                     "' ja e declarada como '" +
                                     scalar_type_name(s->scalar) + "'");
        }
        emit(OP_STORE, sid);
    } while (match(TK_COMMA));
    consume(TK_PUNCT, "Esperado ';' apos a declaracao de '" + lastName + "'");
}

// --- STRUCTS (type Name { ... };) ---
bool Compiler::in_struct_field(const std::string& name) {
    if (current_struct_type.empty()) return false;
    return struct_field_index(current_struct_type, name) != -1;
}

// type Name { field; ...; void method() { ... } };
void Compiler::compile_type_definition(bool isClass) {
    Token nameTok = consume(TK_ID, isClass
        ? "Esperado nome da classe apos 'class'"
        : "Esperado nome do tipo apos 'type'");
    struct_create(nameTok.lexeme); // creates (or recreates empty when redefined in the REPL)
    if (isClass && match(TK_EXTENDS)) { // E4: single inheritance
        Token pTok = consume(TK_ID, "Esperado nome da classe pai apos 'extends'");
        if (!struct_find(pTok.lexeme))
            throw std::runtime_error("Classe pai '" + pTok.lexeme + "' nao definida");
        if (struct_set_parent(nameTok.lexeme, pTok.lexeme) != 0)
            throw std::runtime_error("Heranca circular ou invalida: '" +
                nameTok.lexeme + "' extends '" + pTok.lexeme + "'");
    }
    consume(TK_LBRACE, isClass ? "Esperado '{' apos nome da classe"
                               : "Esperado '{' apos nome do tipo");

    while (!check(TK_RBRACE) && !is_at_end()) {
        // Optional type prefix (fields accept any Value;
        // scalar types i32/str/... are a discarded annotation — E1c)
        match(TK_INT);
        match(TK_FLOAT);
        bool hasVoid = match(TK_VOID);
        if (check(TK_ID) && peek_n(1).type == TK_ID &&
            scalar_type_from_name(peek_n(0).lexeme) != SC_NONE)
            advance(); // discards the field/method scalar type

        Token id = consume(TK_ID, "Esperado nome do campo ou metodo");
        if (check(TK_LPAREN)) {
            compile_method_definition(nameTok.lexeme, id);
        } else {
            if (hasVoid)
                throw std::runtime_error("'void' e reservado para metodos");
            if (!struct_add_field(nameTok.lexeme, id.lexeme))
                throw std::runtime_error("Campo '" + id.lexeme + "' duplicado em '" + nameTok.lexeme + "'");
            consume(TK_PUNCT, "Esperado ';' apos campo '" + id.lexeme + "'");
        }
    }

    consume(TK_RBRACE, isClass ? "Esperado '}' ao fechar a classe '" + nameTok.lexeme + "'"
                               : "Esperado '}' ao fechar o tipo '" + nameTok.lexeme + "'");
    // `type` requires ';'; `class` accepts it with or without ';' (freer style)
    if (isClass) {
        if (check(TK_PUNCT) && peek_n(0).lexeme == ";") advance();
    } else {
        consume(TK_PUNCT, "Esperado ';' apos a definicao do tipo");
    }
}

// Method body: compiled as a normal function "Type::method" with an
// implicit __this parameter (the object pushed by the caller).
void Compiler::compile_method_definition(const std::string& typeName, Token nameTok) {
    int jump_over = emit(OP_JUMP, 0);
    int entry = (int)bytecode.size();
    int owner = entry + code_base; // GLOBAL owner (same as the VM in OP_CALL)

    // Method function symbol
    std::string full = typeName + "::" + nameTok.lexeme;
    int sid = sym_find(full.c_str());
    if (sid == -1) sid = sym_create_func(full.c_str(), owner);
    Symbol* fs = sym_get(sid);
    fs->is_function = true;
    fs->entry_point = owner;
    if (!struct_add_method(typeName, nameTok.lexeme, sid))
        throw std::runtime_error("Metodo '" + nameTok.lexeme + "' duplicado em '" + typeName + "'");

    // Context: the body accesses fields directly (via __this)
    std::string prev_struct = current_struct_type;
    int prev_this = current_this_sid;
    int prev_func = current_func;
    int prev_vararg = current_vararg_sid;
    current_struct_type = typeName;
    current_func = owner;
    current_vararg_sid = -1;
    push_func_ctx(current_func, false); // L12: named method does not capture

    consume(TK_LPAREN, "Esperado '(' apos nome do metodo");
    std::vector<int> params;
    bool is_vararg = false;
    int vararg_sid = -1;
    if (!check(TK_RPAREN)) {
        do {
            if (check(TK_ELLIPSIS)) { // L5: vararg method
                advance();
                is_vararg = true;
                vararg_sid = sym_find_local("__varargs_l5", owner);
                if (vararg_sid == -1)
                    vararg_sid = sym_create_local("__varargs_l5", {VAL_FLOAT}, owner);
                break;
            }
            Token p = consume(TK_ID, "Esperado nome do parametro");
            int psid = sym_find_local(p.lexeme.c_str(), owner);
            if (psid == -1) psid = sym_create_local(p.lexeme.c_str(), {VAL_FLOAT}, owner);
            params.push_back(psid);
        } while (match(TK_COMMA));
    }
    consume(TK_RPAREN, "Esperado ')'");

    // Implicit __this parameter (always created, even without explicit use)
    int thisSid = sym_find_local("__this", owner);
    if (thisSid == -1) thisSid = sym_create_local("__this", {VAL_FLOAT}, owner);
    current_this_sid = thisSid;

    // L5: collects extras before the params (nparams includes __this)
    if (is_vararg)
        emit(OP_COLLECT_VARARGS, (int)params.size() + 1, vararg_sid);
    else
        emit(OP_BIND_PARAMS, (int)params.size() + 1);
    current_vararg_sid = vararg_sid;

    // Binding (LIFO): args first, then __this (ends on top after args)
    for (int i = (int)params.size() - 1; i >= 0; i--) emit(OP_STORE, params[i]);
    emit(OP_STORE, thisSid);

    // Methods do not inherit break/continue context from the outer scope
    int saved_loop_depth = loop_depth;
    size_t saved_breaks = breakJumps.size(), saved_continues = continueJumps.size();
    loop_depth = 0;

    statement(); // body { ... }

    pop_func_ctx(); // L12
    current_func = prev_func;
    current_vararg_sid = prev_vararg;
    current_struct_type = prev_struct;
    current_this_sid = prev_this;
    loop_depth = saved_loop_depth;
    breakJumps.resize(saved_breaks);
    continueJumps.resize(saved_continues);

    emit(OP_RETURN_N, 0); // L5: method without return returns zero values
    patch(jump_over, (int)bytecode.size());
}

// Literal: Cars {"BMW", "X5", "Black", 1999} — fields in declared order
void Compiler::compile_struct_literal(const std::string& typeName) {
    StructType* t = struct_find(typeName);
    if (!t) throw std::runtime_error("Tipo desconhecido: " + typeName);
    int expected = (int)t->fields.size();

    consume(TK_LBRACE, "Esperado '{' para literal de " + typeName);
    int n = 0;
    if (!check(TK_RBRACE)) {
        do { expression(); n++; } while (match(TK_COMMA));
    }
    consume(TK_RBRACE, "Esperado '}' ao fechar literal de " + typeName);

    if (n != expected) {
        throw std::runtime_error("Literal de '" + typeName + "' espera " +
            std::to_string(expected) + " campos, encontrado " + std::to_string(n));
    }
    emit(OP_BUILD_STRUCT, n, 0, typeName);
}

int Compiler::check_builtin(const std::string& name) {
    if (builtinRegistry.count(name)) {
        // Exists (possibly with several arities) — compile_call via builtin_lookup
        // validates compatibility with argCount
        return 0;
    }
    return -2; // Not found
}    

inline int Compiler::builtin_find(const std::string& name) {
    return check_builtin(name);
}

// When emitting the code for function calls:
// L5: can a call return several values? Common builtins return 1
// (avoids packing/expanding in `println(len(x))`); select/unpack and
// user/method/indirect calls are treated as multiple.
static bool call_may_return_multi(const Instruction& ins) {
    switch (ins.op) {
        case OP_CALL:
            // require returns the module's 1st cached value
            return ins.s.str().rfind("require:", 0) != 0;
        case OP_CALL_INDIRECT:
        case OP_CALL_METHOD:
        case OP_CALL_SUPER:
            return true;
        case OP_CALL_BUILTIN: {
            const std::string& n = ins.s.str();
            return n == "select" || n == "unpack";
        }
        default:
            return false;
    }
}

// L5: reads arguments up to ')'. `...` can only be the LAST argument; a
// call in the last argument is also expanded (asks for all values and
// packs via OP_PACK_LAST). The encoded count is -argCount when there is
// expansion (the VM decodes fixed = -n - 1).
void Compiler::parse_call_args(int& argCount, bool& spread, const char* what) {
    argCount = 0;
    spread = false;
    if (check(TK_RPAREN)) return;
    do {
        if (check(TK_ELLIPSIS)) {
            if (current_vararg_sid < 0)
                throw std::runtime_error("'...' usado fora de funcao vararg (" +
                                         std::string(what) + ")");
            if (peek_n(1).type != TK_RPAREN)
                throw std::runtime_error("'...' deve ser o ultimo argumento de " +
                                         std::string(what));
            advance();
            emit(OP_LOAD, current_vararg_sid);
            argCount++;
            spread = true;
            break;
        }
        expression();
        argCount++;
        if (check(TK_RPAREN) && !bytecode.empty() &&
            call_may_return_multi(bytecode.back())) {
            bytecode.back().c = -1; // all values
            emit(OP_PACK_LAST);
            spread = true;
        }
    } while (match(TK_COMMA));
}

void Compiler::compile_call(std::string name, int argCount, bool spread) {
    int enc = spread ? -argCount : argCount;
    const Builtin* bi = builtin_lookup(name, argCount);
    if (!bi) {
        // Fallback: function from an imported `.mod` module (item 16). The module
        // registry is filled by compile itself (import statement),
        // so a call AFTER import(...) already resolves here.
        if (modules_is_callable(name, argCount)) {
            emit(OP_CALL_BUILTIN, -1, enc, name);
            return;
        }
        if (modules_known_function(name)) {
            throw std::runtime_error("Erro: " + name + " aceita " +
                                     std::to_string(modules_arity(name)) +
                                     " args (recebidos: " +
                                     std::to_string(argCount) + ")");
        }
        // It is a builtin, but no overload accepts this arity
        throw std::runtime_error("Erro: " + name + " aceita " +
                                 builtin_arities(name) + " args (recebidos: " +
                                 std::to_string(argCount) + ")");
    }
    // EMITS the id in `a` (O(1) dispatch) and the NAME in `in.s` (errors/debug)
    emit(OP_CALL_BUILTIN, bi->id, enc, name); 
}

// `for` (init/incr): an assignment expression ends in OP_STORE, which ALREADY
// pops the value; an unconditional OP_POP would eat the CALLER's stack when the
// loop runs inside an argument (e.g. printf("%d", f())) — at depth 0
// it was a silent no-op (if sp > 0) and masked the bug.
void Compiler::pop_if_left_value() {
    if (bytecode.empty()) return; // empty expression (for (; ...; ...))
    OpCode op = bytecode.back().op;
    if (op != OP_STORE && op != OP_STORE_FREE && op != OP_INDEX_STORE &&
        op != OP_FIELD_STORE && op != OP_POP && op != OP_INC_LOCAL &&
        op != OP_ADD_STORE && op != OP_SUB_STORE &&
        op != OP_ADD_STORE_K && op != OP_SUB_STORE_K &&
        op != OP_INC_JUMP && op != OP_LT_LOOP && op != OP_LE_LOOP &&
        op != OP_GT_LOOP && op != OP_GE_LOOP)
        emit(OP_POP);
}

// L5: adjusts the last call instruction to ask for N return values.
static bool set_last_call_results(std::vector<Instruction>& bc, int n) {
    if (bc.empty()) return false;
    switch (bc.back().op) {
        case OP_CALL:
        case OP_CALL_INDIRECT:
        case OP_CALL_BUILTIN:
        case OP_CALL_METHOD:
        case OP_CALL_SUPER:
        case OP_TAILCALL:
            bc.back().c = n;
            return true;
        default:
            return false;
    }
}

// --- GRAMMAR AND PRECEDENCE ---
void Compiler::statement() {
    // 1. CODE BLOCKS { ... }
    if (match(TK_LBRACE)) {
        while (!check(TK_RBRACE) && !is_at_end()) statement();
        consume(TK_RBRACE, "Esperado '}'");
    } 
    
    // 2. RESERVED KEYWORDS (Must come before is_function_definition)
    else if (match(TK_RETURN)) {
        // L5: comma-separated expression list (return a, b;).
        // `...` (and a call in the last value) expands all values.
        int nvals = 0;
        bool spreadReturn = false;
        if (!check(TK_PUNCT)) {
            do {
                if (check(TK_ELLIPSIS)) {
                    if (current_vararg_sid < 0)
                        throw std::runtime_error("'...' usado fora de funcao vararg");
                    if (peek_n(1).type != TK_PUNCT)
                        throw std::runtime_error("'...' deve ser o ultimo valor de return");
                    if (module_val_sid != -1 && current_func == -1)
                        throw std::runtime_error("return ... nao suportado em modulo");
                    advance();
                    emit(OP_LOAD, current_vararg_sid);
                    nvals++;
                    spreadReturn = true;
                    break;
                }
                expression();
                nvals++;
                if (nvals > 1 && check(TK_PUNCT) && !bytecode.empty() &&
                    call_may_return_multi(bytecode.back())) {
                    bytecode.back().c = -1;
                    emit(OP_PACK_LAST);
                    spreadReturn = true;
                }
            } while (match(TK_COMMA));
        }
        if (spreadReturn) {
            if (module_val_sid != -1 && current_func == -1)
                throw std::runtime_error("return com expansao nao suportado em modulo");
            consume(TK_PUNCT, "Esperado ';' apos return");
            emit(OP_RETURN_SPREAD, nvals - 1);
            return;
        }
        // E6: `return` at the top of a module body (require) stores the value
        // in __req_val_* before leaving — the 2nd require call returns the
        // SAME value (cache). The cache stores only the 1st value.
        if (module_val_sid != -1 && current_func == -1) {
            if (nvals == 0) { emit(OP_PUSH_NIL); nvals = 1; }
            else if (nvals > 1)
                throw std::runtime_error(
                    "return de modulo aceita no maximo 1 valor");
            else set_last_call_results(bytecode, 1); // cache = 1st value
            emit(OP_STORE, module_val_sid);
            emit(OP_LOAD, module_val_sid);
            nvals = 1;
        }
        // L10/L5: proper tail call — `return f(args);` reuses the current frame and
        // preserves ALL values returned by the callee (the caller
        // decides how many it wants). Only the DIRECT call becomes OP_TAILCALL; for
        // builtins/methods/indirect calls we ask for all values and use
        int retN = nvals;
        if (current_func != -1 && nvals == 1 && !bytecode.empty()) {
            OpCode rop = bytecode.back().op;
            if (rop == OP_CALL) {
                bytecode.back().op = OP_TAILCALL;
            } else if (rop == OP_CALL_BUILTIN || rop == OP_CALL_INDIRECT ||
                       rop == OP_CALL_METHOD || rop == OP_CALL_SUPER) {
                bytecode.back().c = -1;
                retN = -1;
            }
        }
        consume(TK_PUNCT, "Esperado ';' apos return");
        emit(OP_RETURN_N, retN); // in a tailcall this instruction is unreachable
    }

    else if (match(TK_BREAK)) {
        if (loop_depth == 0) throw std::runtime_error("'break' fora de loop");
        consume(TK_PUNCT, "Esperado ';' apos break");
        breakJumps.push_back(emit(OP_JUMP, 0));
    }

    else if (match(TK_CONTINUE)) {
        if (loop_depth == 0) throw std::runtime_error("'continue' fora de loop");
        consume(TK_PUNCT, "Esperado ';' apos continue");
        continueJumps.push_back(emit(OP_JUMP, 0));
    }

    // global a, b; — inside functions (e.g. draw()), assignments use the global
    // instead of creating a local (state shared between frames)
    else if (match(TK_GLOBAL)) {
        do {
            Token gname = consume(TK_ID, "Esperado nome apos 'global'");
            int gsid = sym_find(gname.lexeme.c_str());
            if (gsid == -1) gsid = sym_create(gname.lexeme.c_str(), {VAL_FLOAT});
            sym_get(gsid)->force_global = true;
        } while (match(TK_COMMA));
        consume(TK_PUNCT, "Esperado ';' apos a declaracao global");
    }

    // start/stop/pause/resume <name>(); — thread control (item 15).
    // Becomes PUSH_STR + CALL_BUILTIN (no new opcode): the handler validates
    // name/function/thread at runtime and appends the error line.
    else if (check(TK_START) || check(TK_STOP) || check(TK_PAUSE) ||
             check(TK_RESUME)) {
        Token kw = advance();
        Token name = consume(TK_ID, "Esperado nome da funcao apos '" + kw.lexeme + "'");
        consume(TK_LPAREN, "Esperado '(' apos o nome da funcao");
        consume(TK_RPAREN, "Esperado ')' — start/stop/pause/resume nao recebem argumentos");
        // OPTIONAL `;`: the user syntax (`start f()` with a comment at
        // the end of the line) does not use a semicolon; accept both formats
        match(TK_PUNCT);
        emit(OP_PUSH_STR, str_intern(name.lexeme));
        emit(OP_CALL_BUILTIN, builtin_id_for("thread_" + kw.lexeme, 1), 1,
             "thread_" + kw.lexeme);
        emit(OP_POP);
    }

    // import("file.mod"); — native module (item 16). Processed at
    // compile time (like include): functions enter the module registry and the
    // manifest variables become native symbols pointing to the library.
    else if (match(TK_IMPORT)) {
        consume(TK_LPAREN, "Esperado '(' apos import");
        Token pathTok = consume(TK_STRING,
                                "Esperado \"arquivo.mod\" em import(\"...\")");
        consume(TK_RPAREN, "Esperado ')' apos o caminho do modulo");
        // Sandbox (E8/neg18): import loads a native library at runtime
        if (!g_allow_modules)
            throw std::runtime_error("import desabilitado (allow_modules=false) (linha " +
                                     std::to_string(pathTok.line) + ")");
        try {
            modules_import(pathTok.lexeme);
        } catch (const std::runtime_error& e) {
            std::string msg = e.what();
            if (msg.find("linha ") == std::string::npos)
                msg += " (linha " + std::to_string(pathTok.line) + ")";
            throw std::runtime_error(msg);
        }
        match(TK_PUNCT); // optional ';' (same style as `start f()`)
    }

    else if (match(TK_IF)) {
        consume(TK_LPAREN, "Esperado '('");
        expression();
        consume(TK_RPAREN, "Esperado ')'");
        int jf = emit(OP_JUMP_IF_FALSE, 0);
        statement();
        // if/elseif/else chain (E6): each then executes a JUMP to the end;
        // each false condition falls through to the next. `else if` (two words)
        // stays valid: the else consumes a statement that may be an if.
        std::vector<int> ends;
        while (match(TK_ELSEIF)) {
            ends.push_back(emit(OP_JUMP, 0));     // then executed: leaves the chain
            patch(jf, (int)bytecode.size());      // false → new condition
            consume(TK_LPAREN, "Esperado '(' apos 'elseif'");
            expression();
            consume(TK_RPAREN, "Esperado ')' apos 'elseif'");
            jf = emit(OP_JUMP_IF_FALSE, 0);
            statement();
        }
        if (match(TK_ELSE)) {
            ends.push_back(emit(OP_JUMP, 0));
            patch(jf, (int)bytecode.size());
            statement();
        } else {
            patch(jf, (int)bytecode.size());
        }
        for (int e : ends) patch(e, (int)bytecode.size());
    } 
    
    // L10: repeat { ... } until (cond);  — body runs at least once.
    // continue jumps to the CONDITION evaluation; break leaves the loop.
    else if (match(TK_REPEAT)) {
        int start = (int)bytecode.size();
        size_t b0 = breakJumps.size(), c0 = continueJumps.size();
        loop_depth++;
        statement();
        loop_depth--;
        int condStart = (int)bytecode.size();
        for (size_t i = c0; i < continueJumps.size(); i++)
            patch(continueJumps[i], condStart);
        consume(TK_UNTIL, "Esperado 'until' apos o corpo do repeat");
        expression();
        match(TK_PUNCT); // optional ';' (Lua style)
        emit(OP_JUMP_IF_FALSE, start); // false condition -> repeats
        int exit_target = (int)bytecode.size();
        for (size_t i = b0; i < breakJumps.size(); i++)
            patch(breakJumps[i], exit_target);
        breakJumps.resize(b0);
        continueJumps.resize(c0);
    }

    // L10: goto label;  (resolved at the end of run(), per function)
    else if (match(TK_GOTO)) {
        Token name = consume(TK_ID, "Esperado nome do label apos 'goto'");
        consume(TK_PUNCT, "Esperado ';' apos goto");
        pendingGotos.push_back({current_func, name.lexeme,
                                emit(OP_JUMP, 0), name.line});
    }

    // L10: ::label::  (optionally followed by ';')
    else if (match(TK_DCOLON)) {
        Token name = consume(TK_ID, "Esperado nome do label entre '::'");
        consume(TK_DCOLON, "Esperado '::' apos o nome do label");
        match(TK_PUNCT);
        auto& m = labels[current_func];
        if (m.count(name.lexeme))
            throw std::runtime_error("Label duplicado: " + name.lexeme +
                                     " (linha " + std::to_string(name.line) + ")");
        m[name.lexeme] = (int)bytecode.size();
    }

    else if (match(TK_WHILE)) {
        int start = (int)bytecode.size();
        consume(TK_LPAREN, "("); 
        expression(); 
        consume(TK_RPAREN, ")");
        
        int jf = emit(OP_JUMP_IF_FALSE, 0);
        size_t b0 = breakJumps.size(), c0 = continueJumps.size();
        loop_depth++;
        statement();
        loop_depth--;
        emit(OP_JUMP, start);
        patch(jf, (int)bytecode.size());

        // break leaves after the loop; continue goes back to the condition test
        int exit_target = (int)bytecode.size();
        for (size_t i = b0; i < breakJumps.size(); i++) patch(breakJumps[i], exit_target);
        for (size_t i = c0; i < continueJumps.size(); i++) patch(continueJumps[i], start);
        breakJumps.resize(b0);
        continueJumps.resize(c0);
    }
    
    else if (match(TK_FOR)) {
        consume(TK_LPAREN, "Esperado '(' apos 'for'");

        // Initialization
        if (!match(TK_PUNCT)) {
            expression();
            consume(TK_PUNCT, "Esperado ';' apos inicializacao do for");
            pop_if_left_value();
        }

        int loop_start = (int)bytecode.size();

        // Condition
        int exit_jump = -1;
        if (!match(TK_PUNCT)) {
            expression();
            consume(TK_PUNCT, "Esperado ';' apos condicao do for");
            exit_jump = emit(OP_JUMP_IF_FALSE, 0);
        }

        // Increment (E16): compiled in a separate buffer and emitted AFTER the
        // body, in the classic layout [cond][JIF exit][body][incr][JMP cond].
        // Eliminates one unconditional jump per iteration (the old one went to the
        // body and came back to the increment). Internal jumps of the increment
        std::vector<Instruction> incr_code;
        bool has_incr = false;
        if (!match(TK_RPAREN)) {
            has_incr = true;
            std::vector<Instruction> outer = std::move(bytecode);
            bytecode.clear();
            expression();
            pop_if_left_value();
            incr_code = std::move(bytecode);
            bytecode = std::move(outer);
            consume(TK_RPAREN, "Esperado ')' apos clausulas do for");
        }

        size_t b0 = breakJumps.size(), c0 = continueJumps.size();
        loop_depth++;
        statement();
        loop_depth--;
        int incr_start = -1;
        if (has_incr) {
            incr_start = (int)bytecode.size();
            for (Instruction& ins : incr_code) {
                switch (ins.op) {
                    case OP_JUMP: case OP_JUMP_IF_FALSE:
                    case OP_LT_JIF_K: case OP_LE_JIF_K: case OP_GT_JIF_K:
                    case OP_GE_JIF_K: case OP_EQ_JIF_K: case OP_NE_JIF_K:
                        ins.a += incr_start; break;
                    default: break;
                }
                bytecode.push_back(ins);
            }
        }
        // E17a: loop fusion. If the increment is exactly one instruction
        // (INC_LOCAL or ADD_STORE_K/SUB_STORE_K over the same sid) and the
        // condition is a single *_JIF_K comparison against a constant, the
        // close becomes *_LOOP (increment + test + jump to the body).
        // Otherwise, fuse only INC+JUMP (INC_JUMP) for any
        // condition form.
        bool loop_fused = false;
        if (has_incr && incr_start >= 0 &&
            (int)bytecode.size() == incr_start + 1) {
            int isid = -1;
            double inc = 0.0;
            bool inc_ok = false;
            const Instruction& ii = bytecode[incr_start];
            if (ii.op == OP_INC_LOCAL) {
                isid = ii.a; inc = ii.n; inc_ok = true;
            } else if (ii.op == OP_ADD_STORE_K) {
                isid = ii.a; inc = ii.n; inc_ok = true;
            } else if (ii.op == OP_SUB_STORE_K) {
                isid = ii.a; inc = -ii.n; inc_ok = true;
            }
            Symbol* isym = isid >= 0 ? sym_get(isid) : nullptr;
            bool simple = isym && !isym->native && isym->scalar == SC_NONE &&
                          inc == std::floor(inc) &&
                          inc >= -2147483648.0 && inc <= 2147483647.0;
            if (inc_ok && simple) {
                OpCode fusedOp = OP_COUNT;
                if (exit_jump == loop_start &&
                    bytecode[loop_start].b == isid) {
                    switch (bytecode[loop_start].op) {
                        case OP_LT_JIF_K: fusedOp = OP_LT_LOOP; break;
                        case OP_LE_JIF_K: fusedOp = OP_LE_LOOP; break;
                        case OP_GT_JIF_K: fusedOp = OP_GT_LOOP; break;
                        case OP_GE_JIF_K: fusedOp = OP_GE_LOOP; break;
                        default: break;
                    }
                }
                bytecode.pop_back(); // removes the increment
                Instruction ins;
                ins.line = current_line;
                if (fusedOp != OP_COUNT) {
                    ins.op = fusedOp;
                    ins.a = loop_start + 1;          // body start
                    ins.b = isid;
                    ins.c = (int)inc;
                    ins.n = bytecode[loop_start].n;  // limit
                    long long lim = 0;
                    ins.s.id = double_exact_i64(ins.n, lim) ? -2 : -1;
                } else {
                    ins.op = OP_INC_JUMP;
                    ins.a = loop_start;
                    ins.b = isid;
                    ins.n = inc;
                    ins.c = ii.c; // L7: exact integer constant increment
                }
                bytecode.push_back(ins);
                loop_fused = true;
            }
        }
        if (!loop_fused) emit(OP_JUMP, loop_start);

        // continue target: increment (if any) and then the condition
        int continue_target = has_incr ? incr_start : loop_start;

        if (exit_jump != -1) {
            patch(exit_jump, (int)bytecode.size());
        }

        int exit_target = (int)bytecode.size();
        for (size_t i = b0; i < breakJumps.size(); i++) patch(breakJumps[i], exit_target);
        for (size_t i = c0; i < continueJumps.size(); i++) patch(continueJumps[i], continue_target);
        breakJumps.resize(b0);
        continueJumps.resize(c0);
    }

    // 3. TYPE DEFINITION (struct): type Name { ... };
    //    or CLASS with inheritance (E4): class Name extends Parent { ... };
    else if (match(TK_TYPE)) {
        compile_type_definition(false);
    }
    else if (match(TK_CLASS)) {
        compile_type_definition(true);
    }

    // 4. TYPED DECLARATION: Type name = value; (structs) or
    //    scalar_type name [= value] [ , ... ]; (E1c — annotates Symbol::scalar)
    else if (check(TK_ID) && peek_n(1).type == TK_ID && !is_function_definition()) {
        if (scalar_type_from_name(peek_n(0).lexeme) != SC_NONE) {
            compile_scalar_declaration(scalar_type_from_name(peek_n(0).lexeme));
        } else {
            Token typeTok = advance();
            Token nameTok = advance();
            if (!struct_find(typeTok.lexeme))
                throw std::runtime_error("Tipo desconhecido: " + typeTok.lexeme);
            consume(TK_ASSIGN, "Esperado '=' na declaracao de '" + nameTok.lexeme + "'");
            if (check(TK_LBRACE)) compile_struct_literal(typeTok.lexeme);
            else expression(); // existing value (e.g. a copy of another object)
            consume(TK_PUNCT, "Esperado ';' apos a declaracao de '" + nameTok.lexeme + "'");

            int sid;
            if (current_func != -1) {
                sid = sym_find_local(nameTok.lexeme.c_str(), current_func);
                if (sid == -1) sid = sym_create_local(nameTok.lexeme.c_str(), {VAL_FLOAT}, current_func);
            } else {
                sid = sym_find(nameTok.lexeme.c_str());
                if (sid == -1) sid = sym_create(nameTok.lexeme.c_str(), {VAL_FLOAT});
            }
            Symbol* s = sym_get(sid);
            if (s->is_function)
                throw std::runtime_error("'" + nameTok.lexeme + "' ja e uma funcao");
            s->type_name = typeTok.lexeme;
            emit(OP_STORE, sid);
        }
    }

    // 4b. `int x = 5;` / `float y = 3.0;` — C-style aliases (i32/f64, E1c)
    else if ((check(TK_INT) || check(TK_FLOAT)) && peek_n(1).type == TK_ID &&
             !is_function_definition()) {
        compile_scalar_declaration(check(TK_INT) ? SC_I32 : SC_F64);
    }

    // 5. FUNCTION DEFINITION (pattern name(...){ )
    // This check uses lookahead and must come after the keywords,
    // since function names are TK_ID.
    else if (is_function_definition()) {
        compile_function_definition();
    }

    // 6. GENERAL EXPRESSIONS (Assignments, function calls, arithmetic)
    else {
        // 1. Rejects isolated constants immediately
        if (check(TK_NUM) || check(TK_STRING)) {
            throw std::runtime_error("Expressao invalida: valor constante isolado nao produz efeito.");
        }

        // 2. Processes the expression (assignment, call, or arithmetic)
        expression(); 
        
        // 3. Checks what was emitted in the Bytecode
        if (!bytecode.empty()) {
            OpCode lastOp = bytecode.back().op;
            
            // If the last instruction is NOT something that "consumes" or "uses" the value...
            if (lastOp != OP_STORE && 
                lastOp != OP_STORE_FREE && // L12: x = ... in a closure
                lastOp != OP_CALL && 
                lastOp != OP_CALL_BUILTIN && 
                lastOp != OP_INDEX_STORE &&
                lastOp != OP_FIELD_STORE &&
                lastOp != OP_CALL_METHOD &&
                lastOp != OP_CALL_INDIRECT &&   // E2: x() with x = function value
                lastOp != OP_CALL_PCALL &&      // E2: pcall(...) as a statement
                lastOp != OP_CALL_SUPER &&      // E4: super.m(...) as a statement
                lastOp != OP_INC_LOCAL &&       // E15: x = x + k fused
                lastOp != OP_ADD_STORE &&       // E17a: x = x + e fused
                lastOp != OP_SUB_STORE &&       // E17a
                lastOp != OP_ADD_STORE_K &&     // E17a
                lastOp != OP_SUB_STORE_K &&     // E17a
                lastOp != OP_INC_JUMP &&        // E17
                lastOp != OP_LT_LOOP && lastOp != OP_LE_LOOP &&
                lastOp != OP_GT_LOOP && lastOp != OP_GE_LOOP &&
                lastOp != OP_POP) {             // E4: new T() ends in POP
                
                throw std::runtime_error("Resultado da expressao nao utilizado. Faltou atribuicao ou chamada de funcao?");
            }

            // Calls used as statements leave the return value on top
            if (lastOp == OP_CALL || lastOp == OP_CALL_BUILTIN ||
                lastOp == OP_CALL_METHOD || lastOp == OP_CALL_INDIRECT ||
                lastOp == OP_CALL_PCALL || lastOp == OP_CALL_SUPER)
                emit(OP_POP);
        }

        consume(TK_PUNCT, "Esperado ';' após a expressão");
        
        // Note: We do not emit OP_POP here because OP_STORE/OP_CALL
        // already handle the value destination in their current VM.
    }

}

void Compiler::expression() { assignment(); }

void Compiler::assignment() {
    if (check(TK_ID) && pos + 1 < (int)tokens.size()) {
        Token id = tokens[pos];
        TokenType next = tokens[pos+1].type;

        // --- MULTI-ASSIGNMENT (E6, negative 9): a, b = v1, v2 ---
        // Form `ID (, ID)+ =` detected before the other branches; the value
        // list is fully evaluated before any STORE, so
        // `a, b = b, a` really swaps (old values pushed first).
        if (next == TK_COMMA) {
            size_t i = pos;
            int ntargets = 0;
            while (i < tokens.size() && tokens[i].type == TK_ID) {
                ntargets++;
                if (i + 1 < tokens.size() && tokens[i+1].type == TK_COMMA) i += 2;
                else break;
            }
            bool pattern = ntargets >= 2 && i < tokens.size() &&
                           tokens[i].type == TK_ID && i + 1 < tokens.size() &&
                           tokens[i+1].type == TK_ASSIGN;
            if (pattern) {
                std::vector<Token> targets;
                do {
                    targets.push_back(consume(TK_ID, "Esperado nome do alvo"));
                } while (match(TK_COMMA));
                consume(TK_ASSIGN, "Esperado '=' na multi-atribuicao");
                int nvals = 0;
                do { expression(); nvals++; } while (match(TK_COMMA));
                if (nvals != (int)targets.size()) {
                    // L5: a single call can fill N targets
                    bool one_call = nvals == 1 && targets.size() > 1 &&
                                    set_last_call_results(bytecode,
                                                          (int)targets.size());
                    if (!one_call)
                        throw std::runtime_error("Multi-atribuicao: " +
                            std::to_string(targets.size()) + " alvo(s) e " +
                            std::to_string(nvals) + " valor(es)");
                }
                // vn on top → stores in reverse order (target[n-1] receives vn)
                for (int k = (int)targets.size() - 1; k >= 0; k--) {
                    const Token& t = targets[k];
                    if (in_struct_field(t.lexeme)) { // method field (__this)
                        emit(OP_LOAD, current_this_sid);
                        emit(OP_FIELD_STORE, 0, 0, t.lexeme);
                        continue;
                    }
                    int sid;
                    if (current_func != -1) {
                        sid = sym_find_local(t.lexeme.c_str(), current_func);
                        if (sid == -1) {
                            if (free_var(t.lexeme)) { // L12: upvalue
                                emit(OP_STORE_FREE, 0, 0, t.lexeme);
                                continue;
                            }
                            int gsid = sym_find(t.lexeme.c_str());
                            if (gsid != -1 && sym_get(gsid)->force_global)
                                sid = gsid;
                            else
                                sid = sym_create_local(t.lexeme.c_str(),
                                                       {VAL_FLOAT}, current_func);
                        }
                    } else {
                        sid = sym_find(t.lexeme.c_str());
                        if (sid == -1) sid = sym_create(t.lexeme.c_str(), {VAL_FLOAT});
                    }
                    emit(OP_STORE, sid);
                }
                return;
            }
        }

        // --- OBJECT FIELD ASSIGNMENT: obj.field op= value (chainable: a.b.c =) ---
        if (next == TK_DOT) {
            int i = pos + 1;
            int nseg = 0;
            while (i + 1 < (int)tokens.size() &&
                   tokens[i].type == TK_DOT && tokens[i+1].type == TK_ID) {
                i += 2;
                nseg++;
            }
            bool assignFollows = i < (int)tokens.size() &&
                (tokens[i].type == TK_ASSIGN     || tokens[i].type == TK_PLUS_ASSIGN ||
                 tokens[i].type == TK_MIN_ASSIGN || tokens[i].type == TK_MUL_ASSIGN ||
                 tokens[i].type == TK_DIV_ASSIGN);

            if (nseg > 0 && assignFollows) {
                Token idTok = advance();                 // base
                std::vector<Token> segs;
                while (match(TK_DOT))
                    segs.push_back(consume(TK_ID, "Esperado nome apos '.'"));
                TokenType opType = advance().type;       // operator

                bool baseFree = false;
                int sid = sym_find_local(idTok.lexeme.c_str(), current_func);
                if (sid == -1) {
                    if (free_var(idTok.lexeme)) baseFree = true; // L12
                    else sid = sym_find(idTok.lexeme.c_str());
                }
                if (!baseFree && sid == -1)
                    throw std::runtime_error("Variavel '" + idTok.lexeme + "' nao declarada.");
                Symbol* baseSym = baseFree ? nullptr : sym_get(sid);
                if (baseSym && baseSym->is_function)
                    throw std::runtime_error("'" + idTok.lexeme + "' e uma funcao");

                // Early validation of the 1st segment when the type is annotated
                if (baseSym && !baseSym->type_name.empty() &&
                    struct_find(baseSym->type_name)) {
                    const std::string& tn = baseSym->type_name;
                    const std::string& s0 = segs[0].lexeme;
                    if (struct_field_index(tn, s0) == -1) {
                        if (struct_method_sid(tn, s0) != -1)
                            throw std::runtime_error("'" + s0 + "' e um metodo; nao aceita atribuicao");
                        throw std::runtime_error("Campo '" + s0 + "' nao existe em '" + tn + "'");
                    }
                }

                // path to the object holding the final field (parent)
                auto emitPath = [&]() {
                    if (baseFree) emit(OP_LOAD_FREE, 0, 0, idTok.lexeme);
                    else emit(OP_LOAD, sid);
                    for (size_t k = 0; k + 1 < segs.size(); k++)
                        emit(OP_FIELD_LOAD, 0, 0, segs[k].lexeme);
                };

                emitPath(); // [parent]
                if (opType != TK_ASSIGN) {
                    emitPath();                                   // [parent, parent]
                    emit(OP_FIELD_LOAD, 0, 0, segs.back().lexeme); // [parent, current]
                }
                expression(); // [parent, (current), value]
                if (opType == TK_PLUS_ASSIGN)      emit(OP_ADD);
                else if (opType == TK_MIN_ASSIGN) emit(OP_SUB);
                else if (opType == TK_MUL_ASSIGN) emit(OP_MUL);
                else if (opType == TK_DIV_ASSIGN) emit(OP_DIV);
                emit(OP_FIELD_STORE, 0, 0, segs.back().lexeme);
                return;
            }
        }

        // --- METHOD FIELD (via __this): field op= value ---
        if (in_struct_field(id.lexeme) &&
            (next == TK_ASSIGN    || next == TK_PLUS_ASSIGN || next == TK_MIN_ASSIGN ||
             next == TK_MUL_ASSIGN || next == TK_DIV_ASSIGN)) {

            advance(); // id (the field)
            TokenType opType = advance().type;

            emit(OP_LOAD, current_this_sid);
            if (opType != TK_ASSIGN) {
                emit(OP_LOAD, current_this_sid);
                emit(OP_FIELD_LOAD, 0, 0, id.lexeme);
            }
            expression();
            if (opType == TK_PLUS_ASSIGN)      emit(OP_ADD);
            else if (opType == TK_MIN_ASSIGN) emit(OP_SUB);
            else if (opType == TK_MUL_ASSIGN) emit(OP_MUL);
            else if (opType == TK_DIV_ASSIGN) emit(OP_DIV);
            emit(OP_FIELD_STORE, 0, 0, id.lexeme);
            return;
        }

        // Support for A[i] = value (INDEX_STORE)
        if (next == TK_LBRACKET) {
             // Guard against confusing this with a primary expression
             // The parser must look beyond the closing bracket
        }

        if (next == TK_ASSIGN || next == TK_PLUS_ASSIGN || next == TK_MIN_ASSIGN || 
            next == TK_MUL_ASSIGN || next == TK_DIV_ASSIGN) {
            
            advance(); // id
            TokenType opType = advance().type;

            double const_cv = 0.0;
            if (math_constant_value(id.lexeme, const_cv))
                throw std::runtime_error("'" + id.lexeme + "' e uma constante e nao pode ser atribuida");

            // Assignment: inside a function creates/uses a LOCAL; at top level, GLOBAL.
            // `global x` (declared in the script) makes the write go to the global
            // — without it, x = x + 4 inside draw() would create a new local
            // on every frame ("reset" state).
            int sid = -1;
            bool isFree = false;
            if (current_func != -1) {
                sid = sym_find_local(id.lexeme.c_str(), current_func);
                if (sid == -1) {
                    if (free_var(id.lexeme)) {
                        isFree = true; // L12: updates the outer-scope upvalue
                    } else {
                        int gsid = sym_find(id.lexeme.c_str());
                        if (gsid != -1 && sym_get(gsid)->force_global)
                            sid = gsid;
                        else
                            sid = sym_create_local(id.lexeme.c_str(), {VAL_FLOAT}, current_func);
                    }
                }
            } else {
                sid = sym_find(id.lexeme.c_str());
                if (sid == -1) sid = sym_create(id.lexeme.c_str(), {VAL_FLOAT});
            }

            if (isFree) {
                if (opType != TK_ASSIGN)
                    emit(OP_LOAD_FREE, 0, 0, id.lexeme);
                expression();
                if (opType == TK_PLUS_ASSIGN)      emit(OP_ADD);
                else if (opType == TK_MIN_ASSIGN) emit(OP_SUB);
                else if (opType == TK_MUL_ASSIGN) emit(OP_MUL);
                else if (opType == TK_DIV_ASSIGN) emit(OP_DIV);
                emit(OP_STORE_FREE, 0, 0, id.lexeme);
                return;
            }

            int load_idx = -1;
            if (opType != TK_ASSIGN) {
                emit(OP_LOAD, sid);
                load_idx = (int)bytecode.size() - 1;
            }

            size_t rhs_start = bytecode.size();
            expression(); 

            // E17: s = s + e  /  s += e  ->  ADD_STORE
            if (opType == TK_ASSIGN && (int)bytecode.size() > (int)rhs_start &&
                bytecode[rhs_start].op == OP_LOAD && bytecode[rhs_start].a == sid)
                load_idx = (int)rhs_start;
            if (load_idx >= 0 && try_fuse_store(sid, load_idx))
                return;

            if (opType == TK_PLUS_ASSIGN)      emit(OP_ADD);
            else if (opType == TK_MIN_ASSIGN) emit(OP_SUB);
            else if (opType == TK_MUL_ASSIGN) emit(OP_MUL);
            else if (opType == TK_DIV_ASSIGN) emit(OP_DIV);

            emit(OP_STORE, sid);
            return;
        }
    }
    logical_or();
}

void Compiler::logical_or() {
    logical_and();
    while (match(TK_OR)) { logical_and(); emit(OP_OR); }
}

void Compiler::logical_and() {
    bitwise_or();
    while (match(TK_AND)) { bitwise_or(); emit(OP_AND); }
}

void Compiler::bitwise_or() {
    bitwise_xor();
    while (match(TK_BIT_OR)) { bitwise_xor(); emit(OP_BIT_OR); }
}

void Compiler::bitwise_xor() {
    bitwise_and();
    while (match(TK_BIT_XOR)) { bitwise_and(); emit(OP_BIT_XOR); }
}

void Compiler::bitwise_and() {
    equality();
    while (match(TK_BIT_AND)) { equality(); emit(OP_BIT_AND); }
}

void Compiler::equality() {
    comparison();
    while (true) {
        if (match(TK_EQ)) { comparison(); emit(OP_EQ); }
        else if (match(TK_NE)) { comparison(); emit(OP_NE); }
        else break;
    }
}

void Compiler::comparison() {
    shift();
    while (true) {
        if (match(TK_LT)) { shift(); emit(OP_LT); }
        else if (match(TK_GT)) { shift(); emit(OP_GT); }
        else if (match(TK_LE)) { shift(); emit(OP_LE); }
        else if (match(TK_GE)) { shift(); emit(OP_GE); }
        else break;
    }
}

void Compiler::shift() {
    term();
    while (true) {
        if (match(TK_SHL)) { term(); emit(OP_SHL); }
        else if (match(TK_SHR)) { term(); emit(OP_SHR); }
        else break;
    }
}

void Compiler::term() {
    factor();
    while (true) {
        if (match(TK_PLUS)) { factor(); emit(OP_ADD); }
        else if (match(TK_MINUS)) { factor(); emit(OP_SUB); }
        else break;
    }
}

void Compiler::factor() {
    unary();
    while (true) {
        if (match(TK_MUL)) { unary(); emit(OP_MUL); }
        else if (match(TK_DIV)) { unary(); emit(OP_DIV); }
        else if (match(TK_MOD)) { unary(); emit(OP_MOD); }
        else break;
    }
}

void Compiler::unary() {
    if (match(TK_MINUS)) {
        emit(OP_PUSH, 0, 0, "0");
        unary();
        emit(OP_SUB);
    } else if (match(TK_BIT_NOT)) {
        unary();
        emit(OP_BIT_NOT);
    } else {
        primary();
    }
}

void Compiler::primary() {
    if (match(TK_NUM)) {
        emit(OP_PUSH, 0, 0, previous().lexeme);
    } 
    else if (match(TK_STRING)) {
        emit(OP_PUSH_STR, str_intern(previous().lexeme));
    }
    else if (match(TK_NIL)) {
        emit(OP_PUSH_NIL); // L6
    }
    else if (match(TK_LBRACKET)) {
        int rows = 0, cols = 0;
        if (check(TK_LBRACKET)) {
            while (match(TK_LBRACKET)) {
                int c = 0;
                do { expression(); c++; } while (match(TK_COMMA));
                consume(TK_RBRACKET, "Esperado ']' ao fechar linha.");
                if (rows == 0) cols = c;
                rows++;
                match(TK_COMMA); 
            }
        } else {
            if (!check(TK_RBRACKET)) {
                do { expression(); cols++; } while (match(TK_COMMA));
            }
            rows = 1;
        }
        consume(TK_RBRACKET, "Esperado ']' ao fechar matriz.");
        emit(OP_BUILD_ARRAY, rows, cols);
    }
    // Boolean literals (E1c): true -> 1, false -> 0 (never go through the
    // symbol table; they are literals, cannot be overwritten)
    else if (check(TK_ID) &&
             (peek_n(0).lexeme == "true" || peek_n(0).lexeme == "false")) {
        bool isTrue = advance().lexeme == "true";
        emit(OP_PUSH, 0, 0, isTrue ? "1" : "0");
    }
    // --- INSTANCE (E4): new T(args) — creates, copies (DUP), calls init, discards
    // init's return and leaves THE INSTANCE on top ([inst] final).
    else if (match(TK_NEW)) {
        Token tname = consume(TK_ID, "Esperado nome do tipo apos 'new'");
        if (!struct_find(tname.lexeme))
            throw std::runtime_error("Tipo desconhecido: " + tname.lexeme);
        if (struct_method_sid(tname.lexeme, "init") == -1)
            throw std::runtime_error("Tipo '" + tname.lexeme + "' nao tem metodo 'init'");
        consume(TK_LPAREN, "Esperado '(' apos nome do tipo em 'new'");
        emit(OP_NEW_INSTANCE, 0, 0, tname.lexeme); // [inst] BEFORE the args
        emit(OP_DUP);                              // [inst, inst]
        int argCount = 0;
        bool spread = false;
        parse_call_args(argCount, spread, "new");
        consume(TK_RPAREN, "Esperado ')' apos argumentos de new");
        if (argCount > 16)
            throw std::runtime_error("new aceita no maximo 16 argumentos");
        emit(OP_CALL_METHOD, 0, spread ? -argCount : argCount, "init"); // consumes 1 copy + args
        emit(OP_POP);                              // discards init's return
    }
    // --- SUPER (E4): super.m(args) — parent class method, current __this ---
    else if (match(TK_SUPER)) {
        consume(TK_DOT, "Esperado '.' apos 'super'");
        Token m = consume(TK_ID, "Esperado nome do metodo apos 'super.'");
        if (current_struct_type.empty())
            throw std::runtime_error("'super' fora de metodo de classe");
        StructType* ct = struct_find(current_struct_type);
        if (!ct || ct->parent.empty())
            throw std::runtime_error("Tipo '" + current_struct_type +
                                     "' nao estende outra classe");
        int msid = struct_method_sid(ct->parent, m.lexeme); // starts at the parent
        if (msid == -1)
            throw std::runtime_error("Metodo '" + m.lexeme +
                "' nao existe em '" + ct->parent + "'");
        consume(TK_LPAREN, "Esperado '(' apos nome do metodo em 'super'");
        emit(OP_LOAD, current_this_sid); // [this] — method call layout
        int argCount = 0;
        bool spread = false;
        parse_call_args(argCount, spread, "super");
        consume(TK_RPAREN, "Esperado ')' apos argumentos de super");
        if (argCount > 16)
            throw std::runtime_error("super aceita no maximo 16 argumentos");
        emit(OP_CALL_SUPER, sym_get(msid)->entry_point,
             spread ? -argCount : argCount,
             current_struct_type + "::" + m.lexeme);
    }
    // --- TABLE LITERAL (E3): {key = value, ...} ---
    // No type prefix: `Type{...}` (TK_ID + '{') remains a struct
    // literal; bare '{' is a table. Keys = names; dynamic keys via
    else if (match(TK_LBRACE)) {
        int n = 0;
        if (!check(TK_RBRACE)) {
            do {
                Token k = consume(TK_ID, "Esperado nome da chave da tabela");
                consume(TK_ASSIGN, "Esperado '=' apos a chave da tabela");
                emit(OP_PUSH_STR, str_intern(k.lexeme));
                expression();
                n++;
            } while (match(TK_COMMA));
        }
        consume(TK_RBRACE, "Esperado '}' ao fechar tabela");
        emit(OP_BUILD_TABLE, n);
    }
    // --- ANONYMOUS FUNCTION (E2): fn(x) { ... } ---
    // Expression that produces a VAL_FUNC without captures (upvalues out of scope).
    // Mirrors compile_function_definition: JUMP over, body compiled as a
    // normal function, OP_RETURN + patch, and in the normal flow emits OP_PUSH_FUNC.
    // IIFE not supported: assign before calling (g = fn(x){...}; g(1)).
    else if (match(TK_FN)) {
        consume(TK_LPAREN, "Esperado '(' apos fn");
        int jump_over = emit(OP_JUMP, 0);
        int entry_point = (int)bytecode.size();

        int prev_func = current_func;
        int prev_vararg = current_vararg_sid;
        current_func = entry_point + code_base;
        current_vararg_sid = -1;
        push_func_ctx(current_func, true); // L12: anonymous fn captures upvalues

        std::vector<int> params;
        bool is_vararg = false;
        int vararg_sid = -1;
        if (!check(TK_RPAREN)) {
            do {
                if (check(TK_ELLIPSIS)) { // L5: fn(...) vararg
                    advance();
                    is_vararg = true;
                    vararg_sid = sym_find_local("__varargs_l5", current_func);
                    if (vararg_sid == -1)
                        vararg_sid = sym_create_local("__varargs_l5", {VAL_FLOAT}, current_func);
                    break;
                }
                Token p = consume(TK_ID, "Esperado nome do parametro de fn");
                int sid = sym_find_local(p.lexeme.c_str(), current_func);
                if (sid == -1) sid = sym_create_local(p.lexeme.c_str(), {VAL_FLOAT}, current_func);
                params.push_back(sid);
            } while (match(TK_COMMA));
        }
        consume(TK_RPAREN, "Esperado ')' apos parametros de fn");

        if (is_vararg) emit(OP_COLLECT_VARARGS, (int)params.size(), vararg_sid);
        else emit(OP_BIND_PARAMS, (int)params.size());
        current_vararg_sid = vararg_sid;

        for (int i = (int)params.size() - 1; i >= 0; i--)
            emit(OP_STORE, params[i]);

        int saved_loop_depth = loop_depth;
        size_t saved_breaks = breakJumps.size(), saved_continues = continueJumps.size();
        loop_depth = 0;

        statement(); // body: block '{ ... }' (statement handles '{')

        pop_func_ctx(); // L12
        current_func = prev_func;
        current_vararg_sid = prev_vararg;
        loop_depth = saved_loop_depth;
        breakJumps.resize(saved_breaks);
        continueJumps.resize(saved_continues);

        emit(OP_RETURN_N, 0); // L5: default return without values
        patch(jump_over, (int)bytecode.size());
        // L12: current_func is now the outer scope; it is the owner whose
        // locals will be migrated to cells when the closure is created.
        emit(OP_PUSH_CLOSURE, entry_point + code_base, current_func, "anonima");
    }
    // --- require("file") (E6): script module preloaded at compile time ---
    // Only the exact form `require("...")`; `require` as a value/variable falls
    // into the normal ID branch. The module runs ONCE (guard in __req_flag_*)
    // and returns the body's `return` value (or 0; cache in __req_val_*).
    else if (check(TK_ID) && peek_n(0).lexeme == "require" &&
             peek_n(1).type == TK_LPAREN && peek_n(2).type == TK_STRING) {
        advance(); // require
        advance(); // (
        Token pathTok = advance(); // "file"
        consume(TK_RPAREN, "Esperado ')' apos require(\"...\")");
        auto it = requiredModules.find(pathTok.lexeme);
        if (it == requiredModules.end())
            throw std::runtime_error("require: modulo '" + pathTok.lexeme +
                                     "' nao carregado (linha " +
                                     std::to_string(pathTok.line) + ")");
        // LOCAL entry + code_base = global address (same as OP_PUSH_FUNC)
        emit(OP_CALL, it->second.entry + code_base, 0,
             "require:" + pathTok.lexeme);
    }
    // --- VARARGS (L5): in an ordinary expression, ... yields the 1st extra ---
    else if (match(TK_ELLIPSIS)) {
        if (current_vararg_sid < 0)
            throw std::runtime_error("'...' usado fora de funcao vararg");
        emit(OP_VARARG_FIRST, current_vararg_sid);
    }
    else if (match(TK_ID)) {
        std::string name = previous().lexeme;

        // --- METHOD FIELD (direct access, via __this) ---
        // E.g.: inside show(), 'brand' becomes __this + FIELD_LOAD
        bool baseLoaded = false;
        if (in_struct_field(name)) {
            if (match(TK_LPAREN))
                throw std::runtime_error("'" + name + "' e um campo, nao uma funcao");
            emit(OP_LOAD, current_this_sid);
            emit(OP_FIELD_LOAD, 0, 0, name);
            baseLoaded = true;
        }

        // --- MATH CONSTANTS (literals; do not become symbols) ---
        if (!baseLoaded) {
            double cv = 0.0;
            if (math_constant_value(name, cv)) {
                if (check(TK_LPAREN))
                    throw std::runtime_error("'" + name + "' e uma constante, nao uma funcao");
                emit(OP_PUSH, 0, 0, math_constant_literal(cv));
                return;
            }
        }

        // --- FUNCTION CALL HANDLING ---
        if (!baseLoaded && match(TK_LPAREN)) {
            // eval(code) is pure sugar for load(code)(): compiles the chunk
            // now and calls the returned function with zero arguments.
            if (name == "eval") {
                if (check(TK_RPAREN))
                    throw std::runtime_error(
                        "eval: informe o codigo (ex.: eval(\"return 1;\"))");
                expression();
                if (match(TK_COMMA))
                    throw std::runtime_error("eval: aceita apenas um argumento");
                consume(TK_RPAREN, "Esperado ')' apos o codigo de eval");
                compile_call("load", 1, false);
                emit(OP_CALL_INDIRECT, 0);
                return;
            }

            // The mode is decided BEFORE the arguments: the indirect call
            // must push the function value BEFORE the args ([f, args...])
            enum CallMode { M_BUILTIN, M_DIRECT, M_INDIRECT, M_INDIRECT_FREE,
                            M_MODULE, M_PCALL };
            CallMode mode;
            int directEntry = -1, indirectSid = -1;
            if (name == "pcall") {
                // E2: protected call. The 1st arg (the function) is already pushed
                // by the normal expression() — pcall(f, x) => [f, x].
                mode = M_PCALL;
            } else if (builtinRegistry.count(name)) {
                mode = M_BUILTIN;
            } else if (free_var(name)) {
                mode = M_INDIRECT_FREE; // L12: function in an upvalue
            } else {
                // L12: locals can also hold functions (e.g. inside
                // a function, `m = fn(...){...}; m(5)`).
                int sid = sym_find_local(name.c_str(), current_func);
                if (sid == -1) sid = sym_find(name.c_str());
                Symbol* s = (sid != -1) ? sym_get(sid) : nullptr;
                if (s && s->is_function) {
                    mode = M_DIRECT;
                    directEntry = s->entry_point;
                } else if (s) {
                    mode = M_INDIRECT;                // function value in a variable (E2)
                    indirectSid = sid;
                } else if (modules_known_function(name)) {
                    mode = M_MODULE;
                } else {
                    throw std::runtime_error("Funcao nao definida: " + name);
                }
            }
            if (mode == M_INDIRECT)
                emit(OP_LOAD, indirectSid); // function value below the args
            else if (mode == M_INDIRECT_FREE)
                emit(OP_LOAD_FREE, 0, 0, name); // L12: upvalue

            int argCount = 0;
            bool spread = false;
            parse_call_args(argCount, spread, name.c_str());
            consume(TK_RPAREN, "Esperado ')' apos argumentos.");
            int enc = spread ? -argCount : argCount;

            if (mode == M_PCALL) {
                if (argCount < 1)
                    throw std::runtime_error("pcall: informe a funcao (ex.: pcall(f))");
                emit(OP_CALL_PCALL, enc);
            } else if (mode == M_BUILTIN) {
                compile_call(name, argCount, spread);
            } else if (mode == M_DIRECT) {
                // field 'a' = entry address; 's' = name (traceback E2)
                emit(OP_CALL, directEntry, enc, name);
            } else if (mode == M_INDIRECT || mode == M_INDIRECT_FREE) {
                emit(OP_CALL_INDIRECT, enc);
            } else {
                compile_call(name, argCount, spread); // .mod module function
            }
        } 
        // --- STRUCT LITERAL IN EXPRESSION: Point{1, 2} ---
        else if (!baseLoaded && check(TK_LBRACE)) {
            if (!struct_find(name))
                throw std::runtime_error("Tipo desconhecido: " + name);
            compile_struct_literal(name);
            return;
        }
        // --- OBJECT ACCESS: obj.field / obj.method(args) / chained a.b.c ---
        else if (!baseLoaded && match(TK_DOT)) {
            bool baseFree = false;
            int sid = sym_find_local(name.c_str(), current_func);
            if (sid == -1) {
                if (free_var(name)) baseFree = true; // L12: upvalue
                else sid = sym_find(name.c_str());
            }
            if (!baseFree && sid == -1)
                throw std::runtime_error("Variavel '" + name + "' nao declarada.");
            Symbol* baseSym = baseFree ? nullptr : sym_get(sid);
            if (baseSym && baseSym->is_function)
                throw std::runtime_error("'" + name + "' e uma funcao; nao possui campos");
            if (baseFree) emit(OP_LOAD_FREE, 0, 0, name);
            else emit(OP_LOAD, sid); // object (this) — below the args

            bool first = true; // only the 1st segment has a type known at compile time
            while (true) {
                Token fld = consume(TK_ID, "Esperado nome apos '.'");
                bool hasCall = check(TK_LPAREN);
                bool known = false, isField = false, isMethod = false;

                // Early validation when the annotated type is known
                if (first && baseSym && !baseSym->type_name.empty() &&
                    struct_find(baseSym->type_name)) {
                    const std::string& tn = baseSym->type_name;
                    known = true;
                    isField = struct_field_index(tn, fld.lexeme) != -1;
                    isMethod = struct_method_sid(tn, fld.lexeme) != -1;
                    if (!isField && !isMethod) {
                        throw std::runtime_error(
                            std::string(hasCall ? "Metodo '" : "Campo '") + fld.lexeme +
                            "' nao existe em '" + tn + "'");
                    }
                }
                first = false;

                if (hasCall && known && isField)
                    throw std::runtime_error("'" + fld.lexeme +
                        "' e um campo; use '" + name + "." + fld.lexeme + "'");

                if (match(TK_LPAREN)) {
                    // --- METHOD CALL: obj.method(args) ---
                    int argCount = 0;
                    bool spread = false;
                    parse_call_args(argCount, spread, fld.lexeme.c_str());
                    consume(TK_RPAREN, "Esperado ')' apos argumentos de '" + fld.lexeme + "'");
                    if (argCount > 16)
                        throw std::runtime_error("Metodo aceita no maximo 16 argumentos");
                    emit(OP_CALL_METHOD, 0, spread ? -argCount : argCount, fld.lexeme);
                } else {
                    // --- FIELD READ: obj.field ---
                    if (known && isMethod)
                        throw std::runtime_error("'" + fld.lexeme +
                            "' e um metodo; use '" + name + "." + fld.lexeme + "()'");
                    emit(OP_FIELD_LOAD, 0, 0, fld.lexeme);
                }

                if (!match(TK_DOT)) break; // next chained segment (c.p.x)
            }
        }
        // --- INDEXING A[i] AND SLICES A[a:b] ---
        else if (match(TK_LBRACKET)) {
            if (!baseLoaded) { // method field is already on the stack
                int sid = sym_find_local(name.c_str(), current_func);
                bool baseFree = false;
                if (sid == -1) {
                    if (free_var(name)) baseFree = true; // L12: upvalue
                    else sid = sym_find(name.c_str());
                }
                if (!baseFree && sid == -1)
                    throw std::runtime_error("Variavel '" + name + "' nao declarada.");
                if (baseFree) emit(OP_LOAD_FREE, 0, 0, name);
                else emit(OP_LOAD, sid);
            }

            // OP_INDEX_SLICE flags: bit1 = start present, bit0 = end present
            if (match(TK_COLON)) {
                // v[:b] or v[:] — start absent (placeholder 0)
                emit(OP_PUSH, 0, 0, "0");
                int flags = 0;
                if (!check(TK_RBRACKET)) { expression(); flags |= 1; }
                else emit(OP_PUSH, 0, 0, "0");
                consume(TK_RBRACKET, "Esperado ']' apos fatia.");
                emit(OP_INDEX_SLICE, flags);
                if (match(TK_ASSIGN)) throw std::runtime_error("Atribuicao em fatia nao suportada");
            } else {
                expression(); // single index or slice start
                if (match(TK_COLON)) {
                    int flags = 2; // start present
                    if (!check(TK_RBRACKET)) { expression(); flags |= 1; }
                    else emit(OP_PUSH, 0, 0, "0");
                    consume(TK_RBRACKET, "Esperado ']' apos fatia.");
                    emit(OP_INDEX_SLICE, flags);
                    if (match(TK_ASSIGN)) throw std::runtime_error("Atribuicao em fatia nao suportada");
                } else {
                    consume(TK_RBRACKET, "Esperado ']'");
                    if (match(TK_ASSIGN)) {
                        expression(); // value to be stored
                        emit(OP_INDEX_STORE);
                    } else {
                        emit(OP_INDEX_LOAD);
                    }
                }
            }
        }
        // --- SIMPLE VARIABLE HANDLING ---
        else {
            if (baseLoaded) return; // field value is already on the stack
            // Scope: current function local -> upvalue -> global -> create.
            int sid = sym_find_local(name.c_str(), current_func);
            if (sid == -1 && free_var(name)) {
                emit(OP_LOAD_FREE, 0, 0, name); // L12: lexical capture
                return;
            }
            if (sid == -1) sid = sym_find(name.c_str());
            if (sid == -1) {
                if (current_func != -1)
                    sid = sym_create_local(name.c_str(), {VAL_FLOAT}, current_func);
                else
                    sid = sym_create(name.c_str(), {VAL_FLOAT});
            }
            // Bare function reference (E2): becomes a VAL_FUNC value in the VM
            // (`f = twice; g = fn(x){...}`; the call uses OP_CALL_INDIRECT)
            emit(OP_LOAD, sid);
        }
    } 
    else if (match(TK_LPAREN)) {
        expression();
        consume(TK_RPAREN, "Esperado ')'");
    }
    else {
        // No valid expression-start token (e.g. 'a +;' or 'if (x < )')
        throw std::runtime_error("Esperado expressao");
    }
}

// E19 (fix): the fusion `x = x OP expr` is only valid if the LOAD of `x` is
// the LEFT operand of the final OP. Simulates the stack depth relative
// to the loaded value (depth 1): if an intermediate op consumes that
// value (e.g. `x = x * y + z`, where MUL consumes the loaded x), do not fuse.
// Without this, `x = x * y + z` generated ADD_STORE with an insufficient stack
// (Stack underflow) — a regression found by the complex benchmark.
static bool store_fusion_ok(const std::vector<Instruction>& code,
                            size_t load_idx, OpCode final_op) {
    int depth = 1; // the value loaded from `x`
    for (size_t i = load_idx + 1; i + 1 < code.size(); i++) {
        switch (code[i].op) {
            case OP_LOAD: case OP_PUSH: case OP_PUSH_STR:
            case OP_PUSH_FUNC: case OP_DUP:
                depth++; break;
            case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_MOD:
            case OP_EQ: case OP_NE: case OP_LT: case OP_LE: case OP_GT: case OP_GE:
            case OP_AND: case OP_OR:
            case OP_BIT_AND: case OP_BIT_OR: case OP_BIT_XOR:
            case OP_SHL: case OP_SHR:
                if (depth < 3) return false; // would consume the value of `x`
                depth--;
                break;
            case OP_ADD_K: case OP_SUB_K: case OP_MUL_K: case OP_DIV_K: case OP_MOD_K:
            case OP_EQ_K: case OP_NE_K: case OP_LT_K: case OP_LE_K:
            case OP_GT_K: case OP_GE_K:
            case OP_BIT_NOT:
                if (depth < 2) return false; // would consume the value of `x`
                break;
            case OP_INDEX_LOAD:
                if (depth < 3) return false;
                depth--;
                break;
            case OP_FIELD_LOAD:
                if (depth < 2) return false;
                break;
            default:
                return false; // calls, jumps, compound literals: conservative
        }
    }
    // OP_K consumes only `x` (immediate constant); a binary OP consumes `x` + 1 value.
    return (final_op == OP_ADD_K || final_op == OP_SUB_K) ? depth == 1
                                                          : depth == 2;
}

// HELPERS KEPT IN FULL
bool Compiler::try_fuse_store(int sid, int load_idx) {
    Symbol* sym = sym_get(sid);
    if (!sym || sym->native || sym->scalar != SC_NONE) return false;
    OpCode st = OP_COUNT;
    double k = 0.0;
    int int_const = 0;
    switch (bytecode.back().op) {
        case OP_ADD:   st = OP_ADD_STORE;   break;
        case OP_SUB:   st = OP_SUB_STORE;   break;
        case OP_ADD_K: st = OP_ADD_STORE_K; k = bytecode.back().n;
                       int_const = bytecode.back().c; break;
        case OP_SUB_K: st = OP_SUB_STORE_K; k = bytecode.back().n;
                       int_const = bytecode.back().c; break;
        default: return false;
    }
    // E19: only fuses if the LOAD of `sid` is the left operand of the final OP
    // (no jumps/calls/side effects in between — order preserved).
    if (!store_fusion_ok(bytecode, (size_t)load_idx, bytecode.back().op))
        return false;
    bytecode.pop_back();                          // removes ADD/SUB[_K]
    bytecode.erase(bytecode.begin() + load_idx);  // removes LOAD sid
    Instruction ins;
    ins.op = st; ins.a = sid; ins.n = k; ins.c = int_const;
    ins.line = current_line;
    bytecode.push_back(ins);
    return true;
}

int Compiler::emit(OpCode op, int a, int b, std::string s) {
    // E9/neg22: accumulated bytecode ceiling in the REPL (0 = no limit).
    if (g_code_limit > 0 &&
        (long long)code_base + (long long)bytecode.size() >= g_code_limit)
        throw std::runtime_error(
            "limite de bytecode do REPL atingido (" +
            std::to_string(g_code_limit) +
            " instrucoes; ajuste MYINTERP_REPL_CODE_LIMIT)");
    if (op == OP_PUSH_CLOSURE) g_program_has_closures = true;
    // --- E15: fusion/specialization at emission time --------------------
    // (1) binary with constant RHS: [.. PUSH c] OP -> [.. OP_K c]
    //     Safe: the immediately preceding PUSH is the right operand and
    //     no jump points into the middle of an expression.
    if (a == 0 && b == 0 && s.empty()) {
        OpCode kop = OP_COUNT;
        switch (op) {
            case OP_ADD: kop = OP_ADD_K; break;
            case OP_SUB: kop = OP_SUB_K; break;
            case OP_MUL: kop = OP_MUL_K; break;
            case OP_DIV: kop = OP_DIV_K; break;
            case OP_MOD: kop = OP_MOD_K; break;
            case OP_EQ:  kop = OP_EQ_K;  break;
            case OP_NE:  kop = OP_NE_K;  break;
            case OP_LT:  kop = OP_LT_K;  break;
            case OP_LE:  kop = OP_LE_K;  break;
            case OP_GT:  kop = OP_GT_K;  break;
            case OP_GE:  kop = OP_GE_K;  break;
            default: break;
        }
        if (kop != OP_COUNT && !bytecode.empty()) {
            Instruction& last = bytecode.back();
            long long iv = 0;
            bool int_const = push_i64_value(last, iv) && i64_exact_in_double(iv);
            if (last.op == OP_PUSH && (last.b == 0 || int_const)) {
                double c = last.b == 2 ? (double)iv : last.n;
                bytecode.pop_back();
                Instruction ins;
                ins.op = kop; ins.n = c; ins.line = current_line;
                ins.c = int_const ? 1 : 0; // L7: exact integer constant
                bytecode.push_back(ins);
                return (int)bytecode.size() - 1;
            }
        }
    }
    // (2) x = x + k / x = x - k  ->  INC_LOCAL (no stack)
    if (op == OP_STORE && a >= 0 && bytecode.size() >= 2) {
        Instruction& k = bytecode.back();
        Instruction& l = bytecode[bytecode.size() - 2];
        if (l.op == OP_LOAD && l.a == a) {
            if (k.op == OP_ADD_K || k.op == OP_SUB_K) {
                double delta = (k.op == OP_ADD_K) ? k.n : -k.n;
                int int_const = k.c;
                bytecode.pop_back(); bytecode.pop_back();
                Instruction ins;
                ins.op = OP_INC_LOCAL; ins.a = a; ins.n = delta;
                ins.c = int_const;
                ins.line = current_line;
                bytecode.push_back(ins);
                return (int)bytecode.size() - 1;
            }
        }
    }
    // (3) [LOAD sid, CMP_K c] JUMP_IF_FALSE -> CMP_JIF_K (a=target, b=sid)
    if (op == OP_JUMP_IF_FALSE && a == 0 && b == 0 && s.empty()
        && bytecode.size() >= 2) {
        Instruction& k = bytecode.back();
        Instruction& l = bytecode[bytecode.size() - 2];
        OpCode fused = OP_COUNT;
        switch (k.op) {
            case OP_LT_K: fused = OP_LT_JIF_K; break;
            case OP_LE_K: fused = OP_LE_JIF_K; break;
            case OP_GT_K: fused = OP_GT_JIF_K; break;
            case OP_GE_K: fused = OP_GE_JIF_K; break;
            case OP_EQ_K: fused = OP_EQ_JIF_K; break;
            case OP_NE_K: fused = OP_NE_JIF_K; break;
            default: break;
        }
        if (fused != OP_COUNT && l.op == OP_LOAD) {
            int sid = l.a; double c = k.n;
            int int_const = k.c;
            bytecode.pop_back(); bytecode.pop_back();
            Instruction ins;
            ins.op = fused; ins.b = sid; ins.n = c; ins.c = int_const;
            ins.line = current_line;
            bytecode.push_back(ins);
            return (int)bytecode.size() - 1;
        }
    }

    Instruction ins;
    ins.op = op;
    ins.a = a;
    ins.b = b;
    ins.line = current_line;
    // E10: pre-parses numeric constants of OP_PUSH at compile time
    // (eliminates std::stod from the VM hot path). Imaginary literal "3j"
    // becomes b=1 with n = imaginary part.
    if (op == OP_PUSH) {
        if (!s.empty() && (s.back() == 'j' || s.back() == 'J')) {
            std::string num = s.substr(0, s.size() - 1);
            ins.b = 1;
            try { ins.n = num.empty() ? 1.0 : std::stod(num); }
            catch (...) { ins.n = 1.0; }
        } else {
            long long iv = 0;
            if (parse_i64_literal(s, iv)) {
                // L7: exact native integer (low/high 32 bits in a/c).
                ins.b = 2;
                ins.a = (int)(uint32_t)((unsigned long long)iv & 0xffffffffu);
                ins.c = (int)(uint32_t)(((unsigned long long)iv >> 32) & 0xffffffffu);
                ins.n = (double)iv;
            } else {
                try { ins.n = s.empty() ? (double)ins.a : std::stod(s); }
                catch (...) { ins.n = (double)ins.a; }
            }
        }
    }
    ins.s = s; // E16f: interned (index into the pool)
    bytecode.push_back(ins);
    return (int)bytecode.size() - 1;
}

void Compiler::patch(int index, int value) { bytecode[index].a = value; }
bool Compiler::match(TokenType t) { if (check(t)) { advance(); return true; } return false; }
bool Compiler::check(TokenType t) { return !is_at_end() && tokens[pos].type == t; }
Token Compiler::advance() {
    current_line = tokens[pos].line;
    return tokens[pos++];
}
Token Compiler::previous() { return tokens[pos - 1]; }
Token Compiler::consume(TokenType t, std::string m) { if (check(t)) return advance(); throw std::runtime_error(m); }
bool Compiler::is_at_end() { return (size_t)pos >= tokens.size() || tokens[pos].type == TK_EOF; }

void Compiler::debug_dump_bytecode() {
    std::cout << "\n=== BYTECODE ===\n";
    for(size_t i=0; i<bytecode.size(); i++) {
        std::cout << std::setw(3) << i << ": " << std::left << std::setw(16)
                  << op_name(bytecode[i].op) << std::right
                  << " A:" << bytecode[i].a << " B:" << bytecode[i].b 
                  << " C:" << bytecode[i].c
                  << " S:" << (bytecode[i].op == OP_PUSH_STR
                                  ? str_pool_get(bytecode[i].a).str()
                                  : bytecode[i].s.str())
                  << " L:" << bytecode[i].line << "\n";
    }
}