// Isiliel — scripting language interpreter/compiler.
// Embedded: sandbox, embedding API, OOP, 2D graphics and REPL.
// Authors: EEPS and MiMo-V2.6 | Version: 1.0 | Date: 2026-11-08

#include "graphics.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <unordered_map>

#ifdef HAVE_RAYLIB
#include <raylib.h> // loadImage: decodes PNG/JPG/BMP/etc. without a window
#endif

// Factories defined in the backend files
GraphicsBackend* software_backend();
#ifdef HAVE_RAYLIB
GraphicsBackend* raylib_backend();
#endif

namespace {

// Default backend: MYINTERP_GFX (env) > raylib (if compiled) > software.
std::string default_backend() {
    const char* env = std::getenv("MYINTERP_GFX");
    if (env && gfx_backend_available(env)) return env;
#ifdef HAVE_RAYLIB
    return "raylib"; // real window, Processing style (if available)
#else
    return "software";
#endif
}

struct GfxGlobal {
    std::string selected; // chosen backend (before size)
    GraphicsBackend* active = nullptr; // nullptr = screen not created
    bool closedByUser = false;         // show() returns 0 from here on
    int lastW = 0, lastH = 0;
    int frameCount = 0;                // frames presented (frameCount())

    GfxGlobal() : selected(default_backend()) {}
};

GfxGlobal G;

} // namespace

// --- backend registration/lookup -------------------------------------------
bool gfx_backend_available(const std::string& name) {
    if (name == "software") return true;
#ifdef HAVE_RAYLIB
    if (name == "raylib") return true;
#else
    if (name == "raylib") return false;
#endif
    return false;
}

bool gfx_select_backend(const std::string& name) {
    if (G.active) return false;          // already open: switch after closing
    if (!gfx_backend_available(name)) return false;
    G.selected = name;
    return true;
}

std::string gfx_backend_name() {
    return G.active ? G.active->name() : G.selected;
}

// --- screen -----------------------------------------------------------------
bool gfx_size(int w, int h, const std::string& title) {
    if (w <= 0 || h <= 0) return false;
    if (G.active) gfx_shutdown(); // re-size = recreate

    GraphicsBackend* b = nullptr;
    if (G.selected == "software") b = software_backend();
#ifdef HAVE_RAYLIB
    else if (G.selected == "raylib") b = raylib_backend();
#endif
    if (!b) return false;

    if (!b->open(w, h, title)) return false;
    G.active = b;
    G.closedByUser = false;
    G.frameCount = 0;
    G.lastW = w;
    G.lastH = h;
    return true;
}

bool gfx_is_open() { return G.active != nullptr; }
int  gfx_width()   { return G.active ? G.active->width()  : 0; }
int  gfx_height()  { return G.active ? G.active->height() : 0; }
GraphicsBackend* gfx_active() { return G.active; }

// --- frame ------------------------------------------------------------------
bool gfx_show() {
    if (!G.active) throw std::runtime_error("size: chame size() antes de show()");
    if (G.closedByUser) return false;
    if (!G.active->frameBegin()) {
        G.closedByUser = true;
        return false;
    }
    G.active->frameEnd();
    G.frameCount++;
    return true;
}

int gfx_frame_count() { return G.frameCount; }

bool gfx_save_screen(const std::string& path) {
    if (!G.active) throw std::runtime_error("save_screen: chame size() antes de save_screen()");
    return G.active->saveImage(path);
}

void gfx_request_close() {
    // Programmatic close (quit() builtin): the next show() returns 0
    G.closedByUser = true;
}

void gfx_shutdown() {
    if (G.active) {
        G.active->close();
        G.active = nullptr;
    }
    G.closedByUser = false;
}

// --- PPM (P6) utility shared by the backends -------------------------------
bool gfx_write_ppm(const std::string& path,
                   const GfxColor* pixels, int w, int h) {
    if (!pixels || w <= 0 || h <= 0) return false;
    std::ofstream f(path, std::ios::binary);
    if (!f.is_open()) return false;
    f << "P6\n" << w << " " << h << "\n255\n";
    for (int i = 0; i < w * h; i++) {
        char rgb[3] = {(char)pixels[i].r, (char)pixels[i].g, (char)pixels[i].b};
        f.write(rgb, 3);
    }
    return f.good();
}

// ===========================================================================
// Loaded images (loadImage, Processing PImage style)
// ===========================================================================
namespace {

// Handles are never reused (1, 2, 3, ...) — so per-handle caches
// in backends (e.g. raylib texture) never point to the wrong image.
struct ImageRegistry {
    std::unordered_map<int, GfxImage> map;
    int next = 1;
};

ImageRegistry IMG;

// --- own PPM parser (binary P6 / text P3) ----------------------------------
struct PpmP { const char* p; const char* e; bool ok; };

void ppm_skip(PpmP& s) {
    while (s.p < s.e) {
        if (*s.p == '#') {
            while (s.p < s.e && *s.p != '\n') s.p++;
        } else if (std::isspace((unsigned char)*s.p)) {
            s.p++;
        } else {
            break;
        }
    }
}

int ppm_int(PpmP& s) {
    ppm_skip(s);
    if (s.p >= s.e || !std::isdigit((unsigned char)*s.p)) { s.ok = false; return 0; }
    long v = 0;
    while (s.p < s.e && std::isdigit((unsigned char)*s.p)) {
        v = v * 10 + (*s.p - '0');
        if (v > 100000000) { s.ok = false; return 0; }
        s.p++;
    }
    return (int)v;
}

bool ppm_load(const std::string& data, GfxImage& out) {
    if (data.size() < 3 || data[0] != 'P' || (data[1] != '6' && data[1] != '3'))
        return false;
    const bool p6 = data[1] == '6';
    PpmP s{ data.data() + 2, data.data() + data.size(), true };
    const int w = ppm_int(s), h = ppm_int(s), mv = ppm_int(s);
    if (!s.ok || w <= 0 || h <= 0 || w > 16384 || h > 16384 || mv <= 0 || mv > 255)
        return false;
    const size_t n = (size_t)w * (size_t)h;
    out.w = w;
    out.h = h;
    out.px.resize(n);
    if (p6) {
        // after maxval: exactly 1 whitespace, then the RGB bytes
        if (s.p < s.e && std::isspace((unsigned char)*s.p)) s.p++;
        if ((size_t)(s.e - s.p) < n * 3) return false;
        for (size_t i = 0; i < n; i++) {
            unsigned char r = (unsigned char)s.p[i * 3 + 0];
            unsigned char g = (unsigned char)s.p[i * 3 + 1];
            unsigned char b = (unsigned char)s.p[i * 3 + 2];
            if (mv != 255) {
                r = (unsigned char)((int)r * 255 / mv);
                g = (unsigned char)((int)g * 255 / mv);
                b = (unsigned char)((int)b * 255 / mv);
            }
            out.px[i] = GfxColor{r, g, b, 255};
        }
    } else {
        for (size_t i = 0; i < n; i++) {
            int r = ppm_int(s), g = ppm_int(s), b = ppm_int(s);
            if (!s.ok || r > mv || g > mv || b > mv) return false;
            if (mv != 255) { r = r * 255 / mv; g = g * 255 / mv; b = b * 255 / mv; }
            out.px[i] = GfxColor{(unsigned char)r, (unsigned char)g,
                                 (unsigned char)b, 255};
        }
    }
    return true;
}

} // namespace

int gfx_load_image(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open())
        throw std::runtime_error("loadImage: nao foi possivel abrir '" + path + "'");
    const std::string data((std::istreambuf_iterator<char>(f)),
                           std::istreambuf_iterator<char>());

    GfxImage img;
    bool ok = ppm_load(data, img);
#ifdef HAVE_RAYLIB
    if (!ok) {
        // other formats supported by raylib's stb_image
        // (png, jpg, bmp, gif, tga, qoi, psd, pnm...)
        Image im = LoadImage(path.c_str());
        if (im.data && im.width > 0 && im.height > 0 &&
            im.width <= 16384 && im.height <= 16384) {
            ImageFormat(&im, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
            const size_t n = (size_t)im.width * (size_t)im.height;
            img.w = im.width;
            img.h = im.height;
            img.px.resize(n);
            std::memcpy(img.px.data(), im.data, n * 4);
            ok = true;
        }
        if (im.data) UnloadImage(im);
    }
#endif
    if (!ok)
        throw std::runtime_error(
            "loadImage: formato nao suportado (esperado .ppm"
#ifdef HAVE_RAYLIB
            "/png/jpg/bmp..."
#endif
            ") em '" + path + "'");

    const int handle = IMG.next++;
    IMG.map.emplace(handle, std::move(img));
    return handle;
}

void gfx_unload_image(int handle) {
    auto it = IMG.map.find(handle);
    if (it == IMG.map.end()) return;
    if (G.active) G.active->imageDropped(it->second); // releases the cache before the free
    IMG.map.erase(it);
}

const GfxImage* gfx_image(int handle) {
    auto it = IMG.map.find(handle);
    return it == IMG.map.end() ? nullptr : &it->second;
}
