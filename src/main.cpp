// Cube Autoplay - Geode mod (v0.2)
// Mantık: level objeleri önbelleğe alınır; ~1/120 sn'de bir oyuncunun fiziği
// ileri simüle edilir. Fizik değerleri (hız, yerçekimi, zıplama gücü) oyundan
// canlı ölçülerek kalibre edilir. Bot, güvenli zıplama aralığının ORTASINDA tıklar.
// Hook GJBaseGameLayer'da: normal / practice / testmode / editör playtest çalışır.

#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

using namespace geode::prelude;

namespace {
    enum class Kind : int {
        Hazard, Solid,
        YellowPad, PinkPad, RedPad,
        YellowRing, PinkRing, RedRing
    };

    struct Obj {
        float x0, x1, y0, y1;
        Kind kind;
    };

    struct Params {
        float vx;      // birim/sn
        float floorY;  // zemin üstünde küp merkezi
        float half;    // dış hitbox yarı boyutu (zemine inme, dikenler)
        float inner;   // iç hitbox yarı boyutu (blok yanına çarpma = ölüm)
        float k;       // vy birimi -> pozisyon çarpanı
        float g;       // yerçekimi (vy birimi / sn^2)
        float jumpVy;  // zıplama başlangıç hızı (vy birimi)
    };

    constexpr float kSimDt        = 1.f / 240.f; // GD fizik tick'i
    constexpr float kMaxFallV     = -15.f;
    constexpr float kSnap         = 10.f;   // küp için bloğa "yapışma" eşiği
    constexpr int   kDelayStride  = 2;      // gecikme taraması adımı (tick)
    constexpr int   kDelaySlots   = 41;     // 0..80 tick (0.33 sn)
    constexpr float kDefaultFloor = 105.f;

    float padVy(Kind k) {
        switch (k) {
            case Kind::YellowPad: return 16.f;
            case Kind::PinkPad:   return 10.4f;
            default:              return 20.4f;
        }
    }
    float ringVy(Kind k) {
        switch (k) {
            case Kind::YellowRing: return 11.18f;
            case Kind::PinkRing:   return 7.2f;
            default:               return 14.3f;
        }
    }
    bool isRing(Kind k) { return k == Kind::YellowRing || k == Kind::PinkRing || k == Kind::RedRing; }
    bool isPad(Kind k)  { return k == Kind::YellowPad || k == Kind::PinkPad || k == Kind::RedPad; }

    bool overlap(float x, float y, float h, const Obj& o, float pad = 0.f) {
        return x - h < o.x1 + pad && x + h > o.x0 - pad &&
               y - h < o.y1 + pad && y + h > o.y0 - pad;
    }

    bool supported(const std::vector<Obj>& L, float x, float y, float floorY, float half) {
        if (y <= floorY + 0.5f) return true;
        for (auto& o : L) {
            if (o.kind != Kind::Solid) continue;
            if (x + half > o.x0 && x - half < o.x1 && std::fabs((y - half) - o.y1) < 1.5f) return true;
        }
        return false;
    }

    // clickStep = -1: hiç tıklama yok. Aksi halde o adımda tıklanır.
    // Dönüş: kaç adım hayatta kaldı (steps = hiç ölmedi). pts doluysa yolu kaydeder.
    int simulate(const std::vector<Obj>& L, const Params& P, float x, float y, float vy,
                 bool grounded, int clickStep, int steps, std::vector<unsigned char>& used,
                 std::vector<CCPoint>* pts)
    {
        std::fill(used.begin(), used.end(), 0);

        for (int i = 0; i < steps; ++i) {
            const float prevY = y;

            if (grounded && !supported(L, x, y, P.floorY, P.half)) grounded = false;

            if (i == clickStep) {
                if (grounded) {
                    vy = P.jumpVy; grounded = false;
                } else {
                    for (size_t j = 0; j < L.size(); ++j) {
                        if (!isRing(L[j].kind) || used[j]) continue;
                        if (overlap(x, y, P.half, L[j], 8.f)) {
                            vy = ringVy(L[j].kind); used[j] = 1; break;
                        }
                    }
                }
            }

            if (!grounded) {
                vy -= P.g * kSimDt;
                if (vy < kMaxFallV) vy = kMaxFallV;
                y += vy * P.k * kSimDt;
            }
            x += P.vx * kSimDt;

            if (y <= P.floorY) { y = P.floorY; vy = 0.f; grounded = true; }
            if (pts && (i & 1) == 0) pts->push_back(ccp(x, y));

            for (size_t j = 0; j < L.size(); ++j) {
                const Obj& o = L[j];
                switch (o.kind) {
                    case Kind::Solid:
                        if (overlap(x, y, P.half, o)) {
                            bool above = (prevY - P.half) >= o.y1 - kSnap;
                            if (vy <= 0.f && above) {
                                y = o.y1 + P.half; vy = 0.f; grounded = true;
                            } else if (vy > 0.f && (prevY + P.half) <= o.y0 + kSnap) {
                                y = o.y0 - P.half; vy = 0.f;
                            } else if (overlap(x, y, P.inner, o)) {
                                return i; // yandan çarpma
                            }
                        }
                        break;
                    case Kind::Hazard:
                        if (overlap(x, y, P.half, o)) return i;
                        break;
                    default:
                        if (isPad(o.kind) && !used[j] && overlap(x, y, P.half, o)) {
                            vy = padVy(o.kind); grounded = false; used[j] = 1;
                        }
                        break;
                }
            }
        }
        return steps;
    }

    float speedFallback(float s) {
        static const float table[][2] = {
            {0.7f, 251.16f}, {0.9f, 311.58f}, {1.1f, 387.42f}, {1.3f, 468.f}, {1.6f, 576.f}
        };
        for (auto& t : table) if (std::fabs(t[0] - s) < 0.05f) return t[1];
        return s * 346.2f;
    }
}

class $modify(CubeBotGJBGL, GJBaseGameLayer) {
    struct Fields {
        std::vector<Obj> objs;
        std::vector<float> prefMax;
        size_t cachedCount = static_cast<size_t>(-1);

        float acc = 0.f;
        bool pressed = false;
        float floorY = kDefaultFloor;
        std::vector<Obj> local;
        std::vector<unsigned char> used;

        // Canlı kalibrasyon
        bool havePrev = false;
        float prevX = 0.f, prevY = 0.f, prevVy = 0.f;
        bool prevGrounded = true;
        float vx = 0.f;
        float k = 60.f;
        float g = 46.3f;      // başlangıç tahmini; oyundan ölçülünce güncellenir
        float jumpVy = 9.94f; // başlangıç tahmini; ilk zıplamada ölçülür
        bool awaitJump = false;
        float sincePress = 0.f;

        long tick = 0;
        long planTick = -1;
        long pressTick = -1000;

        CCDrawNode* drawNode = nullptr;
    };

    void rebuildCache() {
        auto f = m_fields.self();
        f->objs.clear();
        f->prefMax.clear();
        f->cachedCount = m_objects ? m_objects->count() : 0;
        if (!m_objects) return;

        for (auto obj : CCArrayExt<GameObject*>(m_objects)) {
            if (!obj) continue;
            Kind k;
            switch (obj->m_objectType) {
                case GameObjectType::Hazard:         k = Kind::Hazard; break;
                case GameObjectType::Solid:          k = Kind::Solid; break;
                case GameObjectType::YellowJumpPad:  k = Kind::YellowPad; break;
                case GameObjectType::PinkJumpPad:    k = Kind::PinkPad; break;
                case GameObjectType::RedJumpPad:     k = Kind::RedPad; break;
                case GameObjectType::YellowJumpRing: k = Kind::YellowRing; break;
                case GameObjectType::PinkJumpRing:   k = Kind::PinkRing; break;
                case GameObjectType::RedJumpRing:    k = Kind::RedRing; break;
                default: continue;
            }
            CCRect r = obj->getObjectRect();
            f->objs.push_back({r.getMinX(), r.getMaxX(), r.getMinY(), r.getMaxY(), k});
        }
        std::sort(f->objs.begin(), f->objs.end(),
                  [](const Obj& a, const Obj& b) { return a.x0 < b.x0; });
        float m = -1e9f;
        f->prefMax.reserve(f->objs.size());
        for (auto& o : f->objs) { m = std::max(m, o.x1); f->prefMax.push_back(m); }
    }

    void gather(float px, float py, float reachX) {
        auto f = m_fields.self();
        f->local.clear();
        float minX = px - 45.f, maxX = px + reachX + 45.f;
        size_t start = std::lower_bound(f->prefMax.begin(), f->prefMax.end(), minX) - f->prefMax.begin();
        for (size_t i = start; i < f->objs.size() && f->objs[i].x0 <= maxX; ++i) {
            const Obj& o = f->objs[i];
            if (o.x1 < minX) continue;
            if (o.y0 > py + 320.f) continue;
            f->local.push_back(o);
            if (f->local.size() >= 400) break;
        }
        f->used.assign(f->local.size(), 0);
    }

    void clearPaths() {
        auto f = m_fields.self();
        if (f->drawNode) f->drawNode->clear();
    }

    void drawPaths(const std::vector<CCPoint>& a, ccColor4F ca,
                   const std::vector<CCPoint>& b, ccColor4F cb) {
        auto f = m_fields.self();
        if (!f->drawNode) {
            if (!m_objectLayer) return;
            f->drawNode = CCDrawNode::create();
            m_objectLayer->addChild(f->drawNode, 10000);
        }
        f->drawNode->clear();
        for (size_t i = 1; i < a.size(); ++i) f->drawNode->drawSegment(a[i - 1], a[i], 1.4f, ca);
        for (size_t i = 1; i < b.size(); ++i) f->drawNode->drawSegment(b[i - 1], b[i], 1.4f, cb);
    }

    void releaseIfPressed() {
        auto f = m_fields.self();
        if (f->pressed) { this->handleButton(false, 1, true); f->pressed = false; }
    }

    void resetBotState() {
        auto f = m_fields.self();
        f->havePrev = false;
        f->planTick = -1;
        f->awaitJump = false;
        f->pressed = false;
        f->acc = 0.f;
    }

    void runBot(float dt) {
        auto f = m_fields.self();
        auto pl = m_player1;
        if (!pl) return;

        const bool active = Mod::get()->getSettingValue<bool>("bot-active");
        const bool showTraj = Mod::get()->getSettingValue<bool>("show-trajectory");
        if (!active) releaseIfPressed();
        if (!active && !showTraj) { f->havePrev = false; clearPaths(); return; }
        if (!showTraj) clearPaths();
        if (pl->m_isDead) { resetBotState(); clearPaths(); return; }

        bool cube = !(pl->m_isShip || pl->m_isBird || pl->m_isBall || pl->m_isDart ||
                      pl->m_isRobot || pl->m_isSpider || pl->m_isSwing);
        if (!cube || pl->m_isUpsideDown) { releaseIfPressed(); f->havePrev = false; clearPaths(); return; }

        if (!m_objects || m_objects->count() != f->cachedCount) rebuildCache();

        f->acc += dt;
        f->sincePress += dt;
        if (f->acc < kSimDt * 0.99f) return;
        const float dtp = f->acc;
        f->acc = 0.f;
        f->tick++;

        const float px = pl->getPositionX();
        const float py = pl->getPositionY();
        const float vy = static_cast<float>(pl->m_yVelocity);
        const bool grounded = pl->m_isOnGround;

        // Yeniden doğma / geri atlama: durumu sıfırla
        if (f->havePrev && px < f->prevX - 1.f) resetBotState();

        // --- Kalibrasyon ---
        if (f->havePrev && dtp > 0.f) {
            float vxm = (px - f->prevX) / dtp;
            if (vxm > 150.f && vxm < 800.f) {
                f->vx = f->vx > 0.f ? f->vx * 0.7f + vxm * 0.3f : vxm;
            }
            if (!grounded && !f->prevGrounded) {
                float avg = 0.5f * (vy + f->prevVy);
                if (std::fabs(avg) > 1.f) {
                    float km = (py - f->prevY) / (avg * dtp);
                    if (km > 30.f && km < 120.f) f->k = f->k * 0.9f + km * 0.1f;
                }
                float gm = (f->prevVy - vy) / dtp;
                if (gm > 30.f && gm < 90.f) f->g = f->g * 0.9f + gm * 0.1f;
            }
        }
        if (f->awaitJump) {
            if (!grounded && vy > 3.f) {
                float jv = vy + f->g * f->sincePress;
                if (jv > 6.f && jv < 15.f) f->jumpVy = jv;
            }
            f->awaitJump = false;
        }
        f->havePrev = true;
        f->prevX = px; f->prevY = py; f->prevVy = vy; f->prevGrounded = grounded;
        if (f->vx <= 0.f) f->vx = speedFallback(pl->m_playerSpeed);

        // Tıklamayı 2 tick basılı tut, sonra bırak (orb'lar için taze tıklama gerekir)
        if (f->pressed) {
            if (f->tick - f->pressTick >= 2) releaseIfPressed();
            return;
        }

        // --- Planlama ---
        float scale = pl->m_vehicleSize > 0.f ? pl->m_vehicleSize : 1.f;
        CCRect pr = pl->getObjectRect();
        float half = (pr.size.width > 4.f && pr.size.width < 40.f) ? pr.size.width * 0.5f : 15.f * scale;

        float look = static_cast<float>(Mod::get()->getSettingValue<double>("look-ahead"));
        int steps = std::max(60, static_cast<int>(look / kSimDt));

        gather(px, py, f->vx * look + 60.f);

        if (grounded && !supported(f->local, px, py, -1e9f, half)) f->floorY = py;

        Params P;
        P.vx = f->vx; P.floorY = f->floorY; P.half = half; P.inner = half * 0.3f;
        P.k = f->k; P.g = f->g; P.jumpVy = f->jumpVy;

        auto& L = f->local;
        auto& used = f->used;

        const bool draw = showTraj && (f->tick % 4 == 0);
        const ccColor4F cGreen  = ccc4f(0.2f, 1.0f, 0.3f, 0.95f);
        const ccColor4F cRed    = ccc4f(1.0f, 0.2f, 0.2f, 0.95f);
        const ccColor4F cYellow = ccc4f(1.0f, 0.9f, 0.2f, 0.7f);
        const ccColor4F cOrange = ccc4f(1.0f, 0.55f, 0.1f, 0.95f);
        std::vector<CCPoint> ptsA, ptsB;

        // Gelecek A: zıplamazsam nereye giderim?
        const int aliveBase = simulate(L, P, px, py, vy, grounded, -1, steps, used, draw ? &ptsA : nullptr);
        if (aliveBase >= steps) {
            // Zıplamadan da ölmüyorum: bu versiyonu kullan (tıklama yok)
            f->planTick = -1;
            if (draw) {
                // Bilgi için: "şimdi zıplasam" yolu (sarı)
                simulate(L, P, px, py, vy, grounded, 0, steps, used, &ptsB);
                drawPaths(ptsA, cGreen, ptsB, cYellow);
            }
            return;
        }

        // Gelecek B(d): d tick sonra zıplarsam nereye giderim?
        std::array<int, kDelaySlots> alive{};
        for (int i = 0; i < kDelaySlots; ++i) {
            alive[i] = simulate(L, P, px, py, vy, grounded, i * kDelayStride, steps, used, nullptr);
        }

        int ia = -1;
        for (int i = 0; i < kDelaySlots; ++i) if (alive[i] >= steps) { ia = i; break; }
        int da, db;
        if (ia >= 0) {
            int ib = ia;
            while (ib + 1 < kDelaySlots && alive[ib + 1] >= steps) ++ib;
            da = ia * kDelayStride;
            db = ib * kDelayStride;
        } else {
            // İki versiyon da ölüyor: en uzun yaşayanı seç
            int best = -1, bestAlive = aliveBase;
            for (int i = 0; i < kDelaySlots; ++i) {
                if (alive[i] > bestAlive) { bestAlive = alive[i]; best = i; }
            }
            if (best < 0) {
                if (draw) drawPaths(ptsA, cRed, ptsB, cRed);
                return; // zıplamak daha iyi değil
            }
            da = db = best * kDelayStride;
        }

        // Güvenli aralığın ortasında tıkla (model hatasına karşı tampon)
        long planDelay = f->planTick - f->tick;
        bool planValid = f->planTick >= 0 && planDelay >= da && planDelay <= db;
        if (!planValid) {
            f->planTick = f->tick + (da + db) / 2;
            planDelay = f->planTick - f->tick;
        }

        if (draw) {
            int drawDelay = static_cast<int>(std::max(0L, planDelay));
            int aliveJ = simulate(L, P, px, py, vy, grounded, drawDelay, steps, used, &ptsB);
            drawPaths(ptsA, cRed, ptsB, aliveJ >= steps ? cGreen : cOrange);
        }

        if (planDelay <= 0 && active) {
            this->handleButton(true, 1, true);
            f->pressed = true;
            f->pressTick = f->tick;
            f->planTick = -1;
            if (grounded) { f->awaitJump = true; f->sincePress = 0.f; }
            if (Mod::get()->getSettingValue<bool>("debug-log")) {
                log::info("JUMP x={:.1f} y={:.1f} vy={:.2f} window=[{},{}] vx={:.1f} g={:.2f} k={:.1f} jumpVy={:.2f} half={:.1f}",
                          px, py, vy, da, db, f->vx, f->g, f->k, f->jumpVy, half);
            }
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

        auto label = CCLabelBMFont::create("Cube Autoplay", "bigFont.fnt");
        label->setScale(0.45f);
        label->setAnchorPoint({0.f, 0.5f});
        label->setPosition({52.f, 30.f});

        this->addChild(menu, 100);
        this->addChild(label, 100);
    }

    void onToggleBot(CCObject* sender) {
        auto t = static_cast<CCMenuItemToggler*>(sender);
        // Geode'da callback anında isToggled() eski durumu verir.
        bool newValue = !t->isToggled();
        Mod::get()->setSettingValue<bool>("bot-active", newValue);
    }
};
