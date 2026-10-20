
#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>

using namespace geode::prelude;

class $modify(FrameCorrectionInputLogger, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        double levelTime = m_gameState.m_levelTime;
        long long frame = static_cast<long long>(levelTime * 240.0);

        log::info(
            "[Frame Correction] {} button={} player1={} time={:.4f} frame~{}",
            down ? "PRESS" : "RELEASE",
            button,
            isPlayer1,
            levelTime,
            frame
        );

        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }
};
