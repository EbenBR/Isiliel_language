// Isiliel — scripting language interpreter/compiler.
// Embedded: sandbox, embedding API, OOP, 2D graphics and REPL.
// Authors: Eben (AI-assisted) | Version: 1.0 | Date: 2026-11-08

#include "modules.h"
#include "symbols.h"
#include "sha256.h"

#include <cstdint>
#include <cstring>
#include <cctype>
#include <dlfcn.h>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace {

// =====================================================================
// Minimal JSON (own parser; only the subset used by manifests)
// =====================================================================
struct JValue;
using JPtr = std::shared_ptr<JValue>;

struct JValue {
    enum Type { JNULL, JBOOL, JNUM, JSTR, JOBJ, JARR } type = JNULL;
    bool b = false;
    double num = 0;
    std::string str;
    std::vector<std::pair<std::string, JPtr>> members;
    std::vector<JPtr> items;

    JPtr get(const std::string& key) const {
        for (const auto& m : members)
            if (m.first == key) return m.second;
        return nullptr;
    }
};

class JsonParser {
public:
    explicit JsonParser(const std::string& src) : src(src) {}

    JPtr parse() {
        JPtr v = value();
        ws();
        if (pos < src.size()) fail("dados extras apos o fim do JSON");
        return v;
    }

private:
    const std::string& src;
    size_t pos = 0;
    int line = 1;

    [[noreturn]] void fail(const std::string& msg) const {
        throw std::runtime_error("JSON invalido (linha " + std::to_string(line) +
                                 "): " + msg);
    }

    char peek() const { return pos < src.size() ? src[pos] : '\0'; }
    char advance() {
        char c = src[pos++];
        if (c == '\n') line++;
        return c;
    }
    bool match(char c) {
        if (peek() != c) return false;
        advance();
        return true;
    }
    void expect(char c, const char* what) {
        if (!match(c)) fail(std::string("esperado '") + c + "' " + what);
    }
    void ws() {
        while (pos < src.size()) {
            char c = peek();
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') advance();
            else break;
        }
    }

    JPtr value() {
        ws();
        if (pos >= src.size()) fail("fim inesperado");
        char c = peek();
        if (c == '{') return object();
        if (c == '[') return array();
        if (c == '"') {
            auto v = std::make_shared<JValue>();
            v->type = JValue::JSTR;
            v->str = string();
            return v;
        }
        if (c == 't' || c == 'f') return boolean();
        if (c == 'n') return null();
        return number();
    }

    JPtr object() {
        auto v = std::make_shared<JValue>();
        v->type = JValue::JOBJ;
        expect('{', "para iniciar o objeto");
        ws();
        if (match('}')) return v;
        while (true) {
            ws();
            if (peek() != '"') fail("esperado chave de string no objeto");
            std::string key = string();
            ws();
            expect(':', "apos a chave");
            v->members.push_back({key, value()});
            ws();
            if (match(',')) continue;
            expect('}', "para fechar o objeto");
            return v;
        }
    }

    JPtr array() {
        auto v = std::make_shared<JValue>();
        v->type = JValue::JARR;
        expect('[', "para iniciar o array");
        ws();
        if (match(']')) return v;
        while (true) {
            v->items.push_back(value());
            ws();
            if (match(',')) continue;
            expect(']', "para fechar o array");
            return v;
        }
    }

    JPtr boolean() {
        auto v = std::make_shared<JValue>();
        v->type = JValue::JBOOL;
        if (src.compare(pos, 4, "true") == 0) { pos += 4; v->b = true; }
        else if (src.compare(pos, 5, "false") == 0) { pos += 5; v->b = false; }
        else fail("literal invalido");
        return v;
    }

    JPtr null() {
        if (src.compare(pos, 4, "null") != 0) fail("literal invalido");
        pos += 4;
        return std::make_shared<JValue>();
    }

    JPtr number() {
        size_t start = pos;
        if (peek() == '-' || peek() == '+') advance();
        bool digits = false;
        while (pos < src.size() && (isdigit((unsigned char)peek()) ||
               peek() == '.' || peek() == 'e' || peek() == 'E' ||
               peek() == '-' || peek() == '+')) {
            if (isdigit((unsigned char)peek())) digits = true;
            advance();
        }
        if (!digits) fail("numero invalido");
        auto v = std::make_shared<JValue>();
        v->type = JValue::JNUM;
        v->num = std::stod(src.substr(start, pos - start));
        return v;
    }

    std::string string() {
        expect('"', "para iniciar a string");
        std::string out;
        while (true) {
            if (pos >= src.size()) fail("string nao terminada");
            char c = advance();
            if (c == '"') return out;
            if (c == '\\') {
                if (pos >= src.size()) fail("escape incompleto");
                char e = advance();
                switch (e) {
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;
                    case 't': out += '\t'; break;
                    case 'u': {
                        if (pos + 4 > src.size()) fail("\\u incompleto");
                        unsigned cp = 0;
                        for (int k = 0; k < 4; k++) {
                            char h = advance();
                            cp <<= 4;
                            if (h >= '0' && h <= '9') cp |= (unsigned)(h - '0');
                            else if (h >= 'a' && h <= 'f') cp |= (unsigned)(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') cp |= (unsigned)(h - 'A' + 10);
                            else fail("hexadecimal invalido em \\u");
                        }
                        // basic UTF-8 (BMP)
                        if (cp < 0x80) out += (char)cp;
                        else if (cp < 0x800) {
                            out += (char)(0xC0 | (cp >> 6));
                            out += (char)(0x80 | (cp & 0x3F));
                        } else {
                            out += (char)(0xE0 | (cp >> 12));
                            out += (char)(0x80 | ((cp >> 6) & 0x3F));
                            out += (char)(0x80 | (cp & 0x3F));
                        }
                        break;
                    }
                    default: fail(std::string("escape invalido '\\") + e + "'");
                }
            } else {
                out += c;
            }
        }
    }
};

// =====================================================================
// Manifest / platform helpers
// =====================================================================
std::string current_platform() {
#if defined(__APPLE__)
    return "darwin";
#elif defined(__linux__)
    return "linux";
#else
    return "unknown";
#endif
}

std::string current_arch() {
#if defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#elif defined(__aarch64__)
    return "aarch64";
#elif defined(__arm__)
    return "armv7l";
#elif defined(__i386__)
    return "x86";
#else
    return "unknown";
#endif
}

std::string dir_of(const std::string& path) {
    size_t p = path.find_last_of("/\\");
    if (p == std::string::npos) return ".";
    if (p == 0) return "/";
    return path.substr(0, p);
}

bool is_absolute(const std::string& p) {
    return !p.empty() && (p[0] == '/' || (p.size() > 2 && p[1] == ':'));
}

int parse_type(const std::string& t) {
    if (t == "int8") return NT_INT8;
    if (t == "int16") return NT_INT16;
    if (t == "int32") return NT_INT32;
    if (t == "int64") return NT_INT64;
    if (t == "uint8") return NT_UINT8;
    if (t == "uint16") return NT_UINT16;
    if (t == "uint32") return NT_UINT32;
    if (t == "uint64") return NT_UINT64;
    if (t == "float32") return NT_FLOAT32;
    if (t == "float64") return NT_FLOAT64;
    if (t == "bool") return NT_BOOL;
    if (t == "string") return NT_STRING;
    if (t == "void" || t.empty()) return NT_VOID;
    throw std::runtime_error("tipo desconhecido no manifesto: '" + t + "'");
}

JPtr jfield(const JPtr& obj, const std::string& key) {
    if (!obj || obj->type != JValue::JOBJ) return nullptr;
    return obj->get(key);
}

std::string jstr(const JPtr& obj, const std::string& key, const std::string& dflt) {
    JPtr v = jfield(obj, key);
    if (!v || v->type == JValue::JNULL) return dflt;
    if (v->type != JValue::JSTR)
        throw std::runtime_error("campo '" + key + "' do manifesto deve ser string");
    return v->str;
}

std::string jstr_req(const JPtr& obj, const std::string& key) {
    JPtr v = jfield(obj, key);
    if (!v || v->type == JValue::JNULL || v->type != JValue::JSTR || v->str.empty())
        throw std::runtime_error("campo obrigatorio '" + key + "' ausente no manifesto");
    return v->str;
}

// =====================================================================
// Registry
// =====================================================================
struct ParamSpec {
    std::string name;
    int type = NT_INT32;
    bool out = false;
};

struct FuncSpec {
    std::string name;
    std::string symbol;
    int ret = NT_VOID;
    std::vector<ParamSpec> params;
    void* fn = nullptr;

    int in_arity() const {
        int n = 0;
        for (const ParamSpec& p : params)
            if (!p.out) n++;
        return n;
    }
    int out_count() const {
        int n = 0;
        for (const ParamSpec& p : params)
            if (p.out) n++;
        return n;
    }
};

struct Module {
    std::string path;   // manifest (registry key)
    std::string name;
    void* handle = nullptr;
    int (*shutdown_fn)() = nullptr;
    std::vector<std::unique_ptr<FuncSpec>> funcs;
    std::vector<std::unique_ptr<NativeVar>> vars;
};

// path -> module; function name -> spec (unique per process)
std::map<std::string, std::unique_ptr<Module>>& modules_reg() {
    static std::map<std::string, std::unique_ptr<Module>> reg;
    return reg;
}
std::map<std::string, FuncSpec*>& functions_reg() {
    static std::map<std::string, FuncSpec*> reg;
    return reg;
}

// =====================================================================
// FFI: dispatch by type combination (no libffi)
// Parameter classes normalized in the ABI:
//   value  : int64_t | float | double | const char*
//   out    : long long* | float* | double* | char*
// =====================================================================
enum { PC_INT = 0, PC_F32, PC_F64, PC_STR, PC_INT_P, PC_F32_P, PC_F64_P, PC_STR_P };

struct OutSlot {
    long long i = 0;
    float f32 = 0.0f;
    double f64 = 0.0;
    char buf[256] = {0};
};

constexpr int MAX_FFI_PARAMS = 3;

int param_class(const ParamSpec& p) {
    if (p.out) {
        switch (p.type) {
            case NT_FLOAT32: return PC_F32_P;
            case NT_FLOAT64: return PC_F64_P;
            case NT_STRING: return PC_STR_P;
            default: return PC_INT_P;
        }
    }
    switch (p.type) {
        case NT_FLOAT32: return PC_F32;
        case NT_FLOAT64: return PC_F64;
        case NT_STRING: return PC_STR;
        default: return PC_INT;
    }
}

// Position of the INPUT argument corresponding to parameter i
int arg_pos(const FuncSpec* spec, int i) {
    int n = 0;
    for (int k = 0; k < i; k++)
        if (!spec->params[k].out) n++;
    return n;
}
// Order of the OUTPUT temporary corresponding to parameter i
int out_ord(const FuncSpec* spec, int i) {
    int n = 0;
    for (int k = 0; k < i; k++)
        if (spec->params[k].out) n++;
    return n;
}

// Recursive dispatch: each level fixes a static type per parameter
template <int K, typename Ret, typename... Acc>
struct Disp {
    static Ret run(const FuncSpec* spec, Value* args, OutSlot* outs, int i,
                   Acc... acc) {
        const ParamSpec& p = spec->params[i];
        int cls = param_class(p);
        int ai = arg_pos(spec, i);
        int oi = out_ord(spec, i);
        switch (cls) {
            case PC_INT:
                return Disp<K - 1, Ret, Acc..., long long>::run(
                    spec, args, outs, i + 1, acc..., (long long)args[ai].f);
            case PC_F32:
                return Disp<K - 1, Ret, Acc..., float>::run(
                    spec, args, outs, i + 1, acc..., (float)args[ai].f);
            case PC_F64:
                return Disp<K - 1, Ret, Acc..., double>::run(
                    spec, args, outs, i + 1, acc..., (double)args[ai].f);
            case PC_STR:
                return Disp<K - 1, Ret, Acc..., const char*>::run(
                    spec, args, outs, i + 1, acc..., args[ai].s.c_str());
            case PC_INT_P:
                return Disp<K - 1, Ret, Acc..., long long*>::run(
                    spec, args, outs, i + 1, acc..., &outs[oi].i);
            case PC_F32_P:
                return Disp<K - 1, Ret, Acc..., float*>::run(
                    spec, args, outs, i + 1, acc..., &outs[oi].f32);
            case PC_F64_P:
                return Disp<K - 1, Ret, Acc..., double*>::run(
                    spec, args, outs, i + 1, acc..., &outs[oi].f64);
            case PC_STR_P:
                return Disp<K - 1, Ret, Acc..., char*>::run(
                    spec, args, outs, i + 1, acc..., outs[oi].buf);
        }
        throw std::runtime_error("classe de parametro invalida no FFI");
    }
};

template <typename Ret, typename... Acc>
struct Disp<0, Ret, Acc...> {
    static Ret run(const FuncSpec* spec, Value*, OutSlot*, int, Acc... acc) {
        auto f = reinterpret_cast<Ret (*)(Acc...)>(spec->fn);
        if constexpr (std::is_void<Ret>::value) {
            f(acc...);
            return;
        } else {
            return f(acc...);
        }
    }
};

template <typename Ret>
Ret call_n(const FuncSpec* spec, Value* args, OutSlot* outs) {
    switch ((int)spec->params.size()) {
        case 0: return Disp<0, Ret>::run(spec, args, outs, 0);
        case 1: return Disp<1, Ret>::run(spec, args, outs, 0);
        case 2: return Disp<2, Ret>::run(spec, args, outs, 0);
        case 3: return Disp<3, Ret>::run(spec, args, outs, 0);
    }
    throw std::runtime_error(spec->name + ": funcoes com mais de " +
                             std::to_string(MAX_FFI_PARAMS) +
                             " parametros nao sao suportadas");
}

void invoke(const FuncSpec* spec, Value* args, OutSlot* outs, Value* res) {
    switch (spec->ret) {
        case NT_VOID:
            call_n<void>(spec, args, outs);
            res->type = VAL_FLOAT;
            res->f = 0.0f;
            break;
        case NT_FLOAT32: {
            float r = call_n<float>(spec, args, outs);
            res->type = VAL_FLOAT;
            res->f = r;
            break;
        }
        case NT_FLOAT64: {
            double r = call_n<double>(spec, args, outs);
            res->type = VAL_FLOAT;
            res->f = (float)r;
            break;
        }
        case NT_STRING: {
            const char* r = call_n<const char*>(spec, args, outs);
            res->type = VAL_STRING;
            res->s = r ? r : "";
            break;
        }
        default: {
            long long r = call_n<long long>(spec, args, outs);
            // Extends according to the declared type (the callee only guarantees the width)
            switch (spec->ret) {
                case NT_INT8: r = (long long)(int8_t)r; break;
                case NT_INT16: r = (long long)(int16_t)r; break;
                case NT_INT32: r = (long long)(int32_t)r; break;
                case NT_UINT8: r = (long long)(uint8_t)r; break;
                case NT_UINT16: r = (long long)(uint16_t)r; break;
                case NT_UINT32: r = (long long)(uint32_t)r; break;
                case NT_BOOL: r = r ? 1 : 0; break;
                default: break;
            }
            res->type = VAL_FLOAT;
            res->f = (float)r;
            break;
        }
    }
}

void pack_out(const ParamSpec& p, const OutSlot& o, Value& v) {
    v = Value{VAL_FLOAT, 0.0};
    switch (p.type) {
        case NT_FLOAT32: v.f = o.f32; break;
        case NT_FLOAT64: v.f = o.f64; break;
        case NT_INT8: v.f = (double)(int8_t)(uint8_t)(o.i & 0xFF); break;
        case NT_INT16: v.f = (double)(int16_t)(uint16_t)(o.i & 0xFFFF); break;
        case NT_INT32: v.f = (double)(int32_t)(uint32_t)(o.i & 0xFFFFFFFFu); break;
        case NT_UINT8: v.f = (double)(uint8_t)(o.i & 0xFF); break;
        case NT_UINT16: v.f = (double)(uint16_t)(o.i & 0xFFFF); break;
        case NT_UINT32: v.f = (double)(uint32_t)(o.i & 0xFFFFFFFFu); break;
        case NT_BOOL: v.f = (o.i & 1) ? 1.0f : 0.0f; break;
        case NT_STRING: {
            v.type = VAL_STRING;
            v.s.assign(o.buf, strnlen(o.buf, sizeof(o.buf) - 1));
            break;
        }
        default: v.f = (double)o.i; break; // int64/uint64 (exact up to 2^53)
    }
}

void check_args(const FuncSpec* spec, Value* args) {
    for (size_t i = 0; i < spec->params.size(); i++) {
        const ParamSpec& p = spec->params[i];
        if (p.out) continue;
        const Value& v = args[arg_pos(spec, (int)i)];
        if (param_class(p) == PC_STR) {
            if (v.type != VAL_STRING)
                throw std::runtime_error(spec->name + ": argumento '" +
                                         (p.name.empty() ? std::to_string(i) : p.name) +
                                         "' deve ser string");
        } else {
            if (v.type != VAL_FLOAT && v.type != VAL_INT && v.type != VAL_BOOL)
                throw std::runtime_error(spec->name + ": argumento '" +
                                         (p.name.empty() ? std::to_string(i) : p.name) +
                                         "' deve ser numero");
        }
    }
}

// =====================================================================
// Import
// =====================================================================
FuncSpec* parse_function(const JPtr& jf) {
    auto spec = std::unique_ptr<FuncSpec>(new FuncSpec());
    spec->name = jstr_req(jf, "name");
    spec->symbol = jstr(jf, "symbol", spec->name);
    spec->ret = parse_type(jstr(jf, "return", "void"));

    JPtr params = jfield(jf, "parameters");
    if (params && params->type != JValue::JNULL) {
        if (params->type != JValue::JARR)
            throw std::runtime_error("campo 'parameters' de '" + spec->name +
                                     "' deve ser array");
        for (const JPtr& jp : params->items) {
            if (!jp || jp->type != JValue::JOBJ)
                throw std::runtime_error("parameter de '" + spec->name +
                                         "' deve ser objeto");
            ParamSpec p;
            p.name = jstr(jp, "name", "");
            p.type = parse_type(jstr(jp, "type", "int32"));
            if (p.type == NT_VOID)
                throw std::runtime_error("parameter '" + p.name + "' de '" +
                                         spec->name + "' nao pode ser void");
            std::string dir = jstr(jp, "direction", "in");
            if (dir == "out") p.out = true;
            else if (dir != "in")
                throw std::runtime_error("direction invalida em '" + p.name +
                                         "' (use \"in\" ou \"out\")");
            if ((int)spec->params.size() >= MAX_FFI_PARAMS)
                throw std::runtime_error(spec->name + ": maximo de " +
                                         std::to_string(MAX_FFI_PARAMS) +
                                         " parametros no manifesto");
            spec->params.push_back(p);
        }
    }
    return spec.release();
}

NativeVar* parse_variable(const JPtr& jv, std::string* sym_out) {
    auto nv = std::unique_ptr<NativeVar>(new NativeVar());
    nv->name = jstr_req(jv, "name");
    *sym_out = jstr(jv, "symbol", nv->name);
    nv->type = parse_type(jstr(jv, "type", ""));
    if (nv->type == NT_VOID)
        throw std::runtime_error("variavel '" + nv->name + "' nao pode ser void");
    JPtr ro = jfield(jv, "readonly");
    nv->readonly = (ro && ro->type == JValue::JBOOL) ? ro->b : false;
    JPtr sz = jfield(jv, "size");
    if (sz && sz->type == JValue::JNUM && sz->num > 0) nv->size = (int)sz->num;
    if (nv->type == NT_STRING && nv->size > (int)sizeof(OutSlot::buf))
        nv->size = (int)sizeof(OutSlot::buf);
    return nv.release();
}

// Validates conflicts (step 1) and then rebinds the symbols (step 2): either nothing
// is written, or everything — never symbols pointing to already-freed memory.
void rebind_symbols(Module* mod) {
    for (auto& vptr : mod->vars) {
        NativeVar* nv = vptr.get();
        int sid = sym_find(nv->name.c_str());
        if (sid == -1) continue;
        Symbol* s = sym_get(sid);
        if (s->is_function)
            throw std::runtime_error("'" + nv->name + "' ja e uma funcao");
        if (s->native)
            throw std::runtime_error("'" + nv->name +
                                     "' ja e nativa de outro modulo");
    }
    for (auto& vptr : mod->vars) {
        NativeVar* nv = vptr.get();
        int sid = sym_find(nv->name.c_str());
        if (sid == -1) sid = sym_create(nv->name.c_str(), Value{VAL_FLOAT, 0.0f});
        Symbol* s = sym_get(sid);
        s->native = nv;
        s->force_global = true;
    }
}

bool import_impl(const std::string& path) {
    auto already = modules_reg().find(path);
    if (already != modules_reg().end()) {
        rebind_symbols(already->second.get());
        return false;
    }

    std::ifstream f(path);
    if (!f)
        throw std::runtime_error("nao foi possivel abrir '" + path + "'");
    std::string content((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());

    JPtr root = JsonParser(content).parse();
    if (!root || root->type != JValue::JOBJ)
        throw std::runtime_error("manifesto deve ser um objeto JSON");

    std::string name = jstr_req(root, "name");
    std::string library = jstr_req(root, "library");

    JPtr plat = jfield(root, "platform");
    if (plat && plat->type == JValue::JSTR && plat->str != current_platform())
        throw std::runtime_error("modulo '" + name + "' e para a plataforma '" +
                                 plat->str + "' (atual: " + current_platform() + ")");
    JPtr arch = jfield(root, "arch");
    if (arch && arch->type == JValue::JSTR && arch->str != current_arch())
        throw std::runtime_error("modulo '" + name + "' e para a arquitetura '" +
                                 arch->str + "' (atual: " + current_arch() + ")");

    std::string libpath = is_absolute(library)
                              ? library
                              : dir_of(path) + "/" + library;

    // integrity (E8/neg18): sha256 of the library against the manifest
    std::string integ = jstr(root, "integrity", "");
    if (!integ.empty()) {
        std::string got = sha256_file(libpath);
        auto lower = [](std::string s) {
            for (char& c : s) c = (char)std::tolower((unsigned char)c);
            return s;
        };
        if (lower(got) != lower(integ))
            throw std::runtime_error(
                "integrity: hash sha256 de '" + libpath +
                "' nao confere (esperado " + integ + ", obtido " + got + ")");
    }

    void* handle = dlopen(libpath.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        const char* err = dlerror();
        throw std::runtime_error("nao foi possivel abrir a biblioteca '" +
                                 libpath + "': " + (err ? err : "erro desconhecido"));
    }

    auto mod = std::unique_ptr<Module>(new Module());
    mod->path = path;
    mod->name = name;
    mod->handle = handle;

    try {
        // Functions
        JPtr jfns = jfield(root, "functions");
        if (jfns && jfns->type != JValue::JNULL) {
            if (jfns->type != JValue::JARR)
                throw std::runtime_error("campo 'functions' deve ser array");
            for (const JPtr& jf : jfns->items) {
                std::unique_ptr<FuncSpec> spec(parse_function(jf));
                if (functions_reg().count(spec->name))
                    throw std::runtime_error("funcao '" + spec->name +
                                             "' ja registrada por outro modulo");
                void* sym = dlsym(handle, spec->symbol.c_str());
                if (!sym)
                    throw std::runtime_error(
                        "simbolo '" + spec->symbol + "' (funcao '" + spec->name +
                        "') nao encontrado na biblioteca");
                spec->fn = sym;
                mod->funcs.push_back(std::move(spec));
            }
        }

        // optional shutdown (called only in modules_shutdown)
        std::string shut_name = jstr(root, "shutdown", "");
        if (!shut_name.empty()) {
            int (*shutdown_fn)() = (int (*)())dlsym(handle, shut_name.c_str());
            if (!shutdown_fn)
                throw std::runtime_error("funcao de encerramento '" + shut_name +
                                         "' nao encontrada na biblioteca");
            mod->shutdown_fn = shutdown_fn;
        }

        // Variables (dlsym + conflicts with existing symbols)
        JPtr jvars = jfield(root, "variables");
        if (jvars && jvars->type != JValue::JNULL) {
            if (jvars->type != JValue::JARR)
                throw std::runtime_error("campo 'variables' deve ser array");
            for (const JPtr& jv : jvars->items) {
                std::string symname;
                std::unique_ptr<NativeVar> nv(parse_variable(jv, &symname));
                void* addr = dlsym(handle, symname.c_str());
                if (!addr)
                    throw std::runtime_error(
                        "simbolo '" + symname + "' (variavel '" + nv->name +
                        "') nao encontrado na biblioteca");
                nv->addr = addr;
                int sid = sym_find(nv->name.c_str());
                if (sid != -1) {
                    Symbol* s = sym_get(sid);
                    if (s->is_function)
                        throw std::runtime_error("'" + nv->name + "' ja e uma funcao");
                    if (s->native)
                        throw std::runtime_error("'" + nv->name +
                                                 "' ja e nativa de outro modulo");
                }
                mod->vars.push_back(std::move(nv));
            }
        }

        // optional initialize_function (after parsing, before rebinding
        // symbols): non-zero return = failure
        std::string init_name = jstr(root, "init", "");
        if (!init_name.empty()) {
            int (*init_fn)() = (int (*)())dlsym(handle, init_name.c_str());
            if (!init_fn)
                throw std::runtime_error("funcao de inicializacao '" + init_name +
                                         "' nao encontrada na biblioteca");
            int rc = init_fn();
            if (rc != 0)
                throw std::runtime_error("initialize_function de '" + name +
                                         "' retornou " + std::to_string(rc));
        }

        // Last point that can fail with cleanup: rebinds the symbols.
        // (two steps: validate everything, then write everything)
        rebind_symbols(mod.get());
    } catch (...) {
        dlclose(handle);
        throw;
    }

    // Commits ownership (NativeVar addresses do not change on move)
    for (auto& fp : mod->funcs) functions_reg()[fp->name] = fp.get();
    modules_reg()[path] = std::move(mod);
    return true;
}

} // namespace

// =====================================================================
// Public API
// =====================================================================
bool modules_import(const std::string& path) {
    try {
        return import_impl(path);
    } catch (const std::exception& e) {
        std::string m = e.what();
        if (m.rfind("import:", 0) == 0) throw;
        throw std::runtime_error("import: " + m);
    }
}

bool modules_is_callable(const std::string& name, int argc) {
    auto it = functions_reg().find(name);
    return it != functions_reg().end() && it->second->in_arity() == argc;
}

bool modules_known_function(const std::string& name) {
    return functions_reg().count(name) > 0;
}

int modules_arity(const std::string& name) {
    auto it = functions_reg().find(name);
    return it == functions_reg().end() ? -1 : it->second->in_arity();
}

bool modules_call(const std::string& name, Value* args, int argc, Value* res) {
    auto it = functions_reg().find(name);
    if (it == functions_reg().end()) return false;
    FuncSpec* spec = it->second;
    if (spec->in_arity() != argc) return false;

    check_args(spec, args);

    OutSlot outs[3] = {};
    invoke(spec, args, outs, res);

    int nout = spec->out_count();
    if (nout > 0) {
        Value arr;
        arr.type = VAL_ARRAY;
        arr.obj = std::make_shared<ArrayData>();
        gc_register_obj(GC_OBJ_ARRAY, arr.obj.p);
        arr.arr()->elements.push_back(*res);
        int ord = 0;
        for (const ParamSpec& p : spec->params) {
            if (!p.out) continue;
            Value v;
            pack_out(p, outs[ord], v);
            arr.arr()->elements.push_back(v);
            ord++;
        }
        arr.arr()->rows = 1;
        arr.arr()->cols = 1 + nout;
        *res = arr;
    }
    return true;
}

void modules_shutdown() {
    static bool done = false;
    if (done) return;
    done = true;
    for (auto& kv : modules_reg()) {
        Module* m = kv.second.get();
        if (m->shutdown_fn) {
            try { m->shutdown_fn(); } catch (...) {}
        }
        if (m->handle) {
            dlclose(m->handle);
            m->handle = nullptr;
        }
    }
    functions_reg().clear();
    modules_reg().clear();
}

// =====================================================================
// Native variables (OP_LOAD/OP_STORE)
// =====================================================================
void native_read(const NativeVar* nv, Value* out) {
    if (!nv || !nv->addr)
        throw std::runtime_error("variavel nativa invalida");
    out->type = VAL_FLOAT;
    out->f = 0.0f;
    out->s.clear();
    switch (nv->type) {
        case NT_INT8: out->f = (float)*(int8_t*)nv->addr; break;
        case NT_INT16: out->f = (float)*(int16_t*)nv->addr; break;
        case NT_INT32: out->f = (double)*(int32_t*)nv->addr; break;
        case NT_INT64: out->f = (double)*(int64_t*)nv->addr; break;
        case NT_UINT8: out->f = (double)*(uint8_t*)nv->addr; break;
        case NT_UINT16: out->f = (double)*(uint16_t*)nv->addr; break;
        case NT_UINT32: out->f = (double)*(uint32_t*)nv->addr; break;
        case NT_UINT64: out->f = (double)*(uint64_t*)nv->addr; break;
        case NT_FLOAT32: out->f = *(float*)nv->addr; break;
        case NT_FLOAT64: out->f = *(double*)nv->addr; break;
        case NT_BOOL: out->f = (*(unsigned char*)nv->addr) ? 1.0f : 0.0f; break;
        case NT_STRING: {
            const char* p = (const char*)nv->addr;
            size_t cap = nv->size > 0 ? (size_t)nv->size - 1 : 255;
            out->type = VAL_STRING;
            out->s.assign(p, strnlen(p, cap));
            break;
        }
        default:
            throw std::runtime_error("tipo de variavel nativa nao suportado");
    }
}

void native_write(const NativeVar* nv, const Value& v) {
    if (!nv || !nv->addr)
        throw std::runtime_error("variavel nativa invalida");
    if (nv->readonly)
        throw std::runtime_error("variavel nativa '" + nv->name +
                                 "' e somente leitura");
    if (nv->type == NT_STRING) {
        if (v.type != VAL_STRING)
            throw std::runtime_error("variavel nativa '" + nv->name +
                                     "' espera uma string");
        char* dst = (char*)nv->addr;
        size_t cap = nv->size > 0 ? (size_t)nv->size - 1 : 255;
        size_t n = v.s.size() < cap ? v.s.size() : cap;
        memcpy(dst, v.s.data(), n);
        dst[n] = '\0';
        return;
    }
    if (v.type != VAL_FLOAT && v.type != VAL_INT && v.type != VAL_BOOL)
        throw std::runtime_error("variavel nativa '" + nv->name +
                                 "' espera um numero");
    double x = (double)v.f;
    switch (nv->type) {
        case NT_INT8: *(int8_t*)nv->addr = (int8_t)x; break;
        case NT_INT16: *(int16_t*)nv->addr = (int16_t)x; break;
        case NT_INT32: *(int32_t*)nv->addr = (int32_t)x; break;
        case NT_INT64: *(int64_t*)nv->addr = (int64_t)x; break;
        case NT_UINT8: *(uint8_t*)nv->addr = (uint8_t)x; break;
        case NT_UINT16: *(uint16_t*)nv->addr = (uint16_t)x; break;
        case NT_UINT32: *(uint32_t*)nv->addr = (uint32_t)x; break;
        case NT_UINT64: *(uint64_t*)nv->addr = (uint64_t)x; break;
        case NT_FLOAT32: *(float*)nv->addr = (float)x; break;
        case NT_FLOAT64: *(double*)nv->addr = x; break;
        case NT_BOOL: *(unsigned char*)nv->addr = (x != 0) ? 1 : 0; break;
        default:
            throw std::runtime_error("tipo de variavel nativa nao suportado");
    }
}
