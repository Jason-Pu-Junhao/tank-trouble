// canvas.hpp —— 基于 Xlib 的双缓冲绘图画布（零第三方依赖，只需 libX11）
//
// 设计要点：
//   * 双缓冲：所有绘制先画到后台 Pixmap，最后 present() 一次性 XCopyArea 到窗口，
//     彻底消除闪烁（这是所有实时图形程序的基本功）。
//   * 颜色缓存：XAllocColor 有开销，按 RGB 缓存已分配的像素值。
//   * 内置 5x7 点阵字体：不依赖 X 服务器上装了什么字体，任何环境下都能显示
//     清晰的文字，并且可以整数倍放大（scale），保证像素风格一致。
#pragma once

#include <X11/Xlib.h>
#include <map>
#include <set>
#include <string>

namespace t3 {

class Canvas {
public:
    Canvas(int w, int h, const std::string& title, bool visible = true);
    ~Canvas();
    Canvas(const Canvas&) = delete;
    Canvas& operator=(const Canvas&) = delete;

    int width() const { return w_; }
    int height() const { return h_; }

    // ---- 绘制原语（坐标均为像素，原点在左上角，y 轴向下） ----
    void clear(unsigned rgb);
    void rect(int x, int y, int w, int h, unsigned rgb);
    void rect_outline(int x, int y, int w, int h, int thick, unsigned rgb);
    void circle(int cx, int cy, int r, unsigned rgb);              // 实心圆
    void ring(int cx, int cy, int r, int thick, unsigned rgb);     // 圆环
    void line(int x0, int y0, int x1, int y1, int thick, unsigned rgb);
    void text(int x, int y, const std::string& s, int scale, unsigned rgb);
    int  text_width(const std::string& s, int scale) const;
    int  text_height(int scale) const { return 7 * scale; }

    // 把后台缓冲提交到屏幕
    void present();
    void bell();                                   // 响一声（用于开炮/爆炸音效）

    // ---- 事件 ----
    bool pump();                              // 返回 false 表示用户关闭了窗口
    bool key_down(KeySym k) const;            // 该键当前是否按住
    bool key_hit(KeySym k) const;             // 该键是否在本帧刚按下

    // 把后台缓冲导出为 PPM（用于自动截图、写作业文档）
    bool save_ppm(const std::string& path);

private:
    unsigned long pixel(unsigned rgb);
    void draw_glyph(int x, int y, char c, int scale, unsigned long px);

    Display* dpy_ = nullptr;
    int      screen_ = 0;
    Window   win_ = 0;
    Pixmap   back_ = 0;
    GC       gc_ = nullptr;
    Atom     wm_delete_ = 0;
    int      w_ = 0, h_ = 0;
    bool     visible_ = true;
    bool     quit_ = false;
    std::map<unsigned, unsigned long> palette_;
    std::set<KeySym> down_, hit_;
};

}  // namespace t3
