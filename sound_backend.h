// Isiliel — audio output backend (optional, raylib).
// The API does not expose raylib types: sound.cpp (software/deterministic) calls
// these functions only when MYINTERP_SOUND=device and the device initialized.
// Without HAVE_RAYLIB (embedded target) they all become stubs and the module is
// software-only, with no DAC dependency.
#ifndef SOUND_BACKEND_H
#define SOUND_BACKEND_H

#include <string>

bool sound_backend_init();                 // idempotent; false if no device
bool sound_backend_ready();
void sound_backend_shutdown();             // unloads sounds + closes the device
int  sound_backend_load(const std::string& path); // handle >= 0 or -1
void sound_backend_unload(int handle);
void sound_backend_play(int handle, int loops);
void sound_backend_pause(int handle);
void sound_backend_resume(int handle);
void sound_backend_stop(int handle);
bool sound_backend_playing(int handle);
void sound_backend_set_volume(int handle, float volume);
void sound_backend_update();               // repeats loops when the sound ends

#endif
