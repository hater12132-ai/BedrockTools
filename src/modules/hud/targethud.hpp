#pragma once

#include "../Module.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>

// SoupVisuals-style TargetHUD with skin head, smooth HP + absorption bars.
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

    void onAttackTarget(void* actor);

    float hudPosX = -1.0f;
    float hudPosY = -1.0f;
    bool isHudModule = true;

    float scale = 1.0f;
    float backAlpha = 0.92f;
    float liveTime = 4.0f;
    float deathFadeTime = 1.2f;   // fade after HP hits 0 / entity gone
    float animationSpeed = 1.2f;
    float cardWidth = 220.0f;
    float cardHeight = 64.0f;
    float cornerRadius = 14.0f;
    std::string barColor = "#8B5CFF";
    std::string absorptionColor = "#FFC93A";
    bool showArmorRow = true;
    bool showPlayerTag = true;
    bool showHeads = true;
    float damagePerHit = 1.0f;    // client estimate until Attribute API exists

private:
    struct TargetState {
        void* actorPtr = nullptr;       // only used while still in world (re-validated)
        std::uintptr_t actorId = 0;
        std::string name = "Unknown";
        std::string kind = "Player";
        std::string headKey;
        bool hasHead = false;
        float health = 20.0f;
        float maxHealth = 20.0f;
        float absorption = 0.0f;        // gapple layer
        float displayHealth = 20.0f;
        float displayAbsorption = 0.0f;
        float posX = 0, posY = 0, posZ = 0;
        std::chrono::steady_clock::time_point lastHit{};
        std::chrono::steady_clock::time_point diedAt{};
        bool valid = false;
        bool dead = false;
        bool isPlayer = false;
    };

    TargetState m_target;
    float m_anim = 0.0f;
    mutable std::mutex m_mutex;

    static float textWidth(const std::string& text, float size);
};
