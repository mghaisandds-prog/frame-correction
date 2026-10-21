
#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>

using namespace geode::prelude;
using Clock = std::chrono::steady_clock;

namespace FC {

constexpr double TARGET_TPS = 240.0;
constexpr double TARGET_TICK_US = 1'000'000.0 / TARGET_TPS;

constexpr size_t MAX_PENDING = 2048;
constexpr size_t MAX_HISTORY = 512;
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

struct UpdateSample {
    uint64_t tick;
    int64_t timestampUs;
    float dt;
};

struct Prediction {
    bool valid = false;
    uint64_t estimatedTick = 0;
    double periodUs = 0.0;
    double phaseErrorUs = 0.0;
    double nearestBoundaryUs = 0.0;
};

static std::mutex g_mutex;
static std::deque<InputEvent> g_pending;
static std::deque<UpdateSample> g_history;

static uint64_t g_sequence = 0;
static uint64_t g_updateCount = 0;
static uint64_t g_dropped = 0;
static double g_periodUs = 0.0;

int64_t nowUs() {
    return std::chrono::duration_cast<
        std::chrono::microseconds
    >(Clock::now().time_since_epoch()).count();
}

void capture(InputEvent event) {
    std::lock_guard<std::mutex> lock(g_mutex);

    event.sequence = ++g_sequence;

    if (g_pending.size() >= MAX_PENDING) {
        ++g_dropped;
        return;
    }

    g_pending.push_back(event);
}

void observeUpdate(float dt, int64_t timestampUs) {
    std::lock_guard<std::mutex> lock(g_mutex);

    const uint64_t tick = ++g_updateCount;

    if (!g_history.empty()) {
        const double interval =
            static_cast<double>(
                timestampUs - g_history.back().timestampUs
            );

        if (interval >= MIN_PERIOD_US &&
            interval <= MAX_PERIOD_US) {
            if (g_periodUs == 0.0) {
                g_periodUs = interval;
            } else {
                g_periodUs =
                    (1.0 - SMOOTHING) * g_periodUs +
                    SMOOTHING * interval;
            }
        }
    }

    g_history.push_back({tick, timestampUs, dt});

    while (g_history.size() > MAX_HISTORY) {
        g_history.pop_front();
    }
}

Prediction predict(int64_t inputTimestampUs) {
    std::lock_guard<std::mutex> lock(g_mutex);

    Prediction result;

    if (g_history.size() < 2 || g_periodUs <= 0.0) {
        return result;
    }

    // Find the latest observed update at or before input time.
    const UpdateSample* previous = nullptr;
    const UpdateSample* next = nullptr;

    for (const auto& sample : g_history) {
        if (sample.timestampUs <= inputTimestampUs) {
            previous = &sample;
        } else {
            next = &sample;
            break;
        }
    }

    if (!previous) {
        // The input predates our retained history.
        return result;
    }

    result.valid = true;
    result.periodUs = g_periodUs;

    if (next) {
        // The input is bracketed by two observed update boundaries.
        const double span = static_cast<double>(
            next->timestampUs - previous->timestampUs
        );

        const double offset = static_cast<double>(
            inputTimestampUs - previous->timestampUs
        );

        const double fraction =
            span > 0.0 ? std::clamp(offset / span, 0.0, 1.0) : 0.0;

        result.estimatedTick =
            fraction < 0.5 ? previous->tick : next->tick;

        const double toPrevious = offset;
        const double toNext = span - offset;

        result.phaseErrorUs =
            std::min(toPrevious, toNext);

        result.nearestBoundaryUs =
            toPrevious <= toNext
                ? previous->timestampUs
                : next->timestampUs;
    } else {
        // Input is newer than the latest update.
        const double elapsed = static_cast<double>(
            inputTimestampUs - previous->timestampUs
        );

        const uint64_t periodsAhead =
            static_cast<uint64_t>(elapsed / g_periodUs);

        result.estimatedTick =
            previous->tick + periodsAhead;

        const double phase =
            elapsed - periodsAhead * g_periodUs;

        result.phaseErrorUs =
            std::min(phase, g_periodUs - phase);

        result.nearestBoundaryUs =
            previous->timestampUs +
            static_cast<int64_t>(
                (phase < g_periodUs / 2.0
                    ? periodsAhead
                    : periodsAhead + 1) * g_periodUs
            );
    }

    return result;
}

std::deque<InputEvent> drain() {
    std::lock_guard<std::mutex> lock(g_mutex);

    std::deque<InputEvent> events;
    events.swap(g_pending);
    return events;
}

uint64_t takeDropped() {
    std::lock_guard<std::mutex> lock(g_mutex);

    const uint64_t result = g_dropped;
    g_dropped = 0;
    return result;
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

        // Do not replay or delay the original input.
        GJBaseGameLayer::handleButton(
            down, button, isPlayer1
        );
    }
};


class $modify(FCPlayLayer, PlayLayer) {
    void postUpdate(float dt) {
        FC::observeUpdate(dt, FC::nowUs());

        const auto events = FC::drain();

        for (const auto& event : events) {
            const auto prediction =
                FC::predict(event.timestampUs);

            const double observedMs =
                static_cast<double>(
                    FC::nowUs() - event.timestampUs
                ) / 1000.0;

            if (prediction.valid) {
                log::info(
                    "[FC FIX EXP] seq={} {} button={} "
                    "estimatedTick={} phaseError={:.3f}ms "
                    "period={:.3f}ms observed={:.3f}ms",
                    event.sequence,
                    event.down ? "PRESS" : "RELEASE",
                    event.button,
                    prediction.estimatedTick,
                    prediction.phaseErrorUs / 1000.0,
                    prediction.periodUs / 1000.0,
                    observedMs
                );
            } else {
                log::info(
                    "[FC FIX EXP] seq={} {} "
                    "estimator=insufficient-history "
                    "observed={:.3f}ms",
                    event.sequence,
                    event.down ? "PRESS" : "RELEASE",
                    observedMs
                );
            }
        }

        const uint64_t dropped = FC::takeDropped();

        if (dropped > 0) {
            log::warn(
                "[FC FIX EXP] {} audit events dropped",
                dropped
            );
        }

        PlayLayer::postUpdate(dt);
    }
};
