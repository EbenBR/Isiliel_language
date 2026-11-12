// Isiliel — L9: string pattern matcher (subset of Lua 5.4).
// Adapted from the logic of lstrlib.c (Lua, MIT) without external dependencies.
#include "patterns.h"
#include "vm.h"

#include <cctype>
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr int MAXCAPTURES = 32;
constexpr int CAP_UNFINISHED = -1;
constexpr int MAXDEPTH = 200;

struct MatchState {
    const char* src_init = nullptr;
    const char* src_end = nullptr;
    const char* pat_end = nullptr;
    int matchdepth = MAXDEPTH;
    int level = 0;
    struct Capture { const char* init; ptrdiff_t len; } capture[MAXCAPTURES];
};

int class_match(int c, int cl) {
    int res;
    switch (std::tolower(cl)) {
        case 'a': res = std::isalpha(c); break;
        case 'c': res = std::iscntrl(c); break;
        case 'd': res = std::isdigit(c); break;
        case 'g': res = std::isgraph(c); break;
        case 'l': res = std::islower(c); break;
        case 'p': res = std::ispunct(c); break;
        case 's': res = std::isspace(c); break;
        case 'u': res = std::isupper(c); break;
        case 'w': res = std::isalnum(c); break;
        case 'x': res = std::isxdigit(c); break;
        case 'z': res = (c == 0); break;
        default: return (cl == c);
    }
    if (std::isupper(cl)) res = !res;
    return res;
}

const char* class_end(MatchState& ms, const char* p) {
    if (*p == '%') {
        if (p + 1 >= ms.pat_end)
            throw std::runtime_error("pattern: '%' no fim");
        return p + 2;
    }
    if (*p == '[') {
        p++;
        if (p < ms.pat_end && *p == '^') p++;
        if (p < ms.pat_end && *p == ']') p++; // leading ']' is literal
        while (p < ms.pat_end && *p != ']') {
            if (*p == '%' && p + 1 < ms.pat_end) p++;
            p++;
        }
        if (p >= ms.pat_end)
            throw std::runtime_error("pattern: '[' sem ']'");
        return p + 1;
    }
    return p + 1;
}

bool bracket_match(int c, const char* p, const char* ec) {
    // p points to '['; ec to ']'
    bool sig = true;
    p++;
    if (*p == '^') { sig = false; p++; }
    while (p < ec) {
        if (*p == '%') {
            p++;
            if (class_match(c, (unsigned char)*p)) return sig;
        } else if (p + 2 < ec && p[1] == '-') {
            if ((unsigned char)p[0] <= c && c <= (unsigned char)p[2]) return sig;
            p += 2;
        } else if ((unsigned char)*p == c) {
            return sig;
        }
        p++;
    }
    return !sig;
}

bool single_match(MatchState& ms, const char* s, const char* p, const char* ep) {
    (void)ms;
    if (s >= ms.src_end) return false;
    switch (*p) {
        case '.': return true;
        case '%': return class_match((unsigned char)*s, (unsigned char)p[1]);
        case '[': return bracket_match((unsigned char)*s, p, ep - 1);
        default: return (*p == *s);
    }
}

const char* do_match(MatchState& ms, const char* s, const char* p);

const char* max_expand(MatchState& ms, const char* s, const char* p, const char* ep) {
    ptrdiff_t i = 0;
    while (single_match(ms, s + i, p, ep)) i++;
    while (i >= 0) {
        const char* res = do_match(ms, s + i, ep + 1);
        if (res) return res;
        i--;
    }
    return nullptr;
}

const char* min_expand(MatchState& ms, const char* s, const char* p, const char* ep) {
    for (;;) {
        const char* res = do_match(ms, s, ep + 1);
        if (res) return res;
        if (single_match(ms, s, p, ep)) s++;
        else return nullptr;
    }
}

const char* start_capture(MatchState& ms, const char* s, const char* p, int what) {
    if (ms.level >= MAXCAPTURES)
        throw std::runtime_error("pattern: capturas demais");
    ms.capture[ms.level].len = what;
    ms.capture[ms.level].init = s;
    ms.level++;
    const char* res = do_match(ms, s, p);
    if (!res) ms.level--;
    return res;
}

const char* end_capture(MatchState& ms, const char* s, const char* p) {
    int l = -1;
    for (int i = ms.level - 1; i >= 0; i--) {
        if (ms.capture[i].len == CAP_UNFINISHED) { l = i; break; }
    }
    if (l < 0) throw std::runtime_error("pattern: ')' sem '('");
    ms.capture[l].len = s - ms.capture[l].init;
    const char* res = do_match(ms, s, p);
    if (!res) ms.capture[l].len = CAP_UNFINISHED;
    return res;
}

const char* match_capture(MatchState& ms, const char* s, int l) {
    l = l - '1';
    if (l < 0 || l >= ms.level || ms.capture[l].len == CAP_UNFINISHED)
        throw std::runtime_error("pattern: referencia a captura invalida");
    size_t len = (size_t)ms.capture[l].len;
    if ((size_t)(ms.src_end - s) >= len &&
        std::memcmp(ms.capture[l].init, s, len) == 0)
        return s + len;
    return nullptr;
}

const char* match_balance(MatchState& ms, const char* s, const char* p) {
    if (p + 1 >= ms.pat_end)
        throw std::runtime_error("pattern: %b sem par");
    if (s >= ms.src_end || *s != *p) return nullptr;
    int b = *p, e = p[1];
    int cont = 1;
    s++;
    while (s < ms.src_end) {
        if (*s == e) { if (--cont == 0) return s + 1; }
        else if (*s == b) cont++;
        s++;
    }
    return nullptr;
}

const char* match_frontier(MatchState& ms, const char* s, const char* p) {
    if (p >= ms.pat_end || *p != '[')
        throw std::runtime_error("pattern: %f sem [...]");
    const char* ep = class_end(ms, p);
    int c = (s < ms.src_end) ? (unsigned char)*s : 0;
    int prev = (s == ms.src_init) ? 0 : (unsigned char)s[-1];
    if (bracket_match(prev, p, ep - 1)) return nullptr;
    if (bracket_match(c, p, ep - 1)) return s;
    return nullptr;
}

const char* do_match(MatchState& ms, const char* s, const char* p) {
    if (ms.matchdepth-- == 0)
        throw std::runtime_error("pattern: expressao complexa demais");
    const char* result = nullptr;
    if (p == ms.pat_end) {
        result = s;
    } else {
        switch (*p) {
            case '(':
                if (p + 1 < ms.pat_end && p[1] == ')') // () = empty capture
                    result = start_capture(ms, s, p + 2, 0);
                else
                    result = start_capture(ms, s, p + 1, CAP_UNFINISHED);
                break;
            case ')':
                result = end_capture(ms, s, p + 1);
                break;
            case '$':
                if (p + 1 == ms.pat_end) result = (s == ms.src_end) ? s : nullptr;
                else {
                    const char* ep = class_end(ms, p);
                    if (!single_match(ms, s, p, ep)) result = nullptr;
                    else if (*ep == '*' || *ep == '?' || *ep == '-') result = do_match(ms, s, ep + 1);
                    else result = do_match(ms, s + 1, ep);
                }
                break;
            case '%': {
                if (p + 1 < ms.pat_end && p[1] == 'b') {
                    result = match_balance(ms, s, p + 2);
                    if (result) result = do_match(ms, result, p + 4);
                } else if (p + 1 < ms.pat_end && p[1] == 'f') {
                    result = match_frontier(ms, s, p + 2);
                    if (result) result = do_match(ms, result, class_end(ms, p + 2));
                } else if (p + 1 < ms.pat_end && std::isdigit((unsigned char)p[1])) {
                    result = match_capture(ms, s, (unsigned char)p[1]);
                    if (result) result = do_match(ms, result, p + 2);
                } else {
                    const char* ep = class_end(ms, p);
                    if (!single_match(ms, s, p, ep)) {
                        if (*ep == '*' || *ep == '?' || *ep == '-')
                            result = do_match(ms, s, ep + 1);
                    } else {
                        switch (*ep) {
                            case '?': {
                                const char* r = do_match(ms, s + 1, ep + 1);
                                result = r ? r : do_match(ms, s, ep + 1);
                                break;
                            }
                            case '+': result = max_expand(ms, s + 1, p, ep); break;
                            case '*': result = max_expand(ms, s, p, ep); break;
                            case '-': result = min_expand(ms, s, p, ep); break;
                            default: result = do_match(ms, s + 1, ep); break;
                        }
                    }
                }
                break;
            }
            default: {
                const char* ep = class_end(ms, p);
                if (!single_match(ms, s, p, ep)) {
                    if (*ep == '*' || *ep == '?' || *ep == '-')
                        result = do_match(ms, s, ep + 1);
                } else {
                    switch (*ep) {
                        case '?': {
                            const char* r = do_match(ms, s + 1, ep + 1);
                            result = r ? r : do_match(ms, s, ep + 1);
                            break;
                        }
                        case '+': result = max_expand(ms, s + 1, p, ep); break;
                        case '*': result = max_expand(ms, s, p, ep); break;
                        case '-': result = min_expand(ms, s, p, ep); break;
                        default: result = do_match(ms, s + 1, ep); break;
                    }
                }
                break;
            }
        }
    }
    ms.matchdepth++;
    return result;
}

// Tries to match starting at `init` (0-based offset). Returns the end (offset) and
// the captures (1..level); if level==0, caps[0] = whole match.
bool run_match(const std::string& s, const std::string& pat, size_t init,
               size_t& mstart, size_t& mend, std::vector<std::string>& caps) {
    MatchState ms;
    ms.src_init = s.data();
    ms.src_end = s.data() + s.size();
    ms.pat_end = pat.data() + pat.size();
    const char* p = pat.data();
    bool anchor = (!pat.empty() && *p == '^');
    if (anchor) p++;
    if (init > s.size()) return false;
    const char* s1 = s.data() + init;
    do {
        ms.level = 0;
        ms.matchdepth = MAXDEPTH;
        const char* e = do_match(ms, s1, p);
        if (e) {
            mstart = (size_t)(s1 - s.data());
            mend = (size_t)(e - s.data());
            caps.clear();
            if (ms.level == 0) {
                caps.push_back(s.substr(mstart, mend - mstart));
            } else {
                for (int i = 0; i < ms.level; i++) {
                    if (ms.capture[i].len == CAP_UNFINISHED)
                        throw std::runtime_error("pattern: captura nao fechada");
                    caps.push_back(std::string(ms.capture[i].init,
                                               (size_t)ms.capture[i].len));
                }
            }
            return true;
        }
    } while (s1++ < ms.src_end && !anchor);
    return false;
}

void require_str(const Value& v, const char* fn) {
    if (v.type != VAL_STRING)
        throw std::runtime_error(std::string(fn) + ": espera texto");
}

// match(s, pat[, init]) -> first match (or 1st capture); "" on failure
void b_match(Value* a, int n, Value* r) {
    require_str(a[0], "match");
    require_str(a[1], "match");
    size_t init = (n >= 3) ? (size_t)(a[2].f < 0 ? 0 : a[2].f) : 0;
    size_t ms, me;
    std::vector<std::string> caps;
    if (!run_match(a[0].s.str(), a[1].s.str(), init, ms, me, caps)) {
        *r = make_string("");
        return;
    }
    *r = make_string(caps.empty() ? std::string() : caps[0]);
}

// gmatch(s, pat) -> 1D list with all matches (no iterator)
void b_gmatch(Value* a, int n, Value* r) {
    (void)n;
    require_str(a[0], "gmatch");
    require_str(a[1], "gmatch");
    const std::string& s = a[0].s.str();
    const std::string& pat = a[1].s.str();
    auto ad = std::make_shared<ArrayData>();
    gc_register_obj(GC_OBJ_ARRAY, ad);
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t ms, me;
        std::vector<std::string> caps;
        if (!run_match(s, pat, pos, ms, me, caps)) break;
        ad->elements.push_back(make_string(caps.empty() ? std::string() : caps[0]));
        if (me > pos) pos = me;
        else pos = me + 1; // empty match: advance 1 to avoid repeating
    }
    ad->rows = 1;
    ad->cols = (int)ad->elements.size();
    Value v;
    v.type = VAL_ARRAY;
    v.obj = RefObj(ad);
    *r = v;
}

// gsub(s, pat, repl) -> new string; text repl with %0..%9 and %%
void b_gsub(Value* a, int n, Value* r) {
    (void)n;
    require_str(a[0], "gsub");
    require_str(a[1], "gsub");
    require_str(a[2], "gsub");
    const std::string& s = a[0].s.str();
    const std::string& pat = a[1].s.str();
    const std::string& repl = a[2].s.str();
    std::string out;
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t ms, me;
        std::vector<std::string> caps;
        if (!run_match(s, pat, pos, ms, me, caps)) break;
        out.append(s, pos, ms - pos);
        for (size_t i = 0; i < repl.size(); i++) {
            if (repl[i] == '%' && i + 1 < repl.size()) {
                char c = repl[i + 1];
                if (c == '%') { out += '%'; i++; }
                else if (c >= '0' && c <= '9') {
                    size_t idx = (size_t)(c - '0');
                    if (idx == 0) out.append(s, ms, me - ms);
                    else if (idx <= caps.size()) out += caps[idx - 1];
                    i++;
                } else { out += '%'; }
            } else {
                out += repl[i];
            }
        }
        if (me > pos) pos = me;
        else { if (pos < s.size()) out += s[pos]; pos = me + 1; }
    }
    if (pos < s.size()) out.append(s, pos, std::string::npos);
    *r = make_string(out);
}

} // namespace

void register_pattern_builtins() {
    static bool done = false;
    if (done) return;
    done = true;
    register_builtin("match", -1, b_match);
    register_builtin("gmatch", 2, b_gmatch);
    register_builtin("gsub", 3, b_gsub);
}
