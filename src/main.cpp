
#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>

#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>

using namespace geode::prelude;
using Clock = std::chrono::steady_clock;

namespace {
    constexpr size_t MAX_PENDING_EVENTS = 4096;

    struct InputEvent {
        int64_t timestampUs;
        double levelTime;
        uint64_t observedUpdate;
        bool down;
        int button;
        bool isPlayer1;
    };

    std::mutex g_mutex;
    std::deque<InputEvent> g_pending;
    uint64_t g_updateCount = 0;
    uint64_t g_droppedAuditEvents = 0;

    int64_t nowUs() {
        return std::chrono::duration_cast<
            std::chrono::microseconds
        >(Clock::now().time_since_epoch()).count();
    }
}

class $modify(FrameCorrectionInputAudit, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        InputEvent event {
            nowUs(),
            m_gameState.m_levelTime,
            0,
            down,
            button,
            isPlayer1
        };

        {
            std::lock_guard<std::mutex> lock(g_mutex);

            event.observedUpdate = g_updateCount;

            if (g_pending.size() < MAX_PENDING_EVENTS) {
                g_pending.push_back(event);
            } else {
                ++g_droppedAuditEvents;
            }
        }

        // Gameplay input tetap diproses oleh fungsi asli.
        GJBaseGameLayer::handleButton(
            down, button, isPlayer1
        );
    }
};

class $modify(FrameCorrectionUpdateAudit, PlayLayer) {
    void postUpdate(float dt) {
        const int64_t startUs = nowUs();
        std::deque<InputEvent> events;
        uint64_t updateNumber;
        uint64_t dropped;

        {
            std::lock_guard<std::mutex> lock(g_mutex);

            updateNumber = ++g_updateCount;
            events.swap(g_pending);

            dropped = g_droppedAuditEvents;
            g_droppedAuditEvents = 0;
        }

        for (const auto& event : events) {
            const double observedMs =
                (startUs - event.timestampUs) / 1000.0;

            log::info(
                "[FC AUDIT] update={} inputUpdate={} "
                "action={} button={} player1={} "
                "observedAfter={:.3f}ms "
                "inputTime={:.6f} updateTime={:.6f} dt={:.6f}",
                updateNumber,
                event.observedUpdate,
                event.down ? "PRESS" : "RELEASE",
                event.button,
                event.isPlayer1,
                observedMs,
                event.levelTime,
                m_gameState.m_levelTime,
                dt
            );
        }

        if (dropped > 0) {
            log::warn(
                "[FC AUDIT] Dropped {} audit events: queue full",
                dropped
            );
        }

        PlayLayer::postUpdate(dt);
    }
};
