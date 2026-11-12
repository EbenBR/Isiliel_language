// Isiliel — scripting language interpreter/compiler.
// Embedded: sandbox, embedding API, OOP, 2D graphics and REPL.
// Authors: Eben (AI-assisted) | Version: 1.0 | Date: 2026-11-08

#include "interp.h"
#include "sound.h"
#include "patterns.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <cstdlib>

#include "lexer.h"
#include "compiler.h"
#include "vm.h"
#include "symbols.h"
#include "config.h"
#include "graphics/graphics.h"
#include "graphics/graphics_builtins.h"
#include "fileio.h"
#include "threads.h"
#include "modules.h"

// 1+2: lexing and compilation. Returns false (and logs) on error.
// code_base: initial global address of this section (the REPL accumulates bytecode).
// drawLoop: Processing model — appends the setup()/draw() loop if draw() exists.
static bool compile_source(const std::string& source,
                           std::vector<Token>& tokens,
                           Compiler& compiler,
                           int code_base = 0,
                           bool drawLoop = false,
                           std::string* err = nullptr,
                           bool setupFirst = true) {
    try {
        Lexer lexer(source);
        tokens = lexer.scan_tokens();
    } catch (const std::exception& e) {
        if (err) *err = std::string("ERRO LEXICO: ") + e.what();
        else std::cerr << "[!] ERRO LEXICO: " << e.what() << std::endl;
        return false;
    }

    #ifdef DEBUG_LEXER
    std::cout << "--- DEBUG: TOKENS ENCONTRADOS ---" << std::endl;
    for (size_t i = 0; i < tokens.size(); i++) {
        std::cout << "[" << i << "] Tipo: " << tokens[i].type
                  << " | Lexema: '" << tokens[i].lexeme << "'" << std::endl;
    }
    std::cout << "---------------------------------\n" << std::endl;
    #endif

    compiler.init(tokens, code_base);
    compiler.set_draw_loop(drawLoop);
    compiler.set_setup_first(setupFirst);
    compiler.quiet_errors = (err != nullptr); // E8: API does not print
    if (!compiler.run()) {
        if (err)
            *err = compiler.last_error.empty()
                       ? std::string("Erro fatal na fase de compilacao.")
                       : compiler.last_error;
        else
            std::cout << "\n[!] Erro fatal na fase de compilacao." << std::endl;
        return false;
    }

    #ifdef DEBUG_COMPILER
    compiler.debug_dump_bytecode();
    #endif
    return true;
}

// 3: runs `code` from `start_ip`. Returns false on a runtime error.
static bool exec_bytecode(const std::vector<Instruction>& chunk, int start_ip,
                          bool verbose, std::string* err = nullptr) {
    // L14: the chunk is appended to the global arena at `start_ip` (the compiler already
    // relocated jumps/entries to that global address).
    int base = code_arena_append(chunk);
    if (base != start_ip) {
        std::string msg = "arena de codigo inconsistente (base " +
                          std::to_string(base) + ", esperado " +
                          std::to_string(start_ip) + ")";
        if (err) *err = msg; else std::cerr << "[!] " << msg << std::endl;
        return false;
    }
    VM vm;
    vm.sp = 0;
    for (int i = 0; i < STACK_MAX; i++) vm.stack[i] = make_float(0.0f);

    if (verbose) std::cout << "\n--- EXECUCAO DA VM ---" << std::endl;
    // Threads share the global arena (stable because it is a deque)
    threads_bind_code(&code_arena());
    try {
        vm_run(&vm, &code_arena(), start_ip);
    } catch (const std::exception& e) {
        // Traceback (E2): frame stack outer -> inner (name + call line)
        std::string full = "ERRO EM TEMPO DE EXECUCAO: ";
        full += e.what();
        for (const auto& fr : vm.frames)
            full += "\n    em " + fr.first.str() + " (linha " +
                    std::to_string(fr.second) + ")";
        full += "\n    Stack Pointer: " + std::to_string(vm.sp) + " / " +
                std::to_string(STACK_MAX);
        if (err) {
            *err = full; // E8: API returns the message without printing
        } else {
            std::cerr << "\n[!] ERRO EM TEMPO DE EXECUCAO: " << e.what() << std::endl;
            for (const auto& fr : vm.frames)
                std::cerr << "    em " << fr.first.str() << " (linha " << fr.second << ")" << std::endl;
            std::cerr << "    Stack Pointer: " << vm.sp << " / " << STACK_MAX << std::endl;
        }
        if (verbose) std::cout << "----------------------\n" << std::endl;
        return false;
    }
    if (verbose) std::cout << "----------------------\n" << std::endl;
    return true;
}

bool run_pipeline(const std::string& source, bool resetSymbols, bool verbose) {
    if (verbose)
        std::cout << ">>> INICIANDO PIPELINE DE EXECUCAO <<<\n" << std::endl;

    // Graphics/file builtins (idempotent; BEFORE compile)
    register_graphics_builtins();
    register_fileio_builtins();
    register_threads_builtins();
    register_sound_builtins();
    register_pattern_builtins();

    // FIX: Clears the symbol table between executions (unless the REPL asks otherwise)
    if (resetSymbols)
        sym_clear();
    // File constants (NULL/SEEK_*): AFTER sym_clear, otherwise
    // they would be erased right after being created
    fileio_constants();

    code_arena_reset(); // L14: main script starts at address 0
    vm_clear_gc_finalizers(); // L11: does not reuse records from a previous execution
    vm_clear_gc_objects();    // L13

    std::vector<Token> tokens;
    Compiler compiler;
    if (!compile_source(source, tokens, compiler, /*code_base=*/0,
                        /*drawLoop=*/true)) {
        threads_shutdown();
        shutdown_fileio();
        gfx_shutdown();
        modules_shutdown(); // `.mod` modules of this script (process end)
        sound_shutdown();
        return false;
    }

    // Warning: frame loop (draw/setDraw) without a window = 1 frame and no interaction
    if (compiler.has_draw_loop() && gfx_backend_name() == "software") {
        std::cerr << "[i] loop de quadros (draw/setDraw) detectado: backend"
                     " 'software' renderiza 1 quadro (sem janela/interacao).\n"
                     "    Para animacao interativa use o backend raylib"
                     " (backend(\"raylib\")).\n";
    }

    std::vector<Instruction> bytecode = compiler.get_bytecode();
    bool ok = exec_bytecode(bytecode, 0, verbose);

    // With an open window (raylib), keeps the result visible until the user
    // closes it (ESC or X) — Processing style. Without a screen, there is nothing to wait for.
    if (ok && gfx_is_open()) {
        while (gfx_show()) { /* pumps events until close */ }
    }

    // End of script: if all is OK, WAITS for the threads to finish (non-daemon
    // join — `stop f()` stopped earlier; an infinite thread = program alive
    // until Ctrl+C). On ERROR, kills everything. Then files (the thread could
    // be writing) and finally the screen.
    if (ok) {
        threads_join_all();
        // L11: __gc finalizers at the end of the script (best-effort; not in the REPL)
        vm_run_gc_finalizers();
        // L13: breaks cycles left after the finalizers
        gc_collect_cycles(false);
    }
    threads_shutdown(); // no-op after join; safe on any path
    shutdown_fileio();
    gfx_shutdown();
    // `.mod` modules are per-script: unload (dlclose) here; the next
    // script needs import(...) again (sym_clear erases the symbols).
    // In the REPL there is no such per-line call — modules persist until exit.
    modules_shutdown();
    sound_shutdown();

    if (!ok) return false;

    // 4. FINAL DATA DUMP
    #ifdef DEBUG_VM
    sym_dump();
    #endif

    return true;
}

// REPL: symbols persist and bytecode is ACCUMULATED in the global arena between
// entries, so functions defined on one line stay valid on the
// next ones. File scope (E8): interp_reset() clears the arena.

// E9/neg22: configurable REPL growth ceiling. Bytecode and
// symbols accumulate between lines by design; without a ceiling a long session
// (or an input loop) would grow indefinitely. 0 disables it.
static long long repl_env_limit(const char* name, long long def) {
    const char* v = std::getenv(name);
    if (!v || !*v) return def;
    char* end = nullptr;
    long long n = std::strtoll(v, &end, 10);
    if (end == v || n < 0) return def;
    return n;
}

bool run_pipeline_repl(const std::string& source) {
    // Graphics builtins also exist in the REPL (the screen persists between
    // instructions; run.cpp closes it on exit)
    register_graphics_builtins();
    register_fileio_builtins();
    register_threads_builtins();
    register_sound_builtins();
    register_pattern_builtins();
    fileio_constants(); // REPL symbols are never cleared (idempotent)

    struct ReplLimits {
        long long code, syms;
        ReplLimits()
            : code(g_code_limit), syms(g_symbol_limit) {
            g_code_limit = repl_env_limit("MYINTERP_REPL_CODE_LIMIT", 250000);
            g_symbol_limit = repl_env_limit("MYINTERP_REPL_SYMBOLS_LIMIT", 50000);
        }
        ~ReplLimits() { g_code_limit = code; g_symbol_limit = syms; }
    } replLimits;

    int start = code_arena_size();

    std::vector<Token> tokens;
    Compiler compiler;
    if (!compile_source(source, tokens, compiler, start,
                        /*drawLoop=*/false, nullptr, /*setupFirst=*/false))
        return false;

    std::vector<Instruction> chunk = compiler.get_bytecode();
    bool ok = exec_bytecode(chunk, start, /*verbose=*/false);
    // Threads in the REPL live only during the line: the next insert() may
    // reallocate the bytecode and the next compile inserts symbols (both
    // unsafe with a thread reading). Use scripts for long-lived threads.
    threads_shutdown();
    if (ok) gc_collect_cycles(false); // L13 (no __gc in the REPL)
    return ok;
}

bool run_file(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        std::cerr << "[!] Nao foi possivel abrir o arquivo: " << path << std::endl;
        return false;
    }

    std::ostringstream content;
    content << file.rdbuf();
    file.close();

    std::cout << ">>> SCRIPT: " << path << std::endl;
    return run_pipeline(content.str());
}

// E7/neg15: disassembler — compiles the script and prints the bytecode
// (without executing). Same builtins as run_pipeline for stable ids.
bool disasm_file(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        std::cerr << "[!] Nao foi possivel abrir o arquivo: " << path << std::endl;
        return false;
    }

    std::ostringstream content;
    content << file.rdbuf();
    file.close();

    register_graphics_builtins();
    register_fileio_builtins();
    register_threads_builtins();
    register_sound_builtins();
    register_pattern_builtins();

    std::vector<Token> tokens;
    Compiler compiler;
    if (!compile_source(content.str(), tokens, compiler, /*code_base=*/0,
                        /*drawLoop=*/false, nullptr, /*setupFirst=*/false))
        return false;

    std::cout << "=== DISASM: " << path << " ===" << std::endl;
    compiler.debug_dump_bytecode();
    return true;
}

// ===========================================================================
// Embedding API (E8/neg16–18)
// ===========================================================================

InterpResult interp_run(const std::string& source, const InterpOptions& opts) {
    InterpResult r;

    // Per-execution sandbox (RAII: restores defaults/previous state at the end,
    // including on error — CLI and nested calls never see stuck flags)
    struct SandboxGuard {
        long long sl, st;
        bool af, am;
        explicit SandboxGuard(const InterpOptions& o)
            : sl(g_step_limit), st(g_steps),
              af(g_allow_fileio), am(g_allow_modules) {
            g_step_limit = o.step_limit;
            g_steps = 0;
            g_allow_fileio = o.allow_fileio;
            g_allow_modules = o.allow_modules;
        }
        ~SandboxGuard() {
            g_step_limit = sl;
            g_steps = st;
            g_allow_fileio = af;
            g_allow_modules = am;
        }
    } sandbox(opts);

    // stdout capture (rdbuf swap; RAII restores at the end)
    std::ostringstream cap;
    std::streambuf* savedBuf = nullptr;
    if (opts.capture) savedBuf = std::cout.rdbuf(cap.rdbuf());
    struct BufRestore {
        std::streambuf* saved;
        ~BufRestore() { if (saved) std::cout.rdbuf(saved); }
    } bufRestore{savedBuf};

    auto finish = [&]() -> InterpResult {
        if (opts.capture) r.output = cap.str();
        return r;
    };

    if (opts.verbose)
        std::cout << ">>> INICIANDO PIPELINE DE EXECUCAO <<<\n" << std::endl;

    // Same steps as run_pipeline (single-state: symbols cleared per
    // call; embedders with accumulated state use interp_reset + REPL)
    register_graphics_builtins();
    register_fileio_builtins();
    register_threads_builtins();
    register_sound_builtins();
    register_pattern_builtins();
    sym_clear();
    fileio_constants();

    code_arena_reset(); // L14: API execution starts at address 0
    vm_clear_gc_finalizers(); // L11
    vm_clear_gc_objects();    // L13

    std::vector<Token> tokens;
    Compiler compiler;
    std::string err;
    if (!compile_source(source, tokens, compiler, /*code_base=*/0,
                        /*drawLoop=*/true, &err)) {
        r.ok = false;
        r.error = err;
        threads_shutdown();
        shutdown_fileio();
        gfx_shutdown();
        modules_shutdown();
        sound_shutdown();
        return finish();
    }

    std::vector<Instruction> bytecode = compiler.get_bytecode();
    if (!exec_bytecode(bytecode, 0, opts.verbose, &err)) {
        r.ok = false;
        r.error = err;
        threads_shutdown();
        shutdown_fileio();
        gfx_shutdown();
        modules_shutdown();
        sound_shutdown();
        return finish();
    }

    // Same behavior as the CLI: with an open window, waits for it to close
    if (gfx_is_open()) {
        while (gfx_show()) { /* pumps events until close */ }
    }

    threads_join_all();
    vm_run_gc_finalizers(); // L11: __gc finalizers at the end of the script
    gc_collect_cycles(false); // L13: breaks remaining cycles
    threads_shutdown();
    shutdown_fileio();
    gfx_shutdown();
    modules_shutdown();
    sound_shutdown();

    r.ok = true;
    return finish();
}

bool interp_reset() {
    // Clean state for the next execution (E8/neg17): everything idempotent
    threads_shutdown();
    shutdown_fileio();
    gfx_shutdown();
    modules_shutdown();
    sound_shutdown();
    vm_clear_gc_finalizers(); // L11: discards pending finalizers
    vm_clear_gc_objects();    // L13: discards the object record
    g_code_limit = 0;         // E9/neg22: limits only apply during the REPL
    g_symbol_limit = 0;
    sym_clear();
    code_arena_reset(); // REPL accumulated bytecode
    g_step_limit = 0;
    g_steps = 0;
    g_allow_fileio = true;
    g_allow_modules = true;
    return true;
}

void interp_register_fn(const char* name, int arity, BuiltinHandler fn) {
    if (!name || !fn) return;
    // Same registration as the builtins: receives a stable id in g_builtins
    // (O(1) dispatch) and takes part in the arity lookup at compile time.
    register_builtin(name, arity, fn);
}

// Output hook: redirects std::cout (where println/printf write) to `os`.
static std::streambuf* g_saved_cout = nullptr;
void interp_set_output(std::ostream* os) {
    if (!os) {
        if (g_saved_cout) {
            std::cout.rdbuf(g_saved_cout);
            g_saved_cout = nullptr;
        }
        return;
    }
    if (!g_saved_cout) g_saved_cout = std::cout.rdbuf();
    std::cout.rdbuf(os->rdbuf());
}
