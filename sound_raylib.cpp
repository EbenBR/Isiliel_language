// Isiliel — real audio backend via raylib (optional).
// Compiled with -DHAVE_RAYLIB when raylib is available; otherwise
// defines stubs to keep the sound module software-only on the embedded target.
#include "sound_backend.h"

#ifdef HAVE_RAYLIB

#include <raylib.h>

#include <map>
#include <mutex>

namespace {

std::mutex g_mtx;
std::map<int, ::Sound> g_sounds;
std::map<int, int> g_loops;        // remaining repeats (-1 = infinite)
std::map<int, bool> g_seen;        // we already saw IsSoundPlaying=true in this play
int g_next = 1;
bool g_ready = false;
bool g_tried = false;

} // namespace

bool sound_backend_init() {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_ready) return true;
    if (g_tried) return false;
    g_tried = true;
    InitAudioDevice();
    g_ready = IsAudioDeviceReady();
    return g_ready;
}

bool sound_backend_ready() {
    std::lock_guard<std::mutex> lk(g_mtx);
    return g_ready;
}

void sound_backend_shutdown() {
    std::lock_guard<std::mutex> lk(g_mtx);
    for (auto& kv : g_sounds) UnloadSound(kv.second);
    g_sounds.clear();
    g_loops.clear();
    g_seen.clear();
    if (g_ready) CloseAudioDevice();
    g_ready = false;
    g_tried = false;
}

int sound_backend_load(const std::string& path) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (!g_ready) return -1;
    ::Sound s = LoadSound(path.c_str());
    if (s.frameCount == 0) return -1;
    int id = g_next++;
    g_sounds[id] = s;
    g_loops[id] = 0;
    g_seen[id] = false;
    return id;
}

void sound_backend_unload(int handle) {
    std::lock_guard<std::mutex> lk(g_mtx);
    auto it = g_sounds.find(handle);
    if (it == g_sounds.end()) return;
    UnloadSound(it->second);
    g_sounds.erase(it);
    g_loops.erase(handle);
    g_seen.erase(handle);
}

void sound_backend_play(int handle, int loops) {
    std::lock_guard<std::mutex> lk(g_mtx);
    auto it = g_sounds.find(handle);
    if (it == g_sounds.end()) return;
    g_loops[handle] = loops;
    g_seen[handle] = false;
    PlaySound(it->second);
}

void sound_backend_pause(int handle) {
    std::lock_guard<std::mutex> lk(g_mtx);
    auto it = g_sounds.find(handle);
    if (it != g_sounds.end()) PauseSound(it->second);
}

void sound_backend_resume(int handle) {
    std::lock_guard<std::mutex> lk(g_mtx);
    auto it = g_sounds.find(handle);
    if (it != g_sounds.end()) ResumeSound(it->second);
}

void sound_backend_stop(int handle) {
    std::lock_guard<std::mutex> lk(g_mtx);
    auto it = g_sounds.find(handle);
    if (it == g_sounds.end()) return;
    StopSound(it->second);
    g_loops[handle] = 0;
    g_seen[handle] = false;
}

bool sound_backend_playing(int handle) {
    std::lock_guard<std::mutex> lk(g_mtx);
    auto it = g_sounds.find(handle);
    if (it == g_sounds.end()) return false;
    return IsSoundPlaying(it->second);
}

void sound_backend_set_volume(int handle, float volume) {
    std::lock_guard<std::mutex> lk(g_mtx);
    auto it = g_sounds.find(handle);
    if (it != g_sounds.end()) SetSoundVolume(it->second, volume);
}

void sound_backend_update() {
    std::lock_guard<std::mutex> lk(g_mtx);
    for (auto& kv : g_sounds) {
        int id = kv.first;
        if (IsSoundPlaying(kv.second)) {
            g_seen[id] = true;
            continue;
        }
        // Only repeats after it has started (avoids a burst on the 1st update).
        if (!g_seen[id] || g_loops[id] == 0) continue;
        if (g_loops[id] > 0) g_loops[id]--;
        g_seen[id] = false;
        PlaySound(kv.second);
    }
}

#else // !HAVE_RAYLIB — stubs (software-only)

bool sound_backend_init() { return false; }
bool sound_backend_ready() { return false; }
void sound_backend_shutdown() {}
int  sound_backend_load(const std::string&) { return -1; }
void sound_backend_unload(int) {}
void sound_backend_play(int, int) {}
void sound_backend_pause(int) {}
void sound_backend_resume(int) {}
void sound_backend_stop(int) {}
bool sound_backend_playing(int) { return false; }
void sound_backend_set_volume(int, float) {}
void sound_backend_update() {}

#endif
