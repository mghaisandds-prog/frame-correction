
#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>

using namespace geode::prelude;

class $modify(FrameCorrectionInputLogger, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        log::info(
            "[Frame Correction] Input event: {} button={} player1={}",
            down ? "PRESS" : "RELEASE",
            button,
            isPlayer1
        );

        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }
};
