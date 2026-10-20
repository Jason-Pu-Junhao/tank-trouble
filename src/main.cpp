// main.cpp —— 坦克动荡：程序入口、窗口与主循环、键盘输入、自动截图
//
// 运行示例：
//   ./tank                      人机对战（默认）
//   ./tank --mode 0             双人同屏（WASD+空格 / 方向键+回车）
//   ./tank --mode 2             电脑互殴（演示模式，可以放着不管）
//   ./tank --level 3 --wins 5   困难 AI，五局三胜
//   ./tank --shot 240 a.ppm     无窗口自动跑 240 帧物理后截图（写作业文档用）
#include "canvas.hpp"
#include "game.hpp"

#include <X11/keysym.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>

using namespace t3;

namespace {

void usage() {
    std::printf(
        "坦克动荡 Tank Trouble (C++17 / Xlib, 无第三方依赖)\n"
        "用法: ./tank [选项]\n"
        "  --mode <0|1|2>    0=双人同屏  1=人机对战(默认)  2=电脑互殴\n"
        "  --level <1|2|3>   电脑难度（简单/普通/困难）\n"
        "  --wins <n>        先赢 n 局者获胜（默认 3）\n"
        "  --seed <n>        随机种子（复现同一张迷宫）\n"
        "  --bounces <n>     子弹最多反弹次数（默认 4）\n"
        "  --cell <px>       迷宫格子像素大小（默认 40）\n"
        "  --cols/--rows     迷宫格数（默认 20 x 15）\n"
        "  --fps <n>         渲染帧率上限（默认 60）\n"
        "  --sound           打开开炮/爆炸提示音（XBell）\n"
        "  --shot <帧数> <文件.ppm>   无窗口跑固定帧数后截图\n"
        "  --frames <总帧数> <步长> <目录>  无窗口导出动画帧（用于制作 GIF）\n"
        "  --start <物理帧数>  与 --frames 配合，跳过开头若干帧再开始导出\n"
        "  --sim <秒数>      无窗口自动对局，结束后打印比分/回合数/每回合耗时\n"
        "  --help            显示本帮助\n"
        "操作: P1 = W/A/S/D + 空格;  P2 = 方向键 + 回车;  P 暂停;  R 重开;  ESC 退出\n");
}

int arg_int(int argc, char** argv, const char* key, int def) {
    for (int i = 1; i + 1 < argc; ++i)
        if (std::strcmp(argv[i], key) == 0) return std::atoi(argv[i + 1]);
    return def;
}
bool arg_flag(int argc, char** argv, const char* key) {
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], key) == 0) return true;
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    if (arg_flag(argc, argv, "--help")) { usage(); return 0; }

    Config cfg;
    cfg.mode = arg_int(argc, argv, "--mode", 1);
    cfg.ai_level = arg_int(argc, argv, "--level", 2);
    cfg.wins_needed = arg_int(argc, argv, "--wins", 3);
    cfg.seed = (unsigned)arg_int(argc, argv, "--seed", 20260915);
    cfg.max_bounces = arg_int(argc, argv, "--bounces", 4);
    cfg.cell = arg_int(argc, argv, "--cell", 40);
    cfg.cols = arg_int(argc, argv, "--cols", 20);
    cfg.rows = arg_int(argc, argv, "--rows", 15);
    cfg.fps_cap = arg_int(argc, argv, "--fps", 60);
    cfg.sound = arg_flag(argc, argv, "--sound");

    // ---- 自动对局模式：无窗口跑 N 秒物理，最后打印统计（用于测试 AI 强度与稳定性） ----
    const int sim_secs = arg_int(argc, argv, "--sim", 0);
    if (sim_secs > 0) {
        Game game(cfg);
        const double step = 1.0 / 120.0;
        const int steps = sim_secs * 120;
        int i = 0;
        for (; i < steps && !game.match_over(); ++i) game.update(step);
        const char* mode_name = cfg.mode == 0 ? "双人" : (cfg.mode == 1 ? "人机" : "电脑互殴");
        std::printf("[sim] 模式=%s 难度=%d 种子=%u 模拟=%d 帧(%.1f 秒)\n",
                    mode_name, cfg.ai_level, cfg.seed, i, i * step);
        std::printf("[sim] 比分 P1=%d P2=%d   已进行 %d 回合   平均每回合 %.2f 秒\n",
                    game.score(Side::P1), game.score(Side::P2), game.round_no(),
                    game.round_no() ? game.elapsed() / game.round_no() : 0.0);
        std::printf("[sim] 比赛结束=%s  阵亡次数 P1=%d P2=%d\n",
                    game.match_over() ? "是" : "否（超时）",
                    game.deaths_of(Side::P1), game.deaths_of(Side::P2));
        return 0;
    }

    // ---- 自动截图模式：不开窗口，跑固定帧数的物理后把画面存成 PPM ----
    // 用法: --shot <帧数> <输出文件.ppm>
    const char* shot_file = nullptr;
    int shot_frames = 0;
    for (int i = 1; i + 2 < argc; ++i)
        if (std::strcmp(argv[i], "--shot") == 0) {
            shot_frames = std::atoi(argv[i + 1]);
            shot_file = argv[i + 2];
        }
    if (shot_file) {
        if (shot_frames <= 0) shot_frames = 60;
        cfg.mode = arg_int(argc, argv, "--mode", 2);      // 截图默认用演示模式
        Game game(cfg);
        Canvas cv(cfg.cols * cfg.cell, cfg.rows * cfg.cell, "tank", /*visible=*/false);
        const double step = 1.0 / 120.0;
        for (int i = 0; i < shot_frames; ++i) game.update(step);
        game.render(cv);
        cv.present();
        if (!cv.save_ppm(shot_file)) {
            std::fprintf(stderr, "截图写入失败: %s\n", shot_file);
            return 1;
        }
        std::printf("已保存截图: %s (%d 帧物理后)\n", shot_file, shot_frames);
        return 0;
    }

    // ---- 动画帧导出模式：无窗口跑 N 帧物理，每 step 帧存一张 PPM（拿去合成 GIF）----
    // 用法: --frames <总帧数> <步长> <输出目录>
    const char* frames_dir = nullptr;
    int frames_total = 0, frames_step = 1;
    const int frames_start = arg_int(argc, argv, "--start", 0);
    for (int i = 1; i + 3 < argc; ++i)
        if (std::strcmp(argv[i], "--frames") == 0) {
            frames_total = std::atoi(argv[i + 1]);
            frames_step = std::atoi(argv[i + 2]);
            frames_dir = argv[i + 3];
        }
    if (frames_dir) {
        if (frames_total <= 0) frames_total = 300;
        if (frames_step <= 0) frames_step = 4;
        cfg.mode = arg_int(argc, argv, "--mode", 2);
        std::error_code ec;
        std::filesystem::create_directories(frames_dir, ec);
        if (ec) {
            std::fprintf(stderr, "创建目录失败: %s\n", frames_dir);
            return 1;
        }
        Game game(cfg);
        Canvas cv(cfg.cols * cfg.cell, cfg.rows * cfg.cell, "tank", /*visible=*/false);
        const double step = 1.0 / 120.0;
        for (int i = 0; i < frames_start; ++i) game.update(step);   // 跳过开头，直接截取精彩片段
        int saved = 0;
        for (int i = 0; i < frames_total; ++i) {
            game.update(step);
            if (i % frames_step != 0) continue;
            game.render(cv);
            char path[512];
            std::snprintf(path, sizeof(path), "%s/f%04d.ppm", frames_dir, saved);
            if (!cv.save_ppm(path)) {
                std::fprintf(stderr, "写入失败: %s\n", path);
                return 1;
            }
            ++saved;
        }
        std::printf("已导出 %d 帧到 %s（从第 %d 帧物理开始，每 %d 帧取 1 帧）\n",
                    saved, frames_dir, frames_start, frames_step);
        return 0;
    }

    Game game(cfg);
    Canvas cv(cfg.cols * cfg.cell, cfg.rows * cfg.cell,
              "坦克动荡 Tank Trouble - C++/Xlib");

    // ---- 固定步长主循环：物理固定 120Hz，渲染按 fps_cap 限速 ----
    const double kStep = 1.0 / 120.0;
    const double kFrame = 1.0 / std::max(1, cfg.fps_cap);
    double acc = 0.0;
    auto prev = std::chrono::steady_clock::now();
    double fps_smooth = cfg.fps_cap;
    bool paused = false;

    while (cv.pump()) {
        const auto now = std::chrono::steady_clock::now();
        double frame = std::chrono::duration<double>(now - prev).count();
        prev = now;
        if (frame > 0.25) frame = 0.25;                 // 防止窗口被挂起后"物理穿越"
        if (frame > 1e-6) fps_smooth = fps_smooth * 0.92 + (1.0 / frame) * 0.08;

        // ---- 键盘 ----
        if (cv.key_hit(XK_p)) paused = !paused;
        if (cv.key_hit(XK_r)) game.request_restart();
        if (!paused && !game.match_over()) {
            auto ax = [&](KeySym neg, KeySym pos) {
                return (cv.key_down(pos) ? 1.0 : 0.0) - (cv.key_down(neg) ? 1.0 : 0.0);
            };
            // 玩家 1：WASD + 空格
            game.set_input(0, ax(XK_s, XK_w), ax(XK_a, XK_d), cv.key_down(XK_space));
            // 玩家 2：方向键 + 回车
            game.set_input(1, ax(XK_Down, XK_Up), ax(XK_Left, XK_Right),
                           cv.key_down(XK_Return) || cv.key_down(XK_KP_Enter));
        }

        if (!paused) {
            acc += frame;
            int guard = 0;
            while (acc >= kStep && guard++ < 40) { game.update(kStep); acc -= kStep; }
        }

        game.render(cv);
        // 右上角状态信息
        char buf[96];
        std::snprintf(buf, sizeof(buf), "FPS %2.0f", fps_smooth);
        cv.text(cv.width() - cv.text_width(buf, 1) - 8, cv.height() - 16, buf, 1, 0x4A5A70);
        if (paused) {
            const char* s = "PAUSED";
            cv.text(cv.width() / 2 - cv.text_width(s, 5) / 2, cv.height() / 2 - 40, s, 5,
                    0xFFFFFF);
            const char* s2 = "PRESS P TO RESUME";
            cv.text(cv.width() / 2 - cv.text_width(s2, 2) / 2, cv.height() / 2 + 20, s2, 2,
                    0x8FA0B8);
        }
        cv.present();

        if (cfg.sound) {
            const int n = game.take_beeps();
            const int m = game.take_booms();
            for (int i = 0; i < n; ++i) cv.bell();
            for (int i = 0; i < m; ++i) { cv.bell(); cv.bell(); }
        }

        // 限帧，避免空转把一个 CPU 核心跑满
        const auto spent = std::chrono::duration<double>(
                               std::chrono::steady_clock::now() - now).count();
        if (spent < kFrame) {
            std::this_thread::sleep_for(
                std::chrono::duration<double>(kFrame - spent));
        }
    }
    return 0;
}
