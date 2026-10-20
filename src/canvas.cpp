// canvas.cpp —— Canvas 的实现：X11 双缓冲 + 内置 5x7 点阵字体
#include "canvas.hpp"

#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/XKBlib.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <cmath>
#include <vector>

namespace t3 {

// ============================ 内置 5x7 点阵字体 ============================
// 每个字形 7 行、每行 5 个像素，'#' 表示点亮。这样写的源码一眼就能看出字形，
// 也便于手工修改；启动时编译成位掩码表，绘制时按整数倍放大画小方块。
namespace {

struct GlyphDef { char ch; const char* rows[7]; };

const GlyphDef kGlyphs[] = {
    {' ',  {"     ", "     ", "     ", "     ", "     ", "     ", "     "}},
    {'0',  {".###.", "#...#", "#..##", "#.#.#", "##..#", "#...#", ".###."}},
    {'1',  {"..#..", ".##..", "..#..", "..#..", "..#..", "..#..", ".###."}},
    {'2',  {".###.", "#...#", "....#", "...#.", "..#..", ".#...", "#####"}},
    {'3',  {"#####", "...#.", "..#..", "...#.", "....#", "#...#", ".###."}},
    {'4',  {"...#.", "..##.", ".#.#.", "#..#.", "#####", "...#.", "...#."}},
    {'5',  {"#####", "#....", "####.", "....#", "....#", "#...#", ".###."}},
    {'6',  {"..##.", ".#...", "#....", "####.", "#...#", "#...#", ".###."}},
    {'7',  {"#####", "....#", "...#.", "..#..", ".#...", ".#...", ".#..."}},
    {'8',  {".###.", "#...#", "#...#", ".###.", "#...#", "#...#", ".###."}},
    {'9',  {".###.", "#...#", "#...#", ".####", "...#.", "..#..", ".##.."}},
    {'A',  {".###.", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"}},
    {'B',  {"####.", "#...#", "#...#", "####.", "#...#", "#...#", "####."}},
    {'C',  {".###.", "#...#", "#....", "#....", "#....", "#...#", ".###."}},
    {'D',  {"####.", "#...#", "#...#", "#...#", "#...#", "#...#", "####."}},
    {'E',  {"#####", "#....", "#....", "####.", "#....", "#....", "#####"}},
    {'F',  {"#####", "#....", "#....", "####.", "#....", "#....", "#...."}},
    {'G',  {".###.", "#...#", "#....", "#.###", "#...#", "#...#", ".###."}},
    {'H',  {"#...#", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"}},
    {'I',  {".###.", "..#..", "..#..", "..#..", "..#..", "..#..", ".###."}},
    {'J',  {"..###", "...#.", "...#.", "...#.", "...#.", "#..#.", ".##.."}},
    {'K',  {"#...#", "#..#.", "#.#..", "##...", "#.#..", "#..#.", "#...#"}},
    {'L',  {"#....", "#....", "#....", "#....", "#....", "#....", "#####"}},
    {'M',  {"#...#", "##.##", "#.#.#", "#...#", "#...#", "#...#", "#...#"}},
    {'N',  {"#...#", "##..#", "#.#.#", "#..##", "#...#", "#...#", "#...#"}},
    {'O',  {".###.", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."}},
    {'P',  {"####.", "#...#", "#...#", "####.", "#....", "#....", "#...."}},
    {'Q',  {".###.", "#...#", "#...#", "#...#", "#.#.#", "#..#.", ".##.#"}},
    {'R',  {"####.", "#...#", "#...#", "####.", "#.#..", "#..#.", "#...#"}},
    {'S',  {".####", "#....", "#....", ".###.", "....#", "....#", "####."}},
    {'T',  {"#####", "..#..", "..#..", "..#..", "..#..", "..#..", "..#.."}},
    {'U',  {"#...#", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."}},
    {'V',  {"#...#", "#...#", "#...#", "#...#", "#...#", ".#.#.", "..#.."}},
    {'W',  {"#...#", "#...#", "#...#", "#...#", "#.#.#", "##.##", "#...#"}},
    {'X',  {"#...#", "#...#", ".#.#.", "..#..", ".#.#.", "#...#", "#...#"}},
    {'Y',  {"#...#", "#...#", ".#.#.", "..#..", "..#..", "..#..", "..#.."}},
    {'Z',  {"#####", "....#", "...#.", "..#..", ".#...", "#....", "#####"}},
    {':',  {".....", "..#..", "..#..", ".....", "..#..", "..#..", "....."}},
    {'-',  {".....", ".....", ".....", "#####", ".....", ".....", "....."}},
    {'.',  {".....", ".....", ".....", ".....", ".....", ".##..", ".##.."}},
    {',',  {".....", ".....", ".....", ".....", ".##..", ".##..", ".#..."}},
    {'/',  {"....#", "....#", "...#.", "..#..", ".#...", "#....", "#...."}},
    {'!',  {"..#..", "..#..", "..#..", "..#..", "..#..", ".....", "..#.."}},
    {'?',  {".###.", "#...#", "....#", "...#.", "..#..", ".....", "..#.."}},
    {'%',  {"##..#", "##..#", "...#.", "..#..", ".#...", "#..##", "#..##"}},
    {'+',  {".....", "..#..", "..#..", "#####", "..#..", "..#..", "....."}},
    {'=',  {".....", ".....", "#####", ".....", "#####", ".....", "....."}},
    {'*',  {".....", "#.#.#", ".###.", "#####", ".###.", "#.#.#", "....."}},
    {']',  {".###.", "..#..", "..#..", "..#..", "..#..", "..#..", ".###."}},
    {'#',  {".#.#.", "#####", ".#.#.", ".#.#.", "#####", ".#.#.", "....."}},
};

// 编译后的位掩码表：bit(4-col) 表示该行第 col 列点亮
unsigned char g_font[256][7];
bool g_font_ready = false;

void build_font() {
    if (g_font_ready) return;
    for (auto& row : g_font) std::memset(row, 0, sizeof(row));
    for (const auto& g : kGlyphs)
        for (int r = 0; r < 7; ++r) {
            unsigned char bits = 0;
            for (int c = 0; c < 5; ++c)
                if (g.rows[r][c] == '#') bits |= (unsigned char)(1u << (4 - c));
            g_font[(unsigned char)g.ch][r] = bits;
        }
    g_font_ready = true;
}

}  // namespace

// ================================ Canvas ================================
Canvas::Canvas(int w, int h, const std::string& title, bool visible)
    : w_(w), h_(h), visible_(visible) {
    build_font();
    dpy_ = XOpenDisplay(nullptr);
    if (!dpy_) {
        std::fprintf(stderr, "错误：无法连接到 X 显示（DISPLAY=%s）。\n",
                     std::getenv("DISPLAY") ? std::getenv("DISPLAY") : "(未设置)");
        std::exit(1);
    }
    screen_ = DefaultScreen(dpy_);
    const unsigned long black = BlackPixel(dpy_, screen_);
    win_ = XCreateSimpleWindow(dpy_, RootWindow(dpy_, screen_), 0, 0,
                               (unsigned)w_, (unsigned)h_, 0, black, black);
    XStoreName(dpy_, win_, title.c_str());
    XSelectInput(dpy_, win_, ExposureMask | KeyPressMask | KeyReleaseMask |
                                 StructureNotifyMask);
    // 让 X 只在"真正按下"时发 KeyPress，长按不会插入 KeyRelease，避免按住方向键时抖动
    Bool supported = False;
    XkbSetDetectableAutoRepeat(dpy_, True, &supported);
    if (visible_) {
        XMapWindow(dpy_, win_);
        XMoveWindow(dpy_, win_, 80, 80);
    }
    wm_delete_ = XInternAtom(dpy_, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy_, win_, &wm_delete_, 1);

    back_ = XCreatePixmap(dpy_, win_, (unsigned)w_, (unsigned)h_,
                          (unsigned)DefaultDepth(dpy_, screen_));
    gc_ = XCreateGC(dpy_, back_, 0, nullptr);
    XSetLineAttributes(dpy_, gc_, 1, LineSolid, CapButt, JoinMiter);
}

Canvas::~Canvas() {
    if (dpy_) {
        if (gc_) XFreeGC(dpy_, gc_);
        if (back_) XFreePixmap(dpy_, back_);
        if (win_) XDestroyWindow(dpy_, win_);
        XCloseDisplay(dpy_);
    }
}

unsigned long Canvas::pixel(unsigned rgb) {
    auto it = palette_.find(rgb);
    if (it != palette_.end()) return it->second;
    XColor c;
    c.red   = (unsigned short)(((rgb >> 16) & 0xFF) * 257);
    c.green = (unsigned short)(((rgb >> 8) & 0xFF) * 257);
    c.blue  = (unsigned short)((rgb & 0xFF) * 257);
    c.flags = DoRed | DoGreen | DoBlue;
    if (!XAllocColor(dpy_, DefaultColormap(dpy_, screen_), &c)) c.pixel = 0;
    palette_[rgb] = c.pixel;
    return c.pixel;
}

void Canvas::clear(unsigned rgb) {
    XSetForeground(dpy_, gc_, pixel(rgb));
    XFillRectangle(dpy_, back_, gc_, 0, 0, (unsigned)w_, (unsigned)h_);
}

void Canvas::rect(int x, int y, int w, int h, unsigned rgb) {
    if (w <= 0 || h <= 0) return;
    XSetForeground(dpy_, gc_, pixel(rgb));
    XFillRectangle(dpy_, back_, gc_, x, y, (unsigned)w, (unsigned)h);
}

void Canvas::rect_outline(int x, int y, int w, int h, int thick, unsigned rgb) {
    if (w <= 0 || h <= 0 || thick <= 0) return;
    rect(x, y, w, thick, rgb);
    rect(x, y + h - thick, w, thick, rgb);
    rect(x, y, thick, h, rgb);
    rect(x + w - thick, y, thick, h, rgb);
}

void Canvas::circle(int cx, int cy, int r, unsigned rgb) {
    if (r <= 0) return;
    XSetForeground(dpy_, gc_, pixel(rgb));
    XFillArc(dpy_, back_, gc_, cx - r, cy - r, (unsigned)(2 * r), (unsigned)(2 * r),
             0, 360 * 64);
}

void Canvas::ring(int cx, int cy, int r, int thick, unsigned rgb) {
    if (r <= 0 || thick <= 0) return;
    XSetForeground(dpy_, gc_, pixel(rgb));
    XSetLineAttributes(dpy_, gc_, (unsigned)thick, LineSolid, CapButt, JoinMiter);
    XDrawArc(dpy_, back_, gc_, cx - r, cy - r, (unsigned)(2 * r), (unsigned)(2 * r),
             0, 360 * 64);
    XSetLineAttributes(dpy_, gc_, 1, LineSolid, CapButt, JoinMiter);
}

void Canvas::line(int x0, int y0, int x1, int y1, int thick, unsigned rgb) {
    XSetForeground(dpy_, gc_, pixel(rgb));
    XSetLineAttributes(dpy_, gc_, (unsigned)thick, LineSolid, CapRound, JoinRound);
    XDrawLine(dpy_, back_, gc_, x0, y0, x1, y1);
    XSetLineAttributes(dpy_, gc_, 1, LineSolid, CapButt, JoinMiter);
}

void Canvas::draw_glyph(int x, int y, char c, int scale, unsigned long /*px*/) {
    const unsigned char* rows = g_font[(unsigned char)c];
    for (int r = 0; r < 7; ++r) {
        const unsigned char bits = rows[r];
        int c0 = -1;
        for (int col = 0; col <= 5; ++col) {          // 把连续点亮的列合并成一次 FillRectangle
            const bool on = (col < 5) && (bits & (1u << (4 - col)));
            if (on && c0 < 0) c0 = col;
            if (!on && c0 >= 0) {
                XFillRectangle(dpy_, back_, gc_, x + c0 * scale, y + r * scale,
                               (unsigned)((col - c0) * scale), (unsigned)scale);
                c0 = -1;
            }
        }
    }
}

void Canvas::text(int x, int y, const std::string& s, int scale, unsigned rgb) {
    if (scale < 1) scale = 1;
    XSetForeground(dpy_, gc_, pixel(rgb));
    int cx = x;
    for (char ch : s) {
        if (ch == '\n') { continue; }
        draw_glyph(cx, y, (char)std::toupper((unsigned char)ch), scale, 0);
        cx += 6 * scale;                              // 5 列字宽 + 1 列字距
    }
}

int Canvas::text_width(const std::string& s, int scale) const {
    return (int)s.size() * 6 * scale;
}

void Canvas::present() {
    XCopyArea(dpy_, back_, win_, gc_, 0, 0, (unsigned)w_, (unsigned)h_, 0, 0);
    XFlush(dpy_);
}

bool Canvas::pump() {
    hit_.clear();
    while (XPending(dpy_)) {
        XEvent ev;
        XNextEvent(dpy_, &ev);
        switch (ev.type) {
        case KeyPress:
        case KeyRelease: {
            const KeySym k = XLookupKeysym(&ev.xkey, 0);
            if (ev.type == KeyPress) {
                if (!down_.count(k)) hit_.insert(k);
                down_.insert(k);
            } else {
                down_.erase(k);
            }
            break;
        }
        case ClientMessage:
            if ((Atom)ev.xclient.data.l[0] == wm_delete_) quit_ = true;
            break;
        case DestroyNotify:
            quit_ = true;
            break;
        default:
            break;
        }
    }
    // 也允许用 Esc / Q 直接退出
    if (key_hit(XK_Escape)) quit_ = true;
    return !quit_;
}

void Canvas::bell() { XBell(dpy_, 0); }

bool Canvas::key_down(KeySym k) const { return down_.count(k) != 0; }
bool Canvas::key_hit(KeySym k) const { return hit_.count(k) != 0; }

bool Canvas::save_ppm(const std::string& path) {
    XImage* img = XGetImage(dpy_, back_, 0, 0, (unsigned)w_, (unsigned)h_,
                            AllPlanes, ZPixmap);
    if (!img) return false;
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) { XDestroyImage(img); return false; }
    std::fprintf(f, "P6\n%d %d\n255\n", w_, h_);
    std::vector<unsigned char> row((size_t)w_ * 3);
    for (int y = 0; y < h_; ++y) {
        for (int x = 0; x < w_; ++x) {
            const unsigned long p = XGetPixel(img, x, y);
            row[(size_t)x * 3 + 0] = (unsigned char)((p >> 16) & 0xFF);
            row[(size_t)x * 3 + 1] = (unsigned char)((p >> 8) & 0xFF);
            row[(size_t)x * 3 + 2] = (unsigned char)(p & 0xFF);
        }
        std::fwrite(row.data(), 1, row.size(), f);
    }
    std::fclose(f);
    XDestroyImage(img);
    return true;
}

}  // namespace t3
