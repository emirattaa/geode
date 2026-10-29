// Geode Autoplay v1.1 - clearance odaklı, güvenli geçiş, level bitirme
//
// v1.0 -> v1.1 (ekran görüntüsü geri bildirimi):
//  * CLEARANCE GÜÇLENDİRİLDİ: kClrRange=45 (hitbox 3x), kClrW=400 (tıklamanın 4x),
//    ceza eğrisi t^3 (keskin). Bot artık "kıl payı" yerine geniş boşluk arar.
//  * stepSim'de s.clr = sep - haz. Yani clearance güvenlik payını da içeriyor,
//    güvenli sınır 0 kabul edilir.
//  * simMargin güçlendirildi: eff*0.6, min 3.5, max 8 (önceden max 4.2).
//  * Ölümcül sınırlarda sim margin tam uygulanıyor (0.5 çarpanı kaldırıldı).
//  * Beam'de tehlikeye yakın geçen düğümler çok daha pahalı.
//  * Aday seçimi: marginBase'e ulaşan plan bulunca erken çık, bulamazsa en iyi clearance.
//  * pathFromPlan simMargin kullanıyor (görsel plan = uygulanan plan).
//
// Fizik: GD 2.2081 decompile topluluğu.

#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

using namespace geode::prelude;

namespace {
    constexpr float K       = 0.225f;
    constexpr float G0      = 0.958199f;
    constexpr float kStepDt = 1.f / 240.f;
    constexpr float kInf    = 1e9f;
    constexpr int   kMaxOrb = 512;

    enum Mode : int { M_CUBE = 0, M_SHIP, M_BALL, M_UFO, M_WAVE, M_ROBOT, M_SPIDER, M_SWING, M_COUNT };
    enum class Kind : uint8_t { Hazard, Solid, Slope, Pad, Ring, Portal };
    enum Eff : uint8_t { E_YELLOW, E_PINK, E_RED, E_BLUE, E_GREEN, E_BLACK };
    enum PortalT : uint8_t { PT_MODE, PT_GRAV_N, PT_GRAV_F, PT_GRAV_T, PT_MINI, PT_BIG, PT_SPEED };

    struct Obj {
        float x0 = 0, x1 = 0, y0 = 0, y1 = 0;
        Kind kind = Kind::Solid;
        uint8_t sub = 0;
        uint8_t mode = 0;
        int16_t orb = -1;
        float lx = 0, ly = 0, rx = 1, ry = 0;
        float val = 0;
    };

    struct Cfg {
        float gMult[M_COUNT] = {1.f, 0.4f, 0.6f, 0.5f, 0.f, 0.9f, 0.6f, 0.4f};
        float jumpV  = 11.18f;
        float ufoV   = 7.f;
        float shipAcc[4] = {0.4f * G0 * K, 0.5f * G0 * K, 1.2f * 0.4f * G0 * K, 0.8f * 0.4f * G0 * K};
        float shipUp = 8.f, shipDown = 6.4f;
        float groundB = 90.f;
        float kPos = K;
    };

    struct S {
        float x = 0, y = 0, vy = 0, vx = 311.58f;
        float g = 1.f;
        float B = 90.f, T = kInf;
        float gb = 90.f;
        float slope = 0.f;
        int8_t mode = 0;
        int16_t robotBoost = 0;
        bool mini = false, grounded = true, held = false, prevHeld = false, onSlope = false;
        float clr = kInf;
        uint64_t used[kMaxOrb / 64] = {};
    };

    inline bool flyingMode(int m) { return m == M_SHIP || m == M_UFO || m == M_WAVE || m == M_SWING; }
    inline bool groundMode(int m) { return m == M_CUBE || m == M_ROBOT || m == M_BALL || m == M_SPIDER; }
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
    inline bool isUsed(const S& s, int i) {
        return i >= 0 && i < kMaxOrb && ((s.used[i >> 6] >> (i & 63)) & 1ull);
    }
    inline void setUsed(S& s, int i) {
        if (i >= 0 && i < kMaxOrb) s.used[i >> 6] |= (1ull << (i & 63));
    }
    inline float slopeRatio(const Obj& o) {
        const float w = o.rx - o.lx;
        return std::fabs(w) < 1e-3f ? 0.f : (o.ry - o.ly) / w;
    }

    struct World { const Obj* L; const float* pm; int n; };
    inline int firstIdx(const World& W, float minX) {
        return static_cast<int>(std::lower_bound(W.pm, W.pm + W.n, minX) - W.pm);
    }

    void areaFromCenter(S& s, int mode, float pc) {
        float H = modeHeight(mode);
        if (H >= kInf) { s.B = s.gb; s.T = kInf; return; }
        s.B = std::max(s.gb, pc - H * 0.5f);
        s.T = s.B + H;
        if (s.y - 15.f < s.B) s.B = s.y - 15.f;
        if (s.y + 15.f > s.T) s.T = s.y + 15.f;
    }

    void orbFx(S& s, uint8_t eff, float jv, bool pad) {
        const float base = pad ? 16.f : jv;
        if (s.mode == M_WAVE) {
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

    bool supportedNow(S& s, const World& W, float half) {
        const float g = s.g;
        if (g > 0.f) { if (s.y - half <= s.B + 0.6f) { s.onSlope = false; return true; } }
        else if (s.T < kInf * 0.5f) { if (s.y + half >= s.T - 0.6f) { s.onSlope = false; return true; } }
        for (int i = firstIdx(W, s.x - half - 4.f); i < W.n && W.L[i].x0 <= s.x + half + 4.f; ++i) {
            const Obj& o = W.L[i];
            if (o.x1 < s.x - half - 4.f) continue;
            if (o.kind == Kind::Solid) {
                if (s.x + half > o.x0 && s.x - half < o.x1) {
                    float feet = s.y - g * half, face = g > 0.f ? o.y1 : o.y0;
                    if (std::fabs(feet - face) < 1.5f) { s.onSlope = false; return true; }
                }
            } else if (o.kind == Kind::Slope) {
                if ((g > 0.f) != (o.sub == 1)) continue;
                float ratio = slopeRatio(o);
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

    bool spiderTeleport(S& s, const World& W, float half) {
        const float dir = -s.g;
        const float start = s.y + dir * half;
        float best = dir > 0.f ? s.T : s.B;
        for (int i = firstIdx(W, s.x - half); i < W.n && W.L[i].x0 <= s.x + half; ++i) {
            const Obj& o = W.L[i];
            if (o.kind != Kind::Solid) continue;
            if (!(s.x + half - 1.f > o.x0 && s.x - half + 1.f < o.x1)) continue;
            if (dir > 0.f) { if (o.y0 >= start - 1.f && o.y0 < best) best = o.y0; }
            else           { if (o.y1 <= start + 1.f && o.y1 > best) best = o.y1; }
        }
        if (std::fabs(best) > kInf * 0.5f) return false;
        for (int i = firstIdx(W, s.x - half); i < W.n && W.L[i].x0 <= s.x + half; ++i) {
            const Obj& o = W.L[i];
            if (o.kind != Kind::Hazard) continue;
            if (!(s.x + half > o.x0 && s.x - half < o.x1)) continue;
            if (dir > 0.f) { if (o.y1 > start && o.y0 < best) return false; }
            else           { if (o.y0 < start && o.y1 > best) return false; }
        }
        s.y = best - dir * half; s.g = -s.g; s.vy = 0.f; s.grounded = true; s.onSlope = false;
        return true;
    }

    bool stepSim(S& s, const World& W, const Cfg& C, float haz) {
        const bool wave = s.mode == M_WAVE;
        const bool fly = flyingMode(s.mode);
        const bool cubeLike = (s.mode == M_CUBE || s.mode == M_ROBOT);
        const float half = halfOf(s);
        const float inner = half * 0.3f;
        const float snap = fly ? 6.f : 10.f;
        const float dx = s.vx / 240.f;
        const float gstep = G0 * C.kPos;
        const float jv = C.jumpV * (s.mini ? 0.8f : 1.f);
        const bool press = s.held && !s.prevHeld;
        const float prevY = s.y;

        if (!wave && !fly && s.grounded) {
            if (!supportedNow(s, W, half)) {
                s.grounded = false;
                if (s.onSlope) s.vy = s.slope * (s.vx / (240.f * C.kPos));
                s.onSlope = false;
            }
        }

        bool consumed = false;
        if (press) {
            for (int i = firstIdx(W, s.x - half - 10.f); i < W.n && W.L[i].x0 <= s.x + half + 10.f; ++i) {
                const Obj& o = W.L[i];
                if (o.kind != Kind::Ring || isUsed(s, o.orb)) continue;
                if (o.x1 < s.x - half - 10.f) continue;
                if (ovr(s.x, s.y, half, o, 0.f)) {
                    setUsed(s, o.orb); orbFx(s, o.sub, jv, false); consumed = true; break;
                }
            }
        }

        float g = s.g, u = s.vy * g;

        if (!consumed && !wave) {
            switch (s.mode) {
                case M_CUBE:   if (s.grounded && s.held) { u = jv; s.grounded = false; s.onSlope = false; } break;
                case M_ROBOT:  if (s.grounded && s.held) { u = 0.5f * jv; s.grounded = false; s.onSlope = false; s.robotBoost = 67; } break;
                case M_BALL:   if (s.grounded && press) { g = -g; u = -3.354f; s.grounded = false; s.onSlope = false; } break;
                case M_SPIDER:
                    if (s.grounded && press) {
                        s.g = g; s.vy = u * g;
                        if (!spiderTeleport(s, W, half)) return false;
                        g = s.g; u = s.vy * g;
                    }
                    break;
                case M_UFO:    if (press) { u = C.ufoV * (s.mini ? 0.97f : 1.f); s.grounded = false; } break;
                case M_SWING:  if (press) { g = -g; u = -0.8f * u; s.grounded = false; } break;
                default: break;
            }
        }

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
            if (!s.grounded) s.y += s.vy * C.kPos;
        }
        s.x += dx;

        const float lo = s.B + half, hi = s.T - half;
        if (s.y <= lo) {
            if (cubeLike && s.g < 0.f) return false;
            s.y = lo;
            if (!wave && s.vy < 0.f) s.vy = 0.f;
            if (s.g > 0.f && !fly && !wave) { s.grounded = true; s.onSlope = false; }
        }
        if (s.T < kInf * 0.5f && s.y >= hi) {
            s.y = hi;
            if (!wave && s.vy > 0.f) s.vy = 0.f;
            if (s.g < 0.f && !fly && !wave) { s.grounded = true; s.onSlope = false; }
        }

        for (int i = firstIdx(W, s.x - half - 30.f); i < W.n && W.L[i].x0 <= s.x + half + 30.f; ++i) {
            const Obj& o = W.L[i];
            if (o.x1 < s.x - half - 30.f) continue;
            switch (o.kind) {
                case Kind::Hazard: {
                    if (ovr(s.x, s.y, half, o, haz)) return false;
                    // DÜZELTME: clearance güvenlik payını da içerir (negatifse güvenlik sınırına girmiş)
                    const float sx = std::max(o.x0 - (s.x + half), (s.x - half) - o.x1);
                    const float sy = std::max(o.y0 - (s.y + half), (s.y - half) - o.y1);
                    const float sep = std::max(sx, sy) - haz;
                    if (sep < s.clr) s.clr = sep;
                    break;
                }
                case Kind::Solid:
                    if (ovr(s.x, s.y, half, o, 0.f)) {
                        if (wave) {
                            float prevFeet = prevY - half, prevHead = prevY + half;
                            if (s.y < prevY && prevFeet >= o.y1 - snap)      s.y = o.y1 + half;
                            else if (s.y > prevY && prevHead <= o.y0 + snap) s.y = o.y0 - half;
                            else if (ovr(s.x, s.y, half * 0.8f, o, haz)) return false;
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
                            } else if (ovr(s.x, s.y, inner, o, haz)) {
                                return false;
                            }
                        }
                    }
                    break;
                case Kind::Slope: {
                    const float ratio = slopeRatio(o);
                    if (wave) {
                        if (!ovr(s.x, s.y, half, o, 0.f)) break;
                        const bool floorType = (o.sub == 1);
                        const float xc = (floorType == (ratio > 0.f)) ? s.x + half : s.x - half;
                        if (xc >= o.lx - 0.5f && xc <= o.rx + 0.5f) {
                            const float ys = o.ly + (std::clamp(xc, o.lx, o.rx) - o.lx) * ratio;
                            const float tol = snap + (1.f + std::fabs(ratio)) * std::fabs(dx) * (s.mini ? 2.f : 1.f) + 2.f;
                            if (floorType) {
                                const float pen = ys - (s.y - half);
                                if (pen > 0.f) { if (pen <= tol) s.y = ys + half; else return false; }
                            } else {
                                const float pen = (s.y + half) - ys;
                                if (pen > 0.f) { if (pen <= tol) s.y = ys - half; else return false; }
                            }
                        }
                        break;
                    }
                    const float gg = s.g;
                    const bool floorSide = (gg > 0.f) == (o.sub == 1);
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
                case Kind::Ring: break;
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

    struct PathLine { std::vector<CCPoint> pts; int kind = 0; };

    struct Node {
        S s;
        int parent = -1;
        int steps = 0;
        float cost = 0.f;
        uint8_t act = 0;
        uint8_t d = 0;
        bool dead = false;
        float x = 0, y = 0;
    };

    struct SearchOut {
        std::vector<uint8_t> plan;
        bool survived = false;
        std::vector<PathLine> lines;
    };

    struct BeamKey { uint64_t k; float cost; int idx; };

    struct SearchBuf {
        std::vector<Node> nodes;
        std::vector<int> beam, cand, uniq;
        std::vector<BeamKey> keys;
        std::vector<std::pair<float, int>> order;
    };

    inline uint64_t mix64(uint64_t x) {
        x ^= x >> 33; x *= 0xff51afd7ed558ccdull;
        x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ull;
        x ^= x >> 33;
        return x;
    }

    uint64_t stateKey(const S& s) {
        const int yq = static_cast<int>(std::floor(s.y / 3.f));
        const int vq = static_cast<int>(std::lround(s.vy * 3.f));
        uint64_t flags = (s.held ? 1u : 0u) | (s.grounded ? 2u : 0u) | (s.g > 0.f ? 4u : 0u) |
                         (s.mini ? 8u : 0u) | (s.onSlope ? 16u : 0u) | (s.robotBoost > 0 ? 32u : 0u) |
                         (static_cast<uint64_t>(s.mode) << 8);
        uint64_t k = mix64(static_cast<uint64_t>(static_cast<uint32_t>(yq + 1000000)));
        k = mix64(k ^ (static_cast<uint64_t>(static_cast<uint32_t>(vq + 1000000)) << 1));
        k = mix64(k ^ (flags * 0x9E3779B97F4A7C15ull));
        for (uint64_t w : s.used) k = mix64(k ^ w);
        return k;
    }

    bool replayAlive(const S& root, const std::vector<uint8_t>& plan, size_t from, size_t maxSteps,
                     const World& W, const Cfg& C, float margin) {
        S s = root;
        size_t end = std::min(plan.size(), from + maxSteps);
        for (size_t i = from; i < end; ++i) {
            s.prevHeld = s.held; s.held = plan[i] != 0;
            if (!stepSim(s, W, C, margin)) return false;
        }
        return true;
    }

    std::vector<uint8_t> extractPlan(const std::vector<Node>& nodes, int idx) {
        std::vector<int> chain;
        for (int i = idx; i > 0; i = nodes[i].parent) chain.push_back(i);
        std::reverse(chain.begin(), chain.end());
        std::vector<uint8_t> plan;
        for (int i : chain) for (int k = 0; k < nodes[i].d; ++k) plan.push_back(nodes[i].act);
        return plan;
    }

    void chainPoints(const std::vector<Node>& nodes, int idx, PathLine& pl) {
        for (int i = idx; i >= 0; i = nodes[i].parent) pl.pts.push_back(ccp(nodes[i].x, nodes[i].y));
        std::reverse(pl.pts.begin(), pl.pts.end());
    }

    constexpr float kDead = -1e8f;

    float replayClearance(const S& root, const std::vector<uint8_t>& plan, size_t maxSteps,
                          const World& W, const Cfg& C, float margin) {
        S s = root; s.clr = kInf;
        const size_t end = std::min(plan.size(), maxSteps);
        for (size_t i = 0; i < end; ++i) {
            s.prevHeld = s.held; s.held = plan[i] != 0;
            if (!stepSim(s, W, C, margin)) return kDead;
        }
        return s.clr;
    }

    void pathFromPlan(const S& root, const std::vector<uint8_t>& plan, const World& W, const Cfg& C, float margin, PathLine& pl) {
        S s = root;
        pl.pts.clear();
        pl.pts.push_back(ccp(s.x, s.y));
        for (size_t i = 0; i < plan.size(); ++i) {
            s.prevHeld = s.held; s.held = plan[i] != 0;
            if (!stepSim(s, W, C, margin)) { pl.pts.push_back(ccp(s.x, s.y)); break; }
            const size_t every = i < 600 ? 2 : 6;
            if (i % every == 0) pl.pts.push_back(ccp(s.x, s.y));
        }
    }

    inline int stageD(int stepsDone, int dNear, int nearSteps) {
        if (stepsDone < nearSteps) return dNear;
        if (stepsDone < 480) return 5;
        if (stepsDone < 1200) return 10;
        return 20;
    }

    // DÜZELTME: çok daha geniş ve ağır clearance
    constexpr float kClrRange = 45.f;
    constexpr float kClrW     = 400.f;

    SearchOut runSearch(const S& root, const World& W, const Cfg& C, int Kbeam, int horizon, bool wantLines,
                        float marginBase, float simMargin,
                        const std::vector<uint8_t>* hint, size_t hintPos) {
        SearchOut out;
        static thread_local SearchBuf B;
        auto& nodes = B.nodes;
        auto& beam = B.beam;
        auto& cand = B.cand;
        auto& uniq = B.uniq;
        auto& keys = B.keys;

        const int dNear = root.mode == M_WAVE ? 1 : ((root.mode == M_SHIP || root.mode == M_UFO || root.mode == M_SWING) ? 2 : 3);
        const int nearSteps = root.mode == M_WAVE ? 96 : 144;
        size_t estStages = 2;
        for (int sd = 0; sd < horizon; sd += stageD(sd, dNear, nearSteps)) ++estStages;
        const size_t want = estStages * static_cast<size_t>(Kbeam) * 2 + 8;
        nodes.clear();
        if (nodes.capacity() < want) nodes.reserve(want);
        Node r; r.s = root; r.act = root.held ? 1 : 0; r.x = root.x; r.y = root.y;
        nodes.push_back(r);

        beam.assign(1, 0); cand.clear(); uniq.clear(); keys.clear();
        int stepsDone = 0, bestDead = -1;

        while (stepsDone < horizon) {
            const int D = std::min(stageD(stepsDone, dNear, nearSteps), horizon - stepsDone);
            const bool hintOk = hint && hintPos + static_cast<size_t>(stepsDone) < hint->size();
            const int hintAct = hintOk ? ((*hint)[hintPos + static_cast<size_t>(stepsDone)] != 0 ? 1 : 0) : -1;
            cand.clear();
            for (int bi : beam) {
                for (int a = 0; a < 2; ++a) {
                    Node nd = nodes[bi];
                    const int prevAct = nd.act;
                    const int act = (a == 0) ? prevAct : 1 - prevAct;
                    nd.parent = bi; nd.act = static_cast<uint8_t>(act); nd.d = static_cast<uint8_t>(D); nd.dead = false;
                    nd.s.clr = kInf;
                    int k = 0;
                    for (; k < D; ++k) {
                        nd.s.prevHeld = nd.s.held; nd.s.held = act != 0;
                        if (!stepSim(nd.s, W, C, simMargin)) { nd.dead = true; break; }
                    }
                    nd.steps += k;
                    nd.x = nd.s.x; nd.y = nd.s.y;

                    float inc = 0.f;
                    if (groundMode(nd.s.mode)) {
                        if (act && !prevAct) inc += 100.f;
                        if (act) inc += 2.f;
                        if (!nd.s.grounded) inc += 0.2f * D;
                        if (nd.s.T >= kInf * 0.5f && nd.s.y > nd.s.gb + 450.f) inc += 20.f;
                    } else {
                        inc += std::fabs(nd.s.y - root.y) * 0.08f;
                        if (act != prevAct) inc += 1.f;
                    }
                    // DÜZELTME: çok daha ağır clearance cezası (t^3)
                    if (nd.s.clr < kClrRange) {
                        const float t = (kClrRange - std::max(nd.s.clr, 0.f)) / kClrRange;
                        inc += kClrW * t * t * t * (static_cast<float>(D) / 3.f);
                    }
                    if (hintAct >= 0 && act != hintAct) inc += 3.f;
                    nd.cost += inc;

                    nodes.push_back(nd);
                    const int idx = static_cast<int>(nodes.size()) - 1;
                    if (nd.dead) {
                        if (bestDead < 0 || nd.steps > nodes[bestDead].steps ||
                            (nd.steps == nodes[bestDead].steps && nd.cost < nodes[bestDead].cost)) bestDead = idx;
                    } else cand.push_back(idx);
                }
            }
            if (cand.empty()) break;
            stepsDone += D;

            keys.clear();
            for (int idx : cand) keys.push_back({stateKey(nodes[idx].s), nodes[idx].cost, idx});
            std::sort(keys.begin(), keys.end(), [](const BeamKey& a, const BeamKey& b) {
                if (a.k != b.k) return a.k < b.k;
                if (a.cost != b.cost) return a.cost < b.cost;
                return a.idx < b.idx;
            });
            uniq.clear();
            for (size_t i = 0; i < keys.size(); ++i)
                if (i == 0 || keys[i].k != keys[i - 1].k) uniq.push_back(keys[i].idx);
            if (static_cast<int>(uniq.size()) > Kbeam) {
                std::sort(uniq.begin(), uniq.end(), [&](int a, int b) {
                    if (nodes[a].cost != nodes[b].cost) return nodes[a].cost < nodes[b].cost;
                    return a < b;
                });
                const int keep = std::max(1, Kbeam / 2);
                const int need = Kbeam - keep;
                const size_t rest = uniq.size() - keep;
                beam.assign(uniq.begin(), uniq.begin() + keep);
                for (int i = 0; i < need; ++i) beam.push_back(uniq[keep + static_cast<size_t>(i) * rest / need]);
            } else {
                beam = uniq;
            }
        }

        int chosen = -1;
        if (stepsDone >= horizon && !beam.empty()) {
            out.survived = true;
            auto& order = B.order;
            order.clear();
            for (int idx : beam) {
                float c = nodes[idx].cost + ((groundMode(nodes[idx].s.mode) && !nodes[idx].s.grounded) ? 15.f : 0.f);
                order.push_back({c, idx});
            }
            std::sort(order.begin(), order.end());

            // Geniş aday tarama, iyi plan bulunca erken çık
            float bestScore = -1e9f;
            int tried = 0;
            const int maxTries = std::min(50, static_cast<int>(order.size()));
            for (auto& pr : order) {
                if (++tried > maxTries) break;
                auto plan = extractPlan(nodes, pr.second);
                const float clr = replayClearance(root, plan, 480, W, C, simMargin);
                if (clr <= kDead * 0.5f) continue;
                const float score = std::min(clr, marginBase);
                if (score > bestScore + 0.25f) {
                    bestScore = score; chosen = pr.second; out.plan = std::move(plan);
                }
                if (bestScore >= marginBase - 0.25f) break;
            }
            if (chosen < 0) {
                chosen = order[0].second;
                out.plan = extractPlan(nodes, chosen);
            }
        } else if (bestDead >= 0) {
            chosen = bestDead;
            out.plan = extractPlan(nodes, chosen);
        }

        if (wantLines) {
            int shown = 0;
            for (int idx : beam) {
                if (shown++ >= 14) break;
                PathLine pl; pl.kind = 0; chainPoints(nodes, idx, pl); out.lines.push_back(std::move(pl));
            }
            int deadShown = 0;
            for (int i = static_cast<int>(nodes.size()) - 1; i > 0 && deadShown < 8; --i) {
                if (!nodes[i].dead) continue;
                PathLine pl; pl.kind = 1; chainPoints(nodes, i, pl);
                out.lines.push_back(std::move(pl)); ++deadShown;
            }
            if (chosen >= 0 && !out.plan.empty()) {
                PathLine pl; pl.kind = 2; pathFromPlan(root, out.plan, W, C, simMargin, pl);
                out.lines.push_back(std::move(pl));
            }
        }
        return out;
    }

    inline const char* modeName(int m) {
        static const char* const n[M_COUNT] = {"CUBE", "SHIP", "BALL", "UFO", "WAVE", "ROBOT", "SPIDER", "SWING"};
        return (m >= 0 && m < M_COUNT) ? n[m] : "?";
    }

    bool classify(int id, GameObjectType type, Obj& o) {
        switch (id) {
            case 35:   o.kind = Kind::Pad;  o.sub = E_YELLOW; return true;
            case 140:  o.kind = Kind::Pad;  o.sub = E_PINK;   return true;
            case 1332: o.kind = Kind::Pad;  o.sub = E_RED;    return true;
            case 67:   o.kind = Kind::Pad;  o.sub = E_BLUE;   return true;
            case 36:   o.kind = Kind::Ring; o.sub = E_YELLOW; return true;
            case 141:  o.kind = Kind::Ring; o.sub = E_PINK;   return true;
            case 1333: o.kind = Kind::Ring; o.sub = E_RED;    return true;
            case 84:   o.kind = Kind::Ring; o.sub = E_BLUE;   return true;
            case 1022: o.kind = Kind::Ring; o.sub = E_GREEN;  return true;
            case 1330: o.kind = Kind::Ring; o.sub = E_BLACK;  return true;
            case 12:   o.kind = Kind::Portal; o.sub = PT_MODE; o.mode = M_CUBE;   return true;
            case 13:   o.kind = Kind::Portal; o.sub = PT_MODE; o.mode = M_SHIP;   return true;
            case 47:   o.kind = Kind::Portal; o.sub = PT_MODE; o.mode = M_BALL;   return true;
            case 111:  o.kind = Kind::Portal; o.sub = PT_MODE; o.mode = M_UFO;    return true;
            case 660:  o.kind = Kind::Portal; o.sub = PT_MODE; o.mode = M_WAVE;   return true;
            case 745:  o.kind = Kind::Portal; o.sub = PT_MODE; o.mode = M_ROBOT;  return true;
            case 1331: o.kind = Kind::Portal; o.sub = PT_MODE; o.mode = M_SPIDER; return true;
            case 1933: o.kind = Kind::Portal; o.sub = PT_MODE; o.mode = M_SWING;  return true;
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
        o.sub = (ry < yline) ? 1 : 0;
    }
}

class $modify(CubeBotGJBGL, GJBaseGameLayer) {
    struct Fields {
        std::vector<Obj> objs;
        std::vector<float> prefMax;
        std::vector<Obj> modePortals;
        size_t cachedCount = static_cast<size_t>(-1);
        std::vector<Obj> local;
        std::vector<float> localPm;
        float gatherX = -1e9f, gatherReach = 0.f;
        float searchMs = 1.f;
        long lastDraw = -1000;

        Cfg cfg;
        bool groundKnown = false;
        float acc = 0.f;
        bool havePrev = false;
        float prevX = 0.f, prevY = 0.f, prevVyW = 0.f, prevG = 1.f;
        int prevMode = -1;
        bool prevGrounded = true;
        float vx = 0.f;
        bool held = false, held2 = false;
        float flipSign = 1.f;
        int flipEvid = 0;
        int robotBoost = 0;
        std::vector<std::pair<float, float>> usedRings;

        long tick = 0;
        long lastSearch = -1000;
        std::vector<uint8_t> plan;
        size_t planPos = 0;

        CCDrawNode* drawNode = nullptr;

        struct Unsup { float x, y; int id; };
        std::vector<Unsup> unsup;
        int cntDash = 0, cntDual = 0, cntTele = 0, cntMirror = 0, cntTrig = 0;
        S pred; bool predValid = false;
        float errY[M_COUNT] = {}, errV[M_COUNT] = {};
        float lastErrY = 0.f, lastErrV = 0.f;
        int bigErr = 0, inputMismatch = 0;
        float lastBigErrX = -1.f;
        S lastRoot; bool haveLastRoot = false;
        bool lastSurvived = true;
        bool deadReported = false;
        long reportUntil = 0;
        CCLabelBMFont* reportLabel = nullptr;

        float startX = -1e9f;
        float maxX = -1e9f;
        int stuckFrames = 0;
        int escalation = 0;
        long lastProgressTick = 0;
        long lastEscalationTick = 0;
        int desperateFrames = 0;
    };

    void rebuildCache() {
        auto f = m_fields.self();
        f->objs.clear(); f->prefMax.clear(); f->modePortals.clear();
        f->local.clear(); f->localPm.clear();
        f->gatherX = -1e9f; f->gatherReach = 0.f;
        f->unsup.clear(); f->cntDash = f->cntDual = f->cntTele = f->cntMirror = f->cntTrig = 0;
        f->cachedCount = m_objects ? m_objects->count() : 0;
        if (!m_objects) return;
        for (auto go : CCArrayExt<GameObject*>(m_objects)) {
            if (!go) continue;
            Obj o;
            CCRect r = go->getObjectRect();
            o.x0 = r.getMinX(); o.x1 = r.getMaxX(); o.y0 = r.getMinY(); o.y1 = r.getMaxY();
            {
                const int oid = go->m_objectID;
                bool u = true;
                if (oid == 1704 || oid == 1751) ++f->cntDash;
                else if (oid == 286 || oid == 287) ++f->cntDual;
                else if (oid == 747) ++f->cntTele;
                else if (oid == 45 || oid == 46) ++f->cntMirror;
                else if (oid == 901 || oid == 1346 || oid == 1049) ++f->cntTrig;
                else u = false;
                if (u) f->unsup.push_back({(o.x0 + o.x1) * 0.5f, (o.y0 + o.y1) * 0.5f, oid});
            }
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
        f->gatherX = px; f->gatherReach = reachX;
        float minX = px - 45.f, maxX = px + reachX + 45.f;
        size_t start = std::lower_bound(f->prefMax.begin(), f->prefMax.end(), minX) - f->prefMax.begin();
        int orb = 0;
        for (size_t i = start; i < f->objs.size() && f->objs[i].x0 <= maxX; ++i) {
            Obj o = f->objs[i];
            if (o.x1 < minX) continue;
            if (o.kind == Kind::Pad || o.kind == Kind::Ring || o.kind == Kind::Portal) o.orb = orb < kMaxOrb ? static_cast<int16_t>(orb++) : -1;
            f->local.push_back(o);
            if (f->local.size() >= 12000) break;
        }
        f->localPm.resize(f->local.size());
        float m = -1e9f;
        for (size_t i = 0; i < f->local.size(); ++i) { m = std::max(m, f->local[i].x1); f->localPm[i] = m; }
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
        f->usedRings.clear(); f->robotBoost = 0;
        f->vx = 0.f;
        f->startX = -1e9f; f->maxX = -1e9f; f->stuckFrames = 0; f->escalation = 0;
        f->lastProgressTick = f->tick; f->lastEscalationTick = f->tick; f->desperateFrames = 0;
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

    bool ringUsed(const Obj& o) {
        auto f = m_fields.self();
        for (auto& p : f->usedRings)
            if (std::fabs(p.first - o.x0) < 0.05f && std::fabs(p.second - o.y0) < 0.05f) return true;
        return false;
    }

    void showReport(const std::string& text, long ticks) {
        auto f = m_fields.self();
        if (!f->reportLabel) {
            f->reportLabel = CCLabelBMFont::create("", "chatFont.fnt");
            f->reportLabel->setAnchorPoint({0.f, 1.f});
            f->reportLabel->setScale(0.55f);
            f->reportLabel->setPosition({6.f, CCDirector::get()->getWinSize().height - 6.f});
            this->addChild(f->reportLabel, 5000);
        }
        f->reportLabel->setString(text.c_str());
        f->reportLabel->setVisible(true);
        f->reportUntil = f->tick + ticks;
    }

    void buildDeathReport() {
        auto f = m_fields.self();
        if (!f->haveLastRoot) return;
        const S& r = f->lastRoot;
        const Cfg& C = f->cfg;
        int nh = 0, ns = 0, np = 0, no = 0, nt = 0;
        for (const Obj& o : f->local) {
            if (o.x1 < r.x - 60.f || o.x0 > r.x + 60.f || o.y1 < r.y - 60.f || o.y0 > r.y + 60.f) continue;
            switch (o.kind) {
                case Kind::Hazard: ++nh; break;
                case Kind::Solid: case Kind::Slope: ++ns; break;
                case Kind::Pad: ++np; break;
                case Kind::Ring: ++no; break;
                case Kind::Portal: ++nt; break;
            }
        }
        std::string nearTxt;
        for (auto& u : f->unsup) {
            if (std::fabs(u.x - r.x) < 150.f && std::fabs(u.y - r.y) < 180.f) {
                const char* nm = (u.id == 1704 || u.id == 1751) ? "DASH-ORB" : (u.id == 286 || u.id == 287) ? "DUAL" :
                                 (u.id == 747) ? "TELEPORT" : (u.id == 45 || u.id == 46) ? "AYNA" : "TRIGGER";
                if (nearTxt.find(nm) == std::string::npos) nearTxt += std::string(nm) + " ";
            }
        }
        const char* why = !f->lastSurvived ? "Planlayici COZUM BULAMADI"
                          : (f->bigErr > 0 ? "Model SAPTI" : "Plan bulundu ama yine de oldu");
        char buf[1024];
        std::snprintf(buf, sizeof buf,
            "OLUM x=%.0f y=%.0f mod=%s g=%+.0f mini=%d hiz=%.0f\n"
            "%s | esc=%d stuck=%d\n"
            "sapma son dy=%.2f dv=%.2f | ort dy=%.2f dv=%.2f | buyuk sapma=%d\n"
            "giris uyusmazligi=%d | tehlike=%d blok=%d pad=%d orb=%d portal=%d\n"
            "DESTEK YOK: %s | dash=%d dual=%d tele=%d ayna=%d trig=%d\n"
            "kalib: kPos=%.4f jumpV=%.2f ufoV=%.2f",
            r.x, r.y, modeName(r.mode), r.g, r.mini ? 1 : 0, r.vx,
            why, f->escalation, f->stuckFrames,
            f->lastErrY, f->lastErrV, f->errY[r.mode], f->errV[r.mode], f->bigErr,
            f->inputMismatch, nh, ns, np, no, nt,
            nearTxt.empty() ? "yok" : nearTxt.c_str(),
            f->cntDash, f->cntDual, f->cntTele, f->cntMirror, f->cntTrig,
            C.kPos, C.jumpV, C.ufoV);
        showReport(buf, 240 * 12);
        log::info("{}", buf);
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
        if (pl->m_isDead) {
            if (!f->deadReported && Mod::get()->getSettingValue<bool>("death-report")) { f->deadReported = true; buildDeathReport(); }
            resetBotState(); clearPaths(); return;
        }
        f->deadReported = false;

        if (!m_objects || m_objects->count() != f->cachedCount) rebuildCache();

        f->acc += dt;
        int steps = static_cast<int>(std::lround(f->acc / kStepDt));
        if (steps < 1) return;
        const float dtp = steps * kStepDt;
        f->acc -= dtp;
        f->tick += steps;

        if (f->reportLabel && f->reportLabel->isVisible() && f->tick > f->reportUntil) f->reportLabel->setVisible(false);

        const int mode = readMode(pl);
        const float px = pl->getPositionX(), py = pl->getPositionY();
        const float g = pl->m_isUpsideDown ? -1.f : 1.f;
        const bool grounded = pl->m_isOnGround;
        const bool mini = pl->m_vehicleSize > 0.f && pl->m_vehicleSize < 0.9f;
        const float vyRaw = static_cast<float>(pl->m_yVelocity);
        const float vyW = vyRaw * (g < 0.f ? f->flipSign : 1.f);

        if (f->havePrev && px < f->prevX - 1.f) resetBotState();

        Cfg& C = f->cfg;
        const bool debug = Mod::get()->getSettingValue<bool>("debug-log");
        const bool autoCal = Mod::get()->getSettingValue<bool>("auto-calibrate");

        if (f->startX < -1e8f) { f->startX = px; f->maxX = px; f->lastProgressTick = f->tick; }
        if (px > f->maxX + 1.f) {
            f->maxX = px; f->stuckFrames = 0; f->lastProgressTick = f->tick;
        } else if (f->maxX - px > 5.f) {
            f->stuckFrames++;
        }
        if (f->stuckFrames > 120 && f->tick - f->lastEscalationTick > 90) {
            f->escalation = std::min(3, f->escalation + 1);
            f->lastEscalationTick = f->tick;
            f->stuckFrames = 0;
        }
        if (f->escalation > 0 && f->tick - f->lastProgressTick > 600) {
            f->escalation = 0;
        }

        if (f->havePrev) {
            float vxm = (px - f->prevX) / dtp;
            if (vxm > 150.f && vxm < 800.f) {
                if (f->vx <= 0.f || std::fabs(vxm - f->vx) > 25.f) f->vx = vxm;
                else f->vx = f->vx * 0.5f + vxm * 0.5f;
            }
            if (g < 0.f && f->prevG < 0.f && !grounded && !f->prevGrounded) {
                float dy = py - f->prevY;
                if (std::fabs(dy) > 0.3f && std::fabs(vyRaw) > 1.f) {
                    f->flipEvid = std::clamp(f->flipEvid + (dy * vyRaw > 0.f ? 1 : -1), -5, 5);
                    if (f->flipEvid >= 3) f->flipSign = 1.f;
                    if (f->flipEvid <= -3) f->flipSign = -1.f;
                }
            }
            const bool sameState = autoCal && (mode == f->prevMode && g == f->prevG && steps == 1);
            const bool pressed = f->held && !f->held2;
            const bool airborne = !grounded && !f->prevGrounded;
            const float duU = (vyW - f->prevVyW) * g;
            const float gstep = G0 * C.kPos;
            if (sameState && airborne && mode != M_WAVE && std::fabs(vyW - f->prevVyW) < 0.6f) {
                float vAvg = 0.5f * (vyW + f->prevVyW);
                if (std::fabs(vAvg) > 2.f) {
                    float ke = (py - f->prevY) / vAvg;
                    if (ke > 0.15f && ke < 0.35f) C.kPos = C.kPos * 0.95f + ke * 0.05f;
                }
            }
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
                float acc2 = f->held ? duU : -duU;
                float curU = vyW * g;
                if (acc2 > 0.002f && acc2 < 0.5f && std::fabs(curU) < C.shipUp - 0.2f && curU > -C.shipDown + 0.2f) {
                    C.shipAcc[ci] = std::clamp(C.shipAcc[ci] * 0.9f + acc2 * 0.1f, 0.02f * G0 * K, 0.8f * G0 * K);
                }
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

        if (f->predValid && steps == 1) {
            const float ey = std::fabs(f->pred.y - py), ev = std::fabs(f->pred.vy - vyW);
            f->lastErrY = ey; f->lastErrV = ev;
            if (mode == f->pred.mode) {
                f->errY[mode] = f->errY[mode] * 0.98f + ey * 0.02f;
                f->errV[mode] = f->errV[mode] * 0.98f + ev * 0.02f;
            }
            if (ey > 3.f || mode != f->pred.mode) { ++f->bigErr; f->lastBigErrX = px; }
            if (active && pl->m_holdingButtons[(int)PlayerButton::Jump] != f->held) ++f->inputMismatch;
        }
        f->predValid = false;

        if (mode == M_ROBOT) {
            if (f->prevMode == M_ROBOT && f->prevGrounded && !grounded && f->held) {
                f->robotBoost = 67;
            } else if (f->robotBoost > 0 && f->held && !grounded) {
                f->robotBoost = std::max(0, f->robotBoost - steps);
            } else {
                f->robotBoost = 0;
            }
        } else {
            f->robotBoost = 0;
        }

        f->havePrev = true;
        f->prevX = px; f->prevY = py; f->prevVyW = vyW; f->prevG = g;
        f->prevMode = mode; f->prevGrounded = grounded;
        if (f->vx <= 0.f) f->vx = 311.58f;

        const float look = static_cast<float>(Mod::get()->getSettingValue<double>("look-ahead"));
        int horizonSteps = std::clamp(static_cast<int>(look * 240.f), 120, 2400);
        const float reach = f->vx * look * 1.3f + 80.f;
        if (f->gatherReach <= 0.f || px < f->gatherX || px > f->gatherX + 150.f || reach > f->gatherReach - 150.f)
            gather(px, reach + 200.f);
        const int n = static_cast<int>(f->local.size());
        World W{f->local.data(), f->localPm.data(), n};

        S root;
        root.x = px; root.y = py; root.vy = vyW; root.vx = f->vx; root.g = g;
        root.mode = static_cast<int8_t>(mode); root.mini = mini; root.grounded = grounded;
        root.held = f->held; root.prevHeld = f->held2;
        root.robotBoost = static_cast<int16_t>(f->robotBoost);
        const float halfNow = halfOf(root);

        if (grounded && g > 0.f && (mode == M_CUBE || mode == M_ROBOT)) {
            bool onObj = false;
            for (int i = 0; i < n && !onObj; ++i) {
                const Obj& o = W.L[i];
                if ((o.kind == Kind::Solid || o.kind == Kind::Slope) && px + halfNow > o.x0 && px - halfNow < o.x1 &&
                    std::fabs((py - halfNow) - o.y1) < 3.f) onObj = true;
            }
            if (!onObj) {
                float gbm = py - halfNow;
                if (std::fabs(gbm - C.groundB) < 4.f || (!f->groundKnown && std::fabs(gbm - 90.f) < 200.f)) {
                    C.groundB = gbm; f->groundKnown = true;
                }
            }
        }
        root.gb = C.groundB;
        {
            float pc = py;
            for (auto& p : f->modePortals) { if (p.x1 <= px + 1.f) pc = (p.y0 + p.y1) * 0.5f; else break; }
            areaFromCenter(root, mode, pc);
        }

        {
            for (const Obj& o : f->local) {
                if (o.x0 > px + halfNow + 2.f) break;
                if (o.orb < 0 || o.x1 < px - halfNow - 2.f) continue;
                bool used = false;
                if (o.kind == Kind::Pad || o.kind == Kind::Portal) used = ovr(px, py, halfNow, o, 0.5f);
                else if (o.kind == Kind::Ring) used = ringUsed(o);
                if (used) setUsed(root, o.orb);
            }
            auto& ur = f->usedRings;
            ur.erase(std::remove_if(ur.begin(), ur.end(), [&](const std::pair<float, float>& p) { return p.first < px - 300.f; }), ur.end());
        }

        // DÜZELTME: simMargin çok daha güçlü
        const float baseMargin = std::clamp(static_cast<float>(Mod::get()->getSettingValue<double>("safety-margin")), 0.f, 14.f);
        float effMargin = baseMargin;
        if (f->escalation >= 2) effMargin *= 0.7f;
        if (f->escalation >= 3) effMargin *= 0.5f;
        const float simMargin = std::max(3.5f, std::min(8.f, effMargin * 0.6f));

        bool need = f->plan.empty() || f->planPos + 24 > f->plan.size() || !f->lastSurvived;
        if (!need) need = !replayAlive(root, f->plan, f->planPos, 120, W, C, simMargin);
        const int interval = std::clamp(static_cast<int>(std::ceil(f->searchMs / 0.6f)), 6, 48);
        if (!need && f->tick - f->lastSearch >= interval) need = true;
        if (!active) need = showTraj && (f->tick - f->lastDraw >= 12);

        if (need) {
            int baseKb = std::clamp(static_cast<int>(Mod::get()->getSettingValue<int64_t>("trajectory-count")), 4, 100);
            if (f->searchMs > 12.f) baseKb = std::max(8, static_cast<int>(baseKb * 12.f / f->searchMs));
            int Kb = baseKb;
            int horizon = horizonSteps;
            if (f->escalation >= 1) Kb = std::min(150, Kb * 2);
            if (f->escalation >= 2) { Kb = std::min(150, Kb * 2); horizon = std::min(2400, horizon + 480); }
            if (f->escalation >= 3) { Kb = std::min(180, Kb); horizon = 2400; }
            if (f->desperateFrames > 300) { Kb = std::max(4, baseKb / 2); horizon = std::min(horizon, 300); }
            if (f->searchMs > 25.f && f->escalation > 0) Kb = std::max(baseKb / 2, Kb / 2);

            const bool draw = showTraj && (f->tick - f->lastDraw >= 12);
            auto t0 = std::chrono::steady_clock::now();
            auto res = runSearch(root, W, C, Kb, horizon, draw, effMargin, simMargin,
                                 f->plan.empty() ? nullptr : &f->plan, f->planPos);
            float ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
            f->searchMs = f->searchMs * 0.8f + ms * 0.2f;
            f->lastSurvived = res.survived;
            f->plan = std::move(res.plan);
            f->planPos = 0;
            f->lastSearch = f->tick;
            if (res.survived) f->desperateFrames = 0;
            else f->desperateFrames++;
            if (draw) { drawLines(res.lines); f->lastDraw = f->tick; }
            if (debug) {
                log::info("SEARCH survived={} Kb={} horizon={} esc={} ms={:.2f} simM={:.1f}",
                          res.survived ? 1 : 0, Kb, horizon, f->escalation, ms, simMargin);
            }
        }

        f->lastRoot = root; f->haveLastRoot = true;

        bool want = false;
        if (!f->plan.empty() && f->planPos < f->plan.size()) {
            int ones = 0, total = 0;
            for (int i = 0; i < steps && f->planPos + i < f->plan.size(); ++i) {
                ones += f->plan[f->planPos + i];
                ++total;
            }
            want = total > 0 && (ones * 2 >= total);
        }
        if (!active) want = false;
        if (want != f->held) {
            this->handleButton(want, 1, true);
            if (want) {
                for (const Obj& o : f->local) {
                    if (o.x0 > px + halfNow + 10.f) break;
                    if (o.kind == Kind::Ring && !ringUsed(o) && ovr(px, py, halfNow, o, 0.f)) {
                        f->usedRings.push_back({o.x0, o.y0});
                        break;
                    }
                }
            }
            if (debug) {
                log::info("INPUT {} mode={} x={:.1f} y={:.1f} vy={:.2f} vx={:.1f} esc={} searchMs={:.2f}",
                          want ? "DOWN" : "UP", mode, px, py, vyW, f->vx, f->escalation, f->searchMs);
            }
        }
        f->held2 = f->held;
        f->held = want;

        {
            S p = root; p.prevHeld = root.held; p.held = want;
            f->predValid = stepSim(p, W, C, 0.f);
            f->pred = p;
        }
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
        bool newValue = !t->isToggled();
        Mod::get()->setSettingValue<bool>("bot-active", newValue);
    }
};
