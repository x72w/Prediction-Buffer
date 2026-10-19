// Prediction Buffer - clean-room recreation of the behaviour of "Predictive Input Buffer".
//
// How it works
//   1. A click (or, in wave, a release) reaches GJBaseGameLayer::handleButton.
//   2. We copy the real player into a hidden "shadow" PlayerObject and run the game's own
//      physics (PlayerObject::update + GJBaseGameLayer::checkCollisions) forward a few ticks
//      with the input applied after d ticks, for d = 0, 1, 2 ... limit.
//   3. If d = 0 survives, the click passes through untouched. Otherwise the smallest d that
//      survives is used: the input is queued and re-injected d ticks later.
//   4. While simulating, side effects (death, triggers, particles, jump counter, ...) are
//      suppressed by hooks, and object activation flags are restored afterwards.
//
// Stages: 1..5 -> 2/4/8/16/32 ticks (4 ticks = 1 frame @ 60fps). F6 (keyboard) or the
// "Buffer" button in the pause menu (touch / Android) cycles through them, then OFF.

#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/EffectGameObject.hpp>
#include <Geode/modify/GameObject.hpp>
#include <Geode/modify/HardStreak.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <Geode/modify/PauseLayer.hpp>

#include <algorithm>
#include <deque>
#include <tuple>
#include <vector>

using namespace geode::prelude;

namespace pb {

// ---------------------------------------------------------------- state --

constexpr int kStageTicks[5] = {2, 4, 8, 16, 32};
constexpr int kTicksPerFrame = 4;   // 240 TPS / 60 FPS
constexpr int kMaxHorizon = 160;

int g_stage = 1;                    // 0 = OFF, 1..5
bool g_stageInit = false;

struct SimState {
    bool active = false;
    bool died = false;
    // (object, activated, byP1, byP2) so we can undo what the simulation touched
    std::vector<std::tuple<EffectGameObject*, bool, bool, bool>> touched;
};
SimState g_sim;

struct PendingInput {
    long long fireAt;
    bool down;
    int button;
    bool isPlayer1;
};
std::deque<PendingInput> g_queue;
long long g_tick = 0;
int g_shift = 0;                    // delay currently applied to the running input chain
bool g_bypass = false;              // true while we re-inject a delayed input
bool g_held[2] = {false, false};    // real "jump held" state per player (delivered inputs only)

PlayerObject* g_shadow[2] = {nullptr, nullptr};

void resetRuntime() {
    g_queue.clear();
    g_tick = 0;
    g_shift = 0;
    g_bypass = false;
    g_held[0] = g_held[1] = false;
    g_sim = SimState{};
}

void releaseShadows() {
    for (auto& s : g_shadow) {
        if (s) {
            s->release();
            s = nullptr;
        }
    }
}

// ------------------------------------------------------------- settings --

bool isEnabled() {
    return Mod::get()->getSettingValue<bool>("enabled") && g_stage > 0;
}

void initStageFromSettings() {
    if (g_stageInit) return;
    g_stageInit = true;
    g_stage = static_cast<int>(Mod::get()->getSettingValue<int64_t>("start-stage"));
    g_stage = std::clamp(g_stage, 0, 5);
}

int stageLimitTicks() {
    if (g_stage <= 0) return 0;
    int v = kStageTicks[g_stage - 1];
    if (Mod::get()->getSettingValue<std::string>("unit") == "Frames") v *= kTicksPerFrame;
    return v;
}

int waveLimitTicks() {
    int cap = static_cast<int>(Mod::get()->getSettingValue<int64_t>("wave-cap"));
    return std::min(stageLimitTicks(), cap);
}

std::string stageName() {
    if (g_stage <= 0) return "OFF (raw input)";
    return fmt::format("Stage {}: {} ticks", g_stage, stageLimitTicks());
}

void cycleStage() {
    initStageFromSettings();
    g_stage = (g_stage + 1) % 6; // 1->2->3->4->5->OFF(0)->1
    g_queue.clear();
    g_shift = 0;
    Notification::create(stageName(), NotificationIcon::None, 1.f)->show();
}

// ----------------------------------------------------------- simulation --

bool ensureShadows(GJBaseGameLayer* gl) {
    for (int i = 0; i < 2; ++i) {
        if (g_shadow[i]) continue;
        auto* s = PlayerObject::create(1, 1, gl, gl->m_objectLayer, true);
        if (!s) return false;
        s->retain();
        s->setVisible(false);
        s->m_playEffects = false;
        g_shadow[i] = s;
    }
    return true;
}

void copyState(PlayerObject* from, PlayerObject* to, bool heldJump) {
    auto* cp = PlayerCheckpoint::create();
    from->saveToCheckpoint(cp);
    to->loadFromCheckpoint(cp);

    // things the checkpoint doesn't carry but physics needs
    to->m_yVelocity = from->m_yVelocity;
    to->m_gravity = from->m_gravity;
    to->m_playerSpeed = from->m_playerSpeed;
    to->m_isOnGround = from->m_isOnGround;
    to->m_isOnGround2 = from->m_isOnGround2;
    to->m_isOnGround3 = from->m_isOnGround3;
    to->m_isOnGround4 = from->m_isOnGround4;
    to->m_stateOnGround = from->m_stateOnGround;
    to->m_isUpsideDown = from->m_isUpsideDown;
    to->m_isSideways = from->m_isSideways;
    to->m_jumpBuffered = from->m_jumpBuffered;
    to->m_hasEverJumped = from->m_hasEverJumped;
    to->m_isLocked = from->m_isLocked;
    to->m_isDead = false;
    to->m_gameLayer = from->m_gameLayer;
    to->m_playEffects = false;

    to->releaseAllButtons();
    if (heldJump) to->pushButton(PlayerButton::Jump);
}

// Returns how many ticks the shadow survives (== horizon means "did not die").
int simulate(GJBaseGameLayer* gl, int delay, bool down, bool targetIsP1, int horizon) {
    PlayerObject* real[2] = {gl->m_player1, gl->m_player2};
    bool dual = gl->m_gameState.m_isDualMode && real[1] != nullptr;
    int count = dual ? 2 : 1;
    if (!real[0] || !ensureShadows(gl)) return horizon;

    for (int i = 0; i < count; ++i) copyState(real[i], g_shadow[i], g_held[i]);

    g_sim = SimState{};
    g_sim.active = true;

    float dt = (1.f / 240.f) * gl->m_gameState.m_timeWarp;
    int survived = horizon;

    for (int t = 0; t < horizon; ++t) {
        if (t == delay) {
            // in a dual level a classic click affects both players
            for (int i = 0; i < count; ++i) {
                if (count == 1 && !targetIsP1) break;
                if (down) g_shadow[i]->pushButton(PlayerButton::Jump);
                else g_shadow[i]->releaseButton(PlayerButton::Jump);
            }
        }
        for (int i = 0; i < count; ++i) {
            g_shadow[i]->update(dt);
            gl->checkCollisions(g_shadow[i], dt, false);
            if (g_sim.died || g_shadow[i]->m_isDead) break;
        }
        if (g_sim.died || g_shadow[0]->m_isDead || (count == 2 && g_shadow[1]->m_isDead)) {
            survived = t;
            break;
        }
    }

    // undo object activation done by the simulated run (orbs/pads/portals)
    for (auto it = g_sim.touched.rbegin(); it != g_sim.touched.rend(); ++it) {
        auto [obj, act, p1, p2] = *it;
        obj->m_activated = act;
        obj->m_activatedByPlayer1 = p1;
        obj->m_activatedByPlayer2 = p2;
    }

    for (int i = 0; i < count; ++i) g_shadow[i]->releaseAllButtons();
    g_sim = SimState{};
    return survived;
}

// Smallest delay in [1, limit] that survives; 0 means "just let the input through".
int chooseDelay(GJBaseGameLayer* gl, bool down, bool isP1, int limit) {
    int horizon = std::clamp(limit + 12, 24, kMaxHorizon);

    if (simulate(gl, 0, down, isP1, horizon) >= horizon) return 0;

    int step = limit > 16 ? 4 : 1;
    for (int d = step; d <= limit; d += step) {
        if (simulate(gl, d, down, isP1, horizon) >= horizon) {
            // coarse hit: refine downwards to the smallest working delay
            for (int r = d - step + 1; r < d; ++r) {
                if (simulate(gl, r, down, isP1, horizon) >= horizon) return r;
            }
            return d;
        }
    }
    return 0; // nothing saves us, don't interfere
}

bool inRealPlay(GJBaseGameLayer* gl) {
    auto* pl = PlayLayer::get();
    return pl && static_cast<GJBaseGameLayer*>(pl) == gl && !pl->m_levelEndAnimationStarted;
}

void flushQueue(GJBaseGameLayer* gl) {
    while (!g_queue.empty() && g_queue.front().fireAt <= g_tick) {
        auto ev = g_queue.front();
        g_queue.pop_front();
        g_bypass = true;
        gl->handleButton(ev.down, ev.button, ev.isPlayer1);
        g_bypass = false;
        if (ev.button == 1) g_held[ev.isPlayer1 ? 0 : 1] = ev.down;
    }
    if (g_queue.empty()) g_shift = 0;
}

} // namespace pb

// ---------------------------------------------------------------- hooks --

class $modify(PBGameLayer, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        using namespace pb;
        initStageFromSettings();

        if (g_bypass || g_sim.active || button != 1 || !isEnabled() || !inRealPlay(this)) {
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            if (!g_sim.active && button == 1) g_held[isPlayer1 ? 0 : 1] = down;
            return;
        }

        // keep ordering: if earlier inputs are still waiting, ride the same shift
        if (!g_queue.empty()) {
            long long at = std::max(g_tick + g_shift, g_queue.back().fireAt);
            g_queue.push_back({at, down, button, isPlayer1});
            return;
        }
        g_shift = 0;

        PlayerObject* pl = isPlayer1 ? m_player1 : m_player2;
        if (!pl) {
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            g_held[isPlayer1 ? 0 : 1] = down;
            return;
        }

        bool wave = pl->m_isDart;
        int limit = wave ? waveLimitTicks() : stageLimitTicks();
        // releases only matter in wave
        int d = (down || wave) && limit > 0 ? chooseDelay(this, down, isPlayer1, limit) : 0;

        if (d <= 0) {
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            g_held[isPlayer1 ? 0 : 1] = down;
            return;
        }

        if (Mod::get()->getSettingValue<bool>("verbose")) {
            log::info("[PredictionBuffer] {} delayed +{} ticks ({:.2f} frames) [limit {}]",
                      down ? "press" : "release", d, d / 4.0, limit);
        }
        g_shift = d;
        g_queue.push_back({g_tick + d, down, button, isPlayer1});
    }
};

class $modify(PBPlayer, PlayerObject) {
    void update(float dt) {
        using namespace pb;
        if (!g_sim.active && !g_bypass && m_gameLayer && this == m_gameLayer->m_player1 &&
            inRealPlay(m_gameLayer)) {
            ++g_tick;
            flushQueue(m_gameLayer);
        }
        PlayerObject::update(dt);
    }

    // --- side effects silenced while simulating ---
    void incrementJumps() {
        if (pb::g_sim.active) return;
        PlayerObject::incrementJumps();
    }

    void playSpiderDashEffect(CCPoint from, CCPoint to) {
        if (pb::g_sim.active) return;
        PlayerObject::playSpiderDashEffect(from, to);
    }

    void ringJump(RingObject* obj, bool skipCheck) {
        if (pb::g_sim.active && obj) {
            pb::g_sim.touched.emplace_back(obj, obj->m_activated, obj->m_activatedByPlayer1,
                                           obj->m_activatedByPlayer2);
        }
        PlayerObject::ringJump(obj, skipCheck);
    }
};

class $modify(PBLayer, PlayLayer) {
    void destroyPlayer(PlayerObject* player, GameObject* obj) {
        if (pb::g_sim.active) {
            pb::g_sim.died = true;
            return;
        }
        pb::g_queue.clear();
        pb::g_shift = 0;
        PlayLayer::destroyPlayer(player, obj);
    }

    void playEndAnimationToPos(CCPoint pos) {
        if (pb::g_sim.active) return;
        PlayLayer::playEndAnimationToPos(pos);
    }

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        pb::releaseShadows();
        pb::resetRuntime();
        return PlayLayer::init(level, useReplay, dontCreateObjects);
    }

    void resetLevel() {
        pb::resetRuntime();
        PlayLayer::resetLevel();
    }

    void onQuit() {
        pb::releaseShadows();
        pb::resetRuntime();
        PlayLayer::onQuit();
    }
};

class $modify(PBBase2, GJBaseGameLayer) {
    // record objects whose activation state the simulation may change
    bool canBeActivatedByPlayer(PlayerObject* player, EffectGameObject* obj) {
        if (pb::g_sim.active && obj) {
            pb::g_sim.touched.emplace_back(obj, obj->m_activated, obj->m_activatedByPlayer1,
                                           obj->m_activatedByPlayer2);
        }
        return GJBaseGameLayer::canBeActivatedByPlayer(player, obj);
    }

    void playerTouchedRing(PlayerObject* player, RingObject* obj) {
        if (pb::g_sim.active && obj) {
            pb::g_sim.touched.emplace_back(obj, obj->m_activated, obj->m_activatedByPlayer1,
                                           obj->m_activatedByPlayer2);
        }
        GJBaseGameLayer::playerTouchedRing(player, obj);
    }
};

class $modify(PBEffectObject, EffectGameObject) {
    void triggerObject(GJBaseGameLayer* layer, int uniqueID, gd::vector<int> const* remapKeys) {
        if (pb::g_sim.active) return; // never fire real triggers from the simulation
        EffectGameObject::triggerObject(layer, uniqueID, remapKeys);
    }
};

class $modify(PBGameObject, GameObject) {
    void playShineEffect() {
        if (pb::g_sim.active) return;
        GameObject::playShineEffect();
    }
};

class $modify(PBStreak, HardStreak) {
    void addPoint(CCPoint point) {
        if (pb::g_sim.active) return;
        HardStreak::addPoint(point);
    }
};

class $modify(PBKeys, CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(enumKeyCodes key, bool down, bool repeat, double timestamp) {
        if (down && !repeat && key == KEY_F6) {
            pb::cycleStage();
        }
        return CCKeyboardDispatcher::dispatchKeyboardMSG(key, down, repeat, timestamp);
    }
};

// Touch-friendly stage switch (Android has no F6): button in the pause menu.
class $modify(PBPause, PauseLayer) {
    struct Fields {
        ButtonSprite* label = nullptr;
    };

    static std::string buttonText() {
        pb::initStageFromSettings();
        return pb::g_stage <= 0 ? "Buffer: OFF" : fmt::format("Buffer: S{}", pb::g_stage);
    }

    void customSetup() {
        PauseLayer::customSetup();
        auto win = CCDirector::sharedDirector()->getWinSize();

        auto spr = ButtonSprite::create(buttonText().c_str(), "goldFont.fnt", "GJ_button_04.png", .6f);
        m_fields->label = spr;
        auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(PBPause::onCycleStage));

        auto menu = CCMenu::create();
        menu->addChild(btn);
        menu->setPosition({win.width - 70.f, 30.f});
        this->addChild(menu, 10);
    }

    void onCycleStage(CCObject*) {
        pb::cycleStage();
        if (m_fields->label) m_fields->label->setString(buttonText().c_str());
    }
};
