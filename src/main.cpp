// Cube Autoplay - Geode mod
// Mantık: level objeleri bir kez önbelleğe alınır; her ~1/120 sn'de oyuncunun
// fiziği ileriye simüle edilir. Bot, hayatta kalan en geç zıplama anını seçer.
// Hook GJBaseGameLayer'da olduğu için normal / practice / testmode / editör
// playtest hepsinde çalışır.

#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <algorithm>
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

    // Fizik sabitleri (yaklaşık, birim/saniye). Gerekirse ayarla.
    constexpr float kSimDt     = 1.f / 120.f;
    constexpr float kGravity   = 0.958199f * 3600.f;
    constexpr float kJumpV     = 11.180032f * 60.f;
    constexpr float kMaxFall   = -15.f * 60.f;
    constexpr float kHalf      = 15.f;   // küp yarı boyutu (katı objeler için)
    constexpr float kHazardHalf = 8.f;   // diken/testere için daha küçük hitbox
    constexpr int   kMaxDelay  = 40;     // en fazla 40 * 1/120 sn sonrası için zıplama dene
    constexpr float kDefaultFloor = 105.f;

    float padVel(Kind k) {
        switch (k) {
            case Kind::YellowPad: return 16.f * 60.f;
            case Kind::PinkPad:   return 10.4f * 60.f;
            default:              return 20.4f * 60.f; // red pad
        }
    }
    float ringVel(Kind k) {
        switch (k) {
            case Kind::YellowRing: return 11.18f * 60.f;
            case Kind::PinkRing:   return 7.2f * 60.f;
            default:               return 14.3f * 60.f; // red ring
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
    bool survive(const std::vector<Obj>& L, float x, float y, float vy, bool grounded,
                 float vx, float floorY, float scale, int clickStep, int steps,
                 std::vector<unsigned char>& used)
    {
        std::fill(used.begin(), used.end(), 0);
        const float half = kHalf * scale;
        const float hz = kHazardHalf * scale;

        for (int i = 0; i < steps; ++i) {
            float prevY = y;

            if (grounded && !supported(L, x, y, floorY, half)) grounded = false;

            if (i == clickStep) {
                if (grounded) {
                    vy = kJumpV; grounded = false;
                } else {
                    for (size_t j = 0; j < L.size(); ++j) {
                        if (!isRing(L[j].kind) || used[j]) continue;
                        if (overlap(x, y, half, L[j], 8.f)) {
                            vy = ringVel(L[j].kind); used[j] = 1; break;
                        }
                    }
                }
            }

            if (!grounded) {
                vy -= kGravity * kSimDt;
                if (vy < kMaxFall) vy = kMaxFall;
                y += vy * kSimDt;
            }
            x += vx * kSimDt;

            if (y <= floorY) { y = floorY; vy = 0.f; grounded = true; }

            for (size_t j = 0; j < L.size(); ++j) {
                const Obj& o = L[j];
                switch (o.kind) {
                    case Kind::Solid:
                        if (overlap(x, y, half, o)) {
                            bool wasAbove = (prevY - half) >= o.y1 - 4.f;
                            if (vy <= 0.f && wasAbove) {
                                y = o.y1 + half; vy = 0.f; grounded = true;
                            } else {
                                return false;
                            }
                        }
                        break;
                    case Kind::Hazard:
                        if (overlap(x, y, hz, o)) return false;
                        break;
                    default:
                        if (isPad(o.kind) && !used[j] && overlap(x, y, half, o)) {
                            vy = padVel(o.kind); grounded = false; used[j] = 1;
                        }
                        break;
                }
            }
        }
        return true;
    }

    float speedToUnits(float s) {
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
                case GameObjectType::Hazard:        k = Kind::Hazard; break;
                case GameObjectType::Solid:         k = Kind::Solid; break;
                case GameObjectType::YellowJumpPad: k = Kind::YellowPad; break;
                case GameObjectType::PinkJumpPad:   k = Kind::PinkPad; break;
                case GameObjectType::RedJumpPad:    k = Kind::RedPad; break;
                case GameObjectType::YellowJumpRing: k = Kind::YellowRing; break;
                case GameObjectType::PinkJumpRing:  k = Kind::PinkRing; break;
                case GameObjectType::RedJumpRing:   k = Kind::RedRing; break;
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

    void releaseIfPressed() {
        auto f = m_fields.self();
        if (f->pressed) { this->handleButton(false, 1, true); f->pressed = false; }
    }

    void runBot(float dt) {
        auto f = m_fields.self();
        auto pl = m_player1;
        if (!pl) return;

        if (!Mod::get()->getSettingValue<bool>("bot-active")) {
            releaseIfPressed();
            return;
        }
        if (pl->m_isDead) { f->pressed = false; return; }

        // Sadece cube: diğer modlarda bot bir şey yapmaz.
        bool cube = !(pl->m_isShip || pl->m_isBird || pl->m_isBall || pl->m_isDart ||
                      pl->m_isRobot || pl->m_isSpider || pl->m_isSwing);
        if (!cube || pl->m_isUpsideDown) { releaseIfPressed(); return; }

        if (!m_objects || m_objects->count() != f->cachedCount) rebuildCache();

        f->acc += dt;
        if (f->acc < kSimDt) return;
        f->acc = 0.f;

        // Önceki tıklamayı bırak (orb'lar için taze tıklama gerekir).
        if (f->pressed) { releaseIfPressed(); return; }

        float px = pl->getPositionX();
        float py = pl->getPositionY();
        float vx = speedToUnits(pl->m_playerSpeed);
        float vy = static_cast<float>(pl->m_yVelocity) * 60.f;
        bool grounded = pl->m_isOnGround;
        float scale = pl->m_vehicleSize > 0.f ? pl->m_vehicleSize : 1.f;

        float look = static_cast<float>(Mod::get()->getSettingValue<double>("look-ahead"));
        int steps = std::max(30, static_cast<int>(look / kSimDt));

        gather(px, py, vx * look + 60.f);

        // Yerde duruyor ve altında katı yoksa zemin yüksekliğini öğren.
        if (grounded && !supported(f->local, px, py, -1e9f, kHalf * scale)) {
            f->floorY = py;
        }

        auto& L = f->local;
        auto& used = f->used;

        // Tıklamazsak hayatta kalıyor muyuz?
        if (survive(L, px, py, vy, grounded, vx, f->floorY, scale, -1, steps, used)) return;

        // Hangi gecikmeyle zıplarsak hayatta kalırız?
        bool ok[kMaxDelay + 2];
        for (int d = 0; d <= kMaxDelay + 1; ++d) {
            ok[d] = survive(L, px, py, vy, grounded, vx, f->floorY, scale, d, steps, used);
        }
        int pick = -1;
        for (int d = 0; d <= kMaxDelay; ++d) {
            if (ok[d] && !ok[d + 1]) { pick = d; break; }
        }
        if (pick == -1) {
            // Kurtuluş yok: yine de yerdeyse dene.
            if (grounded) pick = 0;
        }
        if (pick == 0) {
            this->handleButton(true, 1, true);
            f->pressed = true;
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

        auto win = CCDirector::get()->getWinSize();
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
        (void)win;
    }

    void onToggleBot(CCObject* sender) {
        auto t = static_cast<CCMenuItemToggler*>(sender);
        // Geode'da callback anında isToggled() eski durumu verir.
        bool newValue = !t->isToggled();
        Mod::get()->setSettingValue<bool>("bot-active", newValue);
    }
};
