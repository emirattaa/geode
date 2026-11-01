#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/LevelEditorLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/EffectGameObject.hpp>
#include <Geode/modify/GameObject.hpp>
#include <Geode/modify/HardStreak.hpp>
#include "TrajectoryNode.hpp"

using namespace geode::prelude;

class $modify(TrajPlayLayer, PlayLayer)
{
    void createObjectsFromSetupFinished()
    {
        PlayLayer::createObjectsFromSetupFinished();
        m_objectLayer->addChild(TrajectoryNode::create(this));
    }

    void destroyPlayer(PlayerObject* player, GameObject* object)
    {
        if (TrajectoryNode::active() && player)
        {
            player->m_isDead = true;
            return;
        }
        PlayLayer::destroyPlayer(player, object);
    }

    void playEndAnimationToPos(cocos2d::CCPoint p0)
    {
        if (!TrajectoryNode::active())
            PlayLayer::playEndAnimationToPos(p0);
    }
};

class $modify(TrajEditor, LevelEditorLayer)
{
    bool init(GJGameLevel* level, bool noUI)
    {
        if (!LevelEditorLayer::init(level, noUI)) return false;
        m_objectLayer->addChild(TrajectoryNode::create(this));
        return true;
    }
};

class $modify(TrajBGL, GJBaseGameLayer)
{
    // simulasyon sirasinda sadece portal / pad / ring etkilerini elle uygula
    void customCollisionCheck(PlayerObject* player, GameObject* obj)
    {
        bool intersects;
        if (obj->m_orientedBox && obj->m_shouldUseOuterOb)
            intersects = obj->m_orientedBox->overlaps(player->m_orientedBox);
        else
            intersects = player->getObjectRect().intersectsRect(obj->getObjectRect());
        if (!intersects) return;

        if (obj->m_objectID == 1829) player->stopDashing();

        switch (obj->m_objectType)
        {
            case GameObjectType::CubePortal:
                player->toggleFlyMode(false, false);
                player->toggleRollMode(false, false);
                player->toggleBirdMode(false, false);
                player->toggleDartMode(false, false);
                player->toggleRobotMode(false, false);
                player->toggleSpiderMode(false, false);
                player->toggleSwingMode(false, false);
                break;
            case GameObjectType::ShipPortal:   player->toggleFlyMode(true, true); break;
            case GameObjectType::BallPortal:   player->toggleRollMode(true, true); break;
            case GameObjectType::UfoPortal:    player->toggleBirdMode(true, true); break;
            case GameObjectType::WavePortal:   player->toggleDartMode(true, true); break;
            case GameObjectType::RobotPortal:  player->toggleRobotMode(true, true); break;
            case GameObjectType::SpiderPortal: player->toggleSpiderMode(true, true); break;
            case GameObjectType::SwingPortal:  player->toggleSwingMode(true, true); break;

            case GameObjectType::NormalGravityPortal:  this->flipGravity(player, false, true); break;
            case GameObjectType::InverseGravityPortal: this->flipGravity(player, true, true); break;
            case GameObjectType::GravityTogglePortal:  this->flipGravity(player, !player->m_isUpsideDown, true); break;

            case GameObjectType::YellowJumpPad:
            case GameObjectType::PinkJumpPad:
            case GameObjectType::RedJumpPad:
            case GameObjectType::SpiderPad:
                if (static_cast<EffectGameObject*>(obj)->m_isReverse)
                    player->reversePlayer(static_cast<EffectGameObject*>(obj));
                player->bumpPlayer(this->getBumpMod(player, (int)obj->m_objectType),
                                   (int)obj->m_objectType, true, nullptr);
                break;

            case GameObjectType::YellowJumpRing:
            case GameObjectType::PinkJumpRing:
            case GameObjectType::GravityRing:
            case GameObjectType::GreenRing:
            case GameObjectType::DropRing:
            case GameObjectType::RedJumpRing:
            case GameObjectType::CustomRing:
            case GameObjectType::DashRing:
            case GameObjectType::GravityDashRing:
            case GameObjectType::SpiderOrb:
                // halkalar simulasyonda tiklanmis sayilmaz (gercek oyuncu basmadan etkilenmez)
                break;

            default: break;
        }
    }

    void collisionCheckObjects(PlayerObject* p0, gd::vector<GameObject*>* p1, int p2, float p3)
    {
        if (!TrajectoryNode::active() || !p1)
        {
            GJBaseGameLayer::collisionCheckObjects(p0, p1, p2, p3);
            return;
        }

        gd::vector<GameObject*> solids;
        for (auto obj : *p1)
        {
            bool keep = false;
            switch (obj->m_objectType)
            {
                case GameObjectType::Solid:
                case GameObjectType::Hazard:
                case GameObjectType::AnimatedHazard:
                case GameObjectType::Slope:
                    keep = true; break;
                default: break;
            }
            if (keep) solids.push_back(obj);
            else this->customCollisionCheck(p0, obj);
        }
        GJBaseGameLayer::collisionCheckObjects(p0, &solids, (int)solids.size(), p3);
    }

    bool canBeActivatedByPlayer(PlayerObject* p0, EffectGameObject* p1)
    {
        if (TrajectoryNode::active()) return false;
        return GJBaseGameLayer::canBeActivatedByPlayer(p0, p1);
    }

    void playerTouchedRing(PlayerObject* p0, RingObject* p1)
    {
        if (!TrajectoryNode::active())
            GJBaseGameLayer::playerTouchedRing(p0, p1);
    }

    void activateSFXTrigger(SFXTriggerGameObject* p0)
    {
        if (!TrajectoryNode::active()) GJBaseGameLayer::activateSFXTrigger(p0);
    }

    void activateSongEditTrigger(SongTriggerGameObject* p0)
    {
        if (!TrajectoryNode::active()) GJBaseGameLayer::activateSongEditTrigger(p0);
    }
};

class $modify(TrajStreak, HardStreak)
{
    void addPoint(cocos2d::CCPoint p0)
    {
        if (!TrajectoryNode::active()) HardStreak::addPoint(p0);
    }
};

class $modify(TrajObj, GameObject)
{
    void playShineEffect()
    {
        if (!TrajectoryNode::active()) GameObject::playShineEffect();
    }
};

class $modify(TrajTrigger, EffectGameObject)
{
    void triggerObject(GJBaseGameLayer* p0, int p1, gd::vector<int> const* p2)
    {
        if (!TrajectoryNode::active()) EffectGameObject::triggerObject(p0, p1, p2);
    }
};
