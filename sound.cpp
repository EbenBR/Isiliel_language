// Isiliel — audio module (L3): in-memory WAV + playback state.
// No device dependency: the "software" backend is deterministic and
// testable (sound_update(dt)); the buffer is accessible via sound_sample().
#include "sound.h"
#include "vm.h"
#include "sound_backend.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <stdexcept>

namespace {

std::mutex g_mutex;
std::map<int, Sound> g_sounds;
int g_next_id = 1;
bool g_registered = false;
std::chrono::steady_clock::time_point g_last_update;
int g_device_mode = -1; // -1 = undecided, 0 = software, 1 = device

// MYINTERP_SOUND=device enables real output (raylib). Absent/empty = deterministic
// software (tests and embedded target). If the device fails, falls back to software.
bool device_locked_off() {
    const char* m = std::getenv("MYINTERP_SOUND");
    return m && std::strcmp(m, "software") == 0;
}

bool device_enabled() {
    if (g_device_mode < 0) {
        const char* m = std::getenv("MYINTERP_SOUND");
        bool want = m && std::strcmp(m, "device") == 0;
        if (want && !sound_backend_init()) {
            std::fprintf(stderr, "AVISO: MYINTERP_SOUND=device, mas o device "
                                 "de audio nao inicializou; usando software.\n");
            want = false;
        }
        g_device_mode = want ? 1 : 0;
    }
    return g_device_mode == 1;
}

// Mirrors the already-loaded sounds into the backend (used when enabling the device at runtime).
void sync_backends_locked() {
    for (auto& kv : g_sounds) {
        Sound& s = kv.second;
        if (s.backend >= 0) continue;
        s.backend = sound_backend_load(s.path);
        if (s.backend >= 0) {
            sound_backend_set_volume(s.backend, s.volume);
            if (s.playing && !s.paused)
                sound_backend_play(s.backend, s.loops_left);
        }
    }
}

void drop_backends_locked() {
    for (auto& kv : g_sounds) {
        if (kv.second.backend >= 0) {
            sound_backend_unload(kv.second.backend);
            kv.second.backend = -1;
        }
    }
}

uint16_t rd_u16(const std::vector<char>& b, size_t o) {
    return (uint16_t)((uint8_t)b[o] | ((uint16_t)(uint8_t)b[o + 1] << 8));
}
uint32_t rd_u32(const std::vector<char>& b, size_t o) {
    return (uint32_t)((uint8_t)b[o] | ((uint32_t)(uint8_t)b[o + 1] << 8) |
                      ((uint32_t)(uint8_t)b[o + 2] << 16) |
                      ((uint32_t)(uint8_t)b[o + 3] << 24));
}

// Reads a RIFF WAV (PCM 8/16/24/32 or float32) into `out` (normalized).
bool read_wav(const std::string& path, Sound& out, std::string& err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { err = "nao foi possivel abrir '" + path + "'"; return false; }
    std::vector<char> buf((std::istreambuf_iterator<char>(f)),
                          std::istreambuf_iterator<char>());
    if (buf.size() < 12 || std::memcmp(buf.data(), "RIFF", 4) != 0 ||
        std::memcmp(buf.data() + 8, "WAVE", 4) != 0) {
        err = "formato invalido (esperado RIFF/WAVE)";
        return false;
    }
    int fmt = 0, ch = 0, rate = 0, bits = 0;
    size_t data_off = 0, data_len = 0;
    size_t p = 12;
    while (p + 8 <= buf.size()) {
        const char* id = buf.data() + p;
        uint32_t sz = rd_u32(buf, p + 4);
        size_t body = p + 8;
        if (body > buf.size()) break;
        size_t avail = std::min<size_t>(sz, buf.size() - body);
        if (std::memcmp(id, "fmt ", 4) == 0 && avail >= 16) {
            fmt = rd_u16(buf, body);
            ch = rd_u16(buf, body + 2);
            rate = (int)rd_u32(buf, body + 4);
            bits = rd_u16(buf, body + 14);
        } else if (std::memcmp(id, "data", 4) == 0) {
            data_off = body;
            data_len = avail;
        }
        p = body + sz + (sz & 1); // word-aligned chunks
    }
    if (fmt == 0 || ch <= 0 || rate <= 0 || bits <= 0 || data_off == 0) {
        err = "WAV sem chunk fmt/data valido";
        return false;
    }
    int bytes = bits / 8;
    if (bytes <= 0 || ch > 64) { err = "WAV com formato nao suportado"; return false; }
    size_t block = (size_t)bytes * (size_t)ch;
    size_t frames = data_len / block;
    if (frames > 100000000u) { err = "WAV grande demais"; return false; }

    out.samples.resize(frames * (size_t)ch);
    for (size_t i = 0; i < frames; i++) {
        for (int c = 0; c < ch; c++) {
            const char* q = buf.data() + data_off + i * block + (size_t)c * bytes;
            float v = 0.0f;
            if (fmt == 1) { // integer PCM
                if (bits == 8) {
                    v = ((int)(uint8_t)q[0] - 128) / 128.0f;
                } else if (bits == 16) {
                    int16_t x = (int16_t)(uint16_t)((uint8_t)q[0] |
                                                    ((uint16_t)(uint8_t)q[1] << 8));
                    v = x / 32768.0f;
                } else if (bits == 24) {
                    int32_t x = (int32_t)((uint8_t)q[0] |
                                          ((uint32_t)(uint8_t)q[1] << 8) |
                                          ((uint32_t)(uint8_t)q[2] << 16));
                    if (x & 0x800000) x -= 0x1000000;
                    v = x / 8388608.0f;
                } else if (bits == 32) {
                    int32_t x = (int32_t)rd_u32(
                        std::vector<char>(q, q + 4), 0);
                    v = (float)(x / 2147483648.0);
                } else {
                    err = "PCM com " + std::to_string(bits) + " bits nao suportado";
                    return false;
                }
            } else if (fmt == 3 && bits == 32) { // IEEE float
                std::memcpy(&v, q, 4);
            } else {
                err = "WAV fmt=" + std::to_string(fmt) + " bits=" +
                      std::to_string(bits) + " nao suportado";
                return false;
            }
            out.samples[i * (size_t)ch + (size_t)c] = v;
        }
    }
    out.path = path;
    out.sample_rate = rate;
    out.channels = ch;
    out.bits = bits;
    out.frames = (int)frames;
    return true;
}

Sound* find(int id) {
    auto it = g_sounds.find(id);
    return it == g_sounds.end() ? nullptr : &it->second;
}

// Advances a sound's position by dt seconds (handles loop/end).
void advance(Sound& s, double dt) {
    if (!s.playing || s.paused || s.frames <= 0 || dt <= 0.0) return;
    double pos = s.position + dt * (double)s.sample_rate;
    while (pos >= (double)s.frames) {
        if (s.loops_left < 0) { // infinite
            pos = std::fmod(pos, (double)s.frames);
            break;
        }
        if (s.loops_left > 0) {
            s.loops_left--;
            pos -= (double)s.frames;
        } else {
            pos = (double)s.frames;
            s.playing = false;
            s.ended = true;
            break;
        }
    }
    s.position = pos;
}

// ---- builtins -------------------------------------------------------------

void b_load(Value* a, int n, Value* r) {
    if (n < 1 || a[0].type != VAL_STRING)
        throw std::runtime_error("sound_load espera o caminho (texto)");
    std::lock_guard<std::mutex> lk(g_mutex);
    Sound s;
    std::string err;
    if (!read_wav(a[0].s.str(), s, err))
        throw std::runtime_error("sound_load: " + err);
    s.id = g_next_id++;
    int id = s.id;
    g_sounds[id] = std::move(s);
    if (device_enabled()) {
        Sound* sp = find(id);
        if (sp) sp->backend = sound_backend_load(sp->path);
    }
    *r = make_float((double)id);
}

void b_play(Value* a, int n, Value* r) {
    std::lock_guard<std::mutex> lk(g_mutex);
    Sound* s = (n >= 1) ? find((int)a[0].f) : nullptr;
    if (!s) { *r = make_float(0); return; }
    int loops = (n >= 2) ? (int)a[1].f : 0;
    s->playing = true;
    s->paused = false;
    s->ended = false;
    s->position = 0.0;
    s->loops_left = (loops < 0) ? -1 : loops;
    if (s->backend >= 0) {
        sound_backend_set_volume(s->backend, s->volume);
        sound_backend_play(s->backend, s->loops_left);
    }
    *r = make_float(1);
}

void b_pause(Value* a, int n, Value* r) {
    std::lock_guard<std::mutex> lk(g_mutex);
    Sound* s = (n >= 1) ? find((int)a[0].f) : nullptr;
    if (!s || !s->playing || s->paused) { *r = make_float(0); return; }
    s->paused = true;
    if (s->backend >= 0) sound_backend_pause(s->backend);
    *r = make_float(1);
}

void b_resume(Value* a, int n, Value* r) {
    std::lock_guard<std::mutex> lk(g_mutex);
    Sound* s = (n >= 1) ? find((int)a[0].f) : nullptr;
    if (!s || !s->playing || !s->paused) { *r = make_float(0); return; }
    s->paused = false;
    if (s->backend >= 0) sound_backend_resume(s->backend);
    *r = make_float(1);
}

void b_stop(Value* a, int n, Value* r) {
    std::lock_guard<std::mutex> lk(g_mutex);
    Sound* s = (n >= 1) ? find((int)a[0].f) : nullptr;
    if (!s) { *r = make_float(0); return; }
    s->playing = false;
    s->paused = false;
    s->ended = false;
    s->position = 0.0;
    s->loops_left = 0;
    if (s->backend >= 0) sound_backend_stop(s->backend);
    *r = make_float(1);
}

void b_unload(Value* a, int n, Value* r) {
    std::lock_guard<std::mutex> lk(g_mutex);
    int id = (n >= 1) ? (int)a[0].f : -1;
    Sound* s = find(id);
    if (s && s->backend >= 0) sound_backend_unload(s->backend);
    *r = make_float(g_sounds.erase(id) ? 1 : 0);
}

void b_is_playing(Value* a, int n, Value* r) {
    std::lock_guard<std::mutex> lk(g_mutex);
    Sound* s = (n >= 1) ? find((int)a[0].f) : nullptr;
    if (!s) { *r = make_float(0); return; }
    if (s->backend >= 0) {
        *r = make_float(sound_backend_playing(s->backend) ? 1 : 0);
        return;
    }
    *r = make_float((s->playing && !s->paused) ? 1 : 0);
}

void b_is_paused(Value* a, int n, Value* r) {
    std::lock_guard<std::mutex> lk(g_mutex);
    Sound* s = (n >= 1) ? find((int)a[0].f) : nullptr;
    *r = make_float((s && s->paused) ? 1 : 0);
}

void b_duration(Value* a, int n, Value* r) {
    std::lock_guard<std::mutex> lk(g_mutex);
    Sound* s = (n >= 1) ? find((int)a[0].f) : nullptr;
    *r = make_float((s && s->sample_rate > 0)
                        ? (double)s->frames / (double)s->sample_rate : 0.0);
}

void b_position(Value* a, int n, Value* r) {
    std::lock_guard<std::mutex> lk(g_mutex);
    Sound* s = (n >= 1) ? find((int)a[0].f) : nullptr;
    *r = make_float((s && s->sample_rate > 0)
                        ? s->position / (double)s->sample_rate : 0.0);
}

void b_seek(Value* a, int n, Value* r) {
    std::lock_guard<std::mutex> lk(g_mutex);
    Sound* s = (n >= 1) ? find((int)a[0].f) : nullptr;
    if (!s) { *r = make_float(0); return; }
    double sec = (n >= 2) ? a[1].f : 0.0;
    if (sec < 0.0) sec = 0.0;
    double maxp = (double)s->frames;
    double pos = sec * (double)s->sample_rate;
    if (pos > maxp) pos = maxp;
    s->position = pos;
    *r = make_float(1);
}

void b_set_volume(Value* a, int n, Value* r) {
    std::lock_guard<std::mutex> lk(g_mutex);
    Sound* s = (n >= 1) ? find((int)a[0].f) : nullptr;
    if (!s) { *r = make_float(0); return; }
    float v = (n >= 2) ? (float)a[1].f : 1.0f;
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    s->volume = v;
    if (s->backend >= 0) sound_backend_set_volume(s->backend, v);
    *r = make_float(1);
}

void b_volume(Value* a, int n, Value* r) {
    std::lock_guard<std::mutex> lk(g_mutex);
    Sound* s = (n >= 1) ? find((int)a[0].f) : nullptr;
    *r = make_float(s ? s->volume : 0.0f);
}

void b_sample_rate(Value* a, int n, Value* r) {
    std::lock_guard<std::mutex> lk(g_mutex);
    Sound* s = (n >= 1) ? find((int)a[0].f) : nullptr;
    *r = make_float(s ? s->sample_rate : 0);
}

void b_channels(Value* a, int n, Value* r) {
    std::lock_guard<std::mutex> lk(g_mutex);
    Sound* s = (n >= 1) ? find((int)a[0].f) : nullptr;
    *r = make_float(s ? s->channels : 0);
}

void b_frames(Value* a, int n, Value* r) {
    std::lock_guard<std::mutex> lk(g_mutex);
    Sound* s = (n >= 1) ? find((int)a[0].f) : nullptr;
    *r = make_float(s ? s->frames : 0);
}

// sound_sample(h, i[, channel]) -> normalized sample [-1,1] (0 if invalid)
void b_sample(Value* a, int n, Value* r) {
    std::lock_guard<std::mutex> lk(g_mutex);
    Sound* s = (n >= 1) ? find((int)a[0].f) : nullptr;
    if (!s || n < 2) { *r = make_float(0); return; }
    int i = (int)a[1].f;
    int c = (n >= 3) ? (int)a[2].f : 0;
    if (i < 0 || i >= s->frames || c < 0 || c >= s->channels) {
        *r = make_float(0);
        return;
    }
    *r = make_float(s->samples[(size_t)i * (size_t)s->channels + (size_t)c]);
}

// sound_update([dt]) -> 1. Without dt, uses the clock monotonically since the
// last call (default: call once per frame). With a fixed dt it is deterministic.
void b_update(Value* a, int n, Value* r) {
    std::lock_guard<std::mutex> lk(g_mutex);
    double dt;
    if (n >= 1) {
        dt = a[0].f;
        if (dt < 0.0) throw std::runtime_error("sound_update: dt negativo");
    } else {
        auto now = std::chrono::steady_clock::now();
        dt = std::chrono::duration<double>(now - g_last_update).count();
        g_last_update = now;
    }
    for (auto& kv : g_sounds) advance(kv.second, dt);
    sound_backend_update(); // repeats loops on the real device
    *r = make_float(1);
}

void b_clear(Value*, int, Value* r) {
    std::lock_guard<std::mutex> lk(g_mutex);
    int n = (int)g_sounds.size();
    for (auto& kv : g_sounds)
        if (kv.second.backend >= 0) sound_backend_unload(kv.second.backend);
    g_sounds.clear();
    *r = make_float(n);
}

// sound_backend("device"|"software") -> 1/0. Allows enabling real output without
// depending on MYINTERP_SOUND; "software" returns to deterministic mode.
void b_backend(Value* a, int n, Value* r) {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (n < 1 || a[0].type != VAL_STRING)
        throw std::runtime_error("sound_backend espera \"device\" ou \"software\"");
    const std::string& mode = a[0].s.str();
    if (mode == "device") {
        if (device_locked_off()) {
            *r = make_float(0);
            return;
        }
        if (!device_enabled()) {
            if (!sound_backend_init()) {
                std::fprintf(stderr, "AVISO: sound_backend(\"device\") "
                                     "indisponivel; usando software.\n");
                *r = make_float(0);
                return;
            }
            g_device_mode = 1;
        }
        sync_backends_locked();
        *r = make_float(1);
        return;
    }
    if (mode == "software") {
        drop_backends_locked();
        sound_backend_shutdown();
        g_device_mode = 0;
        *r = make_float(1);
        return;
    }
    throw std::runtime_error("sound_backend: use \"device\" ou \"software\"");
}

} // namespace

void register_sound_builtins() {
    if (g_registered) return;
    g_registered = true;
    g_last_update = std::chrono::steady_clock::now();

    register_builtin("sound_load", 1, b_load);
    register_builtin("sound_play", -1, b_play);
    register_builtin("sound_pause", 1, b_pause);
    register_builtin("sound_resume", 1, b_resume);
    register_builtin("sound_stop", 1, b_stop);
    register_builtin("sound_unload", 1, b_unload);
    register_builtin("sound_is_playing", 1, b_is_playing);
    register_builtin("sound_is_paused", 1, b_is_paused);
    register_builtin("sound_duration", 1, b_duration);
    register_builtin("sound_position", 1, b_position);
    register_builtin("sound_seek", -1, b_seek);
    register_builtin("sound_set_volume", -1, b_set_volume);
    register_builtin("sound_volume", 1, b_volume);
    register_builtin("sound_sample_rate", 1, b_sample_rate);
    register_builtin("sound_channels", 1, b_channels);
    register_builtin("sound_frames", 1, b_frames);
    register_builtin("sound_sample", -1, b_sample);
    register_builtin("sound_update", -1, b_update);
    register_builtin("sound_clear", 0, b_clear);
    register_builtin("sound_backend", 1, b_backend);
}

void sound_shutdown() {
    std::lock_guard<std::mutex> lk(g_mutex);
    sound_backend_shutdown();
    g_sounds.clear();
    g_last_update = std::chrono::steady_clock::now();
}
