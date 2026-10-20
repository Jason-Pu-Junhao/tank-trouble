// game.hpp —— 坦克动荡（Tank Trouble 风格）小游戏的核心逻辑
//
// 结构：
//   Game  = 状态机(开局/对战/回合结束/比赛结束) + 迷宫 + 两辆坦克 + 子弹 + 粒子
//   Game::update(dt) 用固定步长推进物理，与显示帧率解耦（保证不同电脑上手感一致）
//   Game::render(cv) 只负责把当前状态画出来，不做任何状态修改
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "canvas.hpp"

namespace t3 {

// ------------------------------------------------------------------ 基础数学
struct Vec2 {
    double x = 0, y = 0;
    Vec2() = default;
    Vec2(double x_, double y_) : x(x_), y(y_) {}
};
inline Vec2 operator+(Vec2 a, Vec2 b) { return {a.x + b.x, a.y + b.y}; }
inline Vec2 operator-(Vec2 a, Vec2 b) { return {a.x - b.x, a.y - b.y}; }
inline Vec2 operator*(Vec2 a, double s) { return {a.x * s, a.y * s}; }
inline double dot(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }
inline double len(Vec2 a) { return std::sqrt(a.x * a.x + a.y * a.y); }
inline Vec2 norm(Vec2 a) {
    const double l = len(a);
    return l > 1e-9 ? Vec2{a.x / l, a.y / l} : Vec2{0, 0};
}
inline double clampd(double v, double lo, double hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

struct Rect {
    double x = 0, y = 0, w = 0, h = 0;
    double cx() const { return x + w / 2; }
    double cy() const { return y + h / 2; }
};

// 圆与矩形是否相交 / 相交时的最小分离轴
struct Hit { bool hit = false; double nx = 0, ny = 0, depth = 0; };
Hit circle_vs_rect(Vec2 c, double r, const Rect& rc);

// ------------------------------------------------------------------ 游戏对象
enum class Side { Nobody = 0, P1 = 1, P2 = 2 };

struct Bullet {
    Vec2 pos, vel;
    Side owner = Side::Nobody;
    int  bounces = 0;
    double age = 0;
    bool alive = true;
    std::vector<Vec2> trail;      // 拖尾
};

struct Tank {
    Vec2   pos;
    double ang = 0;               // 车头朝向（弧度）
    Side   side = Side::Nobody;
    bool   alive = true;
    bool   ai = false;            // 由电脑控制？
    int    live_bullets = 0;      // 我自己在场上的子弹数（限制同时最多 5 发）
    double cooldown = 0;
    double invuln = 0;            // 出生保护
    int    deaths = 0;

    // 每帧由外部（键盘或 AI）写入的"意图"
    double drive = 0;             // +1 前进 / -1 后退
    double turn = 0;              // +1 右转 / -1 左转
    bool   want_fire = false;
};

struct Particle {
    Vec2   pos, vel;
    double life = 0, max_life = 1, size = 2;
    unsigned color = 0xFFFFFF;
    double drag = 0.94;
};

struct AiBrain {
    double think = 0;             // 决策计时
    double aim_err = 0;           // 当前瞄准误差（弧度）
    double strafe_t = 0;          // 走位计时
    double strafe_dir = 1;        // 绕行方向
    double fire_delay = 0;        // 反应延迟

    // --- 真预判：用最近几帧的位移估计对手速度 ---
    Vec2   foe_last;
    Vec2   foe_vel;
    double foe_sample_t = 0;
    bool   foe_init = false;

    // --- BFS 寻路：每格到对手的步数，以及下一步该往哪走 ---
    std::vector<int> dist_grid;
    int    path_cells = -1;       // 我在网格上到对手的实际步数（-1 = 不可达）
    Vec2   step_dir;              // 沿最短路的下一个方向

    // --- 反弹射线：找到的可用开火角（每 0.15 秒重新搜索一次）---
    double aim_scan_t = 0;
    bool   bank_ok = false;
    double bank_ang = 0;

    // --- 自伤风险 ---
    bool   self_harm = false;
    // --- 防卡死：周期性检查"想走却几乎没动"，就强制打方向脱困 ---
    Vec2   last_pos;
    double move_check_t = 0;
    int    stuck = 0;
    double unstick_t = 0;
    double unstick_dir = 1;
};

enum class Phase { Intro, Playing, RoundOver, MatchOver };

struct Config {
    int      cols = 20, rows = 15, cell = 40;   // 迷宫格数与格子像素
    int      wins_needed = 3;
    int      mode = 1;              // 0=双人同屏, 1=人机对战, 2=电脑互殴(演示)
    unsigned seed = 20260915;
    bool     sound = false;         // 用 XBell 发声（有些环境不允许，默认关）
    int      max_bounces = 4;       // 子弹最多反弹几次
    double   bullet_speed = 470;
    double   tank_speed = 158;
    int      ai_level = 2;          // 1=简单 2=普通 3=困难
    int      fps_cap = 60;
};

class Game {
public:
    explicit Game(const Config& cfg);

    void update(double dt);                   // 固定步长推进（推荐 1/120 s）
    void render(Canvas& cv);                  // 绘制一帧
    void set_input(int player, double drive, double turn, bool fire);

    bool match_over() const { return phase_ == Phase::MatchOver; }
    int  take_beeps();                        // 取走"开炮"音效事件数
    int  take_booms();                        // 取走"爆炸"音效事件数
    bool should_quit() const { return quit_; }
    void request_restart();                   // 按 R 重开一场
    int  score(Side s) const { return score_[(int)s]; }
    int  round_no() const { return round_; }
    double elapsed() const { return match_time_; }
    int  deaths_of(Side s) const { return tanks_[(int)s - 1].deaths; }
    int  self_kills() const { return self_kills_; }     // 被自己的反弹球打死的次数
    int  bank_shots() const { return bank_shots_; }     // 打出"反弹球"的次数
    Phase phase() const { return phase_; }
    // 供无头自动截图使用：直接跳到对战画面
    void set_demo(bool on) { demo_ = on; }

private:
    void  new_round();                        // 生成新迷宫并把坦克放回出生点
    void  generate_maze();
    double maze_connected() const;             // 可达空地占比（用于连通性拒绝采样）
    void  place_tanks();
    void  spawn_bullet(Tank& t);
    void  kill_tank(Tank& t);
    void  explode(Vec2 pos, unsigned color, int count, double power);
    void  update_tank(Tank& t, double dt);
    void  update_bullets(double dt);
    void  update_ai(Tank& t, const Tank& foe, double dt);
    void  bfs_from(int sx, int sy, std::vector<int>& dist) const;   // 网格 BFS
    // 射线：撞墙镜面反射，最多 max_bounces 次；命中 target 返回路程长度，否则 -1
    double raycast(Vec2 origin, Vec2 dir, int max_bounces, Vec2 target,
                   double hit_r, double max_len, int* used_bounces) const;
    void  collapse_walls(int pairs);          // 僵局时拆墙：让场地逐渐打开
    bool  line_clear(Vec2 a, Vec2 b) const;   // 两点之间是否没有墙
    void  add_screen_shake(double amount) { shake_ = std::max(shake_, amount); }

    Config cfg_;
    std::mt19937_64 rng_;
    std::vector<uint8_t> cells_;              // 1 = 墙
    int W_ = 0, H_ = 0;                       // 像素尺寸
    Tank tanks_[2];
    AiBrain brain_[2];
    std::vector<Bullet> bullets_;
    std::vector<Particle> particles_;
    Phase phase_ = Phase::Intro;
    double timer_ = 0;                        // 阶段计时器
    int    round_ = 1;
    int    score_[3] = {0, 0, 0};             // 下标 1/2 对应 P1/P2
    Side   last_winner_ = Side::Nobody;
    bool   quit_ = false;
    bool   demo_ = false;
    double shake_ = 0;
    double match_time_ = 0;
    double round_time_ = 0;                   // 本回合已进行时间
    double sd_timer_ = 0;                     // 僵局拆墙计时
    bool   sudden_death_ = false;
    int    beeps_ = 0, booms_ = 0;
    int    self_kills_ = 0, bank_shots_ = 0;

    friend struct GameTestAccess;
};

}  // namespace t3
