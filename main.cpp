// Isiliel — scripting language interpreter/compiler.
// Embedded: sandbox, embedding API, OOP, 2D graphics and REPL.
// Authors: Eben (AI-assisted) | Version: 1.0 | Date: 2026-11-08

// Isiliel: main.cpp
#include <iostream>
#include <vector>
#include <string>
#include "lexer.h"
#include "compiler.h"
#include "vm.h"
#include "symbols.h"
#include "config.h"

#include <fstream>
#include <sstream>

void run_pipeline(const std::string& source) {
    std::cout << ">>> INICIANDO PIPELINE DE EXECUCAO <<<\n" << std::endl;

    // FIX: Clears the symbol table between executions
    sym_clear();

    // 1. LEXICAL ANALYSIS
    Lexer lexer(source);
    std::vector<Token> tokens = lexer.scan_tokens();

    #ifdef DEBUG_LEXER
    std::cout << "--- DEBUG: TOKENS ENCONTRADOS ---" << std::endl;
    for (size_t i = 0; i < tokens.size(); i++) {
        std::cout << "[" << i << "] Tipo: " << tokens[i].type 
                  << " | Lexema: '" << tokens[i].lexeme << "'" << std::endl;
    }
    std::cout << "---------------------------------\n" << std::endl;
    #endif

    // 2. COMPILATION
    Compiler compiler;
    compiler.init(tokens);
    if (!compiler.run()) {
        std::cout << "\n[!] Erro fatal na fase de compilacao." << std::endl;
        return;
    }

    #ifdef DEBUG_COMPILER
    compiler.debug_dump_bytecode();
    #endif
    
    std::vector<Instruction> bytecode = compiler.get_bytecode();

    // 3. EXECUTION ON THE VM (L14: bytecode lives in the stable global arena)
    code_arena_reset();
    int entry = code_arena_append(bytecode);
    VM vm;
    vm.sp = 0;
    // Explicitly clears the stack if needed
    for(int i=0; i<STACK_MAX; i++) vm.stack[i] = make_float(0.0f);
    //while(!vm.return_stack.empty()) vm.return_stack.pop();
    
    std::cout << "\n--- EXECUCAO DA VM ---" << std::endl;
    try {
        vm_run(&vm, &code_arena(), entry);
    } catch (const std::exception& e) {
        std::cerr << "\n[!] ERRO EM TEMPO DE EXECUCAO: " << e.what() << std::endl;
        std::cerr << "    Stack Pointer: " << vm.sp << " / " << STACK_MAX << std::endl;
    }
    std::cout << "----------------------\n" << std::endl;

    // 4. FINAL DATA DUMP
    #ifdef DEBUG_VM
    sym_dump();
    #endif
}

int main(int argc, char* argv[]) {

    if (argc < 1) {
        std::cerr << "Erro: Nenhum argumento fornecido." << std::endl;
        return 1;
    }

    std::ifstream file(argv[1]);
    std::ostringstream content;
    
    if (file.is_open()) {
        // Read the entire file through rdbuf() into the ostringstream
        content << file.rdbuf();
        file.close();

        // Get the content as a standard string
        std::string script = content.str();
        //std::cout << "Full file content:\n" << script << std::endl;
        run_pipeline(script);
    } else {
        std::cout << "Unable to open file" << std::endl;
    }

    return 0;
}
