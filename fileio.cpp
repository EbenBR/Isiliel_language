// Isiliel — scripting language interpreter/compiler.
// Embedded: sandbox, embedding API, OOP, 2D graphics and REPL.
// Authors: EEPS and MiMo-V2.6 | Version: 1.0 | Date: 2026-11-08

// ===========================================================================
// C stdio-style file builtins:
//
//   f = fopen(path, mode)       handle (>=1) or 0 on failure (== NULL)
//   fclose(f)                   closes; invalid handle = error with line
//   fwrite(s, f) / fputs(s, f)  writes text; returns bytes written
//   fread(n, f)                 reads up to n bytes -> string ("" = EOF)
//   fgets(f)                    reads one line (with "\n"); "" = EOF
//   fgetc(f)                    1 character (string); "" = EOF
//   fputc(c, f)                 writes 1 character -> 1
//   fseek(f, offset, whence)    whence: SEEK_SET/SEEK_CUR/SEEK_END (0/1/2)
//   ftell(f) / rewind(f)        current position / back to the start
//   feof(f) / ferror(f)         0/1
//   fprintf(f, fmt, ...)        like printf(), but writes to the file
//
// Handles are monotonic integers (never reused): a handle of an
// already-closed file remains "invalid" and raises an error, never another file.
// Forgotten files are closed by shutdown_fileio().
// ===========================================================================

#include "fileio.h"

#include "symbols.h"
#include "vm.h"

#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

struct FileRegistry {
    std::unordered_map<int, FILE*> map;
    int next = 1;
};

FileRegistry F;

bool is_num(const Value& v) {
    return v.type == VAL_FLOAT || v.type == VAL_INT || v.type == VAL_COMPLEX;
}

int iarg(Value* v, const char* fn, const char* what) {
    if (!is_num(*v))
        throw std::runtime_error(std::string(fn) + ": " + what + " deve ser numero");
    return (int)std::lround(v->f);
}

// File handle -> FILE* (error with line if not a valid handle)
FILE* f_of(Value* v, const char* fn) {
    int h = iarg(v, fn, "handle");
    auto it = F.map.find(h);
    if (it == F.map.end())
        throw std::runtime_error(std::string(fn) +
            ": handle de arquivo invalido (use o valor de fopen)");
    return it->second;
}

// --- handlers ---------------------------------------------------------------

// fopen(path, mode) -> handle >= 1; failure returns 0 (== C NULL)
void b_fopen(Value* a, int, Value* r) {
    if (a[0].type != VAL_STRING)
        throw std::runtime_error("fopen: caminho deve ser string");
    if (a[1].type != VAL_STRING)
        throw std::runtime_error("fopen: modo deve ser string (ex.: \"r\", \"w\")");
    FILE* f = std::fopen(a[0].s.c_str(), a[1].s.c_str());
    if (!f) { *r = make_float(0.0f); return; }
    const int h = F.next++;
    F.map.emplace(h, f);
    *r = make_float((double)h);
}

// fclose(f) -> 1
void b_fclose(Value* a, int, Value* r) {
    FILE* f = f_of(&a[0], "fclose");
    int h = iarg(&a[0], "fclose", "handle");
    std::fclose(f);
    F.map.erase(h);
    *r = make_float(1.0f);
}

// fwrite(data, f) -> bytes written (0 = failure)
void b_fwrite(Value* a, int, Value* r) {
    if (a[0].type != VAL_STRING)
        throw std::runtime_error("fwrite: dados devem ser string");
    FILE* f = f_of(&a[1], "fwrite");
    const std::string& s = a[0].s;
    size_t n = s.empty() ? 0 : std::fwrite(s.data(), 1, s.size(), f);
    *r = make_float((double)n);
}

// fputs(data, f) -> bytes written (same semantics as fwrite in text mode)
void b_fputs(Value* a, int, Value* r) {
    if (a[0].type != VAL_STRING)
        throw std::runtime_error("fputs: dados devem ser string");
    FILE* f = f_of(&a[1], "fputs");
    const std::string& s = a[0].s;
    size_t n = s.empty() ? 0 : std::fwrite(s.data(), 1, s.size(), f);
    *r = make_float((double)n);
}

// fread(n, f) -> string of up to n bytes ("" = EOF/error); n < 0 = error
void b_fread(Value* a, int, Value* r) {
    int n = iarg(&a[0], "fread", "quantidade");
    if (n < 0)
        throw std::runtime_error("fread: quantidade deve ser >= 0");
    FILE* f = f_of(&a[1], "fread");
    std::string out((size_t)n, '\0');
    size_t got = n > 0 ? std::fread(&out[0], 1, (size_t)n, f) : 0;
    out.resize(got);
    *r = make_string(out);
}

// fgets(f) -> line WITH "\n" if present; "" = EOF
void b_fgets(Value* a, int, Value* r) {
    FILE* f = f_of(&a[0], "fgets");
    std::string line;
    int c;
    while ((c = std::fgetc(f)) != EOF) {
        line += (char)c;
        if (c == '\n') break;
    }
    *r = make_string(line);
}

// fgetc(f) -> 1-character string; "" = EOF
void b_fgetc(Value* a, int, Value* r) {
    FILE* f = f_of(&a[0], "fgetc");
    int c = std::fgetc(f);
    *r = make_string(c == EOF ? std::string() : std::string(1, (char)c));
}

// fputc(c, f) -> 1 (accepts a 1-char string or a number = code)
void b_fputc(Value* a, int, Value* r) {
    FILE* f = f_of(&a[1], "fputc");
    int c;
    if (a[0].type == VAL_STRING) {
        if (a[0].s.empty())
            throw std::runtime_error("fputc: caractere vazio");
        c = (unsigned char)a[0].s[0];
    } else if (is_num(a[0])) {
        c = (int)std::lround(a[0].f) & 0xFF;
    } else {
        throw std::runtime_error("fputc: caractere deve ser string ou numero");
    }
    int rc = std::fputc(c, f);
    *r = make_float(rc == EOF ? 0.0f : 1.0f);
}

// fseek(f, offset, whence) -> 1 (ok) / 0 (failure); whence 0/1/2 or
// SEEK_SET/SEEK_CUR/SEEK_END (created by fileio_constants)
void b_fseek(Value* a, int, Value* r) {
    FILE* f = f_of(&a[0], "fseek");
    long off = (long)iarg(&a[1], "fseek", "deslocamento");
    int wh = iarg(&a[2], "fseek", "whence");
    if (wh < 0 || wh > 2)
        throw std::runtime_error("fseek: whence invalido (use SEEK_SET, "
                                 "SEEK_CUR ou SEEK_END)");
    *r = make_float(std::fseek(f, off, wh) == 0 ? 1.0f : 0.0f);
}

// ftell(f) -> current position (-1 on error)
void b_ftell(Value* a, int, Value* r) {
    FILE* f = f_of(&a[0], "ftell");
    *r = make_float((double)std::ftell(f));
}

// rewind(f) -> 1
void b_rewind(Value* a, int, Value* r) {
    FILE* f = f_of(&a[0], "rewind");
    std::rewind(f);
    *r = make_float(1.0f);
}

// feof(f) -> 0/1
void b_feof(Value* a, int, Value* r) {
    FILE* f = f_of(&a[0], "feof");
    *r = make_float(std::feof(f) ? 1.0f : 0.0f);
}

// ferror(f) -> 0/1
void b_ferror(Value* a, int, Value* r) {
    FILE* f = f_of(&a[0], "ferror");
    *r = make_float(std::ferror(f) ? 1.0f : 0.0f);
}

// fprintf(f, fmt, ...) -> characters written (format same as printf)
void b_fprintf(Value* a, int n, Value* r) {
    if (n < 2)
        throw std::runtime_error("fprintf: use fprintf(arquivo, \"formato\", ...)");
    if (a[1].type != VAL_STRING)
        throw std::runtime_error("fprintf: o segundo argumento deve ser uma string");
    FILE* f = f_of(&a[0], "fprintf");
    const std::string out = format_values(a[1].s, a + 2, n - 2, "fprintf");
    size_t got = out.empty() ? 0 : std::fwrite(out.data(), 1, out.size(), f);
    *r = make_float((double)got);
}

} // namespace

void register_fileio_builtins() {
    static bool registered = false;
    if (registered) return;
    registered = true;

    register_builtin("fopen", 2, b_fopen);
    register_builtin("fclose", 1, b_fclose);
    register_builtin("fwrite", 2, b_fwrite);
    register_builtin("fputs", 2, b_fputs);
    register_builtin("fread", 2, b_fread);
    register_builtin("fgets", 1, b_fgets);
    register_builtin("fgetc", 1, b_fgetc);
    register_builtin("fputc", 2, b_fputc);
    register_builtin("fseek", 3, b_fseek);
    register_builtin("ftell", 1, b_ftell);
    register_builtin("rewind", 1, b_rewind);
    register_builtin("feof", 1, b_feof);
    register_builtin("ferror", 1, b_ferror);
    register_builtin("fprintf", -1, b_fprintf);
}

// Sandbox (E8/neg18): file builtin names for the central gate in the vm
bool fileio_is_restricted(const std::string& name) {
    static const char* names[] = {
        "fopen", "fclose", "fwrite", "fputs", "fread", "fgets", "fgetc",
        "fputc", "fseek", "ftell", "rewind", "feof", "ferror", "fprintf"
    };
    for (const char* n : names)
        if (name == n) return true;
    return false;
}

// Global constants (idempotent; AFTER sym_clear in run_pipeline)
void fileio_constants() {
    sym_create("NULL", make_float(0.0f));
    sym_create("SEEK_SET", make_float(0.0f));
    sym_create("SEEK_CUR", make_float(1.0f));
    sym_create("SEEK_END", make_float(2.0f));
}

void shutdown_fileio() {
    for (auto& kv : F.map) std::fclose(kv.second);
    F.map.clear();
}
