#pragma once
#include <Geode/Geode.hpp>

class TrajectoryNode : public cocos2d::CCNode
{
protected:
    static inline TrajectoryNode* s_instance = nullptr;

    GJBaseGameLayer* m_layer = nullptr;
    PlayerObject* m_sim = nullptr;
    cocos2d::CCDrawNode* m_draw = nullptr;
    bool m_simulating = false;

    ~TrajectoryNode() override;
    bool init(GJBaseGameLayer* layer);

    void update(float dt) override;
    void redraw();
    void simulate(PlayerObject* real, bool held, bool isP1);
    void copyState(PlayerObject* from, PlayerObject* to);

public:
    static TrajectoryNode* create(GJBaseGameLayer* layer);
    static TrajectoryNode* get() { return s_instance; }
    // true yalnizca simulasyon sirasinda
    static bool active() { return s_instance && s_instance->m_simulating; }
};
