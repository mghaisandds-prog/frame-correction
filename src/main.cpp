#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <chrono>
#include <atomic>

using namespace geode::prelude;

// Gunakan atomic agar aman dibaca/tulis antar-thread di ARM64 Android
auto g_lastUpdateRealTime = std::chrono::steady_clock::now();
std::atomic<bool> g_inputJustHappened(false);
std::atomic<int64_t> g_realInputTimestamp(0);
std::atomic<double> g_inputLevelTime(0.0);

class $modify(AuditPlayLayer, PlayLayer) {
    // Kita kembali gunakan postUpdate karena terbukti terpanggil di kode aslimu
    void postUpdate(float dt) {
        auto now = std::chrono::steady_clock::now();
        double realDtMs = std::chrono::duration_cast<std::chrono::microseconds>(now - g_lastUpdateRealTime).count() / 1000.0;
        g_lastUpdateRealTime = now;

        // Cek apakah ada input dari thread sebelah
        if (g_inputJustHappened.load()) {
            double latencyMs = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count() / 1000.0 - (g_realInputTimestamp.load() / 1000.0);
            
            log::info(
                "[AUDIT UPDATE] Respon Input! Latensi: {:.2f}ms | Real dt: {:.2f}ms | Game dt: {:.5f} | InputTime: {:.4f} -> ExecTime: {:.4f}", 
                latencyMs, realDtMs, dt, g_inputLevelTime.load(), m_gameState.m_levelTime
            );
            
            // Reset state
            g_inputJustHappened.store(false);
        }

        PlayLayer::postUpdate(dt);
    }
};

class $modify(AuditInputLayer, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        auto now = std::chrono::steady_clock::now();
        
        // Simpan waktu nyata dan waktu game ke variabel atomic
        g_realInputTimestamp.store(std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count());
        g_inputLevelTime.store(m_gameState.m_levelTime);
        
        log::info(
            "[AUDIT INPUT] {} | Button: {} | LevelTime: {:.4f}", 
            down ? "PRESS" : "RELEASE", button, m_gameState.m_levelTime
        );
        
        // Beri sinyal ke postUpdate bahwa ada event baru
        g_inputJustHappened.store(true);
        
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }
};
