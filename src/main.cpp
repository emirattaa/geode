#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <cocos2d.h>
#include <vector>
#include <cmath>

using namespace geode::prelude;

class $modify(MyPlayLayer, PlayLayer) {
    struct Fields {
        cocos2d::CCDrawNode* m_trajectoryNode = nullptr;
        bool m_isHoldingDash = false;
        GameObject* m_currentDashOrb = nullptr;
        int m_jumpCooldown = 0;
    };

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) {
            return false;
        }

        // Yörünge çizimi için CCDrawNode oluşturulup sahneye ekleniyor
        m_fields->m_trajectoryNode = cocos2d::CCDrawNode::create();
        if (m_fields->m_trajectoryNode && m_objectLayer) {
            m_objectLayer->addChild(m_fields->m_trajectoryNode, 999);
        }

        return true;
    }

    void update(float dt) {
        PlayLayer::update(dt);

        if (!m_player1 || !m_fields->m_trajectoryNode) {
            return;
        }

        // 1. Otomatik Oynama (Spike / Orb / Dash Algılama)
        processAutoPlay();

        // 2. Yörünge Çizgisi Hesaplama ve Çizimi
        renderTrajectory();
    }

    void processAutoPlay() {
        if (!m_player1 || !m_objects) return;

        cocos2d::CCPoint playerPos = m_player1->getPosition();
        bool shouldJump = false;
        bool inDashOrbRange = false;

        if (m_fields->m_jumpCooldown > 0) {
            m_fields->m_jumpCooldown--;
        }

        CCObject* obj = nullptr;
        CCARRAY_FOREACH(m_objects, obj) {
            auto gameObj = typeinfo_cast<GameObject*>(obj);
            if (!gameObj) continue;

            cocos2d::CCPoint objPos = gameObj->getPosition();
            float distX = objPos.x - playerPos.x;
            float distY = std::abs(objPos.y - playerPos.y);
            float totalDist = std::sqrt(distX * distX + (objPos.y - playerPos.y) * (objPos.y - playerPos.y));

            int objID = gameObj->m_objectID;

            // Standart Orblar (Sarı: 36, Pembe: 141, Kırmızı: 1331, Mavi: 84, Yeşil Zıplama: 1022, Örümcek: 1334, Siyah: 1705)
            bool isStandardOrb = (objID == 36 || objID == 141 || objID == 1331 || objID == 84 || objID == 1022 || objID == 1334 || objID == 1705);
            
            // Basılı tutulan Dash Orbları (Yeşil Dash: 1704, Pembe Dash: 1751)
            bool isDashOrb = (objID == 1704 || objID == 1751);

            // 1. Orb Kontrolleri
            if (isStandardOrb) {
                if (totalDist <= 45.0f && distX >= -10.0f && distX <= 35.0f) {
                    if (m_fields->m_jumpCooldown == 0) {
                        shouldJump = true;
                    }
                }
            } else if (isDashOrb) {
                if (totalDist <= 50.0f) {
                    inDashOrbRange = true;
                    m_fields->m_currentDashOrb = gameObj;
                }
            }

            // 2. Diken (Spike) ve Engel Kontrolleri
            bool isSpike = (gameObj->m_objectType == GameObjectType::Hazard || objID == 8 || objID == 9 || objID == 39 || objID == 103 || objID == 361);
            if (isSpike) {
                if (distX > 0.0f && distX <= 38.0f && distY <= 30.0f && m_player1->m_isOnGround) {
                    if (m_fields->m_jumpCooldown == 0) {
                        shouldJump = true;
                    }
                }
            }
        }

        // Dash Orbları İçin Basılı Tutma Mantığı
        if (inDashOrbRange) {
            if (!m_fields->m_isHoldingDash) {
                m_player1->pushButton(PlayerButton::Jump);
                m_fields->m_isHoldingDash = true;
            }
        } else {
            if (m_fields->m_isHoldingDash) {
                m_player1->releaseButton(PlayerButton::Jump);
                m_fields->m_isHoldingDash = false;
                m_fields->m_currentDashOrb = nullptr;
            }
        }

        // Tek Seferlik Zıplama/Orb Tetikleme Mantığı
        if (shouldJump && !m_fields->m_isHoldingDash) {
            m_player1->pushButton(PlayerButton::Jump);
            m_player1->releaseButton(PlayerButton::Jump);
            m_fields->m_jumpCooldown = 6;
        }
    }

    void renderTrajectory() {
        m_fields->m_trajectoryNode->clear();

        if (!m_player1) return;

        cocos2d::CCPoint currentPos = m_player1->getPosition();
        double xVel = m_player1->m_xVelocity;
        double yVel = m_player1->m_yVelocity;

        float speedX = (xVel != 0.0) ? static_cast<float>(xVel) : 10.3f;
        float velY = static_cast<float>(yVel);
        float gravity = -0.95f;

        cocos2d::CCPoint simPos = currentPos;
        int simSteps = 60;
        float dtStep = 1.0f / 60.0f;

        for (int i = 0; i < simSteps; i++) {
            cocos2d::CCPoint nextPos;
            nextPos.x = simPos.x + (speedX * 30.0f * dtStep);
            
            velY += gravity;
            nextPos.y = simPos.y + velY;

            // Zemin Hizası Kontrolü
            if (nextPos.y < 105.0f && m_player1->m_isOnGround) {
                nextPos.y = simPos.y;
                velY = 0.0f;
            }

            // Dash durumuna göre yörünge rengi değişimi
            cocos2d::ccColor4F lineColor = ccc4f(0.0f, 1.0f, 0.4f, 0.8f);
            if (m_fields->m_isHoldingDash) {
                lineColor = ccc4f(1.0f, 0.3f, 0.8f, 0.9f);
            }

            m_fields->m_trajectoryNode->drawSegment(simPos, nextPos, 1.5f, lineColor);
            m_fields->m_trajectoryNode->drawDot(nextPos, 2.5f, ccc4f(1.0f, 1.0f, 1.0f, 0.6f));

            simPos = nextPos;
        }
    }
};
