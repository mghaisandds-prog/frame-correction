
#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>

#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>

using namespace geode::prelude;

using Clock = std::chrono::steady_clock;

struct InputAuditEvent {
    int64_t timestampUs;
    double levelTime;
    bool down;
    int button;
    bool isPlayer1;
};

static std::mutex g_inputMutex;
static std::deque<InputAuditEvent> g_pendingInputs;

static int64_t nowUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        Clock::now().time_since_epoch()
    ).count();
}

class $modify(AuditInputLayer, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        InputAuditEvent event {
            nowUs(),
            m_gameState.m_levelTime,
            down,
            button,
            isPlayer1
        };

        {
            std::lock_guard<std::mutex> lock(g_inputMutex);
            g_pendingInputs.push_back(event);
        }

        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }
};

class $modify(AuditPlayLayer, PlayLayer) {
    void postUpdate(float dt) {
        const int64_t updateStartUs = nowUs();

        std::deque<InputAuditEvent> events;

        {
            std::lock_guard<std::mutex> lock(g_inputMutex);
            events.swap(g_pendingInputs);
        }

        for (const auto& event : events) {
            const double observedMs =
                (updateStartUs - event.timestampUs) / 1000.0;

            log::info(
                "[AUDIT] {} button={} player1={} "
                "observedAfter={:.3f}ms "
                "inputLevelTime={:.6f} "
                "updateLevelTime={:.6f} dt={:.6f}",
                event.down ? "PRESS" : "RELEASE",
                event.button,
                event.isPlayer1,
                observedMs,
                event.levelTime,
                m_gameState.m_levelTime,
                dt
            );
        }

        PlayLayer::postUpdate(dt);
    }
};
