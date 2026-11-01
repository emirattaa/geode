#include "TrajectoryNode.hpp"

using namespace geode::prelude;

static bool opt(char const* id) { return Mod::get()->getSettingValue<bool>(id); }

TrajectoryNode* TrajectoryNode::create(GJBaseGameLayer* layer)
{
    auto ret = new TrajectoryNode();
    if (ret && ret->init(layer)) { ret->autorelease(); return ret; }
    CC_SAFE_DELETE(ret);
    return nullptr;
}

TrajectoryNode::~TrajectoryNode()
{
    if (s_instance == this) s_instance = nullptr;
}

bool TrajectoryNode::init(GJBaseGameLayer* layer)
{
    if (!CCNode::init()) return false;
    m_layer = layer;
    s_instance = this;

    m_draw = CCDrawNode::create();
    this->addChild(m_draw);

    m_sim = PlayerObject::create(0, 0, layer, layer->m_objectLayer, false);
    m_sim->setVisible(false);
    layer->m_objectLayer->addChild(m_sim);

    this->setZOrder(1000);
    this->scheduleUpdate();
    return true;
}

void TrajectoryNode::update(float)
{
    this->redraw();
}

void TrajectoryNode::redraw()
{
    m_draw->clear();
    if (!opt("enabled")) return;

    // editorde sadece oynatirken calis
    if (m_layer->m_isEditor && m_layer->m_playbackMode != PlaybackMode::Playing) return;

    if (opt("player1") && m_layer->m_player1 && !m_layer->m_player1->m_isDead)
    {
        simulate(m_layer->m_player1, false, true);
        simulate(m_layer->m_player1, true, true);
    }
    if (opt("player2") && m_layer->m_gameState.m_isDualMode &&
        m_layer->m_player2 && !m_layer->m_player2->m_isDead)
    {
        simulate(m_layer->m_player2, false, false);
        simulate(m_layer->m_player2, true, false);
    }
}

void TrajectoryNode::copyState(PlayerObject* from, PlayerObject* to)
{
    auto cp = PlayerCheckpoint::create();
    from->saveToCheckpoint(cp);
    to->loadFromCheckpoint(cp);

    to->setPosition(from->getPosition());
    to->m_isShip = from->m_isShip;
    to->m_isBall = from->m_isBall;
    to->m_isBird = from->m_isBird;
    to->m_isDart = from->m_isDart;
    to->m_isRobot = from->m_isRobot;
    to->m_isSpider = from->m_isSpider;
    to->m_isSwing = from->m_isSwing;
    to->m_isUpsideDown = from->m_isUpsideDown;
    to->m_isSideways = from->m_isSideways;
    to->m_yVelocity = from->m_yVelocity;
    to->m_isOnGround = from->m_isOnGround;
    to->m_playerSpeed = from->m_playerSpeed;
}

void TrajectoryNode::simulate(PlayerObject* real, bool held, bool isP1)
{
    m_simulating = true;

    copyState(real, m_sim);

    m_sim->releaseButton(PlayerButton::Jump);
    if (held) m_sim->pushButton(PlayerButton::Jump);

    bool realHeld = real->m_holdingButtons[(int)PlayerButton::Jump];
    auto holdCol = Mod::get()->getSettingValue<ccColor3B>("hold-col");
    auto relCol = Mod::get()->getSettingValue<ccColor3B>("rel-col");
    // gercek duruma uyan cizgi "aktif" renkte
    ccColor4F col = ccc4FFromccc3B((held == realHeld) ? holdCol : relCol);
    (void)isP1;

    if (m_sim->m_regularTrail) m_sim->m_regularTrail->setVisible(false);

    int steps = (int)Mod::get()->getSettingValue<int64_t>("steps");
    float dt = (float)Mod::get()->getSettingValue<double>("delta");

    CCPoint prev = m_sim->getPosition();
    for (int i = 0; i < steps; i++)
    {
        m_sim->m_collisionLogTop->removeAllObjects();
        m_sim->m_collisionLogBottom->removeAllObjects();
        m_sim->m_collisionLogLeft->removeAllObjects();
        m_sim->m_collisionLogRight->removeAllObjects();

        m_sim->update(dt);
        m_layer->checkCollisions(m_sim, dt, false);
        m_sim->updateRotation(dt);
        m_sim->updatePlayerScale();

        auto pos = m_sim->getPosition();
        m_draw->drawSegment(prev, pos, 1.f, col);
        prev = pos;

        if (m_sim->m_isDead || m_sim->m_maybeIsColliding)
        {
            if (opt("hitbox"))
            {
                auto r = m_sim->getObjectRect();
                CCPoint pts[4] = {
                    { r.getMinX(), r.getMinY() }, { r.getMinX(), r.getMaxY() },
                    { r.getMaxX(), r.getMaxY() }, { r.getMaxX(), r.getMinY() }
                };
                m_draw->drawPolygon(pts, 4, ccc4f(0, 0, 0, 0), 0.5f, ccc4f(1, 0, 0, 1));
            }
            break;
        }
    }

    m_simulating = false;
}
