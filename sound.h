// Isiliel — audio module (L3): WAV PCM/float loaded into memory.
// Current backend: "software" (no DAC) — keeps the buffer and playback
// state (position/pause/loop/volume) deterministically. Real output
// can be plugged in later without changing the API/structs.
#ifndef SOUND_H
#define SOUND_H

#include <string>
#include <vector>

struct Sound {
    int id = 0;
    std::string path;
    int sample_rate = 0;          // Hz
    int channels = 0;             // 1 = mono, 2 = stereo, ...
    int bits = 0;                 // bits per sample in the file
    int frames = 0;               // frames per channel
    std::vector<float> samples;   // interleaved: frames*channels, [-1, 1]
    float volume = 1.0f;          // 0..1
    bool playing = false;         // started and not finished/stopped
    bool paused = false;
    bool ended = false;
    int loops_left = 0;           // 0 = no repeat; -1 = infinite
    double position = 0.0;        // in frames (fractional)
    int backend = -1;             // real backend handle (raylib) or -1
};

// Registers the sound_* builtins (idempotent; call before compiling).
void register_sound_builtins();
// Releases all sounds (end of script / interp_reset).
void sound_shutdown();

#endif
