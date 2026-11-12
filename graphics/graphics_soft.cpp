// Isiliel — scripting language interpreter/compiler.
// Embedded: sandbox, embedding API, OOP, 2D graphics and REPL.
// Authors: EEPS and MiMo-V2.6 | Version: 1.0 | Date: 2026-11-08

#include "graphics.h"

#include <cmath>
#include <vector>

// ===========================================================================
// "software" backend: in-memory RGBA framebuffer + PPM export.
// It is the default (headless) backend and the portability reference: an embedded
// target writes an equivalent GraphicsBackend pointing to an
// LCD framebuffer (same interface, same builtins).
// ===========================================================================

namespace {

class SoftwareBackend : public GraphicsBackend {
public:
    const char* name() const override { return "software"; }

    bool open(int w, int h, const std::string&) override {
        if (w <= 0 || h <= 0) return false;
        W = w;
        H = h;
        px.assign((size_t)W * H, GfxColor{0, 0, 0, 255});
        opened = true;
        framesDrawn = 0;
        return true;
    }

    void close() override {
        px.clear();
        W = H = 0;
        opened = false;
    }

    // Headless: without a window there are no events or user close, so the
    // screen "closes" itself after the 1st frame — the draw() loop runs ONCE
    // (no risk of an infinite loop on a server/tests).
    bool frameBegin() override {
        if (!opened) return false;
        if (framesDrawn > 0) return false;
        framesDrawn++;
        return true;
    }
    void frameEnd() override {}                  // nothing to present

    void clear(const GfxColor& c) override {
        for (GfxColor& p : px) p = c;
    }

    void drawPixel(int x, int y, const GfxColor& c) override { put(x, y, c); }

    void drawLine(int x1, int y1, int x2, int y2,
                  int weight, const GfxColor& c) override {
        // Bresenham + square brush for thickness
        int dx = std::abs(x2 - x1), sx = x1 < x2 ? 1 : -1;
        int dy = -std::abs(y2 - y1), sy = y1 < y2 ? 1 : -1;
        int err = dx + dy;
        int guard = 0;
        while (guard++ < 1000000) {
            brush(x1, y1, weight, c);
            if (x1 == x2 && y1 == y2) break;
            int e2 = 2 * err;
            if (e2 >= dy) { err += dy; x1 += sx; }
            if (e2 <= dx) { err += dx; y1 += sy; }
        }
    }

    void drawRectOutline(int x, int y, int w, int h,
                         int weight, const GfxColor& c) override {
        if (w < 0) { x += w; w = -w; }
        if (h < 0) { y += h; h = -h; }
        drawLine(x, y, x + w - 1, y, weight, c);
        drawLine(x, y + h - 1, x + w - 1, y + h - 1, weight, c);
        drawLine(x, y, x, y + h - 1, weight, c);
        drawLine(x + w - 1, y, x + w - 1, y + h - 1, weight, c);
    }

    void drawRectFilled(int x, int y, int w, int h,
                        const GfxColor& c) override {
        if (w < 0) { x += w; w = -w; }
        if (h < 0) { y += h; h = -h; }
        int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
        int x1 = x + w, y1 = y + h;
        if (x1 > W) x1 = W;
        if (y1 > H) y1 = H;
        for (int yy = y0; yy < y1; yy++)
            for (int xx = x0; xx < x1; xx++)
                put(xx, yy, c);
    }

    void drawEllipseOutline(int cx, int cy, int rx, int ry,
                            int weight, const GfxColor& c) override {
        if (rx < 0) rx = -rx;
        if (ry < 0) ry = -ry;
        if (rx == 0 && ry == 0) { brush(cx, cy, weight, c); return; }
        if (rx == 0) {
            drawLine(cx, cy - ry, cx, cy + ry, weight, c);
            return;
        }
        if (ry == 0) {
            drawLine(cx - rx, cy, cx + rx, cy, weight, c);
            return;
        }
        // Midpoint variant: for each x, y = round(ry*sqrt(1-(x/rx)^2))
        for (int x = 0; x <= rx; x++) {
            float t = 1.0f - (float)((long)x * x) / (float)((long)rx * rx);
            if (t < 0.0f) t = 0.0f;
            int y = (int)std::floor(ry * std::sqrt(t) + 0.5f);
            brush(cx + x, cy - y, weight, c);
            brush(cx - x, cy - y, weight, c);
            brush(cx + x, cy + y, weight, c);
            brush(cx - x, cy + y, weight, c);
        }
    }

    void drawEllipseFilled(int cx, int cy, int rx, int ry,
                           const GfxColor& c) override {
        if (rx < 0) rx = -rx;
        if (ry < 0) ry = -ry;
        if (rx == 0 || ry == 0) { put(cx, cy, c); return; }
        for (int dy = -ry; dy <= ry; dy++) {
            float t = 1.0f - (float)((long)dy * dy) / (float)((long)ry * ry);
            if (t < 0.0f) t = 0.0f;
            int dx = (int)std::floor(rx * std::sqrt(t));
            for (int x = cx - dx; x <= cx + dx; x++) put(x, cy + dy, c);
        }
    }

    void drawText(const std::string& s, int x, int y,
                  int scale, const GfxColor& c,
                  const GfxFont& font) override {
        if (scale < 1) scale = 1;
        const int adv = (font.w + 1) * scale; // 1 px of spacing
        int ox = x;
        for (size_t i = 0; i < s.size(); i++) {
            char ch = s[i];
            if (ch == '\n') { x = ox; y += (font.h + 1) * scale; continue; }
            if (ch == '\t') { x += adv * 4; continue; }
            const GfxGlyph* g = gfx_glyph(font, ch);
            if (g) {
                for (int gy = 0; gy < font.h && g->rows[gy]; gy++) {
                    const char* row = g->rows[gy];
                    for (int gx = 0; gx < font.w; gx++) {
                        if (row[gx] != '1') continue;
                        for (int sy = 0; sy < scale; sy++)
                            for (int sx = 0; sx < scale; sx++)
                                put(x + gx * scale + sx, y + gy * scale + sy, c);
                    }
                }
            }
            x += adv;
        }
    }

    void drawImage(const GfxImage& img, int x, int y,
                   int w, int h) override {
        if (img.w <= 0 || img.h <= 0 || img.px.empty()) return;
        if (w <= 0 || h <= 0) return;
        // nearest neighbor scaling (Processing image() style)
        for (int dy = 0; dy < h; dy++) {
            int sy = (int)((long long)dy * img.h / h);
            for (int dx = 0; dx < w; dx++) {
                int sx = (int)((long long)dx * img.w / w);
                put(x + dx, y + dy, img.px[(size_t)sy * img.w + sx]);
            }
        }
    }

    bool saveImage(const std::string& path) override {
        if (!opened) return false;
        return gfx_write_ppm(path, px.data(), W, H);
    }

    int width()  const override { return W; }
    int height() const override { return H; }

private:
    std::vector<GfxColor> px;
    int W = 0, H = 0;
    bool opened = false;
    int framesDrawn = 0;

    void put(int x, int y, const GfxColor& c) {
        if (x < 0 || y < 0 || x >= W || y >= H) return;
        px[(size_t)y * W + x] = c;
    }

    // Square brush: weight = diameter (1 = single pixel)
    void brush(int x, int y, int weight, const GfxColor& c) {
        if (weight <= 1) { put(x, y, c); return; }
        int off = weight / 2;
        for (int dy = 0; dy < weight; dy++)
            for (int dx = 0; dx < weight; dx++)
                put(x + dx - off, y + dy - off, c);
    }
};

SoftwareBackend g_software;

} // namespace

GraphicsBackend* software_backend() { return &g_software; }
