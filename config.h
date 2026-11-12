// Isiliel — scripting language interpreter/compiler.
// Embedded: sandbox, embedding API, OOP, 2D graphics and REPL.
// Authors: EEPS and MiMo-V2.6 | Version: 1.0 | Date: 2026-11-08

#ifndef CONFIG_H
#define CONFIG_H

// Debug directives - uncomment to enable/disable
//#define DEBUG_LEXER     1  // Lexer debug
//#define DEBUG_COMPILER  1  // Compiler debug
//#define DEBUG_VM        1  // Virtual machine debug
//#define DEBUG_ALL        1  // Enables all debugs (overrides the specific ones)

// Enables all debug messages if DEBUG_ALL is defined
#ifdef DEBUG_ALL
    #define DEBUG_LEXER
    #define DEBUG_COMPILER
    #define DEBUG_VM
#endif

#endif // CONFIG_H