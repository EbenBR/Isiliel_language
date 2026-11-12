// Isiliel — scripting language interpreter/compiler.
// Embedded: sandbox, embedding API, OOP, 2D graphics and REPL.
// Authors: EEPS and MiMo-V2.6 | Version: 1.0 | Date: 2026-11-08

#ifndef LEXER_H
#define LEXER_H

#include "defs.h"
#include <vector>
#include <string>

class Lexer {
public:
    Lexer(const std::string& source);
    std::vector<Token> scan_tokens();

private:
    std::string source;
    std::vector<Token> tokens;
    int start = 0;
    int current = 0;
    int line = 1;

    bool is_at_end();
    char advance();
    char peek();
    char peek_next();
    bool match(char expected);
    
    void scan_token();
    void identifier();
    void number();
    void string();
    void add_token(TokenType type);
    void add_token(TokenType type, std::string lexeme);
};

#endif