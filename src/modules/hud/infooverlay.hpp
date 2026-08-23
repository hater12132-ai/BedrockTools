#pragma once

#include "../Module.hpp"
#include <chrono>
#include <cstdint>
#include <string>

// "Info Overlay" -- a rounded capsule watermark showing a logo badge, a label, live FPS,
// and frame time (ms), styled after the reference screenshot ("Really Visuals" pill with
// an orange checkmark badge, e.g. "RV  Really Visuals · 2251 FPS · 5 ms").
//
// Unlike TotemTweaksModule, this one needs no new memory signatures at all -- FPS/frame
// time are measured purely from onFrame() timing (this project's per-rendered-frame
// callback), and everything else is drawn with draw command types this project already
// uses elsewhere (PL_DRAW_RECT_FILLED with the x3 corner-radius field for the pill/badge,
// PL_DRAW_LINE for the checkmark strokes, PL_DRAW_CIRCLE_FILLED for the joint + dot
// separators, PL_DRAW_TEXT for the label/stats) -- see breakindicator.cpp / pingcounter.cpp
// / debugmenu.cpp for the exact precedents each piece below is drawn from.
class InfoOverlayModule : public Module {
public:
    InfoOverlayModule();
    ~InfoOverlayModule() override;

    void onInit()    override;
    void onEnable()  override;
    void onDisable() override;
    void onFrame()   override;

    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j)       override;

    // -- content --
    std::string m_labelText   = "Really Visuals"; // wordmark shown next to the badge; edit freely to rebrand
    bool m_showLogo           = true;
    bool m_showLabel          = true;
    bool m_showFps            = true;
    bool m_showFrameTime      = true;

    // -- layout --
    float hudPosX   = 20.f;
    float hudPosY   = 20.f;
    bool  isHudModule = true;

    float m_textSize  = 22.f;  // px, drives badge size and row height too
    float m_paddingX  = 16.f;  // inner left/right padding of the pill
    float m_paddingY  = 10.f;  // inner top/bottom padding of the pill
    float m_itemGap   = 10.f;  // gap around each " · " separator and after the badge

    // -- colors (RGB packed with FF alpha baked in, same convention as
    //    breakindicator.cpp's m_barColorHex / m_outlineColorHex) --
    uint32_t m_backgroundColorHex   = 0xFF141414; // near-black capsule fill
    float    m_backgroundOpacity    = 0.82f;      // 0-1, applied on top of the RGB above
    uint32_t m_logoColorHex         = 0xFFFF8C1A; // orange badge
    uint32_t m_logoCheckColorHex    = 0xFFFFFFFF; // checkmark stroke
    uint32_t m_textColorHex         = 0xFFFFFFFF; // label / FPS / ms text
    uint32_t m_separatorColorHex    = 0xFF8C8C8C; // the small dots between segments
    float    m_separatorOpacity     = 0.9f;

    // -- FPS/frame-time sampling --
    float m_fpsUpdateSpeed = 0.5f; // seconds between readout refreshes (named for the menu's "speed" range: 0.05-1.0)

private:
    std::chrono::steady_clock::time_point m_lastFrameTime{};
    bool  m_hasLastFrame     = false;
    float m_accumSeconds     = 0.f;
    int   m_accumFrameCount  = 0;
    int   m_displayFps       = 0;
    float m_displayFrameMs   = 0.f;
};
