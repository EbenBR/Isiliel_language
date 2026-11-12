// Isiliel — scripting language interpreter/compiler.
// Embedded: sandbox, embedding API, OOP, 2D graphics and REPL.
// Authors: EEPS and MiMo-V2.6 | Version: 1.0 | Date: 2026-11-08

#ifndef THREADS_H
#define THREADS_H

// pthreads-style threads (roadmap item 15):
//
//   thread f() {...}     — global function without parameters; runs on its own thread
//   start f();           — starts (or RESUMES, if paused); already running = error
//   pause f();           — pauses at the next VM checkpoint
//   resume f();          — resumes
//   stop f();            — stops permanently (signals + join)
//   delay(ms)            — delay in milliseconds VIA PORTABLE MACRO below
//
// The TLS `g_thread_ctl` points to the ThreadCtl of the running thread
// (nullptr on the main thread). The VM checks the checkpoint at the top of every
// instruction (vm.cpp): on the main thread the cost is a never-taken branch.

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

#include "defs.h"

// --- portable delay macro (milliseconds) ---
// delay() implemented as a macro to ease portability:
// Windows -> Sleep(); POSIX -> nanosleep().
#ifdef _WIN32
#  include <windows.h>
#  define INTERP_SLEEP_MS(ms) Sleep((DWORD)(ms))
#else
#  include <ctime>
#  define INTERP_SLEEP_MS(ms) do {                                   \
        struct timespec interp_ts__ = { (long)((ms) / 1000),         \
                                        (long)(((ms) % 1000) * 1000000L) }; \
        nanosleep(&interp_ts__, nullptr);                            \
    } while (0)
#endif

// Control of ONE running thread (created by start, destroyed on join)
struct ThreadCtl {
    std::atomic<bool> stop{false};            // stop/shutdown requested exit
    std::atomic<bool> pause_requested{false}; // pause requested suspension
    std::atomic<bool> finished{false};        // the thread's vm_run returned
    bool is_paused = false;                   // protected by m (pause ack)
    std::mutex m;
    std::condition_variable cv;

    // Called by the VM on the owning thread: wakes the pauser (is_paused) and waits
    // for resume/stop. NEVER call from the one who called pause (self-deadlock
    // — the handler itself already handles self-pause before waiting for the ack).
    void wait_paused();
    // Wakes whoever waits on cv (stop/resume/shutdown): brief lock + notify
    void wake();
};

// ctl of the thread running now (nullptr = main thread)
inline thread_local ThreadCtl* g_thread_ctl = nullptr;

// Bytecode in effect for the execution (the thread runs this buffer; it lives until the
// end of the execution — all threads are terminated before it dies).
// Call at the start of each execution (also resets the shutdown flag).
void threads_bind_code(CodeStore* code);

// L13: true while any script thread is running. The collector registration
// avoids the mutex on the common path (single-threaded scripts).
bool threads_any_active();

// Registers the builtins (idempotent; BEFORE compile, in run_pipeline*)
void register_threads_builtins();

// Waits for ALL threads to finish (non-daemon-style join) — used at the
// end of a successful script: `stop f()` ends early; an infinite thread keeps the
// program alive until Ctrl+C (classic example: start as the last line).
// Does not signal stop (whoever wants to stop earlier calls stop f()).
void threads_join_all();

// Signals stop + join on ALL threads (end with a script ERROR, of every
// REPL line and on interpreter exit).
void threads_shutdown();

#endif
