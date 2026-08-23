#pragma once

#include "../Module.hpp"
#include <array>
#include <cstdint>
#include <string>

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

    void onLocalPlayerTick(void* localPlayer);

    float hudPosX = 16.0f;
    float hudPosY = 140.0f;
    bool isHudModule = true;

    float m_nameSize = 26.0f;
    bool m_showHead = true;
    bool m_background = true;
    float m_backgroundOpacity = 0.55f;
    uint32_t m_barColor = 0xFFFF8C00; // orange

private:
    bool m_hasTarget = false;
    std::string m_targetName;
    std::string m_imageKey;

    // Not wired to a real value yet - see refreshHealth()'s comment.
    float m_currentHealth = -1.0f;
    float m_maxHealth = -1.0f;

    int m_refreshTicks = 0;

    void refreshHealth(void* targetActor);
};
