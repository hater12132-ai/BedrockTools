#pragma once

#include "../Module.hpp"

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>

// SoupVisuals-style TargetHUD for BedrockTools.
class TargetHudModule : public Module {
public:
    TargetHudModule();
    ~TargetHudModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void onFrame() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    // Called from AttackEvent
    void onAttackTarget(void* actor);

    // HUD editor
    float hudPosX = -1.0f;
    float hudPosY = -1.0f;
    bool isHudModule = true;

    // Appearance
    int style = 1;              // 0 Default (sharp), 1 Round
    float scale = 1.0f;
    float backAlpha = 0.85f;
    float liveTime = 3.5f;      // seconds to keep showing after last hit
    int animationMode = 2;      // 0 Scale, 1 Fade, 2 Both
    float animationSpeed = 1.0f;
    float cardWidth = 160.0f;
    float cardHeight = 52.0f;
    float cornerRadius = 10.0f;
    std::string accentColor = "#55FF55";
    bool showDistance = true;
    bool showHealthBar = true;
    bool showHurtFlash = true;

private:
    struct TargetState {
        void* actor = nullptr;
        std::string name = "Unknown";
        float posX = 0, posY = 0, posZ = 0;
        float health = 20.0f;
        float maxHealth = 20.0f;
        float displayHealth = 20.0f; // smoothed
        int hurtTime = 0;
        std::chrono::steady_clock::time_point lastHit{};
        bool valid = false;
    };

    TargetState m_target;
    float m_anim = 0.0f; // 0 hidden .. 1 fully shown
    mutable std::mutex m_mutex;

    static float calcTextWidth(const std::string& text, float size);
};
