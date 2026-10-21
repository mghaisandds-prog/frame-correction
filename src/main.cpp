
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
using FCClock = std::chrono::steady_clock;

namespace FrameCorrection {

// ============================================================
// CONFIGURATION
// ============================================================

constexpr double TARGET_TPS = 240.0;
constexpr double TARGET_TICK_US = 1'000'000.0 / TARGET_TPS;

constexpr size_t MAX_PENDING_EVENTS = 2048;
constexpr size_t MAX_UPDATE_HISTORY = 512;

constexpr double MIN_UPDATE_INTERVAL_US = 1000.0;
constexpr double MAX_UPDATE_INTERVAL_US = 100000.0;
constexpr double OUTLIER_FACTOR = 3.0;
constexpr double EMA_ALPHA = 0.15;

// Safe mode: original game input is never replayed.
// This prototype estimates timing but does not modify physics.
constexpr bool SAFE_INPUT_MODE = true;

// ============================================================
// DATA
// ============================================================

struct InputEvent {
    uint64_t sequence;
    int64_t timestampUs;
    double levelTime;
    bool down;
    int button;
    bool player1;
};

struct UpdateSample {
    uint64_t updateNumber;
    int64_t timestampUs;
    double levelTime;
    double dt;
};

struct TimingStats {
    uint64_t updateCount = 0;
    uint64_t eventCount = 0;
    uint64_t droppedEvents = 0;
    uint64_t staleEvents = 0;
    uint64_t outlierIntervals = 0;

    int64_t lastUpdateUs = 0;
    double estimatedUpdateUs = 0.0;
    double lastDt = 0.0;
};

struct TimingPrediction {
    bool valid = false;
    bool stale = false;

    uint64_t estimatedUpdate = 0;

    double estimatedPeriodUs = 0.0;
    double phaseErrorUs = 0.0;
    double untilNextUpdateUs = 0.0;
    double targetTickMs = TARGET_TICK_US / 1000.0;
};

static std::mutex g_mutex;
static std::deque<InputEvent> g_pending;
static std::deque<UpdateSample> g_history;
static TimingStats g_stats;

static uint64_t g_nextSequence = 0;

// ============================================================
// CLOCK
// ============================================================

static int64_t nowUs() {
    return std::chrono::duration_cast<
        std::chrono::microseconds
    >(FCClock::now().time_since_epoch()).count();
}

// ============================================================
// INPUT CAPTURE
// ============================================================

static void captureInput(
    bool down,
    int button,
    bool player1,
    double levelTime
) {
    const int64_t timestamp = nowUs();

    std::lock_guard<std::mutex> lock(g_mutex);

    ++g_stats.eventCount;

    if (g_pending.size() >= MAX_PENDING_EVENTS) {
        ++g_stats.droppedEvents;
        return;
    }

    g_pending.push_back({
        ++g_nextSequence,
        timestamp,
        levelTime,
        down,
        button,
        player1
    });
}

// ============================================================
// UPDATE OBSERVATION
// ============================================================

static void observeUpdate(
    double dt,
    double levelTime,
    int64_t timestamp
) {
    std::lock_guard<std::mutex> lock(g_mutex);

    ++g_stats.updateCount;

    if (g_stats.lastUpdateUs != 0) {
        const double interval =
            static_cast<double>(
                timestamp - g_stats.lastUpdateUs
            );

        if (interval >= MIN_UPDATE_INTERVAL_US &&
            interval <= MAX_UPDATE_INTERVAL_US) {

            const bool isOutlier =
                g_stats.estimatedUpdateUs > 0.0 &&
                (interval >
                    g_stats.estimatedUpdateUs * OUTLIER_FACTOR ||
                 interval <
                    g_stats.estimatedUpdateUs / OUTLIER_FACTOR);

            if (isOutlier) {
                ++g_stats.outlierIntervals;
            }

            // Do not let large pauses distort the estimator.
            if (!isOutlier) {
                if (g_stats.estimatedUpdateUs <= 0.0) {
                    g_stats.estimatedUpdateUs = interval;
                } else {
                    g_stats.estimatedUpdateUs =
                        (1.0 - EMA_ALPHA) *
                            g_stats.estimatedUpdateUs +
                        EMA_ALPHA * interval;
                }
            }
        } else {
            ++g_stats.outlierIntervals;
        }
    }

    g_stats.lastUpdateUs = timestamp;
    g_stats.lastDt = dt;

    g_history.push_back({
        g_stats.updateCount,
        timestamp,
        levelTime,
        dt
    });

    while (g_history.size() > MAX_UPDATE_HISTORY) {
        g_history.pop_front();
    }
}

// ============================================================
// PREDICTIVE TIMING ESTIMATOR
// ============================================================

static TimingPrediction predictInput(
    int64_t inputTimestampUs
) {
    std::lock_guard<std::mutex> lock(g_mutex);

    TimingPrediction result;

    result.estimatedPeriodUs =
        g_stats.estimatedUpdateUs;

    if (g_history.size() < 2 ||
        g_stats.estimatedUpdateUs <= 0.0) {
        return result;
    }

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

    // Input older than retained history: prediction is unreliable.
    if (!previous) {
        result.stale = true;
        return result;
    }

    result.valid = true;

    if (next) {
        // Input is between two observed update boundaries.
        const double span = static_cast<double>(
            next->timestampUs - previous->timestampUs
        );

        const double offset = static_cast<double>(
            inputTimestampUs - previous->timestampUs
        );

        if (span <= 0.0) {
            result.valid = false;
            return result;
        }

        const double fraction =
            std::clamp(offset / span, 0.0, 1.0);

        result.estimatedUpdate =
            fraction < 0.5
                ? previous->updateNumber
                : next->updateNumber;

        result.phaseErrorUs =
            std::min(offset, span - offset);

        result.untilNextUpdateUs =
            span - offset;

        return result;
    }

    // Input is newer than the latest observed update.
    const double elapsed = static_cast<double>(
        inputTimestampUs - previous->timestampUs
    );

    const double period = g_stats.estimatedUpdateUs;

    if (elapsed > period * OUTLIER_FACTOR) {
        result.stale = true;
        result.valid = false;
        return result;
    }

    const auto periodsAhead =
        static_cast<uint64_t>(elapsed / period);

    const double phase =
        elapsed - periodsAhead * period;

    result.estimatedUpdate =
        previous->updateNumber + periodsAhead;

    result.phaseErrorUs =
        std::min(phase, period - phase);

    result.untilNextUpdateUs =
        period - phase;

    return result;
}

// ============================================================
// EVENT QUEUE
// ============================================================

static std::deque<InputEvent> drainEvents() {
    std::lock_guard<std::mutex> lock(g_mutex);

    std::deque<InputEvent> events;
    events.swap(g_pending);

    return events;
}

static TimingStats getStats() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_stats;
}

static void markStale() {
    std::lock_guard<std::mutex> lock(g_mutex);
    ++g_stats.staleEvents;
}

// ============================================================
// DIAGNOSTICS
// ============================================================

static void logEvent(const InputEvent& event) {
    const auto prediction =
        predictInput(event.timestampUs);

    const double ageMs =
        static_cast<double>(
            nowUs() - event.timestampUs
        ) / 1000.0;

    if (!prediction.valid) {
        log::info(
            "[FC ALL] seq={} {} button={} "
            "prediction={} age={:.3f}ms",
            event.sequence,
            event.down ? "PRESS" : "RELEASE",
            event.button,
            prediction.stale ? "STALE" : "WARMING_UP",
            ageMs
        );

        if (prediction.stale) {
            markStale();
        }

        return;
    }

    log::info(
        "[FC ALL] seq={} {} button={} "
        "update={} period={:.3f}ms "
        "phaseError={:.3f}ms "
        "untilNext={:.3f}ms "
        "target240TPS={:.3f}ms age={:.3f}ms",
        event.sequence,
        event.down ? "PRESS" : "RELEASE",
        event.button,
        prediction.estimatedUpdate,
        prediction.estimatedPeriodUs / 1000.0,
        prediction.phaseErrorUs / 1000.0,
        prediction.untilNextUpdateUs / 1000.0,
        prediction.targetTickMs,
        ageMs
    );
}

} // namespace FrameCorrection

// ============================================================
// ORIGINAL INPUT HOOK
// ============================================================

class $modify(FCInputLayer, GJBaseGameLayer) {
    void handleButton(
        bool down,
        int button,
        bool isPlayer1
    ) {
        FrameCorrection::captureInput(
            down,
            button,
            isPlayer1,
            m_gameState.m_levelTime
        );

        // Original game input is preserved exactly once.
        GJBaseGameLayer::handleButton(
            down,
            button,
            isPlayer1
        );
    }
};

// ============================================================
// UPDATE HOOK
// ============================================================

class $modify(FCPlayLayer, PlayLayer) {
    void postUpdate(float dt) {
        FrameCorrection::observeUpdate(
            dt,
            m_gameState.m_levelTime,
            FrameCorrection::nowUs()
        );

        const auto events =
            FrameCorrection::drainEvents();

        for (const auto& event : events) {
            FrameCorrection::logEvent(event);
        }

        const auto stats =
            FrameCorrection::getStats();

        if (stats.droppedEvents > 0) {
            log::warn(
                "[FC ALL] droppedEvents={} staleEvents={} "
                "outlierIntervals={} updates={} "
                "estimatedPeriod={:.3f}ms",
                stats.droppedEvents,
                stats.staleEvents,
                stats.outlierIntervals,
                stats.updateCount,
                stats.estimatedUpdateUs / 1000.0
            );
        }

        // Preserve normal game update behavior.
        PlayLayer::postUpdate(dt);
    }
};
