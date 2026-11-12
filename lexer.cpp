// Isiliel — scripting language interpreter/compiler.
// Embedded: sandbox, embedding API, OOP, 2D graphics and REPL.
// Authors: EEPS and MiMo-V2.6 | Version: 1.0 | Date: 2026-11-08

#include "lexer.h"
#include <map>
#include <stdexcept>

Lexer::Lexer(const std::string& source) : source(source) {}

// FIX: Cast to avoid a signed/unsigned comparison warning
bool Lexer::is_at_end() { 
    return current >= (int)source.length(); 
}

char Lexer::advance() { 
    return source[current++]; 
}

char Lexer::peek() { 
    return is_at_end() ? '\0' : source[current]; 
}

// FIX: Cast to avoid a signed/unsigned comparison warning
char Lexer::peek_next() {
    if (current + 1 >= (int)source.length()) return '\0';
    return source[current + 1];
}

bool Lexer::match(char expected) {
    if (is_at_end()) return false;
    if (source[current] != expected) return false;
    current++;
    return true;
}

void Lexer::add_token(TokenType type) {
    std::string text = source.substr(start, current - start);
    tokens.push_back({type, text, line});
}

void Lexer::add_token(TokenType type, std::string lexeme) {
    tokens.push_back({type, lexeme, line});
}

std::vector<Token> Lexer::scan_tokens() {
    while (!is_at_end()) {
        start = current;
        scan_token();
    }
    tokens.push_back({TK_EOF, "", line});
    return tokens;
}

void Lexer::scan_token() {
    char c = advance();
    switch (c) {
        case '(': add_token(TK_LPAREN); break;
        case ')': add_token(TK_RPAREN); break;
        case '[': add_token(TK_LBRACKET); break;
        case ']': add_token(TK_RBRACKET); break;
        case '{': add_token(TK_LBRACE); break;
        case '}': add_token(TK_RBRACE); break;
        case ',': add_token(TK_COMMA); break;
        case ';': add_token(TK_PUNCT); break;
        case ':': add_token(match(':') ? TK_DCOLON : TK_COLON); break;
        case '.':
            if (match('.')) {
                if (match('.')) add_token(TK_ELLIPSIS);
                else add_token(TK_DOT);
            } else add_token(TK_DOT);
            break;
        case '~': add_token(TK_BIT_NOT); break;
        case '^': add_token(TK_BIT_XOR); break;
        case '%': add_token(TK_MOD); break;

        case '+': add_token(match('=') ? TK_PLUS_ASSIGN : TK_PLUS); break;
        case '-': add_token(match('=') ? TK_MIN_ASSIGN  : TK_MINUS); break;
        case '*': add_token(match('=') ? TK_MUL_ASSIGN  : TK_MUL); break;
        case '/':
            if (match('/')) { 
                while (peek() != '\n' && !is_at_end()) advance(); 
            }
            else { 
                add_token(match('=') ? TK_DIV_ASSIGN : TK_DIV); 
            }
            break;

        case '&': add_token(match('&') ? TK_AND : TK_BIT_AND); break;
        case '|': add_token(match('|') ? TK_OR  : TK_BIT_OR); break;

        case '=': add_token(match('=') ? TK_EQ : TK_ASSIGN); break;
        case '<':
            if (match('<')) add_token(TK_SHL);
            else add_token(match('=') ? TK_LE : TK_LT);
            break;
        case '>':
            if (match('>')) add_token(TK_SHR);
            else add_token(match('=') ? TK_GE : TK_GT);
            break;
        case '!': 
            add_token(match('=') ? TK_NE : TK_BIT_NOT); 
            break;

        case ' ':
        case '\r':
        case '\t': 
            break;
        // The old '#' syntax is no longer accepted.
        case '#':
            throw std::runtime_error("caractere '#' nao suportado (linha " +
                                     std::to_string(line) + ")");
        case '\n': 
            line++; 
            break;

        case '"': 
            string(); 
            break;

        default:
            if (isdigit(c)) number();
            else if (isalpha(c) || c == '_') identifier();
            break;
    }
}

void Lexer::string() {
    std::string value = "";
    int start_line = line;
    while (peek() != '"' && !is_at_end()) {
        // Escapes: \n \t \r \\ \" \0 (unknown ones keep the backslash)
        if (peek() == '\\') {
            advance(); // consumes '\'
            if (is_at_end()) break;
            char e = advance();
            switch (e) {
                case 'n':  value += '\n';  break;
                case 't':  value += '\t';  break;
                case 'r':  value += '\r';  break;
                case '\\': value += '\\';  break;
                case '"':  value += '"';   break;
                case '0':  value += '\0';  break;
                default:
                    value += '\\';
                    value += e;
                    break;
            }
            continue;
        }
        if (peek() == '\n') line++;
        value += advance();
    }
    if (is_at_end()) {
        throw std::runtime_error("String nao terminada (linha " + std::to_string(start_line) + ")");
    }
    advance(); // Consumes the last "
    tokens.push_back({TK_STRING, value, start_line});
}

void Lexer::number() {
    while (isdigit(peek())) advance();
    if (peek() == '.' && isdigit(peek_next())) {
        advance();
        while (isdigit(peek())) advance();
    }
    // Imaginary number suffix (Python style): 3j, 2.5j, 4J
    if (peek() == 'j' || peek() == 'J') advance();
    add_token(TK_NUM);
}

void Lexer::identifier() {
    while (isalnum(peek()) || peek() == '_') advance();
    std::string text = source.substr(start, current - start);

    static std::map<std::string, TokenType> keywords = {
        {"if", TK_IF}, {"else", TK_ELSE}, {"elseif", TK_ELSEIF}, // E6: elseif
        {"while", TK_WHILE}, {"for", TK_FOR},
        {"void", TK_VOID}, {"int", TK_INT}, {"float", TK_FLOAT},
        {"fn", TK_FN}, // anonymous function (E2): fn(x) { ... }
        {"class", TK_CLASS}, {"extends", TK_EXTENDS}, // E4: inheritance
        {"new", TK_NEW}, {"super", TK_SUPER},         // E4: instance/super
        {"return", TK_RETURN},
        {"break", TK_BREAK}, {"continue", TK_CONTINUE},
        {"include", TK_INCLUDE},
        {"import", TK_IMPORT},
        {"type", TK_TYPE},
        {"global", TK_GLOBAL},
        {"thread", TK_THREAD}, {"start", TK_START}, {"stop", TK_STOP},
        {"pause", TK_PAUSE}, {"resume", TK_RESUME},
        {"repeat", TK_REPEAT}, {"until", TK_UNTIL}, {"goto", TK_GOTO},
        {"nil", TK_NIL} // L6: null value
    };

    // L4: `type(expr)` is the basic function (Lua), not the struct keyword.
    // `type Name { ... }` remains TK_TYPE; it only becomes TK_ID when the next
    // non-blank token is '(' (allows `type (x)` with a space).
    if (text == "type") {
        int k = current;
        while (k < (int)source.size() &&
               (source[k] == ' ' || source[k] == '\t' ||
                source[k] == '\r' || source[k] == '\n')) k++;
        if (k < (int)source.size() && source[k] == '(') {
            add_token(TK_ID, text);
            return;
        }
    }

    if (keywords.count(text)) {
        add_token(keywords[text]);
    } else {
        add_token(TK_ID);
    }
}