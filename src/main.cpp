// Geode Autoplay v0.5 - tüm modlar, orb/pad/portal/slope, 30 yörünge beam search
//
// Fizik kaynakları (GD 2.2081 decompile'larından türetilmiş, topluluk projeleri):
//  - 240 Hz adım, hız birimi: 1 birim = 0.225 koordinat/adım (54 u/s)
//  - küp zıplama 11.18, yerçekimi 0.958199/adım, terminal hız 15
//  - ball/spider yerçekimi x0.6, robot x0.9, swing x0.4, mini zıplama x0.8
//  - ship limit +8/-6.4, ufo zıplama 7, wave 45 derece (mini 2:1)
//  - hitbox: oyuncu 30 (mini 18), wave 10, spider 27; iç hitbox 0.3x;
//    diken dış hitbox ile öldürür; blok iniş toleransı 10 (uçan modlar 6)
// Değerler oyundan CANLI ölçülerek kalibre edilir (yerçekimi, zıplama, ship ivmesi, hız).
//
// Planlama: her ~8 adımda beam search (K yörünge) -> en güvenli giriş planı seçilir.
// Plan her adımda gerçek durumdan yeniden doğrulanır; bozulursa anında yeniden aranır.

#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

using namespace geode::prelude;

namespace {
    constexpr float K       = 0.225f;       // hız birimi -> koordinat / adım
    constexpr float G0      = 0.958199f;
    constexpr float kStepDt = 1.f / 240.f;
    constexpr float kInf    = 1e9f;

    enum Mode : int { M_CUBE = 0, M_SHIP, M_BALL, M_UFO, M_WAVE, M_ROBOT, M_SPIDER, M_SWING, M_COUNT };
    enum class Kind : uint8_t { Hazard, Solid, Slope, Pad, Ring, Portal };
    enum Eff : uint8_t { E_YELLOW, E_PINK, E_RED, E_BLUE, E_GREEN, E_BLACK };
    enum PortalT : uint8_t { PT_MODE, PT_GRAV_N, PT_GRAV_F, PT_GRAV_T, PT_MINI, PT_BIG, PT_SPEED };

    struct Obj {
        float x0 = 0, x1 = 0, y0 = 0, y1 = 0;
        Kind kind = Kind::Solid;
        uint8_t sub = 0;      // pad/ring: Eff, portal: PortalT, slope: 1 = katı altta
        uint8_t mode = 0;     // mod portalı hedefi
        int16_t orb = -1;     // "bir kez tetiklendi" bit indeksi
        float lx = 0, ly = 0, rx = 1, ry = 0; // slope yüzey çizgisi (soldan sağa)
        float val = 0;        // hız portalı
    };

    struct Cfg {
        float gMult[M_COUNT] = {1.f, 0.4f, 0.6f, 0.5f, 0.f, 0.9f, 0.6f, 0.4f};
        float jumpV  = 11.18f;
        float ufoV   = 7.f;
        // [0] basılı&u>=0, [1] basılı&u<0, [2] bırakık&u>0, [3] bırakık&u<=0
        float shipAcc[4] = {0.4f * G0 * K, 0.5f * G0 * K, 1.2f * 0.4f * G0 * K, 0.8f * 0.4f * G0 * K};
        float shipUp = 8.f, shipDown = 6.4f;
        float groundB = 90.f;
    };

    struct S {
        float x = 0, y = 0, vy = 0, vx = 311.58f;
        float g = 1.f;               // +1 normal, -1 ters yerçekimi
        float B = 90.f, T = kInf;    // mod alanı alt/üst sınırı
        float gb = 90.f;             // zemin
        float slope = 0.f;
        int8_t mode = 0;
        int16_t robotBoost = 0;
        bool mini = false, grounded = true, held = false, prevHeld = false, onSlope = false;
        uint64_t used[2] = {0, 0};
    };

    inline bool flyingMode(int m) { return m == M_SHIP || m == M_UFO || m == M_WAVE || m == M_SWING; }
    inline float modeHeight(int m) {
        if (m == M_SHIP || m == M_UFO || m == M_WAVE || m == M_SWING) return 300.f;
        if (m == M_BALL || m == M_SPIDER) return 240.f;
        return kInf;
    }
    inline float halfOf(const S& s) {
        float h = s.mode == M_WAVE ? 5.f : (s.mode == M_SPIDER ? 13.5f : 15.f);
        return s.mini ? h * 0.6f : h;
    }
    inline bool ovr(float x, float y, float h, const Obj& o, float pad) {
        return x - h < o.x1 + pad && x + h > o.x0 - pad && y - h < o.y1 + pad && y + h > o.y0 - pad;
    }
    inline bool isUsed(const S& s, int i) { return i >= 0 && ((s.used[i >> 6] >> (i & 63)) & 1ull); }
    inline void setUsed(S& s, int i) { if (i >= 0) s.used[i >> 6] |= (1ull << (i & 63)); }

    void areaFromCenter(S& s, int mode, float pc) {
        float H = modeHeight(mode);
        if (H >= kInf) { s.B = s.gb; s.T = kInf; return; }
        s.B = std::max(s.gb, s.gb + 30.f * std::floor((pc - s.gb - H * 0.5f) / 30.f + 0.5f));
        s.T = s.B + H;
        if (s.y - 15.f < s.B) s.B = s.y - 15.f;
        if (s.y + 15.f > s.T) s.T = s.y + 15.f;
    }

    // Orb / pad etkisi (hız birimi: oyunun native birimi)
    void orbFx(S& s, uint8_t eff, float jv, bool pad) {
        const float base = pad ? 16.f : jv;
        if (s.mode == M_WAVE) {                       // wave: sadece yerçekimi değişir
            if (eff == E_BLUE || eff == E_GREEN) s.g = -s.g;
            return;
        }
        const float mm = (s.mode == M_BALL || s.mode == M_SPIDER) ? 0.6f : 1.f;
        switch (eff) {
            case E_YELLOW: s.vy = s.g * base * mm; break;
            case E_PINK:   s.vy = s.g * base * (pad ? 0.65f : 0.72f) * mm; break;
            case E_RED:    s.vy = s.g * base * (pad ? 1.25f : 1.38f) * mm; break;
            case E_BLUE:   s.g = -s.g; s.vy = s.g * (-0.4f * base * mm); break;
            case E_GREEN:  s.g = -s.g; s.vy = s.g * (-0.9f * base * mm); break;
            case E_BLACK:  s.vy = s.g * (-15.f); break;
        }
        s.grounded = false; s.onSlope = false;
    }

    bool supportedNow(S& s, const Obj* L, int n, float half) {
        const float g = s.g;
        if (g > 0.f) { if (s.y - half <= s.B + 0.6f) { s.onSlope = false; return true; } }
        else if (s.T < kInf * 0.5f) { if (s.y + half >= s.T - 0.6f) { s.onSlope = false; return true; } }
        for (int i = 0; i < n; ++i) {
            const Obj& o = L[i];
            if (o.x0 > s.x + half + 4.f || o.x1 < s.x - half - 4.f) continue;
            if (o.kind == Kind::Solid) {
                if (s.x + half > o.x0 && s.x - half < o.x1) {
                    float feet = s.y - g * half, face = g > 0.f ? o.y1 : o.y0;
                    if (std::fabs(feet - face) < 1.5f) { s.onSlope = false; return true; }
                }
            } else if (o.kind == Kind::Slope) {
                if ((g > 0.f) != (o.sub == 1)) continue;
                float ratio = (o.ry - o.ly) / (o.rx - o.lx);
                float xc = ((g > 0.f) == (ratio > 0.f)) ? s.x + half : s.x - half;
                if (xc >= o.lx - 0.5f && xc <= o.rx + 0.5f) {
                    float ys = o.ly + (std::clamp(xc, o.lx, o.rx) - o.lx) * ratio;
                    float feet = s.y - g * half;
                    if (std::fabs(feet - ys) < 3.f) { s.onSlope = true; s.slope = ratio; return true; }
                }
            }
        }
        return false;
    }

    // Spider: anında en yakın yüzeye ışınlanır, yerçekimi döner. false = yolda diken, ölüm.
    bool spiderTeleport(S& s, const Obj* L, int n, float half) {
        const float dir = -s.g;
        const float start = s.y + dir * half;
        float best = dir > 0.f ? s.T : s.B;
        for (int i = 0; i < n; ++i) {
            const Obj& o = L[i];
            if (o.kind != Kind::Solid) continue;
            if (!(s.x + half - 1.f > o.x0 && s.x - half + 1.f < o.x1)) continue;
            if (dir > 0.f) { if (o.y0 >= start - 1.f && o.y0 < best) best = o.y0; }
            else           { if (o.y1 <= start + 1.f && o.y1 > best) best = o.y1; }
        }
        if (std::fabs(best) > kInf * 0.5f) return true;
        for (int i = 0; i < n; ++i) {
            const Obj& o = L[i];
            if (o.kind != Kind::Hazard) continue;
            if (!(s.x + half > o.x0 && s.x - half < o.x1)) continue;
            if (dir > 0.f) { if (o.y1 > start && o.y0 < best) return false; }
            else           { if (o.y0 < start && o.y1 > best) return false; }
        }
        s.y = best - dir * half; s.g = -s.g; s.vy = 0.f; s.grounded = true; s.onSlope = false;
        return true;
    }

    // Tek fizik adımı. false = öldü. haz: tehlike hitbox'ına eklenen güvenlik payı.
    bool stepSim(S& s, const Obj* L, int n, const Cfg& C, float haz) {
        const bool wave = s.mode == M_WAVE;
        const bool fly = flyingMode(s.mode);
        const bool cubeLike = (s.mode == M_CUBE || s.mode == M_ROBOT);
        const float half = halfOf(s);
        const float inner = half * 0.3f;
        const float snap = fly ? 6.f : 10.f;
        const float dx = s.vx / 240.f;
        const float gstep = G0 * K;
        const float jv = C.jumpV * (s.mini ? 0.8f : 1.f);
        const bool press = s.held && !s.prevHeld;
        const float prevY = s.y;

        if (!wave && !fly && s.grounded) {
            if (!supportedNow(s, L, n, half)) {
                s.grounded = false;
                if (s.onSlope) s.vy = s.slope * (s.vx / 54.f);
                s.onSlope = false;
            }
        }

        // --- orb ---
        bool consumed = false;
        if (press) {
            for (int i = 0; i < n; ++i) {
                const Obj& o = L[i];
                if (o.kind != Kind::Ring || isUsed(s, o.orb)) continue;
                if (o.x0 > s.x + half + 10.f || o.x1 < s.x - half - 10.f) continue;
                if (ovr(s.x, s.y, half, o, 0.f)) {
                    setUsed(s, o.orb); orbFx(s, o.sub, jv, false); consumed = true; break;
                }
            }
        }

        float g = s.g, u = s.vy * g;

        // --- moda göre giriş ---
        if (!consumed && !wave) {
            switch (s.mode) {
                case M_CUBE:   if (s.grounded && s.held) { u = jv; s.grounded = false; s.onSlope = false; } break;
                case M_ROBOT:  if (s.grounded && s.held) { u = 0.5f * jv; s.grounded = false; s.onSlope = false; s.robotBoost = 67; } break;
                case M_BALL:   if (s.grounded && press) { g = -g; u = -3.354f; s.grounded = false; s.onSlope = false; } break;
                case M_SPIDER:
                    if (s.grounded && press) {
                        s.g = g; s.vy = u * g;
                        if (!spiderTeleport(s, L, n, half)) return false;
                        g = s.g; u = s.vy * g;
                    }
                    break;
                case M_UFO:    if (press) { u = C.ufoV * (s.mini ? 0.97f : 1.f); s.grounded = false; } break;
                case M_SWING:  if (press) { g = -g; u = -0.8f * u; s.grounded = false; } break;
                default: break;
            }
        }

        // --- hız güncelle + konum ---
        if (wave) {
            float dir = (s.held ? 1.f : -1.f) * g;
            s.y += dir * dx * (s.mini ? 2.f : 1.f);
            s.vy = 0.f;
        } else {
            if (s.mode == M_SHIP) {
                int c = s.held ? (u >= 0.f ? 0 : 1) : (u > 0.f ? 2 : 3);
                u += s.held ? C.shipAcc[c] : -C.shipAcc[c];
                float up = C.shipUp * (s.mini ? 1.f / 0.85f : 1.f);
                float dn = C.shipDown * (s.mini ? 1.f / 0.85f : 1.f);
                u = std::clamp(u, -dn, up);
            } else if (!s.grounded) {
                if (s.mode == M_ROBOT && s.held && s.robotBoost > 0) {
                    --s.robotBoost;
                } else {
                    s.robotBoost = 0;
                    u -= gstep * C.gMult[s.mode];
                    float lim = (s.mode == M_UFO || s.mode == M_SWING) ? -6.4f : -15.f;
                    if (u < lim) u = lim;
                }
            }
            s.g = g; s.vy = u * g;
            if (!s.grounded) s.y += s.vy * K;
        }
        s.x += dx;

        // --- mod alanı sınırları ---
        const float lo = s.B + half, hi = s.T - half;
        if (s.y <= lo) {
            if (cubeLike && s.g < 0.f) return false;     // ters küp zemine çarparsa ölür
            s.y = lo;
            if (!wave && s.vy < 0.f) s.vy = 0.f;
            if (s.g > 0.f && !fly && !wave) { s.grounded = true; s.onSlope = false; }
        }
        if (s.T < kInf * 0.5f && s.y >= hi) {
            s.y = hi;
            if (!wave && s.vy > 0.f) s.vy = 0.f;
            if (s.g < 0.f && !fly && !wave) { s.grounded = true; s.onSlope = false; }
        }

        // --- objeler ---
        for (int i = 0; i < n; ++i) {
            const Obj& o = L[i];
            if (o.x0 > s.x + half + 30.f || o.x1 < s.x - half - 30.f) continue;
            switch (o.kind) {
                case Kind::Hazard:
                    if (ovr(s.x, s.y, half, o, haz)) return false;
                    break;

                case Kind::Solid:
                    if (ovr(s.x, s.y, half, o, 0.f)) {
                        if (wave) {
                            float prevFeet = prevY - half, prevHead = prevY + half;
                            if (s.y < prevY && prevFeet >= o.y1 - snap)      s.y = o.y1 + half;
                            else if (s.y > prevY && prevHead <= o.y0 + snap) s.y = o.y0 - half;
                            else if (ovr(s.x, s.y, inner, o, haz * 0.5f)) return false;
                        } else {
                            const float gg = s.g;
                            const float uu = s.vy * gg;
                            float prevFeet = prevY - gg * half, prevHead = prevY + gg * half;
                            float faceF = gg > 0.f ? o.y1 : o.y0;
                            float faceH = gg > 0.f ? o.y0 : o.y1;
                            bool above = gg > 0.f ? (prevFeet >= faceF - snap) : (prevFeet <= faceF + snap);
                            bool below = gg > 0.f ? (prevHead <= faceH + snap) : (prevHead >= faceH - snap);
                            if (uu <= 0.f && above) {
                                s.y = faceF + gg * half; s.vy = 0.f;
                                s.grounded = !fly; s.onSlope = false;
                            } else if (uu > 0.f && below) {
                                s.y = faceH - gg * half; s.vy = 0.f;
                            } else if (ovr(s.x, s.y, inner, o, haz * 0.5f)) {
                                return false;
                            }
                        }
                    }
                    break;

                case Kind::Slope: {
                    if (wave) break;
                    const float gg = s.g;
                    const bool floorSide = (gg > 0.f) == (o.sub == 1);
                    const float ratio = (o.ry - o.ly) / (o.rx - o.lx);
                    if (floorSide) {
                        float xc = ((gg > 0.f) == (ratio > 0.f)) ? s.x + half : s.x - half;
                        if (xc >= o.lx - 0.5f && xc <= o.rx + 0.5f) {
                            float ys = o.ly + (std::clamp(xc, o.lx, o.rx) - o.lx) * ratio;
                            float feet = s.y - gg * half;
                            float pen = gg > 0.f ? (ys - feet) : (feet - ys);
                            float uu = s.vy * gg;
                            if (pen > 0.f) {
                                if (pen <= snap + std::fabs(ratio) * std::fabs(dx) + 2.f && uu <= 0.5f) {
                                    s.y = ys + gg * half; s.vy = 0.f;
                                    s.grounded = !fly; s.onSlope = true; s.slope = ratio;
                                } else if (ovr(s.x, s.y, inner, o, 0.f)) {
                                    return false;
                                }
                            } else if (s.grounded && s.onSlope && -pen <= 3.f) {
                                s.y = ys + gg * half;
                            }
                        }
                    } else if (ovr(s.x, s.y, inner, o, 0.f)) {
                        float ys = o.ly + (std::clamp(s.x, o.lx, o.rx) - o.lx) * ratio;
                        bool inside = (o.sub == 1) ? (s.y - inner < ys) : (s.y + inner > ys);
                        if (inside) return false;
                    }
                    break;
                }

                case Kind::Pad:
                    if (!isUsed(s, o.orb) && ovr(s.x, s.y, half, o, 0.f)) {
                        setUsed(s, o.orb); orbFx(s, o.sub, jv, true);
                    }
                    break;

                case Kind::Ring:
                    break;

                case Kind::Portal:
                    if (!isUsed(s, o.orb) && ovr(s.x, s.y, half, o, 0.f)) {
                        setUsed(s, o.orb);
                        switch (o.sub) {
                            case PT_MODE:
                                s.mode = static_cast<int8_t>(o.mode);
                                areaFromCenter(s, o.mode, (o.y0 + o.y1) * 0.5f);
                                s.grounded = false; s.onSlope = false; s.robotBoost = 0;
                                break;
                            case PT_GRAV_N: s.g = 1.f; break;
                            case PT_GRAV_F: s.g = -1.f; break;
                            case PT_GRAV_T: s.g = -s.g; break;
                            case PT_MINI:   s.mini = true; break;
                            case PT_BIG:    s.mini = false; break;
                            case PT_SPEED:  s.vx = o.val; break;
                        }
                    }
                    break;
            }
        }
        if (fly) s.grounded = false;
        return true;
    }

    // ---------------- Beam search ----------------
    struct PathLine { std::vector<CCPoint> pts; int kind = 0; };

    struct Node {
        S s;
        int parent = -1;
        int tog = 0;
        uint8_t act = 0;
        bool dead = false;
        float x = 0, y = 0;
    };

    struct SearchOut {
        std::vector<uint8_t> plan;
        bool survived = false;
        std::vector<PathLine> lines;
    };

    bool replayAlive(const S& root, const std::vector<uint8_t>& plan, size_t from,
                     const Obj* L, int n, const Cfg& C, float margin) {
        S s = root;
        for (size_t i = from; i < plan.size(); ++i) {
            s.prevHeld = s.held; s.held = plan[i] != 0;
            if (!stepSim(s, L, n, C, margin)) return false;
        }
        return true;
    }

    std::vector<uint8_t> extractPlan(const std::vector<Node>& nodes, int idx, int D) {
        std::vector<uint8_t> acts;
        for (int i = idx; i > 0; i = nodes[i].parent) acts.push_back(nodes[i].act);
        std::reverse(acts.begin(), acts.end());
        std::vector<uint8_t> plan;
        plan.reserve(acts.size() * D);
        for (uint8_t a : acts) for (int k = 0; k < D; ++k) plan.push_back(a);
        return plan;
    }

    void chainPoints(const std::vector<Node>& nodes, int idx, PathLine& pl) {
        for (int i = idx; i >= 0; i = nodes[i].parent) pl.pts.push_back(ccp(nodes[i].x, nodes[i].y));
        std::reverse(pl.pts.begin(), pl.pts.end());
    }

    SearchOut runSearch(const S& root, const Obj* L, int n, const Cfg& C,
                        int Kbeam, int D, int stages, bool wantLines) {
        SearchOut out;
        std::vector<Node> nodes;
        nodes.reserve(static_cast<size_t>(stages) * Kbeam * 2 + 8);
        Node r; r.s = root; r.act = root.held ? 1 : 0; r.x = root.x; r.y = root.y;
        nodes.push_back(r);

        std::vector<int> beam{0}, cand;
        struct Key { uint64_t k; int tog; int idx; };
        std::vector<Key> keys;
        int lastStage = 0, bestDead = -1, bestDeadStage = -1;

        for (int st = 0; st < stages; ++st) {
            cand.clear();
            for (int bi : beam) {
                for (int a = 0; a < 2; ++a) {
                    Node nd = nodes[bi];
                    int act = (a == 0) ? nd.act : 1 - nd.act;
                    nd.parent = bi; nd.tog = nodes[bi].tog + (act != nodes[bi].act ? 1 : 0);
                    nd.act = static_cast<uint8_t>(act); nd.dead = false;
                    for (int k = 0; k < D; ++k) {
                        nd.s.prevHeld = nd.s.held; nd.s.held = act != 0;
                        if (!stepSim(nd.s, L, n, C, 0.f)) { nd.dead = true; break; }
                    }
                    nd.x = nd.s.x; nd.y = nd.s.y;
                    nodes.push_back(nd);
                    int idx = static_cast<int>(nodes.size()) - 1;
                    if (nd.dead) { if (st >= bestDeadStage) { bestDeadStage = st; bestDead = idx; } }
                    else cand.push_back(idx);
                }
            }
            if (cand.empty()) break;

            keys.clear();
            for (int idx : cand) {
                const S& s = nodes[idx].s;
                int yq = static_cast<int>(std::floor(s.y / 3.f));
                int vq = static_cast<int>(std::lround(s.vy * 3.f));
                uint64_t k = static_cast<uint64_t>(static_cast<uint32_t>(yq + 1000000));
                k = k * 1000003ull + static_cast<uint64_t>(static_cast<uint32_t>(vq + 1000000));
                k = k * 131ull + (s.held ? 1 : 0) + (s.grounded ? 2 : 0) + (s.g > 0.f ? 4 : 0) + (s.mini ? 8 : 0) + 16ull * s.mode;
                k ^= (s.used[0] * 0x9E3779B97F4A7C15ull) ^ (s.used[1] * 0xC2B2AE3D27D4EB4Full);
                keys.push_back({k, nodes[idx].tog, idx});
            }
            std::sort(keys.begin(), keys.end(), [](const Key& a, const Key& b) {
                if (a.k != b.k) return a.k < b.k;
                return a.tog < b.tog;
            });
            beam.clear();
            for (size_t i = 0; i < keys.size(); ++i)
                if (i == 0 || keys[i].k != keys[i - 1].k) beam.push_back(keys[i].idx);
            if (static_cast<int>(beam.size()) > Kbeam) {
                std::sort(beam.begin(), beam.end(), [&](int a, int b) {
                    if (nodes[a].tog != nodes[b].tog) return nodes[a].tog < nodes[b].tog;
                    return a < b;
                });
                beam.resize(Kbeam);
            }
            lastStage = st + 1;
        }

        int chosen = -1;
        if (lastStage == stages && !beam.empty()) {
            out.survived = true;
            std::vector<int> order = beam;
            std::sort(order.begin(), order.end(), [&](int a, int b) {
                if (nodes[a].tog != nodes[b].tog) return nodes[a].tog < nodes[b].tog;
                return a < b;
            });
            // En güvenli planı seç: tehlike hitbox'ını şişirerek test et (model hatası tamponu)
            const float margins[3] = {5.f, 3.f, 1.f};
            bool found = false;
            for (float m : margins) {
                int tried = 0;
                for (int idx : order) {
                    if (++tried > 10) break;
                    auto plan = extractPlan(nodes, idx, D);
                    if (replayAlive(root, plan, 0, L, n, C, m)) { chosen = idx; out.plan = std::move(plan); found = true; break; }
                }
                if (found) break;
            }
            if (!found) { chosen = order[0]; out.plan = extractPlan(nodes, chosen, D); }
        } else if (bestDead >= 0) {
            chosen = bestDead;
            out.plan = extractPlan(nodes, chosen, D);
        }

        if (wantLines) {
            for (int idx : beam) { PathLine pl; pl.kind = 0; chainPoints(nodes, idx, pl); out.lines.push_back(std::move(pl)); }
            int deadShown = 0;
            for (int i = static_cast<int>(nodes.size()) - 1; i > 0 && deadShown < 14; --i) {
                if (!nodes[i].dead) continue;
                PathLine pl; pl.kind = 1; chainPoints(nodes, i, pl);
                out.lines.push_back(std::move(pl)); ++deadShown;
            }
            if (chosen >= 0) { PathLine pl; pl.kind = 2; chainPoints(nodes, chosen, pl); out.lines.push_back(std::move(pl)); }
        }
        return out;
    }

    // Obje sınıflandırma (ID tabanlı: tüm orb / pad / portal)
    bool classify(int id, GameObjectType type, Obj& o) {
        switch (id) {
            // pad
            case 35:   o.kind = Kind::Pad;  o.sub = E_YELLOW; return true;
            case 140:  o.kind = Kind::Pad;  o.sub = E_PINK;   return true;
            case 1332: o.kind = Kind::Pad;  o.sub = E_RED;    return true;
            case 67:   o.kind = Kind::Pad;  o.sub = E_BLUE;   return true;
            // orb
            case 36:   o.kind = Kind::Ring; o.sub = E_YELLOW; return true;
            case 141:  o.kind = Kind::Ring; o.sub = E_PINK;   return true;
            case 1333: o.kind = Kind::Ring; o.sub = E_RED;    return true;
            case 84:   o.kind = Kind::Ring; o.sub = E_BLUE;   return true;
            case 1022: o.kind = Kind::Ring; o.sub = E_GREEN;  return true;
            case 1330: o.kind = Kind::Ring; o.sub = E_BLACK;  return true;
            // mod portalları
            case 12:   o.kind = Kind::Portal; o.sub = PT_MODE; o.mode = M_CUBE;   return true;
            case 13:   o.kind = Kind::Portal; o.sub = PT_MODE; o.mode = M_SHIP;   return true;
            case 47:   o.kind = Kind::Portal; o.sub = PT_MODE; o.mode = M_BALL;   return true;
            case 111:  o.kind = Kind::Portal; o.sub = PT_MODE; o.mode = M_UFO;    return true;
            case 660:  o.kind = Kind::Portal; o.sub = PT_MODE; o.mode = M_WAVE;   return true;
            case 745:  o.kind = Kind::Portal; o.sub = PT_MODE; o.mode = M_ROBOT;  return true;
            case 1331: o.kind = Kind::Portal; o.sub = PT_MODE; o.mode = M_SPIDER; return true;
            case 1933: o.kind = Kind::Portal; o.sub = PT_MODE; o.mode = M_SWING;  return true;
            // yerçekimi / boyut / hız portalları
            case 10:   o.kind = Kind::Portal; o.sub = PT_GRAV_N; return true;
            case 11:   o.kind = Kind::Portal; o.sub = PT_GRAV_F; return true;
            case 2926: o.kind = Kind::Portal; o.sub = PT_GRAV_T; return true;
            case 99:   o.kind = Kind::Portal; o.sub = PT_BIG;    return true;
            case 101:  o.kind = Kind::Portal; o.sub = PT_MINI;   return true;
            case 200:  o.kind = Kind::Portal; o.sub = PT_SPEED; o.val = 251.16f; return true;
            case 201:  o.kind = Kind::Portal; o.sub = PT_SPEED; o.val = 311.58f; return true;
            case 202:  o.kind = Kind::Portal; o.sub = PT_SPEED; o.val = 387.42f; return true;
            case 203:  o.kind = Kind::Portal; o.sub = PT_SPEED; o.val = 468.f;   return true;
            case 1334: o.kind = Kind::Portal; o.sub = PT_SPEED; o.val = 576.f;   return true;
            default: break;
        }
        switch (type) {
            case GameObjectType::Hazard:         o.kind = Kind::Hazard; return true;
            case GameObjectType::Solid:          o.kind = Kind::Solid;  return true;
            case GameObjectType::Slope:          o.kind = Kind::Slope;  return true;
            case GameObjectType::YellowJumpPad:  o.kind = Kind::Pad;  o.sub = E_YELLOW; return true;
            case GameObjectType::PinkJumpPad:    o.kind = Kind::Pad;  o.sub = E_PINK;   return true;
            case GameObjectType::RedJumpPad:     o.kind = Kind::Pad;  o.sub = E_RED;    return true;
            case GameObjectType::YellowJumpRing: o.kind = Kind::Ring; o.sub = E_YELLOW; return true;
            case GameObjectType::PinkJumpRing:   o.kind = Kind::Ring; o.sub = E_PINK;   return true;
            case GameObjectType::RedJumpRing:    o.kind = Kind::Ring; o.sub = E_RED;    return true;
            default: return false;
        }
    }

    // Slope yüzey çizgisi: dönüş (90'ın katı) ve flip'e göre hipotenüs
    void slopeLine(GameObject* go, Obj& o) {
        int rot = static_cast<int>(std::lround(go->getRotation() / 90.f)) % 4;
        if (rot < 0) rot += 4;
        bool fx = go->isFlipX(), fy = go->isFlipY();
        auto tf = [&](float u, float v) {
            if (fx) u = 1.f - u;
            if (fy) v = 1.f - v;
            float cx = u - 0.5f, cy = v - 0.5f;
            for (int i = 0; i < rot; ++i) { float nx = cy, ny = -cx; cx = nx; cy = ny; }
            return CCPoint(cx + 0.5f, cy + 0.5f);
        };
        CCPoint P = tf(0.f, 0.f), Q = tf(1.f, 1.f), R = tf(1.f, 0.f);
        float w = o.x1 - o.x0, h = o.y1 - o.y0;
        float px = o.x0 + P.x * w, py = o.y0 + P.y * h;
        float qx = o.x0 + Q.x * w, qy = o.y0 + Q.y * h;
        float rx = o.x0 + R.x * w, ry = o.y0 + R.y * h;
        if (px > qx) { std::swap(px, qx); std::swap(py, qy); }
        if (qx - px < 0.01f) qx = px + 0.01f;
        float ratio = (qy - py) / (qx - px);
        float yline = py + (std::clamp(rx, px, qx) - px) * ratio;
        o.lx = px; o.ly = py; o.rx = qx; o.ry = qy;
        o.sub = (ry < yline) ? 1 : 0;   // 1: katı çizginin altında
    }
}

class $modify(CubeBotGJBGL, GJBaseGameLayer) {
    struct Fields {
        std::vector<Obj> objs;
        std::vector<float> prefMax;
        std::vector<Obj> modePortals;
        size_t cachedCount = static_cast<size_t>(-1);
        std::vector<Obj> local;

        Cfg cfg;
        float acc = 0.f;
        bool havePrev = false;
        float prevX = 0.f, prevY = 0.f, prevVyW = 0.f, prevG = 1.f;
        int prevMode = -1;
        bool prevGrounded = true;
        float vx = 0.f;
        bool held = false, held2 = false;
        float flipSign = 1.f;
        int flipEvid = 0;

        long tick = 0;
        long lastSearch = -1000;
        std::vector<uint8_t> plan;
        size_t planPos = 0;

        CCDrawNode* drawNode = nullptr;
    };

    void rebuildCache() {
        auto f = m_fields.self();
        f->objs.clear(); f->prefMax.clear(); f->modePortals.clear();
        f->cachedCount = m_objects ? m_objects->count() : 0;
        if (!m_objects) return;
        for (auto go : CCArrayExt<GameObject*>(m_objects)) {
            if (!go) continue;
            Obj o;
            CCRect r = go->getObjectRect();
            o.x0 = r.getMinX(); o.x1 = r.getMaxX(); o.y0 = r.getMinY(); o.y1 = r.getMaxY();
            if (!classify(go->m_objectID, go->m_objectType, o)) continue;
            if (o.kind == Kind::Slope) slopeLine(go, o);
            f->objs.push_back(o);
        }
        std::sort(f->objs.begin(), f->objs.end(), [](const Obj& a, const Obj& b) { return a.x0 < b.x0; });
        float m = -1e9f;
        f->prefMax.reserve(f->objs.size());
        for (auto& o : f->objs) {
            m = std::max(m, o.x1); f->prefMax.push_back(m);
            if (o.kind == Kind::Portal && o.sub == PT_MODE) f->modePortals.push_back(o);
        }
    }

    void gather(float px, float reachX) {
        auto f = m_fields.self();
        f->local.clear();
        float minX = px - 45.f, maxX = px + reachX + 45.f;
        size_t start = std::lower_bound(f->prefMax.begin(), f->prefMax.end(), minX) - f->prefMax.begin();
        int orb = 0;
        for (size_t i = start; i < f->objs.size() && f->objs[i].x0 <= maxX; ++i) {
            Obj o = f->objs[i];
            if (o.x1 < minX) continue;
            if (o.kind == Kind::Pad || o.kind == Kind::Ring || o.kind == Kind::Portal) o.orb = orb < 128 ? static_cast<int16_t>(orb++) : -1;
            f->local.push_back(o);
            if (f->local.size() >= 220) break;
        }
    }

    void clearPaths() { auto f = m_fields.self(); if (f->drawNode) f->drawNode->clear(); }

    void drawLines(const std::vector<PathLine>& lines) {
        auto f = m_fields.self();
        if (!f->drawNode) {
            if (!m_objectLayer) return;
            f->drawNode = CCDrawNode::create();
            m_objectLayer->addChild(f->drawNode, 10000);
        }
        f->drawNode->clear();
        for (int pass = 0; pass < 3; ++pass) {
            for (auto& pl : lines) {
                if (pl.kind != pass || pl.pts.size() < 2) continue;
                ccColor4F col = pass == 0 ? ccc4f(0.3f, 1.f, 0.4f, 0.28f)
                              : pass == 1 ? ccc4f(1.f, 0.25f, 0.25f, 0.45f)
                                          : ccc4f(0.2f, 0.9f, 1.f, 1.f);
                float w = pass == 2 ? 2.2f : 1.0f;
                for (size_t i = 1; i < pl.pts.size(); ++i) f->drawNode->drawSegment(pl.pts[i - 1], pl.pts[i], w, col);
                if (pass == 1) f->drawNode->drawDot(pl.pts.back(), 2.6f, ccc4f(1.f, 0.2f, 0.2f, 0.9f));
            }
        }
    }

    void releaseAll() {
        auto f = m_fields.self();
        if (f->held) { this->handleButton(false, 1, true); f->held = false; }
        f->held2 = false;
    }

    void resetBotState() {
        auto f = m_fields.self();
        f->havePrev = false; f->plan.clear(); f->planPos = 0; f->lastSearch = -1000; f->acc = 0.f;
        releaseAll();
    }

    int readMode(PlayerObject* p) {
        if (p->m_isShip) return M_SHIP;
        if (p->m_isBird) return M_UFO;
        if (p->m_isBall) return M_BALL;
        if (p->m_isDart) return M_WAVE;
        if (p->m_isRobot) return M_ROBOT;
        if (p->m_isSpider) return M_SPIDER;
        if (p->m_isSwing) return M_SWING;
        return M_CUBE;
    }

    void runBot(float dt) {
        auto f = m_fields.self();
        auto pl = m_player1;
        if (!pl) return;

        const bool active = Mod::get()->getSettingValue<bool>("bot-active");
        const bool showTraj = Mod::get()->getSettingValue<bool>("show-trajectory");
        if (!active) releaseAll();
        if (!active && !showTraj) { f->havePrev = false; clearPaths(); return; }
        if (!showTraj) clearPaths();
        if (pl->m_isDead) { resetBotState(); clearPaths(); return; }

        if (!m_objects || m_objects->count() != f->cachedCount) rebuildCache();

        f->acc += dt;
        int steps = static_cast<int>(std::lround(f->acc / kStepDt));
        if (steps < 1) return;
        const float dtp = steps * kStepDt;
        f->acc -= dtp;
        f->tick += steps;

        const int mode = readMode(pl);
        const float px = pl->getPositionX(), py = pl->getPositionY();
        const float g = pl->m_isUpsideDown ? -1.f : 1.f;
        const bool grounded = pl->m_isOnGround;
        const bool mini = pl->m_vehicleSize > 0.f && pl->m_vehicleSize < 0.9f;
        const float vyRaw = static_cast<float>(pl->m_yVelocity);
        const float vyW = vyRaw * (g < 0.f ? f->flipSign : 1.f);

        if (f->havePrev && px < f->prevX - 1.f) resetBotState();

        Cfg& C = f->cfg;

        // ---------- canlı kalibrasyon ----------
        if (f->havePrev) {
            float vxm = (px - f->prevX) / dtp;
            if (vxm > 150.f && vxm < 800.f) f->vx = f->vx > 0.f ? f->vx * 0.7f + vxm * 0.3f : vxm;

            if (g < 0.f && f->prevG < 0.f && !grounded && !f->prevGrounded) {
                float dy = py - f->prevY;
                if (std::fabs(dy) > 0.3f && std::fabs(vyRaw) > 1.f) {
                    f->flipEvid = std::clamp(f->flipEvid + (dy * vyRaw > 0.f ? 1 : -1), -5, 5);
                    if (f->flipEvid >= 3) f->flipSign = 1.f;
                    if (f->flipEvid <= -3) f->flipSign = -1.f;
                }
            }

            const bool sameState = (mode == f->prevMode && g == f->prevG && steps == 1);
            const bool pressed = f->held && !f->held2;
            const bool airborne = !grounded && !f->prevGrounded;
            const float duU = (vyW - f->prevVyW) * g;
            const float gstep = G0 * K;

            if (sameState && airborne && !pressed && mode != M_SHIP && mode != M_WAVE) {
                bool boosting = (mode == M_ROBOT && f->held);
                if (!boosting && duU < -0.004f && duU > -0.6f) {
                    float gm = -duU / gstep;
                    C.gMult[mode] = std::clamp(C.gMult[mode] * 0.9f + gm * 0.1f, 0.15f, 1.5f);
                }
            }
            if (sameState && mode == M_SHIP && !grounded) {
                float uPrev = f->prevVyW * g;
                int ci = f->held ? (uPrev >= 0.f ? 0 : 1) : (uPrev > 0.f ? 2 : 3);
                float acc = f->held ? duU : -duU;
                float curU = vyW * g;
                if (acc > 0.002f && acc < 0.5f && std::fabs(curU) < C.shipUp - 0.2f && curU > -C.shipDown + 0.2f)
                    C.shipAcc[ci] = C.shipAcc[ci] * 0.9f + acc * 0.1f;
            }
            if (mode == M_CUBE && f->prevMode == M_CUBE && f->prevGrounded && !grounded && f->held && vyW * g > 3.f) {
                float jm = (vyW * g + gstep * C.gMult[M_CUBE]) / (mini ? 0.8f : 1.f);
                if (jm > 6.f && jm < 15.f) C.jumpV = C.jumpV * 0.5f + jm * 0.5f;
            }
            if (mode == M_UFO && f->prevMode == M_UFO && pressed && vyW * g > 2.f) {
                float um = vyW * g + gstep * C.gMult[M_UFO];
                if (um > 4.f && um < 12.f) C.ufoV = C.ufoV * 0.5f + um * 0.5f;
            }
        }
        f->havePrev = true;
        f->prevX = px; f->prevY = py; f->prevVyW = vyW; f->prevG = g;
        f->prevMode = mode; f->prevGrounded = grounded;
        if (f->vx <= 0.f) f->vx = 311.58f;

        // ---------- durum + yerel dünya ----------
        const int horizonSteps = std::max(60, static_cast<int>(
            Mod::get()->getSettingValue<double>("look-ahead") * 240.0));
        gather(px, f->vx * (horizonSteps / 240.f) + 60.f);
        const int n = static_cast<int>(f->local.size());
        const Obj* L = f->local.data();

        S root;
        root.x = px; root.y = py; root.vy = vyW; root.vx = f->vx; root.g = g;
        root.mode = static_cast<int8_t>(mode); root.mini = mini; root.grounded = grounded;
        root.held = f->held; root.prevHeld = f->held2;
        root.gb = C.groundB;
        {   // mod alanı: son geçilen mod portalından
            float pc = py;
            for (auto& p : f->modePortals) { if (p.x1 <= px + 1.f) pc = (p.y0 + p.y1) * 0.5f; else break; }
            areaFromCenter(root, mode, pc);
        }
        if (grounded && !flyingMode(mode) && g > 0.f && (mode == M_CUBE || mode == M_ROBOT)) {
            S tmp = root;
            bool onObj = false;
            float half = halfOf(root);
            for (int i = 0; i < n && !onObj; ++i) {
                const Obj& o = L[i];
                if ((o.kind == Kind::Solid || o.kind == Kind::Slope) && px + half > o.x0 && px - half < o.x1 &&
                    std::fabs((py - half) - o.y1) < 3.f) onObj = true;
            }
            if (!onObj) { C.groundB = py - half; root.gb = C.groundB; }
            (void)tmp;
        }

        // ---------- plan: doğrula, gerekirse yeniden ara ----------
        if (!f->plan.empty()) f->planPos += steps;
        const bool draw = showTraj;
        bool need = f->plan.empty() || f->planPos + 24 > f->plan.size();
        if (!need) need = !replayAlive(root, f->plan, f->planPos, L, n, C, 0.f);
        if (!need && f->tick - f->lastSearch >= 4) need = true;

        if (need) {
            int Kb = std::clamp(static_cast<int>(Mod::get()->getSettingValue<int64_t>("trajectory-count")), 4, 60);
            const int D = 3;
            int stagesN = std::max(12, horizonSteps / D);
            auto res = runSearch(root, L, n, C, Kb, D, stagesN, draw);
            f->plan = std::move(res.plan);
            f->planPos = 0;
            f->lastSearch = f->tick;
            if (draw) drawLines(res.lines);
        }

        // ---------- eylem ----------
        bool want = false;
        if (!f->plan.empty()) want = f->plan[std::min(f->planPos, f->plan.size() - 1)] != 0;
        if (!active) want = false;
        if (want != f->held) {
            this->handleButton(want, 1, true);
            if (Mod::get()->getSettingValue<bool>("debug-log")) {
                log::info("INPUT {} mode={} x={:.1f} y={:.1f} vy={:.2f} vx={:.1f} gMult={:.3f} jumpV={:.2f}",
                          want ? "DOWN" : "UP", mode, px, py, vyW, f->vx, C.gMult[mode], C.jumpV);
            }
        }
        f->held2 = f->held;
        f->held = want;
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        runBot(dt);
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
    }
};

class $modify(CubeBotPause, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();

        auto menu = CCMenu::create();
        menu->setID("cube-bot-menu"_spr);
        menu->setPosition({0.f, 0.f});

        auto toggler = CCMenuItemToggler::createWithStandardSprites(
            this, menu_selector(CubeBotPause::onToggleBot), 0.7f);
        toggler->toggle(Mod::get()->getSettingValue<bool>("bot-active"));
        toggler->setPosition({30.f, 30.f});
        menu->addChild(toggler);

        auto label = CCLabelBMFont::create("Autoplay", "bigFont.fnt");
        label->setScale(0.45f);
        label->setAnchorPoint({0.f, 0.5f});
        label->setPosition({52.f, 30.f});

        this->addChild(menu, 100);
        this->addChild(label, 100);
    }

    void onToggleBot(CCObject* sender) {
        auto t = static_cast<CCMenuItemToggler*>(sender);
        bool newValue = !t->isToggled();   // callback anında eski durum
        Mod::get()->setSettingValue<bool>("bot-active", newValue);
    }
};
