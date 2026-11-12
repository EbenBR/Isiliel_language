// Isiliel — scripting language interpreter/compiler.
// Embedded: sandbox, embedding API, OOP, 2D graphics and REPL.
// Authors: Eben (AI-assisted) | Version: 1.0 | Date: 2026-11-08

#include <iostream>
#include <string>

#include "interp.h"
#include "fileio.h"
#include "threads.h"
#include "modules.h"
#include "graphics/graphics.h"

static void print_usage(const char* prog) {
    std::cerr << "Uso: " << prog << " <script> [mais scripts...]\n"
              << "       " << prog << " --disasm <script> [mais...]\n"
              << "       " << prog << "            (REPL interativo)\n"
              << "  Exemplo: " << prog << " scripts/script_1_matriz_lu.isi\n";
}

// Counts braces outside strings/escapes on the line (for multi-line blocks)
static void count_braces(const std::string& line, int& depth) {
    bool in_str = false;
    for (size_t i = 0; i < line.size(); i++) {
        char c = line[i];
        if (in_str) {
            if (c == '\\') { i++; continue; }
            if (c == '"') in_str = false;
            continue;
        }
        if (c == '"') { in_str = true; continue; }
        if (c == '{') depth++;
        else if (c == '}') depth--;
    }
}

static bool is_comment_or_empty(const std::string& s) {
    size_t i = s.find_first_not_of(" \t\r");
    if (i == std::string::npos) return true;
    return s.compare(i, 2, "//") == 0;
}

static int run_repl() {
    std::cout << "Isiliel REPL — digite exit para sair" << std::endl;

    std::string buffer, line;
    int depth = 0;

    while (true) {
        std::cout << (buffer.empty() ? ">>> " : "... ") << std::flush;
        if (!std::getline(std::cin, line)) {
            std::cout << std::endl;
            break;
        }

        // Commands only apply with an empty buffer (start of a statement)
        if (buffer.empty()) {
            std::string t = line;
            while (!t.empty() && (t.front() == ' ' || t.front() == '\t')) t.erase(t.begin());
            while (!t.empty() && (t.back() == ' ' || t.back() == '\r' || t.back() == '\t')) t.pop_back();
            if (t == "exit" || t == "quit") break;
            if (t.empty()) continue;
        }

        buffer += line + "\n";
        count_braces(line, depth);

        if (depth < 0) {
            std::cerr << "[!] '}' inesperado" << std::endl;
            buffer.clear();
            depth = 0;
            continue;
        }

        // Complete statement: closed block OR line ending with ';' or '}'
        bool complete = false;
        if (depth <= 0) {
            size_t e = line.find_last_not_of(" \t\r");
            if (e == std::string::npos) complete = true; // empty line in the middle
            else if (line[e] == ';' || line[e] == '}') complete = true;
            else if (is_comment_or_empty(line)) complete = true;
        }

        if (complete) {
            // Symbols + bytecode persist between REPL statements
            run_pipeline_repl(buffer);
            buffer.clear();
            depth = 0;
        }
    }
    // Closes threads/screen/files opened during the REPL
    threads_shutdown();
    shutdown_fileio();
    gfx_shutdown();
    modules_shutdown(); // unloads `.mod` modules imported in the REPL
    return 0;
}

int main(int argc, char* argv[]) {
    if (argc == 2 && (std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h")) {
        print_usage(argv[0]);
        return 0;
    }
    // E7/neg15: --disasm <script...> — compiles and prints the bytecode
    if (argc >= 2 && std::string(argv[1]) == "--disasm") {
        if (argc < 3) {
            print_usage(argv[0]);
            return 1;
        }
        int failures = 0;
        for (int i = 2; i < argc; i++) {
            if (!disasm_file(argv[i])) failures++;
        }
        return failures > 0 ? 1 : 0;
    }
    if (argc < 2) {
        return run_repl();
    }

    int failures = 0;
    for (int i = 1; i < argc; i++) {
        if (!run_file(argv[i])) {
            failures++;
        }
    }

    if (failures > 0) {
        std::cerr << "\n[!] " << failures << " script(s) falharam." << std::endl;
        return 1;
    }
    return 0;
}
