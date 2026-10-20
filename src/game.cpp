// game.cpp —— 坦克动荡的核心逻辑：迷宫生成、物理、子弹反弹、AI、渲染
#include "game.hpp"

#include <cstdio>
#include <cstring>

namespace t3 {

// ============================== 几何工具 ==============================
Hit circle_vs_rect(Vec2 c, double r, const Rect& rc) {
    const double nx = clampd(c.x, rc.x, rc.x + rc.w);
    const double ny = clampd(c.y, rc.y, rc.y + rc.h);
    const double dx = c.x - nx, dy = c.y - ny;
    const double d2 = dx * dx + dy * dy;
    if (d2 > r * r) return {};
    if (d2 > 1e-12) {
        const double d = std::sqrt(d2);
        return {true, dx / d, dy / d, r - d};
    }
    // 圆心落在矩形内部：沿"最小穿透"的那条轴把圆推出去
    const double dxl = c.x - rc.x, dxr = rc.x + rc.w - c.x;
    const double dyt = c.y - rc.y, dyb = rc.y + rc.h - c.y;
    const double mx = std::min(dxl, dxr), my = std::min(dyt, dyb);
    if (mx < my) return {true, dxl < dxr ? -1.0 : 1.0, 0.0, mx + r};
    return {true, 0.0, dyt < dyb ? -1.0 : 1.0, my + r};
}

namespace {
constexpr double kTankR   = 13.0;   // 坦克半径
constexpr double kBulletR = 4.0;    // 子弹半径
constexpr double kTurnRate = 3.3;   // 车体转向角速度 (rad/s)
constexpr double kMaxBullets = 5;   // 同时最多 5 发（和原作一致）
constexpr double kFireCd = 0.26;    // 开炮冷却
constexpr double kBulletLife = 8.0; // 子弹最长存活时间

const unsigned kColBg      = 0x10141B;
const unsigned kColWall    = 0x2B3A4E;
const unsigned kColWallTop = 0x4A6484;
const unsigned kColP1      = 0x38B6FF;
const unsigned kColP1Dim   = 0x1D6FA5;
const unsigned kColP2      = 0xFF8A3D;
const unsigned kColP2Dim   = 0xA8521B;
const unsigned kColBullet  = 0xFFF2A8;
const unsigned kColText    = 0xE8EEF7;
const unsigned kColTextDim = 0x8FA0B8;

unsigned side_color(Side s) { return s == Side::P1 ? kColP1 : kColP2; }
unsigned side_color_dim(Side s) { return s == Side::P1 ? kColP1Dim : kColP2Dim; }
const char* side_name(Side s) {
    if (s == Side::P1) return "PLAYER 1";
    if (s == Side::P2) return "PLAYER 2";
    return "NOBODY";
}
}  // namespace

// ============================== 构造 / 回合流程 ==============================
Game::Game(const Config& cfg) : cfg_(cfg), rng_(cfg.seed) {
    W_ = cfg_.cols * cfg_.cell;
    H_ = cfg_.rows * cfg_.cell;
    tanks_[0].side = Side::P1;
    tanks_[1].side = Side::P2;
    // 模式：0=双人 1=人机 2=电脑互殴
    tanks_[0].ai = (cfg_.mode == 2);
    tanks_[1].ai = (cfg_.mode >= 1);
    if (cfg_.mode == 2) demo_ = true;
    new_round();
    phase_ = Phase::Intro;
    timer_ = demo_ ? 0.8 : 2.0;
    round_ = 1;
}

void Game::request_restart() {
    score_[1] = score_[2] = 0;
    match_time_ = 0;
    round_ = 1;
    phase_ = Phase::Intro;
    timer_ = 2.0;
    last_winner_ = Side::Nobody;
    particles_.clear();
    new_round();
}

void Game::new_round() {
    generate_maze();
    place_tanks();
    bullets_.clear();
    particles_.clear();
    for (auto& t : tanks_) {
        t.alive = true;
        t.live_bullets = 0;
        t.cooldown = 0;
        t.invuln = 0.35;          // 出生短暂保护，避免同归于尽式的开局
        t.drive = t.turn = 0;
        t.want_fire = false;
    }
    for (auto& b : brain_) b = AiBrain{};
    round_time_ = 0;
    sd_timer_ = 0;
    sudden_death_ = false;
}

namespace {
// 从 (sx,sy) 做一次 BFS，返回"可达空地 / 全部空地"的比例
double connected_ratio(const std::vector<uint8_t>& cells, int C, int R, int sx, int sy) {
    std::vector<uint8_t> vis((size_t)C * R, 0);
    std::vector<int> stack;
    stack.push_back(sy * C + sx);
    vis[(size_t)sy * C + sx] = 1;
    int reached = 1, total = 0;
    for (size_t i = 0; i < cells.size(); ++i) if (!cells[i]) ++total;
    while (!stack.empty()) {
        const int cur = stack.back();
        stack.pop_back();
        const int x = cur % C, y = cur / C;
        const int dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
        for (int d = 0; d < 4; ++d) {
            const int nx = x + dx[d], ny = y + dy[d];
            if (nx < 0 || ny < 0 || nx >= C || ny >= R) continue;
            const int ni = ny * C + nx;
            if (cells[(size_t)ni] || vis[(size_t)ni]) continue;
            vis[(size_t)ni] = 1;
            ++reached;
            stack.push_back(ni);
        }
    }
    return total ? double(reached) / total : 1.0;
}
}  // namespace

// ---------------------------------------------------------------- 迷宫生成
// 用"随机障碍 + 180° 旋转对称 + 连通性拒绝采样"三步走：
//   * 180° 对称保证两名玩家的地形难度完全一致（公平性）；
//   * 拒绝采样保证出生点到全图任意空地都连通（不会把玩家关死）；
//   * 若 200 次都没生成出合格地图，就退回全空场地（一定连通）。
void Game::generate_maze() {
    const int C = cfg_.cols, R = cfg_.rows;
    std::uniform_real_distribution<double> prob(0.0, 1.0);
    const double wall_p = 0.17;
    std::vector<uint8_t> best;
    double best_ratio = -1;

    for (int attempt = 0; attempt < 200; ++attempt) {
        cells_.assign((size_t)C * R, 0);
        for (int y = 0; y < R; ++y)
            for (int x = 0; x < C; ++x)
                if (x == 0 || y == 0 || x == C - 1 || y == R - 1)
                    cells_[(size_t)y * C + x] = 1;                 // 外框
        // 只生成上半部分，再 180° 复制到下半部分（对角复制，保证镜像对称）
        for (int y = 1; y < R - 1; ++y)
            for (int x = 1; x < C - 1; ++x) {
                if (prob(rng_) < wall_p) {
                    cells_[(size_t)y * C + x] = 1;
                    cells_[(size_t)(R - 1 - y) * C + (C - 1 - x)] = 1;
                }
            }
        // 清空出生点周围的 3x3 空地
        const int s1x = 2, s1y = R - 3;
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                const int x = s1x + dx, y = s1y + dy;
                if (x > 0 && y > 0 && x < C - 1 && y < R - 1) cells_[(size_t)y * C + x] = 0;
                const int mx = C - 1 - x, my = R - 1 - y;
                if (mx > 0 && my > 0 && mx < C - 1 && my < R - 1)
                    cells_[(size_t)my * C + mx] = 0;
            }
        const double ratio = maze_connected();
        if (ratio > best_ratio) { best_ratio = ratio; best = cells_; }
        if (ratio > 0.999) return;                                  // 合格地图
    }
    cells_ = best;
    if (best_ratio < 0.99) {                                        // 兜底：拆掉内部墙
        for (int y = 1; y < R - 1; ++y)
            for (int x = 1; x < C - 1; ++x) cells_[(size_t)y * C + x] = 0;
    }
}

// 从 P1 出生点做一次 BFS，返回"可达空地 / 全部空地"的比例
double Game::maze_connected() const {
    return connected_ratio(cells_, cfg_.cols, cfg_.rows, 2, cfg_.rows - 3);
}

void Game::place_tanks() {
    const int C = cfg_.cols, R = cfg_.rows;
    const double cs = cfg_.cell;
    // 出生点：左上角与右下角，互为 180° 镜像
    tanks_[0].pos = {(2 + 0.5) * cs, (R - 3 + 0.5) * cs};
    tanks_[1].pos = {(C - 3 + 0.5) * cs, (2 + 0.5) * cs};
    tanks_[0].ang = -M_PI / 2.0;                 // 朝上
    tanks_[1].ang = M_PI / 2.0;                  // 朝下
}

// ============================== 每帧推进 ==============================
void Game::update(double dt) {
    // 粒子与屏幕震动在任何阶段都继续演进，视觉上更连贯
    for (auto& p : particles_) {
        p.pos = p.pos + p.vel * dt;
        p.vel = p.vel * (1.0 - 3.0 * dt);
        p.life -= dt;
    }
    particles_.erase(std::remove_if(particles_.begin(), particles_.end(),
                                    [](const Particle& p) { return p.life <= 0; }),
                     particles_.end());
    shake_ = std::max(0.0, shake_ - dt * 26.0);

    switch (phase_) {
    case Phase::Intro:
        timer_ -= dt;
        if (timer_ <= 0) { phase_ = Phase::Playing; }
        break;
    case Phase::Playing: {
        match_time_ += dt;
        round_time_ += dt;
        // ---- 僵局处理（Sudden Death）：40 秒还没分出胜负就开始逐步拆墙 ----
        if (round_time_ > 40.0) {
            sudden_death_ = true;
            sd_timer_ -= dt;
            if (sd_timer_ <= 0) { sd_timer_ = 1.2; collapse_walls(2); }
        }
        for (auto& t : tanks_) {
            if (!t.alive) continue;
            if (t.ai) update_ai(t, tanks_[t.side == Side::P1 ? 1 : 0], dt);
            update_tank(t, dt);
        }
        update_bullets(dt);
        break;
    }
    case Phase::RoundOver:
        timer_ -= dt;
        if (timer_ <= 0) {
            if (score_[1] >= cfg_.wins_needed || score_[2] >= cfg_.wins_needed) {
                phase_ = Phase::MatchOver;
            } else {
                ++round_;
                new_round();
                phase_ = Phase::Intro;
                timer_ = demo_ ? 0.7 : 1.6;
            }
        }
        break;
    case Phase::MatchOver:
        if (!tanks_[0].ai || !tanks_[1].ai) { /* 等待玩家按 R */ }
        break;
    }
}

void Game::set_input(int player, double drive, double turn, bool fire) {
    if (player < 0 || player > 1) return;
    Tank& t = tanks_[player];
    if (t.ai) return;                      // AI 控制的坦克不接受键盘
    t.drive = clampd(drive, -1.0, 1.0);
    t.turn = clampd(turn, -1.0, 1.0);
    t.want_fire = fire;
}

// 僵局时按 180° 对称一次拆掉几对内部墙，场地越打越开阔，保证回合一定会结束
void Game::collapse_walls(int pairs) {
    const int C = cfg_.cols, R = cfg_.rows;
    for (int i = 0; i < pairs; ++i) {
        for (int tries = 0; tries < 30; ++tries) {
            const int x = 1 + (int)(rng_() % (unsigned)std::max(1, C - 2));
            const int y = 1 + (int)(rng_() % (unsigned)std::max(1, R - 2));
            if (!cells_[(size_t)y * C + x]) continue;
            const int mx = C - 1 - x, my = R - 1 - y;
            cells_[(size_t)y * C + x] = 0;
            if (mx > 0 && my > 0 && mx < C - 1 && my < R - 1) cells_[(size_t)my * C + mx] = 0;
            // 拆墙特效
            for (int k = 0; k < 14; ++k) {
                Particle p;
                p.pos = {(x + 0.5) * cfg_.cell, (y + 0.5) * cfg_.cell};
                const double a = (rng_() % 6283) / 1000.0;
                const double sp = 40 + rng_() % 140;
                p.vel = {std::cos(a) * sp, std::sin(a) * sp};
                p.life = p.max_life = 0.3 + (rng_() % 40) / 100.0;
                p.size = 3;
                p.color = kColWallTop;
                particles_.push_back(p);
            }
            break;
        }
    }
    add_screen_shake(3.0);
}

// ============================== 坦克 ==============================
void Game::update_tank(Tank& t, double dt) {
    t.cooldown = std::max(0.0, t.cooldown - dt);
    t.invuln = std::max(0.0, t.invuln - dt);

    // 转向
    t.ang += t.turn * kTurnRate * dt;
    while (t.ang > M_PI) t.ang -= 2 * M_PI;
    while (t.ang < -M_PI) t.ang += 2 * M_PI;

    // 前进 / 后退
    if (t.drive != 0) {
        const Vec2 dir{std::cos(t.ang), std::sin(t.ang)};
        const Vec2 delta = dir * (t.drive * cfg_.tank_speed * dt);
        Vec2 np = t.pos + delta;
        // 与墙做"推出式"碰撞解算：最多迭 4 次，处理墙角同时接触两堵墙的情况
        const int cx = (int)(np.x / cfg_.cell), cy = (int)(np.y / cfg_.cell);
        for (int iter = 0; iter < 4; ++iter) {
            bool any = false;
            for (int y = cy - 1; y <= cy + 1; ++y)
                for (int x = cx - 1; x <= cx + 1; ++x) {
                    if (x < 0 || y < 0 || x >= cfg_.cols || y >= cfg_.rows) continue;
                    if (!cells_[(size_t)y * cfg_.cols + x]) continue;
                    const Rect rc{(double)(x * cfg_.cell), (double)(y * cfg_.cell),
                                  (double)cfg_.cell, (double)cfg_.cell};
                    const Hit h = circle_vs_rect(np, kTankR, rc);
                    if (h.hit) {
                        np.x += h.nx * h.depth;
                        np.y += h.ny * h.depth;
                        any = true;
                    }
                }
            if (!any) break;
        }
        // 完全跑出场地也拉回来（正常情况下不会发生）
        np.x = clampd(np.x, kTankR, W_ - kTankR);
        np.y = clampd(np.y, kTankR, H_ - kTankR);
        t.pos = np;
    }

    // 开炮
    if (t.want_fire && t.cooldown <= 0 && t.live_bullets < kMaxBullets) {
        spawn_bullet(t);
        t.cooldown = kFireCd;
    }
    t.want_fire = false;                    // 单发触发，需要持续按住要每帧重新置位
}

void Game::spawn_bullet(Tank& t) {
    const Vec2 dir{std::cos(t.ang), std::sin(t.ang)};
    Bullet b;
    b.pos = t.pos + dir * (kTankR + kBulletR + 2.0);
    b.vel = dir * cfg_.bullet_speed;
    b.owner = t.side;
    b.trail.push_back(b.pos);
    bullets_.push_back(b);
    ++t.live_bullets;
    if (cfg_.sound) ++beeps_;
    // 炮口火花
    for (int i = 0; i < 6; ++i) {
        Particle p;
        p.pos = b.pos;
        const double a = t.ang + (rng_() % 100 - 50) / 100.0 * 0.8;
        const double sp = 60 + rng_() % 90;
        p.vel = {std::cos(a) * sp, std::sin(a) * sp};
        p.life = p.max_life = 0.12 + (rng_() % 10) / 100.0;
        p.size = 2;
        p.color = kColBullet;
        particles_.push_back(p);
    }
}

int Game::take_beeps() { const int n = beeps_; beeps_ = 0; return n; }
int Game::take_booms() { const int n = booms_; booms_ = 0; return n; }

void Game::explode(Vec2 pos, unsigned color, int count, double power) {
    for (int i = 0; i < count; ++i) {
        Particle p;
        p.pos = pos;
        const double a = (rng_() % 6283) / 1000.0;
        const double sp = power * (0.25 + (rng_() % 1000) / 1000.0);
        p.vel = {std::cos(a) * sp, std::sin(a) * sp};
        p.life = p.max_life = 0.35 + (rng_() % 90) / 100.0;
        p.size = 1 + rng_() % 4;
        p.color = (i % 3 == 0) ? 0xFFFFFF : ((i % 3 == 1) ? color : 0xFFC24D);
        particles_.push_back(p);
    }
    add_screen_shake(std::min(9.0, power * 0.07));
}

void Game::kill_tank(Tank& t) {
    t.alive = false;
    if (cfg_.sound) ++booms_;
    ++t.deaths;
    explode(t.pos, side_color(t.side), 46, 260);
    // 死亡冲击环
    for (int i = 0; i < 26; ++i) {
        Particle p;
        const double a = i / 26.0 * 2 * M_PI;
        p.pos = t.pos;
        p.vel = {std::cos(a) * 190, std::sin(a) * 190};
        p.life = p.max_life = 0.35;
        p.size = 3;
        p.color = side_color(t.side);
        p.drag = 0.8;
        particles_.push_back(p);
    }
}

// ============================== 子弹 ==============================
void Game::update_bullets(double dt) {
    bool died[3] = {false, false, false};
    for (auto& b : bullets_) {
        if (!b.alive) continue;
        b.age += dt;
        const double speed = len(b.vel);
        // 把这一步切成若干小步，避免高速子弹"穿过薄墙"（隧道效应）
        const int substeps = std::max(1, (int)std::ceil(speed * dt / 2.5));
        const double h = dt / substeps;
        for (int s = 0; s < substeps && b.alive; ++s) {
            b.pos = b.pos + b.vel * h;
            // ---- 与墙碰撞：沿法线做镜面反射 v' = v - 2(v·n)n ----
            const int cx = (int)(b.pos.x / cfg_.cell), cy = (int)(b.pos.y / cfg_.cell);
            for (int iter = 0; iter < 3; ++iter) {
                bool bounced = false;
                for (int y = cy - 1; y <= cy + 1 && !bounced; ++y)
                    for (int x = cx - 1; x <= cx + 1 && !bounced; ++x) {
                        if (x < 0 || y < 0 || x >= cfg_.cols || y >= cfg_.rows) continue;
                        if (!cells_[(size_t)y * cfg_.cols + x]) continue;
                        const Rect rc{(double)(x * cfg_.cell), (double)(y * cfg_.cell),
                                      (double)cfg_.cell, (double)cfg_.cell};
                        const Hit hit = circle_vs_rect(b.pos, kBulletR, rc);
                        if (!hit.hit) continue;
                        const Vec2 n{hit.nx, hit.ny};
                        const double vn = dot(b.vel, n);
                        b.vel = b.vel - n * (2.0 * vn);
                        b.pos = b.pos + n * (hit.depth + 0.01);
                        ++b.bounces;
                        bounced = true;
                        // 撞击火花
                        for (int i = 0; i < 3; ++i) {
                            Particle p;
                            p.pos = b.pos;
                            const double a = std::atan2(-n.y, -n.x) + (rng_() % 100 - 50) / 250.0;
                            p.vel = {std::cos(a) * (40 + rng_() % 60), std::sin(a) * (40 + rng_() % 60)};
                            p.life = p.max_life = 0.16;
                            p.size = 2;
                            p.color = kColBullet;
                            particles_.push_back(p);
                        }
                        if (b.bounces > cfg_.max_bounces) { b.alive = false; }
                    }
                if (!bounced) break;
            }
            if (!b.alive) break;

            // ---- 与坦克碰撞：圆-圆判定；自己的子弹同样能打死自己（原作精髓）----
            for (auto& t : tanks_) {
                if (!t.alive || t.invuln > 0) continue;
                const Vec2 d = b.pos - t.pos;
                if (dot(d, d) > (kTankR + kBulletR) * (kTankR + kBulletR)) continue;
                if (b.owner == t.side) ++self_kills_;   // 被自己的反弹球打死
                kill_tank(t);
                b.alive = false;
                died[(int)t.side] = true;
                if (cfg_.sound) { /* 音效由上层播放 */ }
                break;
            }
        }
        if (b.alive) {
            if (b.trail.empty() || len(b.pos - b.trail.back()) > 6.0) b.trail.push_back(b.pos);
            if (b.trail.size() > 16) b.trail.erase(b.trail.begin());
        }
        if (b.age > kBulletLife) b.alive = false;
    }

    // 回收子弹并更新每辆坦克的在场弹数
    for (size_t i = 0; i < bullets_.size();) {
        if (!bullets_[i].alive) {
            for (auto& t : tanks_)
                if (t.side == bullets_[i].owner) t.live_bullets = std::max(0, t.live_bullets - 1);
            bullets_.erase(bullets_.begin() + (long)i);
        } else {
            ++i;
        }
    }

    // ---- 结算本回合 ----
    const bool d1 = died[1], d2 = died[2];
    if (d1 || d2) {
        Side winner = Side::Nobody;
        if (d1 && !d2) winner = Side::P2;
        else if (d2 && !d1) winner = Side::P1;
        if (winner != Side::Nobody) {
            ++score_[(int)winner];
            last_winner_ = winner;
        } else {
            last_winner_ = Side::Nobody;      // 同归于尽 -> 本回合平局，不计分
        }
        phase_ = Phase::RoundOver;
        timer_ = demo_ ? 0.9 : 1.9;
    }
}

// ============================== 电脑玩家 ==============================
// 决策周期 + 比例控制 + 简单的"预判瞄准"和"躲子弹"，难度由 ai_level 决定。

// ============================== AI 辅助算法 ==============================
// 网格 BFS：从 (sx,sy) 出发，算出每个空格到它的步数（-1 表示墙或不可达）
void Game::bfs_from(int sx, int sy, std::vector<int>& dist) const {
    const int C = cfg_.cols, R = cfg_.rows;
    dist.assign((size_t)C * R, -1);
    if (sx < 0 || sy < 0 || sx >= C || sy >= R) return;
    if (cells_[(size_t)sy * C + sx]) return;
    std::vector<int> q;
    q.reserve((size_t)C * R);
    dist[(size_t)sy * C + sx] = 0;
    q.push_back(sy * C + sx);
    for (size_t h = 0; h < q.size(); ++h) {
        const int cur = q[h], x = cur % C, y = cur / C;
        const int dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
        for (int d = 0; d < 4; ++d) {
            const int nx = x + dx[d], ny = y + dy[d];
            if (nx < 0 || ny < 0 || nx >= C || ny >= R) continue;
            const int ni = ny * C + nx;
            if (cells_[(size_t)ni] || dist[(size_t)ni] >= 0) continue;
            dist[(size_t)ni] = dist[(size_t)cur] + 1;
            q.push_back(ni);
        }
    }
}

// 射线：撞墙做镜面反射（最多 max_bounces 次），若与 target 的距离 ≤ hit_r，
// 返回累计走过的路程长度；否则返回 -1。用来做"反弹球能不能打到人"的判断。
double Game::raycast(Vec2 origin, Vec2 dir, int max_bounces, Vec2 target,
                     double hit_r, double max_len, int* used_bounces) const {
    const int C = cfg_.cols, R = cfg_.rows;
    const double cs = cfg_.cell;
    const double step = std::max(2.0, cs * 0.1);
    Vec2 v = norm(dir);
    Vec2 p = origin;
    int bounces = 0;
    double travelled = 0;
    const int max_steps = (int)(max_len / step) + 2;
    auto solid = [&](int x, int y) {
        if (x < 0 || y < 0 || x >= C || y >= R) return true;
        return cells_[(size_t)y * C + x] != 0;
    };
    if (solid((int)(p.x / cs), (int)(p.y / cs))) return -1;   // 起点在墙里
    for (int i = 0; i < max_steps; ++i) {
        if (len(p - target) <= hit_r) { if (used_bounces) *used_bounces = bounces; return travelled; }
        const Vec2 np = p + v * step;
        travelled += step;
        if (travelled > max_len) break;
        const int cx = (int)(np.x / cs), cy = (int)(np.y / cs);
        if (!solid(cx, cy)) { p = np; continue; }
        // 撞墙：分轴判断是哪一面（支持墙角），然后镜像反射
        if (bounces >= max_bounces) break;
        const int px = (int)(p.x / cs), py = (int)(p.y / cs);
        const bool hit_x = (cx != px) && solid(cx, py);
        const bool hit_y = (cy != py) && solid(px, cy);
        if (hit_x) v.x = -v.x;
        if (hit_y) v.y = -v.y;
        if (!hit_x && !hit_y) { v.x = -v.x; v.y = -v.y; }     // 正对墙角
        ++bounces;
        // p 保持不动（它仍在空地上），下一轮沿反射后的方向重新前进
        travelled -= step;
    }
    return -1;
}

void Game::update_ai(Tank& me, const Tank& foe, double dt) {
    AiBrain& br = brain_[me.side == Side::P1 ? 0 : 1];
    br.think -= dt;
    br.strafe_t -= dt;
    br.fire_delay -= dt;
    br.aim_scan_t -= dt;

    // 难度决定"启用哪些智能行为"，同时决定误差与反应时间
    const int lvl = std::max(1, std::min(3, cfg_.ai_level));
    const bool use_lead = lvl >= 2;      // 真预判
    const bool use_path = lvl >= 2;      // BFS 寻路
    const bool use_self = lvl >= 2;      // 自伤风险检查
    const bool use_bank = lvl >= 3;      // 反弹射线
    const double aim_noise = 0.22 - 0.06 * (lvl - 1);   // 0.22 / 0.16 / 0.10
    const double reaction  = 0.50 - 0.12 * (lvl - 1);   // 0.50 / 0.38 / 0.26

    // --- 0) 用最近几帧的位移估计对手速度（真预判的数据来源）---
    if (!br.foe_init) { br.foe_last = foe.pos; br.foe_init = true; }
    br.foe_sample_t += dt;
    if (br.foe_sample_t >= 0.10) {
        Vec2 v = (foe.pos - br.foe_last) * (1.0 / br.foe_sample_t);
        if (len(v) > cfg_.tank_speed * 1.5) v = norm(v) * (cfg_.tank_speed * 1.5);
        br.foe_vel = br.foe_vel * 0.5 + v * 0.5;          // 一阶平滑，避免抖动
        br.foe_last = foe.pos;
        br.foe_sample_t = 0;
    }

    // --- 1) 防卡死：每 0.35 秒检查一次"想走却几乎没动"，连续两次就强制脱困 0.7 秒 ---
    br.move_check_t -= dt;
    if (br.move_check_t <= 0) {
        const double moved = len(me.pos - br.last_pos);
        if (me.drive != 0 && moved < 2.0) {
            if (++br.stuck >= 2) {
                br.unstick_t = 0.7;
                br.unstick_dir = (rng_() % 2) ? 1.0 : -1.0;
                br.stuck = 0;
            }
        } else {
            br.stuck = 0;
        }
        br.last_pos = me.pos;
        br.move_check_t = 0.35;
    }

    // --- 2) 躲子弹：预测每颗来袭炮弹未来 0.45 秒内与我的最近距离 ---
    bool dodge = false;
    Vec2 dodge_dir{0, 0};
    for (const auto& b : bullets_) {
        if (!b.alive || b.owner == me.side) continue;
        const Vec2 rel = me.pos - b.pos;
        const double t = clampd(dot(rel, b.vel) / std::max(1.0, dot(b.vel, b.vel)), 0.0, 0.45);
        const Vec2 closest = b.pos + b.vel * t;
        const Vec2 miss = me.pos - closest;
        if (len(miss) < 46.0 && t < 0.45) {
            dodge = true;
            const Vec2 n = norm(Vec2{-b.vel.y, b.vel.x});
            const Vec2 fwd{std::cos(me.ang), std::sin(me.ang)};
            dodge_dir = (dot(n, miss) > 0) ? n : Vec2{-n.x, -n.y};
            if (dot(dodge_dir, fwd) < 0) dodge_dir = Vec2{-dodge_dir.x, -dodge_dir.y};
        }
    }

    // --- 3) 周期性重掷：瞄准误差与绕行方向（像人的反应时间，而不是每帧抖动）---
    if (br.think <= 0) {
        br.think = reaction * (0.6 + (rng_() % 100) / 100.0);
        br.aim_err = ((double)(rng_() % 2001) / 1000.0 - 1.0) * aim_noise;
        if (br.strafe_t <= 0) {
            br.strafe_t = 0.5 + (rng_() % 120) / 100.0;
            br.strafe_dir = (rng_() % 2) ? 1.0 : -1.0;
        }
    }

    // --- 4) 真预判：按子弹飞行时间把瞄准点外推到对手未来的位置 ---
    const Vec2 to_foe = foe.pos - me.pos;
    const double dist = len(to_foe);
    const double flight = dist / cfg_.bullet_speed;
    Vec2 aim_pt = foe.pos;
    if (use_lead) aim_pt = aim_pt + br.foe_vel * std::min(flight, 0.35);
    const double noise_scale = std::min(1.0, std::max(0.0, flight) * 4.0 + 0.35);
    const double want = std::atan2(aim_pt.y - me.pos.y, aim_pt.x - me.pos.x)
                        + br.aim_err * noise_scale;
    double diff = want - me.ang;
    while (diff > M_PI) diff -= 2 * M_PI;
    while (diff < -M_PI) diff += 2 * M_PI;

    // --- 5) BFS：网格上的"实际距离"和下一步方向（不是直线距离）---
    br.path_cells = -1;
    br.step_dir = Vec2{0, 0};
    if (use_path) {
        const int C = cfg_.cols, R = cfg_.rows;
        const int mx = std::max(0, std::min(C - 1, (int)(me.pos.x / cfg_.cell)));
        const int my = std::max(0, std::min(R - 1, (int)(me.pos.y / cfg_.cell)));
        const int fx = std::max(0, std::min(C - 1, (int)(foe.pos.x / cfg_.cell)));
        const int fy = std::max(0, std::min(R - 1, (int)(foe.pos.y / cfg_.cell)));
        bfs_from(fx, fy, br.dist_grid);
        if (br.dist_grid.size() == (size_t)C * R) {
            br.path_cells = br.dist_grid[(size_t)my * C + mx];
            if (br.path_cells > 0) {
                int best = br.path_cells;
                const int ddx[4] = {1, -1, 0, 0}, ddy[4] = {0, 0, 1, -1};
                for (int d = 0; d < 4; ++d) {
                    const int nx = mx + ddx[d], ny = my + ddy[d];
                    if (nx < 0 || ny < 0 || nx >= C || ny >= R) continue;
                    const int v = br.dist_grid[(size_t)ny * C + nx];
                    if (v >= 0 && v < best) { best = v; br.step_dir = Vec2{(double)ddx[d], (double)ddy[d]}; }
                }
            }
        }
    }

    // --- 6) 反弹射线：看不见对手时，找一条"最多 2 次反弹能命中"的方向 ---
    const bool clear = line_clear(me.pos, foe.pos);
    if (clear) br.bank_ok = false;          // 能直射就不打反弹球了
    if (use_bank && br.aim_scan_t <= 0) {
        br.aim_scan_t = 0.15;
        br.bank_ok = false;
        if (!clear) {
            int best_b = 99;
            double best_turn = 1e9;
            for (int k = 0; k < 48; ++k) {
                const double a = -M_PI + k * (2.0 * M_PI / 48.0);
                const Vec2 d{std::cos(a), std::sin(a)};
                int used = 0;
                const double L = raycast(me.pos + d * (kTankR + 8.0), d, 2, aim_pt,
                                         kTankR + kBulletR, 1400.0, &used);
                if (L < 0) continue;
                double dd = a - me.ang;
                while (dd > M_PI) dd -= 2 * M_PI;
                while (dd < -M_PI) dd += 2 * M_PI;
                dd = std::fabs(dd);
                if (used < best_b || (used == best_b && dd < best_turn)) {
                    best_b = used; best_turn = dd; br.bank_ang = a; br.bank_ok = true;
                }
            }
        }
    }
    if (br.bank_ok) {
        diff = br.bank_ang - me.ang;
        while (diff > M_PI) diff -= 2 * M_PI;
        while (diff < -M_PI) diff += 2 * M_PI;
    }

    // --- 7) 转向：比例控制 + 死区 ---
    me.turn = clampd(diff * 2.6, -1.0, 1.0);
    if (std::fabs(diff) < 0.04) me.turn = 0;

    // --- 8) 移动：躲弹 > 沿 BFS 最短路 > 远推进 / 近后退 / 中距离绕行 ---
    const Vec2 fwd{std::cos(me.ang), std::sin(me.ang)};
    const double eff_dist = (use_path && br.path_cells > 0) ? br.path_cells * cfg_.cell : dist;
    if (dodge) {
        const double d = std::atan2(dodge_dir.y, dodge_dir.x);
        double dd = d - me.ang;
        while (dd > M_PI) dd -= 2 * M_PI;
        while (dd < -M_PI) dd += 2 * M_PI;
        me.turn = clampd(dd * 3.0, -1.0, 1.0);
        me.drive = (std::fabs(dd) < 1.0) ? 1.0 : 0.0;
    } else if (!foe.alive) {
        me.drive = 0;
    } else if (br.bank_ok && std::fabs(diff) < 0.6) {
        me.drive = 0;                       // 已经瞄准反弹点，站定打这一杆
    } else if (use_path && !clear && (br.step_dir.x != 0 || br.step_dir.y != 0)) {
        const double d = std::atan2(br.step_dir.y, br.step_dir.x);
        double dd = d - me.ang;
        while (dd > M_PI) dd -= 2 * M_PI;
        while (dd < -M_PI) dd += 2 * M_PI;
        me.turn = clampd(dd * 3.0, -1.0, 1.0);
        me.drive = (std::fabs(dd) < 1.1) ? 1.0 : 0.0;
    } else if (eff_dist > 300) {
        me.drive = (std::fabs(diff) < 0.9) ? 1.0 : 0.0;
    } else if (eff_dist < 130) {
        me.drive = -0.8;
    } else {
        me.drive = (std::fabs(diff) < 1.2) ? 0.7 : 0.0;
        me.turn = clampd(me.turn + br.strafe_dir * 0.45, -1.0, 1.0);
    }

    // --- 9) 开炮：对准 + 炮口不贴墙 + 反应时间 +（直射 / 近距离盲射 / 反弹命中）+ 不自伤 ---
    const bool aimed = std::fabs(diff) < (0.10 + 0.05 * (2 - lvl));   // 0.15 / 0.10 / 0.05
    const Vec2 muzzle = me.pos + fwd * (kTankR + kBulletR + 3.0);
    const int mx = (int)(muzzle.x / cfg_.cell), my = (int)(muzzle.y / cfg_.cell);
    bool muzzle_blocked = false;
    if (mx < 0 || my < 0 || mx >= cfg_.cols || my >= cfg_.rows) muzzle_blocked = true;
    else if (cells_[(size_t)my * cfg_.cols + mx]) muzzle_blocked = true;

    // 自伤风险：模拟这一发球（含反弹）1 秒内的轨迹，若会扫过自己就放弃
    br.self_harm = false;
    if (use_self && !muzzle_blocked) {
        int used = 0;
        const double L = raycast(muzzle, fwd, cfg_.max_bounces, me.pos,
                                 kTankR + kBulletR, cfg_.bullet_speed * 1.0, &used);
        br.self_harm = (L >= 0);
    }

    const bool may_fire = aimed && !muzzle_blocked && br.fire_delay <= 0 &&
                          (clear || dist < 260 || br.bank_ok) && !br.self_harm;
    if (may_fire && me.live_bullets < kMaxBullets) {
        if (br.bank_ok) ++bank_shots_;
        me.want_fire = true;
        br.fire_delay = reaction * (0.8 + (rng_() % 60) / 100.0);
    }

    // --- 10) 脱困动作优先级最高：原地转向并后退，同时禁火 ---
    if (br.unstick_t > 0) {
        br.unstick_t -= dt;
        me.turn = br.unstick_dir;
        me.drive = -0.6;
        me.want_fire = false;
    }
}

// 射线步进检查两点之间有没有墙（用于 AI 判断"能不能直射"）
bool Game::line_clear(Vec2 a, Vec2 b) const {
    const Vec2 d = b - a;
    const double dist = len(d);
    const int steps = std::max(2, (int)(dist / (cfg_.cell * 0.4)));
    for (int i = 1; i < steps; ++i) {
        const Vec2 p = a + d * ((double)i / steps);
        const int x = (int)(p.x / cfg_.cell), y = (int)(p.y / cfg_.cell);
        if (x < 0 || y < 0 || x >= cfg_.cols || y >= cfg_.rows) return false;
        if (cells_[(size_t)y * cfg_.cols + x]) return false;
    }
    return true;
}

// ============================== 渲染 ==============================
void Game::render(Canvas& cv) {
    // 屏幕震动：整帧做一个小偏移
    int ox = 0, oy = 0;
    if (shake_ > 0.5) {
        ox = (int)((rng_() % 2001) / 1000.0 * shake_) - (int)shake_;
        oy = (int)((rng_() % 2001) / 1000.0 * shake_) - (int)shake_;
    }
    auto R = [&](int x, int y, int w, int h, unsigned c) { cv.rect(x + ox, y + oy, w, h, c); };
    auto C = [&](int x, int y, int r, unsigned c) { cv.circle(x + ox, y + oy, r, c); };
    auto G = [&](int x, int y, int r, int t, unsigned c) { cv.ring(x + ox, y + oy, r, t, c); };
    auto L = [&](int x0, int y0, int x1, int y1, int t, unsigned c) {
        cv.line(x0 + ox, y0 + oy, x1 + ox, y1 + oy, t, c);
    };

    cv.clear(kColBg);

    // ---- 迷宫 ----
    for (int y = 0; y < cfg_.rows; ++y)
        for (int x = 0; x < cfg_.cols; ++x) {
            if (!cells_[(size_t)y * cfg_.cols + x]) continue;
            const int px = x * cfg_.cell, py = y * cfg_.cell;
            R(px, py, cfg_.cell, cfg_.cell, kColWall);
            R(px, py, cfg_.cell, 3, kColWallTop);                  // 顶面高光，做出一点立体感
            R(px + cfg_.cell - 2, py, 2, cfg_.cell, 0x22303F);
            R(px, py + cfg_.cell - 2, cfg_.cell, 2, 0x22303F);
        }

    // ---- 子弹拖尾与本体 ----
    for (const auto& b : bullets_) {
        const unsigned col = side_color(b.owner);
        for (size_t i = 1; i < b.trail.size(); ++i) {
            const double f = (double)i / (double)b.trail.size();
            const int r = (int)(1 + 3 * f);
            C((int)b.trail[i].x, (int)b.trail[i].y, r, (unsigned)((col & 0xFEFEFE) >> 1));
        }
        C((int)b.pos.x, (int)b.pos.y, (int)kBulletR + 3, (col & 0x7F7F7F));
        C((int)b.pos.x, (int)b.pos.y, (int)kBulletR, kColBullet);
    }

    // ---- 坦克 ----
    for (const auto& t : tanks_) {
        if (!t.alive) continue;
        const int px = (int)t.pos.x, py = (int)t.pos.y;
        const unsigned col = side_color(t.side), dim = side_color_dim(t.side);
        // 履带（沿朝向的两条短粗线）
        const Vec2 fwd{std::cos(t.ang), std::sin(t.ang)};
        const Vec2 side{-fwd.y, fwd.x};
        for (int s = -1; s <= 1; s += 2) {
            const Vec2 a = t.pos + side * (s * 10.5) - fwd * 11;
            const Vec2 b2 = t.pos + side * (s * 10.5) + fwd * 11;
            L((int)a.x, (int)a.y, (int)b2.x, (int)b2.y, 6, dim);
        }
        C(px, py, (int)kTankR, col);                    // 车体
        G(px, py, (int)kTankR, 3, 0x0B0F14);            // 描边
        // 炮管
        const Vec2 m0 = t.pos + fwd * 3.0;
        const Vec2 m1 = t.pos + fwd * (kTankR + 8.0);
        L((int)m0.x, (int)m0.y, (int)m1.x, (int)m1.y, 6, 0x0B0F14);
        L((int)m0.x, (int)m0.y, (int)m1.x, (int)m1.y, 3, 0xE8EEF7);
        C(px, py, 6, 0xE8EEF7);                         // 炮塔中心
        C(px, py, 3, dim);
        if (t.invuln > 0) {                             // 出生保护闪烁
            G(px, py, (int)kTankR + 5, 2, 0xFFFFFF);
        }
    }

    // ---- 粒子 ----
    for (const auto& p : particles_) {
        const double f = clampd(p.life / p.max_life, 0.0, 1.0);
        const int s = std::max(1, (int)(p.size * (0.4 + 0.6 * f)));
        R((int)p.pos.x - s / 2, (int)p.pos.y - s / 2, s, s, p.color);
    }

    // ---- HUD：比分 ----
    const int top = 12;
    for (int i = 0; i < 2; ++i) {
        const Side s = i == 0 ? Side::P1 : Side::P2;
        const char* label = (cfg_.mode == 2) ? (i == 0 ? "AI-1" : "AI-2")
                           : (cfg_.mode == 1 && i == 1) ? "CPU" : side_name(s);
        const int block = cv.text_width(label, 2) + 12 + cfg_.wins_needed * 22;
        const int x = (i == 0) ? 16 : W_ - 18 - block;
        cv.text(x, top, label, 2, side_color(s));
        for (int k = 0; k < cfg_.wins_needed; ++k) {
            const int cx = x + cv.text_width(label, 2) + 12 + k * 22;
            if (score_[(int)s] > k) C(cx, top + 7, 7, side_color(s));
            else G(cx, top + 7, 7, 2, side_color_dim(s));
        }
    }
    {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "ROUND %d", round_);
        cv.text(W_ / 2 - cv.text_width(buf, 2) / 2, top, buf, 2, kColTextDim);
        if (sudden_death_) {
            const char* sd = "SUDDEN DEATH";
            cv.text(W_ / 2 - cv.text_width(sd, 1) / 2, top + 20, sd, 1, 0xFF6B6B);
        }
        const char* hint = "WASD+SPACE  /  ARROWS+ENTER   P=PAUSE  R=RESTART";
        cv.text(W_ / 2 - cv.text_width(hint, 1) / 2, H_ - 16, hint, 1, 0x4A5A70);
    }

    // ---- 阶段覆盖层 ----
    if (phase_ == Phase::Intro) {
        const int secs = (int)std::ceil(timer_);
        const char* big = (timer_ > 1.0) ? "READY" : (secs == 1 ? "GO!" : "SET");
        const int sc = 5;
        cv.text(W_ / 2 - cv.text_width(big, sc) / 2, H_ / 2 - 60, big, sc, kColText);
        char buf[64];
        std::snprintf(buf, sizeof(buf), "ROUND %d", round_);
        cv.text(W_ / 2 - cv.text_width(buf, 3) / 2, H_ / 2 + 10, buf, 3, kColTextDim);
    } else if (phase_ == Phase::RoundOver) {
        char buf[64];
        if (last_winner_ == Side::Nobody) std::snprintf(buf, sizeof(buf), "DOUBLE KO");
        else std::snprintf(buf, sizeof(buf), "%s SCORES", side_name(last_winner_));
        const int sc = 3;
        cv.text(W_ / 2 - cv.text_width(buf, sc) / 2, H_ / 2 - 30, buf, sc,
                last_winner_ == Side::Nobody ? kColTextDim : side_color(last_winner_));
    } else if (phase_ == Phase::MatchOver) {
        const Side w = score_[1] >= cfg_.wins_needed ? Side::P1 : Side::P2;
        const char* who = (cfg_.mode == 2) ? (w == Side::P1 ? "AI-1" : "AI-2")
                        : (cfg_.mode == 1 && w == Side::P2) ? "CPU" : side_name(w);
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%s WINS!", who);
        cv.text(W_ / 2 - cv.text_width(buf, 5) / 2, H_ / 2 - 70, buf, 5, side_color(w));
        const char* hint = "PRESS R TO PLAY AGAIN   ESC TO QUIT";
        cv.text(W_ / 2 - cv.text_width(hint, 2) / 2, H_ / 2 + 10, hint, 2, kColTextDim);
        std::snprintf(buf, sizeof(buf), "FINAL SCORE  %d : %d", score_[1], score_[2]);
        cv.text(W_ / 2 - cv.text_width(buf, 2) / 2, H_ / 2 + 40, buf, 2, kColText);
    }
}

}  // namespace t3
