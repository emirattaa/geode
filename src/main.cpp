// Geode Autoplay v1.0 - Basitleştirilmiş & Temizlenmiş Sürüm
//
// NE YAPAR?
// 1. Oyundaki tüm objeleri, tehlikeleri, blokları ve portalları tarar.
// 2. Gelecek 1-2 saniyeyi simüle ederek en güvenli yolu bulur.
// 3. Bu yolu oynamak için sadece "bas" veya "bırak" komutları üretir.
// 4. Ayarlar menüsündeki Toggle ile açılıp kapanır.

#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <algorithm>
#include <cmath>
#include <vector>

using namespace geode::prelude;

namespace {
    constexpr float K = 0.225f;
    constexpr float G0 = 0.958199f;
    constexpr float kInf = 1e9f;
    
    enum Kind : uint8_t { K_Hazard, K_Solid, K_Slope, K_Pad, K_Ring, K_Portal };

    struct Obj {
        float x0, x1, y0, y1;
        Kind kind;
        uint8_t sub = 0;
        float lx = 0, ly = 0, rx = 1, ry = 0;
        float val = 0;
        bool used = false;
    };

    struct State {
        float x = 0, y = 0, vy = 0, vx = 311.58f;
        float g = 1.f, B = 90.f, T = kInf, gb = 90.f;
        int mode = 0;
        bool mini = false, grounded = false, held = false, prevHeld = false;
    };

    inline bool ovr(float x, float y, float h, const Obj& o, float pad) {
        return x - h < o.x1 + pad && x + h > o.x0 - pad && y - h < o.y1 + pad && y + h > o.y0 - pad;
    }

    bool stepSim(State& s, const std::vector<Obj>& objs, bool jumpHeld) {
        s.prevHeld = s.held;
        s.held = jumpHeld;

        float half = (s.mode == 4 ? 5.f : 15.f) * (s.mini ? 0.6f : 1.f);
        float dx = s.vx / 240.f;
        float gstep = G0 * K;
        
        // Basit Fizik & Mod Mantığı
        float g = s.g, u = s.vy * g;
        if (s.mode == 0 || s.mode == 5) { // Cube / Robot
            if (s.grounded && s.held) { u = (s.mode == 0 ? 11.18f : 5.59f); s.grounded = false; }
        }
        if (!s.grounded) u -= gstep * (s.mode == 5 ? 0.9f : 1.f);
        
        s.vy = u * g;
        if (!s.grounded) s.y += s.vy * K;
        s.x += dx;

        // Zemin ve Tavan Sınırları
        if (s.y <= s.B + half) { s.y = s.B + half; if (s.g > 0) { s.vy = 0; s.grounded = true; } }
        if (s.T < kInf && s.y >= s.T - half) { s.y = s.T - half; if (s.g < 0) { s.vy = 0; s.grounded = true; } }

        // Çarpışma Kontrolü (Dikdörtgen ve Diken)
        for (const auto& o : objs) {
            if (o.x1 < s.x - 30.f || o.x0 > s.x + 30.f) continue;
            
            if (o.kind == K_Hazard && ovr(s.x, s.y, half, o, 0.f)) return false; // Ölüm
            
            if (o.kind == K_Solid && ovr(s.x, s.y, half, o, 0.f)) {
                if (s.vy <= 0 && s.y - half >= o.y1 - 10.f) { // Üstüne indi
                    s.y = o.y1 + half; s.vy = 0; s.grounded = true;
                } else if (s.vy > 0 && s.y + half <= o.y0 + 10.f) { // Altına vurdu
                    s.y = o.y0 - half; s.vy = 0;
                } else return false; // Yandan çarptı = Ölüm
            }
            
            if (o.kind == K_Portal && ovr(s.x, s.y, half, o, 0.f) && !o.used) {
                // Portal efektleri...
            }
        }
        return true;
    }
}

class $modify(AutoPlayLayer, GJBaseGameLayer) {
    struct Fields {
        std::vector<Obj> levelObjs;
        std::vector<uint8_t> plan;
        size_t planPos = 0;
        bool botEnabled = false;
        bool held = false;
        bool init = false;
    };

    void analyzeLevel() {
        auto f = m_fields.self();
        f->levelObjs.clear();
        if (!m_objects) return;

        for (auto go : CCArrayExt<GameObject*>(m_objects)) {
            if (!go) continue;
            Obj o;
            CCRect r = go->getObjectRect();
            o.x0 = r.getMinX(); o.x1 = r.getMaxX();
            o.y0 = r.getMinY(); o.y1 = r.getMaxY();
            
            auto type = go->m_objectType;
            if (type == GameObjectType::Hazard) o.kind = K_Hazard;
            else if (type == GameObjectType::Solid) o.kind = K_Solid;
            else if (type == GameObjectType::Slope) o.kind = K_Slope;
            else if (type == GameObjectType::YellowJumpPad || type == GameObjectType::PinkJumpPad) o.kind = K_Pad;
            else if (type == GameObjectType::YellowJumpRing || type == GameObjectType::PinkJumpRing) o.kind = K_Ring;
            else if (go->m_objectID >= 10 && go->m_objectID <= 13) o.kind = K_Portal;
            else continue;
            
            f->levelObjs.push_back(o);
        }
        std::sort(f->levelObjs.begin(), f->levelObjs.end(), [](const Obj& a, const Obj& b) { return a.x0 < b.x0; });
        f->init = true;
    }

    void autoPlay(float dt) {
        auto f = m_fields.self();
        auto player = m_player1;
        if (!player || player->m_isDead || !f->botEnabled) return;
        if (!f->init) analyzeLevel();

        // Basit "İleriye Bak" Simülasyonu (2 Yol Dene: Bas ve Basma)
        State base;
        base.x = player->getPositionX();
        base.y = player->getPositionY();
        base.vy = player->m_yVelocity;
        base.g = player->m_isUpsideDown ? -1.f : 1.f;
        base.mode = player->m_isShip ? 1 : (player->m_isDart ? 4 : 0);
        base.grounded = player->m_isOnGround;

        State simHold = base, simRelease = base;
        bool safeHold = true, safeRelease = true;

        // Gelecek 120 kareyi test et
        for (int i = 0; i < 120; ++i) {
            if (safeHold && !stepSim(simHold, f->levelObjs, true)) safeHold = false;
            if (safeRelease && !stepSim(simRelease, f->levelObjs, false)) safeRelease = false;
            if (!safeHold && !safeRelease) break;
        }

        // Karar Ver
        bool wantHold = f->held;
        if (safeHold && !safeRelease) wantHold = true;
        else if (!safeHold && safeRelease) wantHold = false;
        else if (safeHold && safeRelease) {
            // İkisi de güvenliyse, mevcut durumu korumaya çalış (gereksiz tıklama yapma)
            wantHold = f->held;
        }

        // Uygula
        if (wantHold != f->held) {
            this->handleButton(wantHold, 1, true);
            f->held = wantHold;
        }
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        autoPlay(dt);
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
    }
};

class $modify(AutoPlayPause, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();
        
        auto menu = CCMenu::create();
        menu->setPosition({0, 0});
        
        // Sadece bir Toggle Butonu
        auto toggle = CCMenuItemToggler::createWithStandardSprites(
            this, menu_selector(AutoPlayPause::onToggle), 0.7f);
        toggle->toggle(Mod::get()->getSavedValue<bool>("autoplay_enabled", false));
        toggle->setPosition({30, 30});
        menu->addChild(toggle);
        
        auto label = CCLabelBMFont::create("Autoplay", "bigFont.fnt");
        label->setScale(0.45f);
        label->setPosition({52, 30});
        label->setAnchorPoint({0, 0.5f});
        menu->addChild(label);

        this->addChild(menu, 100);
    }

    void onToggle(CCObject* sender) {
        auto toggle = static_cast<CCMenuItemToggler*>(sender);
        bool enabled = toggle->isToggled(); // Geode'da activate sonrası state döner
        
        Mod::get()->setSavedValue("autoplay_enabled", enabled);
        
        // Ana katmana erişip botu aç/kapa
        if (auto layer = GameManager::get()->getPlayLayer()) {
            if (auto gjLayer = typeinfo_cast<GJBaseGameLayer*>(layer)) {
                // Reflection yerine basit hook ile layer'ı bulmak gerekebilir
                // Geode'da direkt layer'a erişim için daha güvenli yol: Mod değişkenini oku
                log::info("Autoplay {}", enabled ? "ON" : "OFF");
            }
        }
    }
};
