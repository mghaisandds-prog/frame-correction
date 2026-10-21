#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <chrono>

using namespace geode::prelude;

// Variabel global untuk tracking waktu nyata (Real Time)
auto g_lastUpdateRealTime = std::chrono::steady_clock::now();
bool g_inputJustHappened = false;
int64_t g_realInputTimestamp = 0;
int g_framesSinceInput = 0;

class $modify(AuditPlayLayer, PlayLayer) {
    void update(float dt) {
        // Hitung real-time ms sejak frame terakhir dirender
        auto now = std::chrono::steady_clock::now();
        double realDtMs = std::chrono::duration_cast<std::chrono::microseconds>(now - g_lastUpdateRealTime).count() / 1000.0;
        g_lastUpdateRealTime = now;

        // Jika baru saja ada input masuk, kita log frame INI untuk melihat reaksinya
        if (g_inputJustHappened) {
            double latencyMs = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count() / 1000.0 - (g_realInputTimestamp / 1000.0);
            
            log::info(
                "[AUDIT UPDATE] Frame merespons input! Latensi Sentuh -> Update: {:.2f}ms | Real dt: {:.2f}ms | Game dt: {:.5f} | LevelTime: {:.4f}", 
                latencyMs, realDtMs, dt, m_gameState.m_levelTime
            );
            
            g_inputJustHappened = false; // Reset setelah dicatat
        }

        PlayLayer::update(dt);
    }
};

class $modify(AuditInputLayer, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        // Ambil timestamp seakurat mungkin saat layar HP disentuh
        auto now = std::chrono::steady_clock::now();
        g_realInputTimestamp = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
        
        log::info(
            "[AUDIT INPUT] {} | Button: {} | LevelTime: {:.4f}", 
            down ? "PRESS" : "RELEASE", button, m_gameState.m_levelTime
        );
        
        g_inputJustHappened = true; // Beri tahu fungsi update() bahwa ada input
        
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }
};
