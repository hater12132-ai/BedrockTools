#pragma once

#include "../Module.hpp"
#include <string>
#include <vector>

class ActiveEffectsModule : public Module {
public:
    struct EffectEntry {
        std::string name;
        int amplifier = 0;      // 0-based: 0 = level I
        int durationTicks = 0;  // 20 ticks per second
    };

    ActiveEffectsModule();
    ~ActiveEffectsModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void onFrame() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    void refreshEffects(void* localPlayer);

private:
    std::vector<EffectEntry> m_effects;

    float hudPosX = 16.0f;
    float hudPosY = 420.0f;
    bool isHudModule = true;

    float m_size = 20.0f;
    bool m_background = true;
    float m_backgroundOpacity = 0.55f;
    bool m_showLevel = true;
};
