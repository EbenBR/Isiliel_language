// Isiliel — scripting language interpreter/compiler.
// Embedded: sandbox, embedding API, OOP, 2D graphics and REPL.
// Authors: EEPS and MiMo-V2.6 | Version: 1.0 | Date: 2026-11-08

#ifndef GRAPHICS_HAL_H
#define GRAPHICS_HAL_H

#include <string>
#include <vector>

// ===========================================================================
// 2D graphics HAL (Processing-style)
//
// The language functions (builtins) talk ONLY to this interface.
// To port to embedded systems, just implement GraphicsBackend with
// your own framebuffer and register it in graphics_core.cpp — no
// script call / builtin changes.
//
// Backends compiled in this build:
//   "software"  - in-memory framebuffer + PPM export (default, headless)
//   "raylib"    - real window (Linux/Windows) when HAVE_RAYLIB is active
// ===========================================================================

struct GfxColor {
    unsigned char r = 0, g = 0, b = 0, a = 255;
};

// Image loaded with loadImage() (Processing PImage style).
// px = RGBA8, row by row, origin at the top-left corner.
// Stable address while alive (unordered_map in the core registry).
struct GfxImage {
    int w = 0, h = 0;
    std::vector<GfxColor> px;
};

// Bitmap font glyph: rows[0..font.h-1], "1" = lit pixel.
struct GfxGlyph {
    unsigned char c;
    const char* rows[8];
};

// Embedded bitmap font (data in graphics_font.h; default "8x8").
// base character advance = (w + 1) * scale (1 px of spacing).
struct GfxFont {
    const char* name;
    const GfxGlyph* glyphs;
    int count;
    int w, h;
};

// Glyph lookup (full ASCII 0x20..0x7E in the embedded fonts);
// a char outside the set returns nullptr (only advances the cursor).
inline const GfxGlyph* gfx_glyph(const GfxFont& f, char ch) {
    for (int i = 0; i < f.count; i++)
        if (f.glyphs[i].c == (unsigned char)ch) return &f.glyphs[i];
    return nullptr;
}

// Portable interface: all drawing is immediate 2D primitives.
// If the target needs a "frame bracket" (e.g. raylib requires Begin/End
// Drawing), the backend itself defers playback until frameBegin()/frameEnd().
class GraphicsBackend {
public:
    virtual ~GraphicsBackend() {}

    virtual const char* name() const = 0;

    // Window/framebuffer. title = screen title (ignorable on a fixed LCD).
    virtual bool open(int w, int h, const std::string& title) = 0;
    virtual void close() = 0;

    // Frame cycle (called by show()).
    // frameBegin(): processes events; false = user asked to close.
    virtual bool frameBegin() = 0;
    virtual void frameEnd()   = 0; // presents the frame (swap/pump)

    // 2D primitives (pixel coords, origin at the top-left corner)
    virtual void clear(const GfxColor& c) = 0;
    virtual void drawPixel(int x, int y, const GfxColor& c) = 0;
    virtual void drawLine(int x1, int y1, int x2, int y2,
                          int weight, const GfxColor& c) = 0;
    virtual void drawRectOutline(int x, int y, int w, int h,
                                 int weight, const GfxColor& c) = 0;
    virtual void drawRectFilled(int x, int y, int w, int h,
                                const GfxColor& c) = 0;
    virtual void drawEllipseOutline(int cx, int cy, int rx, int ry,
                                    int weight, const GfxColor& c) = 0;
    virtual void drawEllipseFilled(int cx, int cy, int rx, int ry,
                                   const GfxColor& c) = 0;
    // y = glyph top; scale = integer font factor (>=1);
    // font = active font (font() builtin/default "8x8").
    virtual void drawText(const std::string& s, int x, int y,
                          int scale, const GfxColor& c,
                          const GfxFont& font) = 0;

    // Draws a loaded image (loadImage). w/h different from the native
    // size = scaling (nearest neighbor). Default = does nothing.
    virtual void drawImage(const GfxImage& img, int x, int y,
                           int w, int h) { (void)img; (void)x; (void)y; (void)w; (void)h; }
    // Image will be dropped from the registry: release caches (e.g. texture).
    virtual void imageDropped(const GfxImage& img) { (void)img; }

    // Persists the current frame (windowed backends read the screen; the software
    // backend writes the framebuffer). .ppm format guaranteed; .png when the backend
    // supports it. Returns false on failure.
    virtual bool saveImage(const std::string& path) = 0;

    virtual int width()  const = 0;
    virtual int height() const = 0;

    // --- Input (interaction) -------------------------------------------------
    // Default = headless (0/false/""); windowed backends override.
    // Query during draw() execution (between frameBegin/frameEnd).
    virtual int  mouseX() { return 0; }
    virtual int  mouseY() { return 0; }
    virtual bool mousePressed() { return false; } // some mouse button
    virtual bool keyPressed() { return false; }   // key pressed this frame
    virtual std::string key() { return ""; }      // last key typed
};

// --- API used by the builtins (graphics_core.cpp) ---
// Selects the backend by name ("software", "raylib", ...). Before size().
bool         gfx_select_backend(const std::string& name);
std::string  gfx_backend_name();          // selected backend ("" if none)
// Backend compiled in this build? (for a friendly error message)
bool         gfx_backend_available(const std::string& name);

bool         gfx_size(int w, int h, const std::string& title);
bool         gfx_is_open();
int          gfx_width();
int          gfx_height();
GraphicsBackend* gfx_active();            // nullptr if size() not called

// Presents a frame (frameBegin + frameEnd). false if the program was
// terminated via quit() (the only programmatic close; Ctrl+C in the terminal
// also terminates the process).
bool         gfx_show();
bool         gfx_save_screen(const std::string& path); // writes the SCREEN

// quit(): signals the end of the draw() loop/window wait.
// From the next show() on the result is 0 (idempotent).
void         gfx_request_close();

// Frames presented since size() (Processing style: frameCount).
// Starts at 1 on the first show() — useful for "do something only on frame 1".
int          gfx_frame_count();

// Releases the backend (closes the window / clears the framebuffer). Call at the end of
// the script; idempotent.
void         gfx_shutdown();

// Shared P6 writer (used by the software backend and by raylib readback)
bool         gfx_write_ppm(const std::string& path,
                           const GfxColor* pixels, int w, int h);

// --- images (loadImage/image PImage style) ----------------------------------
// Reads PPM (P6/P3); with raylib, also PNG/JPG/BMP/etc. from disk.
// Throw std::runtime_error with a friendly message on failure.
// Handle = integer >= 1 (never reused); check with gfx_image().
int          gfx_load_image(const std::string& path);
void         gfx_unload_image(int handle);
const GfxImage* gfx_image(int handle);   // nullptr if handle invalid

#endif
