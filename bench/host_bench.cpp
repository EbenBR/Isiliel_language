// Isiliel — benchmark host: loads and runs the same scripts in
// Lua 5.4 and Isiliel, measuring load + execution and checking the checksums.
// Authors: Eben (AI-assisted) | Version: 1.0 | Date: 2026-11-08
#include "interp.h"

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using Clock = std::chrono::steady_clock;

static double ms_since(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

static std::string slurp(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

struct Run {
    bool ok = false;
    double ms = 0.0;
    long long val = 0;
    std::string err;
};

// Lua: luaL_loadfile + lua_pcall timed together (load + execution),
// the same interval measured in Isiliel. The script ends with `return <n>`.
static Run run_lua(const std::string& path) {
    Run r;
    lua_State* L = luaL_newstate();
    if (!L) { r.err = "luaL_newstate failed"; return r; }
    luaL_openlibs(L);
    Clock::time_point t0 = Clock::now();
    if (luaL_loadfile(L, path.c_str()) != LUA_OK) {
        const char* e = lua_tostring(L, -1);
        r.err = e ? e : "Lua load error";
        lua_close(L);
        return r;
    }
    if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
        const char* e = lua_tostring(L, -1);
        r.err = e ? e : "Lua execution error";
        lua_close(L);
        return r;
    }
    r.ms = ms_since(t0);
    if (lua_isinteger(L, -1)) {
        r.val = (long long)lua_tointeger(L, -1);
        r.ok = true;
    } else if (lua_isnumber(L, -1)) {
        r.val = (long long)lua_tonumber(L, -1);
        r.ok = true;
    } else {
        r.err = "Lua script did not return a number";
    }
    lua_close(L);
    return r;
}

// Isiliel: interp_run(source) with stdout capture; the script prints the
// checksum with printf("%d\n", ...). State is restarted on every run.
static Run run_isl(const std::string& path) {
    Run r;
    interp_reset();
    InterpOptions o;
    o.capture = true;
    Clock::time_point t0 = Clock::now();
    InterpResult res = interp_run(slurp(path), o);
    r.ms = ms_since(t0);
    if (!res.ok) {
        r.err = res.error;
        interp_reset();
        return r;
    }
    std::string line, last;
    std::istringstream ss(res.output);
    while (std::getline(ss, line))
        if (line.find_first_not_of(" \t\r\n") != std::string::npos) last = line;
    char* end = nullptr;
    long long v = std::strtoll(last.c_str(), &end, 10);
    if (end == last.c_str() || last.empty()) {
        r.err = "output has no number: '" + last + "'";
        interp_reset();
        return r;
    }
    r.val = v;
    r.ok = true;
    interp_reset();
    return r;
}

struct Bench {
    const char* name;   // label in the table
    const char* lua_path;
    const char* isl_path;
    long long expect;   // expected checksum (same on both sides)
};

static const Bench BENCHES[] = {
    {"loop (3e6 arith iters)",   "bench/lua/loop.lua",    "bench/isl/loop.isi",    2997000000LL},
    {"fib(27) recursive",       "bench/lua/fib_rec.lua", "bench/isl/fib_rec.isi", 196418LL},
    {"strings (1e5 pieces)",    "bench/lua/strings.lua", "bench/isl/strings.isi", 300000LL},
    {"tables (5e5 push+sum)",   "bench/lua/tabelas.lua", "bench/isl/tabelas.isi", 125000250000LL},
    {"complex (mandelbrot)",    "bench/lua/complexos.lua", "bench/isl/complexos.isi", 807857LL},
};

static std::string cpu_model() {
    std::ifstream f("/proc/cpuinfo");
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("model name", 0) == 0) {
            size_t p = line.find(':');
            if (p != std::string::npos) {
                std::string s = line.substr(p + 1);
                while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
                return s;
            }
        }
    }
    return "unknown CPU";
}

int main(int argc, char** argv) {
    int reps = argc > 1 ? std::atoi(argv[1]) : 3;
    if (reps < 1) reps = 1;

    std::printf("Isiliel Bench Host — Lua vs Isiliel (load + execution, best of %d)\n", reps);
    std::printf("Lua: %s.%s.%s (liblua5.4.a) | Isiliel: 1.0 (-O2 -DNDEBUG)\n",
                LUA_VERSION_MAJOR, LUA_VERSION_MINOR, LUA_VERSION_RELEASE);
    std::printf("Host: %s | %s\n", cpu_model().c_str(), __VERSION__);
    std::printf("\n");
    std::printf("%-26s %12s %12s %10s  %-9s\n",
                "benchmark", "Lua (ms)", "Isiliel (ms)", "ratio", "checksum");
    std::printf("%-26s %12s %12s %10s  %-9s\n",
                "--------------------------", "------------", "------------", "----------", "---------");

    int failures = 0;
    for (const Bench& b : BENCHES) {
        double lua_ms = 1e18, isl_ms = 1e18;
        long long lua_val = 0, isl_val = 0;
        std::string err;

        for (int i = 0; i < reps && err.empty(); i++) {
            Run r = run_lua(b.lua_path);
            if (!r.ok) { err = "lua: " + r.err; break; }
            lua_val = r.val;
            if (r.ms < lua_ms) lua_ms = r.ms;
        }
        for (int i = 0; i < reps && err.empty(); i++) {
            Run r = run_isl(b.isl_path);
            if (!r.ok) { err = "isiliel: " + r.err; break; }
            isl_val = r.val;
            if (r.ms < isl_ms) isl_ms = r.ms;
        }

        if (!err.empty()) {
            std::printf("%-26s ERROR: %s\n", b.name, err.c_str());
            failures++;
            continue;
        }
        bool ok = (lua_val == b.expect) && (isl_val == b.expect);
        if (!ok) {
            std::printf("%-26s %12.3f %12.3f %10s  DIVERGES (lua=%lld isl=%lld, expected=%lld)\n",
                        b.name, lua_ms, isl_ms, "-", lua_val, isl_val, b.expect);
            failures++;
            continue;
        }
        std::printf("%-26s %12.3f %12.3f %9.1fx  ok\n",
                    b.name, lua_ms, isl_ms, isl_ms / lua_ms);
    }

    std::printf("\n");
    if (failures == 0)
        std::printf("OK: %d/%d benchmarks with matching checksums on both sides.\n",
                    (int)(sizeof(BENCHES) / sizeof(BENCHES[0])),
                    (int)(sizeof(BENCHES) / sizeof(BENCHES[0])));
    else
        std::printf("FAILURES: %d benchmark(s) with errors or divergent checksums.\n", failures);
    return failures == 0 ? 0 : 1;
}
