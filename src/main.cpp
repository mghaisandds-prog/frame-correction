
#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>

using namespace geode::prelude;

static unsigned long long g_updateTicks = 0;

class $modify(FrameCorrectionPlayLayer, PlayLayer) {
    void postUpdate(float dt) {
        ++g_updateTicks;
        PlayLayer::postUpdate(dt);
    }
};

class $modify(FrameCorrectionInputLogger, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        double levelTime = m_gameState.m_levelTime;

        log::info(
            "[Frame Correction] {} button={} player1={} time={:.4f} updateTick={}",
            down ? "PRESS" : "RELEASE",
            button,
            isPlayer1,
            levelTime,
            g_updateTicks
        );

        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }
};
