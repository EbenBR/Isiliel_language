// Isiliel — scripting language interpreter/compiler.
// Embedded: sandbox, embedding API, OOP, 2D graphics and REPL.
// Authors: EEPS and MiMo-V2.6 | Version: 1.0 | Date: 2026-11-08

#include "graphics.h"

#ifdef HAVE_RAYLIB
// ===========================================================================
// "raylib" backend — real window (Linux/Windows).
// Compiled only when HAVE_RAYLIB is defined (the Makefile detects
// raylib via pkg-config). Without raylib, the file becomes empty.
//
// raylib requires drawing to happen between BeginDrawing/EndDrawing;
// therefore this backend defers the primitives in a queue (HAL callback) and
// replays them in frameBegin(). The language builtins do not change.
// ===========================================================================

#include <raylib.h>
#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

struct Cmd {
    enum Type { CLEAR, PIX, LINE, RECO, RECTF, ELLO, ELLF, TEXT, IMG } type;
    int a = 0, b = 0, c = 0, d = 0;
    int weight = 1, scale = 1;
    GfxColor col;
    std::string text;
    const GfxFont* font = nullptr;  // Cmd::TEXT: active font (static)
    const GfxImage* p = nullptr;    // Cmd::IMG: pointer to registry (stable)
};

inline ::Color rc(const GfxColor& c) {
    return ::Color{c.r, c.g, c.b, c.a};
}

class RaylibBackend : public GraphicsBackend {
public:
    const char* name() const override { return "raylib"; }

    bool open(int w, int h, const std::string& title) override {
        if (w <= 0 || h <= 0) return false;
        queue.clear();
        freeTextures();          // old window: textures do not survive
        if (IsWindowReady()) CloseWindow();
        InitWindow(w, h, title.empty() ? "Isiliel" : title.c_str());
        if (!IsWindowReady()) return false;
        SetTargetFPS(60);
        W = w;
        H = h;
        queue.clear();
        opened = true;
        // Clean initial frame (avoids a dirty screen on the 1st show())
        BeginDrawing();
        ClearBackground(::Color{0, 0, 0, 255});
        EndDrawing();
        return true;
    }

    void close() override {
        queue.clear();
        freeTextures(); // before CloseWindow (raylib requires it)
        if (IsWindowReady()) CloseWindow();
        W = H = 0;
        opened = false;
    }

    bool frameBegin() override {
        if (!opened) return false;
        // Programmatic close: only quit() (builtin) — neither ESC nor the
        // X button close it; the program exits with quit() or Ctrl+C in the terminal.
        // (the core checks closedByUser in gfx_show() before calling frameBegin)
        // Input collection for the builtins (mouseX/key/...) of this frame
        int ch;
        while ((ch = GetCharPressed()) != 0) {
            if (ch > 0 && ch < 128) lastKey = std::string(1, (char)ch);
        }
        keyHitThisFrame = false;
        while (GetKeyPressed()) keyHitThisFrame = true;
        BeginDrawing();
        for (const Cmd& k : queue) replay(k);
        queue.clear();
        return true;
    }

    void frameEnd() override { EndDrawing(); }

    // --- input (queried by the builtins during draw()) ---
    int  mouseX() override { return GetMouseX(); }
    int  mouseY() override { return GetMouseY(); }
    bool mousePressed() override {
        return IsMouseButtonDown(MOUSE_BUTTON_LEFT) ||
               IsMouseButtonDown(MOUSE_BUTTON_RIGHT) ||
               IsMouseButtonDown(MOUSE_BUTTON_MIDDLE);
    }
    bool keyPressed() override { return keyHitThisFrame; }
    std::string key() override { return lastKey; }

    // --- primitives: only enqueue (replay in frameBegin) ---
    void clear(const GfxColor& c) override { push(Cmd::CLEAR).col = c; }
    void drawPixel(int x, int y, const GfxColor& c) override {
        Cmd& k = push(Cmd::PIX); k.a = x; k.b = y; k.col = c;
    }
    void drawLine(int x1, int y1, int x2, int y2,
                  int weight, const GfxColor& c) override {
        Cmd& k = push(Cmd::LINE);
        k.a = x1; k.b = y1; k.c = x2; k.d = y2; k.weight = weight; k.col = c;
    }
    void drawRectOutline(int x, int y, int w, int h,
                         int weight, const GfxColor& c) override {
        Cmd& k = push(Cmd::RECO);
        k.a = x; k.b = y; k.c = w; k.d = h; k.weight = weight; k.col = c;
    }
    void drawRectFilled(int x, int y, int w, int h,
                        const GfxColor& c) override {
        Cmd& k = push(Cmd::RECTF);
        k.a = x; k.b = y; k.c = w; k.d = h; k.col = c;
    }
    void drawEllipseOutline(int cx, int cy, int rx, int ry,
                            int weight, const GfxColor& c) override {
        Cmd& k = push(Cmd::ELLO);
        k.a = cx; k.b = cy; k.c = rx; k.d = ry; k.weight = weight; k.col = c;
    }
    void drawEllipseFilled(int cx, int cy, int rx, int ry,
                           const GfxColor& c) override {
        Cmd& k = push(Cmd::ELLF);
        k.a = cx; k.b = cy; k.c = rx; k.d = ry; k.col = c;
    }
    void drawText(const std::string& s, int x, int y,
                  int scale, const GfxColor& c,
                  const GfxFont& font) override {
        Cmd& k = push(Cmd::TEXT);
        k.text = s; k.a = x; k.b = y; k.scale = scale < 1 ? 1 : scale;
        k.col = c; k.font = &font;
    }

    void drawImage(const GfxImage& img, int x, int y,
                   int w, int h) override {
        Cmd& k = push(Cmd::IMG);
        k.p = &img; k.a = x; k.b = y; k.c = w; k.d = h;
    }

    void imageDropped(const GfxImage& img) override {
        // removes pending drawings of the current frame and the cached texture
        queue.erase(std::remove_if(queue.begin(), queue.end(),
                                   [&](const Cmd& k) {
                                       return k.type == Cmd::IMG && k.p == &img;
                                   }),
                    queue.end());
        auto it = imgTex.find(&img);
        if (it != imgTex.end()) {
            UnloadTexture(it->second);
            imgTex.erase(it);
        }
    }

    bool saveImage(const std::string& path) override {
        if (!opened) return false;
        Image img = LoadImageFromScreen();
        ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);

        bool png = path.size() > 4 &&
                   path.compare(path.size() - 4, 4, ".png") == 0;
        bool ok;
        if (png) {
            ExportImage(img, path.c_str()); // raylib writes PNG (stb)
            ok = true;
        } else {
            const unsigned char* data = (const unsigned char*)img.data;
            std::vector<GfxColor> tmp((size_t)img.width * img.height);
            for (int i = 0; i < img.width * img.height; i++) {
                tmp[i] = GfxColor{data[i * 4], data[i * 4 + 1],
                                  data[i * 4 + 2], data[i * 4 + 3]};
            }
            ok = gfx_write_ppm(path, tmp.data(), img.width, img.height);
        }
        UnloadImage(img);
        return ok;
    }

    int width()  const override { return W; }
    int height() const override { return H; }

private:
    int W = 0, H = 0;
    bool opened = false;
    std::vector<Cmd> queue;
    std::string lastKey;       // last key typed (persists)
    bool keyHitThisFrame = false;
    // GPU texture per image (created on 1st use, until imageDropped/close)
    std::unordered_map<const GfxImage*, Texture2D> imgTex;

    void freeTextures() {
        for (auto& kv : imgTex) UnloadTexture(kv.second);
        imgTex.clear();
    }

    Cmd& push(Cmd::Type t) {
        queue.push_back(Cmd());
        queue.back().type = t;
        return queue.back();
    }

    void replay(const Cmd& k) {
        switch (k.type) {
            case Cmd::CLEAR: ClearBackground(rc(k.col)); break;
            case Cmd::PIX:   DrawPixel(k.a, k.b, rc(k.col)); break;
            case Cmd::LINE:
                DrawLineEx(::Vector2{(float)k.a, (float)k.b},
                           ::Vector2{(float)k.c, (float)k.d},
                           (float)(k.weight < 1 ? 1 : k.weight), rc(k.col));
                break;
            case Cmd::RECO: {
                // Thick border (DrawRectangleLinesEx is always 1px)
                int w = k.weight < 1 ? 1 : k.weight;
                drawThickRect(k.a, k.b, k.c, k.d, w, k.col);
                break;
            }
            case Cmd::RECTF:
                DrawRectangle(k.a, k.b, k.c, k.d, rc(k.col));
                break;
            case Cmd::ELLO: {
                for (int i = 0; i < k.weight; i++) {
                    float rx = (float)(k.c - i), ry = (float)(k.d - i);
                    if (rx < 0.5f || ry < 0.5f) break;
                    DrawEllipseLines((float)k.a, (float)k.b, rx, ry, rc(k.col));
                }
                break;
            }
            case Cmd::ELLF:
                DrawEllipse((float)k.a, (float)k.b, (float)k.c, (float)k.d,
                            rc(k.col));
                break;
            case Cmd::IMG: {
                const GfxImage* im = k.p;
                if (!im) break;
                auto it = imgTex.find(im);
                if (it == imgTex.end()) {
                    Image ri{};
                    ri.data = (void*)im->px.data();
                    ri.width = im->w;
                    ri.height = im->h;
                    ri.mipmaps = 1;
                    ri.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
                    it = imgTex.emplace(im, LoadTextureFromImage(ri)).first;
                }
                if (k.c == im->w && k.d == im->h) {
                    DrawTexture(it->second, k.a, k.b, ::Color{255, 255, 255, 255});
                } else {
                    ::Rectangle src{0.f, 0.f, (float)im->w, (float)im->h};
                    ::Rectangle dst{(float)k.a, (float)k.b, (float)k.c, (float)k.d};
                    DrawTexturePro(it->second, src, dst,
                                   ::Vector2{0.f, 0.f}, 0.f,
                                   ::Color{255, 255, 255, 255});
                }
                break;
            }
            case Cmd::TEXT: {
                if (!k.font) break;
                const GfxFont& font = *k.font;
                const int sc = k.scale < 1 ? 1 : k.scale;
                const int adv = (font.w + 1) * sc; // 1 px of spacing
                int cx = k.a, cy = k.b;
                const int x0 = k.a;
                for (size_t i = 0; i < k.text.size(); i++) {
                    char ch = k.text[i];
                    if (ch == '\n') { cx = x0; cy += (font.h + 1) * sc; continue; }
                    if (ch == '\t') { cx += adv * 4; continue; }
                    const GfxGlyph* g = gfx_glyph(font, ch);
                    if (g) {
                        for (int gy = 0; gy < font.h && g->rows[gy]; gy++) {
                            const char* row = g->rows[gy];
                            for (int gx = 0; gx < font.w; gx++) {
                                if (row[gx] != '1') continue;
                                DrawRectangle(cx + gx * sc, cy + gy * sc, sc, sc,
                                              rc(k.col));
                            }
                        }
                    }
                    cx += adv;
                }
                break;
            }
        }
    }

    void drawThickRect(int x, int y, int w, int h, int weight,
                       const GfxColor& c) {
        ::Vector2 a{(float)x, (float)y};
        ::Vector2 b{(float)(x + w - 1), (float)y};
        ::Vector2 cc{(float)(x + w - 1), (float)(y + h - 1)};
        ::Vector2 d{(float)x, (float)(y + h - 1)};
        DrawLineEx(a, b, (float)weight, rc(c));
        DrawLineEx(b, cc, (float)weight, rc(c));
        DrawLineEx(cc, d, (float)weight, rc(c));
        DrawLineEx(d, a, (float)weight, rc(c));
    }
};

RaylibBackend g_raylib;

} // namespace

GraphicsBackend* raylib_backend() { return &g_raylib; }

#endif // HAVE_RAYLIB
