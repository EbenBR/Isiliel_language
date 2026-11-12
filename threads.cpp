// Isiliel — scripting language interpreter/compiler.
// Embedded: sandbox, embedding API, OOP, 2D graphics and REPL.
// Authors: Eben (AI-assisted) | Version: 1.0 | Date: 2026-11-08

// ===========================================================================
// Threads (item 15): `thread f() {...}` + start/stop/pause/resume f(), delay(ms)
//
// - Each `start` creates a std::thread that runs its OWN vm_run (own value
//   and frame stacks) from the function's entry_point, replicating
//   what OP_CALL does: sym_save_scope (saves and clears the locals by sid) and
//   sym_restore_scope on exit.
// - Globals (owner -1) are shared by all threads — a user
//   requirement. No lock: writing the same global from two threads at the same
//   time is the user's own race (concurrent reads are fine).
// - A SINGLE execution per thread-function at a time (`start` of an already
//   running one = error): locals live in unique slots per sid (same mechanism as
//   recursion), so two simultaneous executions of the SAME function would be
//   unsafe and are rejected at start.
// - Shutdown: stop/shutdown set the flag; the VM checks it at the top of every
//   instruction (vm.cpp) and delay() slices in 20 ms to respond quickly.
// - The registry is protected by a mutex; every access to a ThreadEntry happens
//   WITH the lock (stop's join happens after releasing — the object leaves the map
//   before and dies after the join, so there is never a dangling pointer).
// - Threads live at most until the end of the current execution (script or REPL
//   line): the REPL may reallocate the bytecode and a recompile inserts symbols.
// ===========================================================================

#include "threads.h"

#include "symbols.h"
#include "vm.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace {

struct ThreadEntry {
    std::string name;
    ThreadCtl ctl;
    std::thread th;
};

std::unordered_map<std::string, std::unique_ptr<ThreadEntry>> g_threads;
std::mutex g_threads_mutex;
bool g_shutting_down = false;
CodeStore* g_code = nullptr;
std::atomic<int> g_active_threads{0};

bool is_num(const Value& v) {
    return v.type == VAL_FLOAT || v.type == VAL_INT || v.type == VAL_COMPLEX;
}

std::string arg_name(Value* a, const char* fn) {
    if (a[0].type != VAL_STRING)
        throw std::runtime_error(std::string(fn) + ": esperado o nome da funcao");
    return a[0].s;
}

// Validates: the name exists, is a function, and was declared with `thread`
int validate_thread(const std::string& name, const char* fn) {
    int sid = sym_find(name.c_str());
    if (sid == -1)
        throw std::runtime_error(std::string(fn) + ": funcao '" + name + "' nao existe");
    Symbol* s = sym_get(sid);
    if (!s || !s->is_function)
        throw std::runtime_error(std::string(fn) + ": '" + name + "' nao e uma funcao");
    if (!s->is_thread)
        throw std::runtime_error(std::string(fn) + ": '" + name +
                                 "' nao foi declarada com 'thread'");
    return sid;
}

// Thread body: its own vm_run with a saved scope and cleared locals
// (same semantics as a normal call — see OP_CALL in vm.cpp)
void thread_main(ThreadEntry* e, int entry_point, CodeStore* code) {
    g_active_threads.fetch_add(1, std::memory_order_relaxed);
    g_thread_ctl = &e->ctl;
    std::vector<SavedLocal> scope;
    VM vm; // outside the try: the catch prints the traceback (frames) — E2
    vm.sp = 0;
    for (int i = 0; i < STACK_MAX; i++) vm.stack[i] = make_float(0.0f);
    try {
        vm.callArgc = 0;
        vm.frameEnvs.push_back(nullptr);
        vm.frameEnvParents.push_back(nullptr); // L12: named thread
        sym_save_scope(entry_point, scope); // saves and clears the locals
        vm_run(&vm, code, entry_point);
        sym_restore_scope(scope, 0);
    } catch (const std::exception& ex) {
        std::cerr << "[thread " << e->name << "] erro: " << ex.what() << std::endl;
        for (const auto& fr : vm.frames)
            std::cerr << "    em " << fr.first.str() << " (linha " << fr.second << ")" << std::endl;
    } catch (...) {
        std::cerr << "[thread " << e->name << "] erro desconhecido" << std::endl;
    }
    e->ctl.finished.store(true);
    e->ctl.wake(); // wakes whoever is waiting (pause ack / shutdown)
    g_thread_ctl = nullptr;
    g_active_threads.fetch_sub(1, std::memory_order_relaxed);
}

// --- builtins (called via `start f();` etc — see compiler.cpp) ------------

// start f(); — starts; paused -> resumes; running -> error; finished ->
// join and create a new one
void b_thread_start(Value* a, int, Value* r) {
    std::string name = arg_name(a, "thread_start");
    int sid = validate_thread(name, "thread_start");
    int ep = sym_get(sid)->entry_point;

    std::lock_guard<std::mutex> lk(g_threads_mutex);
    if (g_shutting_down)
        throw std::runtime_error("thread_start: encerramento em andamento");

    auto it = g_threads.find(name);
    if (it != g_threads.end()) {
        ThreadEntry* e = it->second.get();
        if (!e->ctl.finished.load()) {
            if (e->ctl.pause_requested.load()) {
                e->ctl.pause_requested.store(false); // start resumes a paused one
                e->ctl.wake();
                *r = make_float(1.0f);
                return;
            }
            throw std::runtime_error("thread_start: '" + name + "' ja esta em execucao");
        }
        if (e->th.joinable()) e->th.join(); // finished naturally (quick)
        g_threads.erase(it);
    }

    auto entry = std::make_unique<ThreadEntry>();
    entry->name = name;
    ThreadEntry* raw = entry.get();
    g_threads[name] = std::move(entry);
    raw->th = std::thread(thread_main, raw, ep, g_code);
    *r = make_float(1.0f);
}

// stop f(); — signals (even if paused) and joins; never started = no-op
void b_thread_stop(Value* a, int, Value* r) {
    std::string name = arg_name(a, "thread_stop");
    validate_thread(name, "thread_stop");

    std::unique_ptr<ThreadEntry> dying;
    {
        std::lock_guard<std::mutex> lk(g_threads_mutex);
        auto it = g_threads.find(name);
        if (it == g_threads.end()) {
            *r = make_float(1.0f); // never started: nothing to interrupt
            return;
        }
        ThreadEntry* e = it->second.get();
        e->ctl.stop.store(true);
        e->ctl.pause_requested.store(false); // also wakes it if paused
        e->ctl.wake();
        if (e->th.joinable() && e->th.get_id() == std::this_thread::get_id()) {
            *r = make_float(1.0f); // stop of itself: only signals (the join
            return;                // comes later in the external stop/end of script)
        }
        // leaves the registry now; the object dies after the join (outside the lock)
        dying = std::move(it->second);
        g_threads.erase(it);
    }
    if (dying->th.joinable()) dying->th.join();
    *r = make_float(1.0f);
}

// pause f(); — requests the pause and waits for the ack (not running = error).
// self-pause: only signals (the next checkpoint pauses; waiting for its own
// ack here would deadlock)
void b_thread_pause(Value* a, int, Value* r) {
    std::string name = arg_name(a, "thread_pause");
    validate_thread(name, "thread_pause");

    std::lock_guard<std::mutex> lk(g_threads_mutex); // guards access to e
    auto it = g_threads.find(name);
    if (it == g_threads.end() || it->second->ctl.finished.load())
        throw std::runtime_error("thread_pause: '" + name + "' nao esta em execucao");
    ThreadEntry* e = it->second.get();
    e->ctl.pause_requested.store(true);
    if (g_thread_ctl == &e->ctl) {
        *r = make_float(1.0f);
        return;
    }
    // ack: paused (VM checkpoint) or already finished; a 2 s timeout covers
    // the case of the thread being in the middle of a delay/long instruction.
    // g_threads_mutex stays held during the wait: prevents a concurrent stop
    // from destroying the entry we are observing.
    std::unique_lock<std::mutex> lk2(e->ctl.m);
    e->ctl.cv.wait_for(lk2, std::chrono::seconds(2),
                       [&] { return e->ctl.is_paused || e->ctl.finished.load(); });
    *r = make_float(1.0f);
}

// resume f(); — clears the pause and wakes; not running = error
void b_thread_resume(Value* a, int, Value* r) {
    std::string name = arg_name(a, "thread_resume");
    validate_thread(name, "thread_resume");

    std::lock_guard<std::mutex> lk(g_threads_mutex);
    auto it = g_threads.find(name);
    if (it == g_threads.end() || it->second->ctl.finished.load())
        throw std::runtime_error("thread_resume: '" + name + "' nao esta em execucao");
    it->second->ctl.pause_requested.store(false);
    it->second->ctl.wake();
    *r = make_float(1.0f);
}

// delay(ms) — delay in milliseconds (INTERP_SLEEP_MS macro, threads.h).
// On the main thread it sleeps directly; on a thread it slices in 20 ms so
// stop/pause are noticed soon after.
void b_delay(Value* a, int, Value* r) {
    if (!is_num(a[0]))
        throw std::runtime_error("delay: milissegundos devem ser numero");
    long ms = (long)std::lround(a[0].f);
    if (ms < 0)
        throw std::runtime_error("delay: milissegundos devem ser >= 0");

    ThreadCtl* c = g_thread_ctl;
    if (!c) {
        INTERP_SLEEP_MS(ms);
        *r = make_float((double)ms);
        return;
    }
    long done = 0;
    while (done < ms && !c->stop.load(std::memory_order_relaxed)) {
        long chunk = ms - done;
        if (chunk > 20) chunk = 20;
        INTERP_SLEEP_MS(chunk);
        done += chunk;
        if (c->pause_requested.load(std::memory_order_relaxed)) break;
    }
    *r = make_float((double)ms);
}

} // namespace

bool threads_any_active() {
    return g_active_threads.load(std::memory_order_relaxed) > 0;
}

// --- public API -------------------------------------------------------------

void ThreadCtl::wait_paused() {
    std::unique_lock<std::mutex> lk(m);
    is_paused = true;
    cv.notify_all(); // pause ack
    while (pause_requested.load(std::memory_order_relaxed) &&
           !stop.load(std::memory_order_relaxed))
        cv.wait(lk);
    is_paused = false;
    cv.notify_all();
}

void ThreadCtl::wake() {
    { std::lock_guard<std::mutex> lk(m); } // synchronizes with the waiters
    cv.notify_all();
}

void threads_bind_code(CodeStore* code) {
    g_code = code;
    g_shutting_down = false; // new execution (script or REPL line)
}

void register_threads_builtins() {
    static bool registered = false;
    if (registered) return;
    registered = true;

    register_builtin("thread_start", 1, b_thread_start);
    register_builtin("thread_stop", 1, b_thread_stop);
    register_builtin("thread_pause", 1, b_thread_pause);
    register_builtin("thread_resume", 1, b_thread_resume);
    register_builtin("delay", 1, b_delay);
}

void threads_join_all() {
    for (;;) {
        bool empty = false;
        std::vector<std::unique_ptr<ThreadEntry>> done;
        {
            std::lock_guard<std::mutex> lk(g_threads_mutex);
            for (auto it = g_threads.begin(); it != g_threads.end();) {
                if (it->second->ctl.finished.load()) {
                    done.push_back(std::move(it->second));
                    it = g_threads.erase(it);
                } else {
                    ++it;
                }
            }
            empty = g_threads.empty();
        }
        for (auto& e : done)
            if (e->th.joinable()) e->th.join();
        if (empty) return;
        INTERP_SLEEP_MS(10);
    }
}

void threads_shutdown() {
    std::vector<std::unique_ptr<ThreadEntry>> dying;
    {
        std::lock_guard<std::mutex> lk(g_threads_mutex);
        g_shutting_down = true;
        for (auto& kv : g_threads) {
            kv.second->ctl.stop.store(true);
            kv.second->ctl.pause_requested.store(false);
            kv.second->ctl.wake();
            dying.push_back(std::move(kv.second));
        }
        g_threads.clear();
    }
    for (auto& e : dying)
        if (e->th.joinable()) e->th.join();
}
