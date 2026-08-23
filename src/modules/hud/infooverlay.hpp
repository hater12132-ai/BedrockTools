#pragma once

#include "../Module.hpp"
#include <bedrocktools/sdk/Types.hpp>
#include <chrono>
#include <deque>
#include <string>

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

    // Called from the GetDestroyProgress hook installed by this module.
    // Tracks completed block breaks to derive a rolling blocks-per-second rate.
    void trackBlockProgress(void* block, float increment);

    int m_ping = 0;

private:
    // Layout -----------------------------------------------------------------
    float hudPosX = 16.0f;
    float hudPosY = 16.0f;
    bool isHudModule = true;

    float m_size = 22.0f;
    float m_panelHeight = 58.0f;
    float m_rowGap = 8.0f;
    float m_panelRadius = 29.0f;
    float m_panelPadding = 14.0f;
    float m_iconSize = 34.0f;
    float m_iconGap = 10.0f;
    float m_panelSpacing = 8.0f;
    float m_segmentGap = 20.0f; // gap consumed by the "• " divider between merged segments

    // Flat dark styling - matches the reference's sampled palette exactly:
    // panel #141416, logo #F57E2C, text #F0F0F0, dot #6E6E7A. No glow/blur -
    // the reference panel is a solid flat capsule.
    bool m_background = true;
    bool m_blur = false;
    float m_backgroundOpacity = 0.94f;
    float m_glowOpacity = 0.16f;    // unused while m_blur is false; kept for config compatibility
    float m_iconOpacity = 0.92f;    // unused now the logo is drawn at full strength; kept for config compatibility
    unsigned int m_panelColor = 0x141416;   // flat near-black
    unsigned int m_accentColor = 0xF57E2C;  // logo orange
    unsigned int m_textColor = 0xF0F0F0;
    unsigned int m_mutedColor = 0xB9A9D8;
    unsigned int m_dotColor = 0x6E6E7A;     // muted grey divider dot

    // Content ----------------------------------------------------------------
    std::string m_brand = "everlast.cc";
    bool m_showBrand = true;
    bool m_showFps = true;
    bool m_showPing = false;
    bool m_showCoords = false;
    bool m_showBps = false;

    int m_fps = 0;
    int m_frameAccumulator = 0;
    std::chrono::steady_clock::time_point m_fpsWindowStart{};

    bedrocktools::sdk::Vec3 m_currentPos{0.f, 0.f, 0.f};

    bool m_pingHooked = false;

    // Blocks-per-second tracking ----------------------------------------------
    bool m_breakHooked = false;
    void* m_breakBlock = nullptr;
    float m_breakProgress = 0.0f;
    std::chrono::steady_clock::time_point m_breakLastUpdate{};
    std::deque<std::chrono::steady_clock::time_point> m_breakTimestamps;
    float m_bps = 0.0f;
    static constexpr float kBpsWindowSeconds = 5.0f;
};
