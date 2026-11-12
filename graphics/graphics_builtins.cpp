// Isiliel — scripting language interpreter/compiler.
// Embedded: sandbox, embedding API, OOP, 2D graphics and REPL.
// Authors: Eben (AI-assisted) | Version: 1.0 | Date: 2026-11-08

#include "graphics_builtins.h"
#include "graphics.h"
#include "graphics_font.h" // GFX_FONTS (font table; default index 0)
#include "../vm.h"

#include <cmath>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

// ===========================================================================
// Processing-style graphics builtins.
//
// API (identical in ANY backend — portability guaranteed by the HAL):
//   size(w,h)        creates the screen
//   background(g|r,g,b[,a])   paints the background
//   fill(...)/stroke(...)    fill/outline colors
//   noFill()/noStroke()/strokeWeight(px)/textSize(px)
//   point(x,y)|pixel(x,y)  line(x1,y1,x2,y2)  rect(x,y,w,h)
//   ellipse(x,y,w,h)       circle(x,y,d)      text(str,x,y)
//   show()            presents the frame (0 if the window was closed)
//   save_screen(path) writes the screen (.ppm; .png with raylib)
//   width()/height()  screen dimensions
//   backend(name)     chooses the backend BEFORE size()
// ===========================================================================

namespace {

// --- style (drawing state), Processing style --------------------------------
struct Style {
    GfxColor fill{255, 255, 255, 255};   // default: white
    GfxColor stroke{0, 0, 0, 255};       // default: black
    bool fillOn = true;
    bool strokeOn = true;
    int strokeW = 1;
    int textSize = 1;                    // integer scale of the active font
    int fontIdx = 0;                     // index in GFX_FONTS (0 = "8x8")
};

Style S;

inline bool is_num(const Value& v) {
    return v.type == VAL_FLOAT || v.type == VAL_INT || v.type == VAL_COMPLEX;
}

inline int to_byte(float f) {
    if (f < 0.0f) f = 0.0f;
    if (f > 255.0f) f = 255.0f;
    return (int)std::lround(f);
}

// 1 arg = gray; 3 = r,g,b; 4 = r,g,b,a (alpha stored, no blending)
GfxColor parse_color(Value* args, int count, const char* fn) {
    for (int i = 0; i < count; i++)
        if (!is_num(args[i]))
            throw std::runtime_error(std::string(fn) +
                ": cor deve ser numero (cinza) ou r,g,b[,a]");
    GfxColor c;
    if (count == 1) {
        unsigned char g = (unsigned char)to_byte(args[0].f);
        c = GfxColor{g, g, g, 255};
    } else {
        c = GfxColor{(unsigned char)to_byte(args[0].f),
                     (unsigned char)to_byte(args[1].f),
                     (unsigned char)to_byte(args[2].f),
                     (unsigned char)(count == 4 ? to_byte(args[3].f) : 255)};
    }
    return c;
}

// Thread guard (item 15): the screen/backend/input are not thread-safe
// (raylib command queue, framebuffer, GetMouse*/GetKey*...).
// Static initialization = runs before main, when only the main thread
// exists — always reliable as a reference.
static const std::thread::id g_main_tid = std::this_thread::get_id();

void require_main_thread(const char* fn) {
    if (std::this_thread::get_id() != g_main_tid)
        throw std::runtime_error(std::string(fn) +
            ": somente na thread principal (graficos e input nao rodam em threads)");
}

void require_open(const char* fn) {
    require_main_thread(fn);
    if (!gfx_is_open())
        throw std::runtime_error(std::string(fn) + ": chame size() antes");
}

void require_num(Value* v, const char* fn, const char* what) {
    if (!is_num(*v))
        throw std::runtime_error(std::string(fn) + ": " + what + " deve ser numero");
}

int iarg(Value* v, const char* fn, const char* what) {
    require_num(v, fn, what);
    return (int)std::lround(v->f);
}

// --- handlers ----------------------------------------------------------------
void b_size(Value* a, int n, Value* r) {
    (void)n;
    require_main_thread("size");
    int w = iarg(&a[0], "size", "largura");
    int h = iarg(&a[1], "size", "altura");
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192)
        throw std::runtime_error("size: dimensoes invalidas (1..8192)");
    if (!gfx_size(w, h, "Isiliel"))
        throw std::runtime_error("size: nao foi possivel criar a tela no backend '" +
                                 gfx_backend_name() + "'");
    // style returns to the Processing default on every new screen
    S = Style();
    *r = make_float(1.0f);
}

void b_backend(Value* a, int n, Value* r) {
    (void)n;
    require_main_thread("backend");
    if (a[0].type != VAL_STRING)
        throw std::runtime_error("backend: argumento deve ser string");
    if (gfx_is_open())
        throw std::runtime_error("backend: chame backend() antes de size()");
    if (!gfx_select_backend(a[0].s)) {
        if (!gfx_backend_available(a[0].s))
            throw std::runtime_error("backend: '" + a[0].s +
                "' nao esta disponivel nesta build (disponiveis: software"
#ifdef HAVE_RAYLIB
                ", raylib"
#endif
                ")");
        throw std::runtime_error("backend: nome invalido '" + a[0].s + "'");
    }
    *r = make_float(1.0f);
}

void b_background(Value* a, int n, Value* r) {
    require_open("background");
    if (n != 1 && n != 3 && n != 4)
        throw std::runtime_error("background: use (cinza) ou (r, g, b[, a])");
    gfx_active()->clear(parse_color(a, n, "background"));
    *r = make_float(1.0f);
}

void b_fill(Value* a, int n, Value* r) {
    if (n != 1 && n != 3 && n != 4)
        throw std::runtime_error("fill: use (cinza) ou (r, g, b[, a])");
    S.fill = parse_color(a, n, "fill");
    S.fillOn = true;
    *r = make_float(1.0f);
}

void b_stroke(Value* a, int n, Value* r) {
    if (n != 1 && n != 3 && n != 4)
        throw std::runtime_error("stroke: use (cinza) ou (r, g, b[, a])");
    S.stroke = parse_color(a, n, "stroke");
    S.strokeOn = true;
    *r = make_float(1.0f);
}

void b_noFill(Value*, int, Value* r) {
    S.fillOn = false;
    *r = make_float(1.0f);
}

void b_noStroke(Value*, int, Value* r) {
    S.strokeOn = false;
    *r = make_float(1.0f);
}

void b_strokeWeight(Value* a, int, Value* r) {
    int w = iarg(&a[0], "strokeWeight", "espessura");
    if (w < 1 || w > 64)
        throw std::runtime_error("strokeWeight: espessura invalida (1..64)");
    S.strokeW = w;
    *r = make_float(1.0f);
}

void b_textSize(Value* a, int, Value* r) {
    int s = iarg(&a[0], "textSize", "escala");
    if (s < 1 || s > 16)
        throw std::runtime_error("textSize: escala invalida (1..16)");
    S.textSize = s;
    *r = make_float(1.0f);
}

// point/pixel: stroke color (as in Processing)
void b_point(Value* a, int, Value* r) {
    require_open("point");
    int x = iarg(&a[0], "point", "x"), y = iarg(&a[1], "point", "y");
    if (S.strokeOn) gfx_active()->drawPixel(x, y, S.stroke);
    *r = make_float(1.0f);
}

void b_line(Value* a, int, Value* r) {
    require_open("line");
    int x1 = iarg(&a[0], "line", "x1"), y1 = iarg(&a[1], "line", "y1");
    int x2 = iarg(&a[2], "line", "x2"), y2 = iarg(&a[3], "line", "y2");
    if (S.strokeOn) gfx_active()->drawLine(x1, y1, x2, y2, S.strokeW, S.stroke);
    *r = make_float(1.0f);
}

// rect: x,y = top-left corner (rectMode CORNER)
void b_rect(Value* a, int, Value* r) {
    require_open("rect");
    int x = iarg(&a[0], "rect", "x"), y = iarg(&a[1], "rect", "y");
    int w = iarg(&a[2], "rect", "largura"), h = iarg(&a[3], "rect", "altura");
    if (S.fillOn)   gfx_active()->drawRectFilled(x, y, w, h, S.fill);
    if (S.strokeOn) gfx_active()->drawRectOutline(x, y, w, h, S.strokeW, S.stroke);
    *r = make_float(1.0f);
}

// ellipse/circle: x,y = CENTER (ellipseMode CENTER, as in Processing)
void b_ellipse(Value* a, int, Value* r) {
    require_open("ellipse");
    int cx = iarg(&a[0], "ellipse", "x"), cy = iarg(&a[1], "ellipse", "y");
    int w = iarg(&a[2], "ellipse", "largura"), h = iarg(&a[3], "ellipse", "altura");
    int rx = w / 2, ry = h / 2;
    if (S.fillOn)   gfx_active()->drawEllipseFilled(cx, cy, rx, ry, S.fill);
    if (S.strokeOn) gfx_active()->drawEllipseOutline(cx, cy, rx, ry, S.strokeW, S.stroke);
    *r = make_float(1.0f);
}

void b_circle(Value* a, int, Value* r) {
    require_open("circle");
    int cx = iarg(&a[0], "circle", "x"), cy = iarg(&a[1], "circle", "y");
    int d = iarg(&a[2], "circle", "diametro");
    int rad = d / 2;
    if (S.fillOn)   gfx_active()->drawEllipseFilled(cx, cy, rad, rad, S.fill);
    if (S.strokeOn) gfx_active()->drawEllipseOutline(cx, cy, rad, rad, S.strokeW, S.stroke);
    *r = make_float(1.0f);
}

// text: y = glyph top; colors via fill(); \n breaks the line
void b_text(Value* a, int, Value* r) {
    require_open("text");
    std::string s;
    if (a[0].type == VAL_STRING) s = a[0].s;
    else if (is_num(a[0])) {
        std::ostringstream os;
        os << a[0].f;
        s = os.str();
    } else {
        throw std::runtime_error("text: primeiro argumento deve ser string ou numero");
    }
    int x = iarg(&a[1], "text", "x"), y = iarg(&a[2], "text", "y");
    if (S.fillOn)
        gfx_active()->drawText(s, x, y, S.textSize, S.fill, GFX_FONTS[S.fontIdx]);
    *r = make_float(1.0f);
}

// font(): active font for text(); font("5x7") switches it (before/after size)
void b_font(Value* a, int n, Value* r) {
    if (n == 0) {
        *r = make_string(GFX_FONTS[S.fontIdx].name);
        return;
    }
    if (a[0].type != VAL_STRING)
        throw std::runtime_error("font: nome deve ser string");
    int found = -1;
    for (int i = 0; i < GFX_FONTS_COUNT; i++)
        if (std::string(GFX_FONTS[i].name) == a[0].s) { found = i; break; }
    if (found < 0) {
        std::string avail;
        for (int i = 0; i < GFX_FONTS_COUNT; i++) {
            if (i) avail += ", ";
            avail += GFX_FONTS[i].name;
        }
        throw std::runtime_error("font: fonte desconhecida '" + a[0].s +
                                 "' (disponiveis: " + avail + ")");
    }
    S.fontIdx = found;
    *r = make_float(1.0f);
}

void b_show(Value*, int, Value* r) {
    require_main_thread("show");
    *r = make_float(gfx_show() ? 1.0f : 0.0f);
}

void b_save_screen(Value* a, int, Value* r) {
    require_main_thread("save_screen");
    if (a[0].type != VAL_STRING)
        throw std::runtime_error("save_screen: caminho deve ser string");
    *r = make_float(gfx_save_screen(a[0].s) ? 1.0f : 0.0f);
}

void b_width(Value*, int, Value* r) { *r = make_float((float)gfx_width()); }
void b_height(Value*, int, Value* r) { *r = make_float((float)gfx_height()); }

// --- images (PImage: loadImage/image/unloadImage) ---------------------------
void b_loadImage(Value* a, int, Value* r) {
    if (a[0].type != VAL_STRING)
        throw std::runtime_error("loadImage: caminho deve ser string");
    // does not require a window: loading an image works in headless mode
    *r = make_float((float)gfx_load_image(a[0].s));
}

void b_image(Value* a, int n, Value* r) {
    require_open("image");
    int id = iarg(&a[0], "image", "handle (valor de loadImage)");
    const GfxImage* img = gfx_image(id);
    if (!img)
        throw std::runtime_error("image: handle invalido (use o valor de loadImage)");
    int x = iarg(&a[1], "image", "x"), y = iarg(&a[2], "image", "y");
    int w = img->w, h = img->h;
    if (n == 5) {
        w = iarg(&a[3], "image", "largura");
        h = iarg(&a[4], "image", "altura");
    }
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192)
        throw std::runtime_error("image: dimensoes invalidas (1..8192)");
    gfx_active()->drawImage(*img, x, y, w, h);
    *r = make_float(1.0f);
}

void b_unloadImage(Value* a, int, Value* r) {
    int id = iarg(&a[0], "unloadImage", "handle");
    gfx_unload_image(id);
    *r = make_float(1.0f);
}

void b_imageWidth(Value* a, int, Value* r) {
    int id = iarg(&a[0], "imageWidth", "handle");
    const GfxImage* img = gfx_image(id);
    if (!img)
        throw std::runtime_error("imageWidth: handle invalido (use o valor de loadImage)");
    *r = make_float((float)img->w);
}

void b_imageHeight(Value* a, int, Value* r) {
    int id = iarg(&a[0], "imageHeight", "handle");
    const GfxImage* img = gfx_image(id);
    if (!img)
        throw std::runtime_error("imageHeight: handle invalido (use o valor de loadImage)");
    *r = make_float((float)img->h);
}

// --- input (interaction; headless returns 0/false/"") ------------------------
void b_mouseX(Value*, int, Value* r) {
    require_open("mouseX");
    *r = make_float((float)gfx_active()->mouseX());
}

void b_mouseY(Value*, int, Value* r) {
    require_open("mouseY");
    *r = make_float((float)gfx_active()->mouseY());
}

void b_mousePressed(Value*, int, Value* r) {
    require_open("mousePressed");
    *r = make_float(gfx_active()->mousePressed() ? 1.0f : 0.0f);
}

void b_keyPressed(Value*, int, Value* r) {
    require_open("keyPressed");
    *r = make_float(gfx_active()->keyPressed() ? 1.0f : 0.0f);
}

void b_key(Value*, int, Value* r) {
    require_open("key");
    *r = Value{VAL_STRING, 0, gfx_active()->key()};
}

// quit(): ends the draw() loop/the window wait (the only programmatic
// close — the terminal process also exits with Ctrl+C)
void b_quit(Value*, int, Value* r) {
    gfx_request_close();
    *r = make_float(1.0f);
}

// frameCount(): frames presented (1 on the first; useful to guard
// something that must happen only once, e.g. save() only on frame 1)
void b_frameCount(Value*, int, Value* r) {
    *r = make_float((float)gfx_frame_count());
}

// L1: setDraw(fn) — registers the frame callback. Global draw() remains
// the fallback; passing something that is not a function is a runtime error.
void b_setDraw(Value* args, int argc, Value* r) {
    if (argc < 1 || args[0].type != VAL_FUNC)
        throw std::runtime_error("setDraw espera uma funcao");
    vm_set_draw_callback(args[0]);
    *r = make_float(1.0f);
}

} // namespace

void register_graphics_builtins() {
    static bool done = false;
    if (done) return;
    done = true;

    register_builtin("size", 2, b_size);
    register_builtin("backend", 1, b_backend);
    register_builtin("background", -1, b_background);
    register_builtin("fill", -1, b_fill);
    register_builtin("stroke", -1, b_stroke);
    register_builtin("noFill", 0, b_noFill);
    register_builtin("noStroke", 0, b_noStroke);
    register_builtin("strokeWeight", 1, b_strokeWeight);
    register_builtin("textSize", 1, b_textSize);
    register_builtin("font", 0, b_font);
    register_builtin("font", 1, b_font);
    register_builtin("point", 2, b_point);
    register_builtin("pixel", 2, b_point); // alias
    register_builtin("line", 4, b_line);
    register_builtin("rect", 4, b_rect);
    register_builtin("ellipse", 4, b_ellipse);
    register_builtin("circle", 3, b_circle);
    register_builtin("text", 3, b_text);
    register_builtin("show", 0, b_show);
    register_builtin("save_screen", 1, b_save_screen);
    register_builtin("width", 0, b_width);
    register_builtin("height", 0, b_height);
    register_builtin("loadImage", 1, b_loadImage);
    register_builtin("image", 3, b_image);
    register_builtin("image", 5, b_image);
    register_builtin("unloadImage", 1, b_unloadImage);
    register_builtin("imageWidth", 1, b_imageWidth);
    register_builtin("imageHeight", 1, b_imageHeight);
    register_builtin("mouseX", 0, b_mouseX);
    register_builtin("mouseY", 0, b_mouseY);
    register_builtin("mousePressed", 0, b_mousePressed);
    register_builtin("keyPressed", 0, b_keyPressed);
    register_builtin("key", 0, b_key);
    register_builtin("quit", 0, b_quit);
    register_builtin("frameCount", 0, b_frameCount);
    register_builtin("setDraw", 1, b_setDraw);
    register_builtin("onDraw", 1, b_setDraw); // alias
}
