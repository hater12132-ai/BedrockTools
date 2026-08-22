#pragma once

#include "../Module.hpp"
#include <bedrocktools/sdk/Types.hpp>
#include <chrono>

class InfoOverlayModule : public Module {
public:
    InfoOverlayModule();
    ~InfoOverlayModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void onFrame() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    // Called from the RaknetUpdate hook installed by this module.
    void updatePing(int ping);

    int m_ping = 0;

private:
    float hudPosX = 16.0f;
    float hudPosY = 16.0f;
    bool isHudModule = true;

    float m_size = 24.0f;
    bool m_background = true;
    float m_backgroundOpacity = 0.45f;
    bool m_showCoords = true;
    unsigned int m_pillColor = 0x000000; // RGB only, alpha comes from m_backgroundOpacity

    int m_fps = 0;
    int m_frameAccumulator = 0;
    std::chrono::steady_clock::time_point m_fpsWindowStart{};

    bedrocktools::sdk::Vec3 m_currentPos{0.f, 0.f, 0.f};

    bool m_pingHooked = false;
};
