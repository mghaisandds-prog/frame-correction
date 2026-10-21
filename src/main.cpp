
#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <mutex>

using namespace geode::prelude;
using Clock = std::chrono::steady_clock;

namespace FC {

constexpr double TARGET_TPS = 240.0;
constexpr double TARGET_TICK_US = 1'000'000.0 / TARGET_TPS;

constexpr size_t MAX_PENDING = 2048;
constexpr double MIN_PERIOD_US = 1000.0;
constexpr double MAX_PERIOD_US = 100000.0;
constexpr double SMOOTHING = 0.15;

struct InputEvent {
    uint64_t sequence;
    int64_t timestampUs;
    double levelTime;
    bool down;
    int button;
    bool player1;
};

struct TimingState {
    uint64_t updates = 0;
    int64_t lastUpdateUs = 0;
    double estimatedPeriodUs = 0.0;
    double lastDt = 0.0;
    uint64_t dropped = 0;
};

struct Prediction {
    bool valid = false;
    double periodUs = 0.0;
    double untilUpdateUs = 0.0;
    double phaseErrorUs = 0.0;
    int64_t estimatedTick = -1;
};

static std::mutex g_mutex;
static std::deque<InputEvent> g_pending;
static TimingState g_timing;
static uint64_t g_nextSequence = 1;

static int64_t nowUs() {
    return std::chrono::duration_cast<
        std::chrono::microseconds
    >(Clock::now().time_since_epoch()).count();
}

// Capture is diagnostic only. It does not delay gameplay input.
static void capture(InputEvent event) {
    std::lock_guard<std::mutex> lock(g_mutex);

    event.sequence = g_nextSequence++;

    if (g_pending.size() >= MAX_PENDING) {
        ++g_timing.dropped;
        return;
    }

    g_pending.push_back(event);
}

static void observeUpdate(float dt, int64_t now) {
    std::lock_guard<std::mutex> lock(g_mutex);

    if (g_timing.lastUpdateUs != 0) {
        const double interval =
            static_cast<double>(now - g_timing.lastUpdateUs);

        if (interval >= MIN_PERIOD_US &&
            interval <= MAX_PERIOD_US) {
            if (g_timing.estimatedPeriodUs <= 0.0) {
                g_timing.estimatedPeriodUs = interval;
            } else {
                g_timing.estimatedPeriodUs =
                    (1.0 - SMOOTHING) *
                        g_timing.estimatedPeriodUs +
                    SMOOTHING * interval;
            }
        }
    }

    g_timing.lastUpdateUs = now;
    g_timing.lastDt = dt;
    ++g_timing.updates;
}

static Prediction predict(int64_t timestampUs) {
    std::lock_guard<std::mutex> lock(g_mutex);

    Prediction p;

    if (g_timing.lastUpdateUs == 0 ||
        g_timing.estimatedPeriodUs <= 0.0) {
        return p;
    }

    const double period = g_timing.estimatedPeriodUs;
    const double elapsed =
        static_cast<double>(timestampUs - g_timing.lastUpdateUs);

    if (elapsed < 0.0) {
        return p;
    }

    const auto completedPeriods =
        static_cast<int64_t>(std::floor(elapsed / period));

    const double phase =
        elapsed - completedPeriods * period;

    p.valid = true;
    p.periodUs = period;
    p.untilUpdateUs = period - phase;
    p.phaseErrorUs = std::min(phase, period - phase);
    p.estimatedTick =
        static_cast<int64_t>(g_timing.updates) +
        completedPeriods + 1;

    return p;
}

static std::deque<InputEvent> drain() {
    std::lock_guard<std::mutex> lock(g_mutex);

    std::deque<InputEvent> result;
    result.swap(g_pending);
    return result;
}

static TimingState snapshot() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_timing;
}

} // namespace FC


class $modify(FCInputLayer, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        FC::capture({
            0,
            FC::nowUs(),
            m_gameState.m_levelTime,
            down,
            button,
            isPlayer1
        });

        // Original input executes exactly once.
        GJBaseGameLayer::handleButton(
            down, button, isPlayer1
        );
    }
};


class $modify(FCPlayLayer, PlayLayer) {
    void postUpdate(float dt) {
        const int64_t now = FC::nowUs();

        FC::observeUpdate(dt, now);

        auto events = FC::drain();
        const auto state = FC::snapshot();

        for (const auto& event : events) {
            const double observedMs =
                static_cast<double>(
                    now - event.timestampUs
                ) / 1000.0;

            const auto prediction =
                FC::predict(event.timestampUs);

            if (prediction.valid) {
                log::info(
                    "[FC v0.2] seq={} action={} button={} "
                    "observed={:.3f}ms "
                    "updatePeriod={:.3f}ms "
                    "untilPredictedUpdate={:.3f}ms "
                    "phaseError={:.3f}ms "
                    "estimatedTick={} "
                    "dt={:.6f} targetTick={:.3f}ms",
                    event.sequence,
                    event.down ? "PRESS" : "RELEASE",
                    event.button,
                    observedMs,
                    prediction.periodUs / 1000.0,
                    prediction.untilUpdateUs / 1000.0,
                    prediction.phaseErrorUs / 1000.0,
                    prediction.estimatedTick,
                    dt,
                    FC::TARGET_TICK_US / 1000.0
                );
            } else {
                log::info(
                    "[FC v0.2] seq={} action={} button={} "
                    "observed={:.3f}ms estimator=warming-up",
                    event.sequence,
                    event.down ? "PRESS" : "RELEASE",
                    event.button,
                    observedMs
                );
            }
        }

        if (state.dropped > 0) {
            log::warn(
                "[FC v0.2] {} audit events dropped",
                state.dropped
            );
        }

        PlayLayer::postUpdate(dt);
    }
};
