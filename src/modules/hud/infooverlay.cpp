#include "infooverlay.hpp"
#include "modules/ModuleRegistry.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <list>
#include <string>
#include <vector>

namespace {

// Same character-width heuristic pingcounter.cpp uses for laying out text without a real
// text-measurement API.
float calcTextWidth(const std::string& text, float size) {
    float width = 0;
    for (char c : text) {
        if (c == 'i' || c == 'l' || c == '1' || c == ':' || c == '.' || c == ' ') width += size * 0.3f;
        else if (c == 'm' || c == 'w' || c == 'M' || c == 'W') width += size * 0.8f;
        else width += size * 0.58f;
    }
    return width;
}

struct Segment {
    std::string text;
    uint32_t color;
};

} // namespace

InfoOverlayModule::InfoOverlayModule()
    : Module("Info Overlay", "Shows an FPS / frame time capsule watermark on screen.") {}

InfoOverlayModule::~InfoOverlayModule() = default;

void InfoOverlayModule::onInit() {}

void InfoOverlayModule::onEnable() {
    m_hasLastFrame = false;
    m_accumSeconds = 0.f;
    m_accumFrameCount = 0;
}

void InfoOverlayModule::onDisable() {}

void InfoOverlayModule::onFrame() {
    if (!enabled) return;

    // ---- 1) sample FPS / frame time purely from render-frame timing ----
    auto now = std::chrono::steady_clock::now();
    if (m_hasLastFrame) {
        float dt = std::chrono::duration<float>(now - m_lastFrameTime).count();
        if (dt > 0.f && dt < 5.f) { // guard against huge stalls (menu open, alt-tab, etc.)
            m_accumSeconds += dt;
            m_accumFrameCount++;

            float interval = m_fpsUpdateSpeed > 0.01f ? m_fpsUpdateSpeed : 0.5f;
            if (m_accumSeconds >= interval && m_accumFrameCount > 0) {
                m_displayFps = (int)std::lround(m_accumFrameCount / m_accumSeconds);
                m_displayFrameMs = (m_accumSeconds / m_accumFrameCount) * 1000.f;
                m_accumSeconds = 0.f;
                m_accumFrameCount = 0;
            }
        }
    }
    m_lastFrameTime = now;
    m_hasLastFrame = true;

    // ---- 2) build the segment list (label / fps / ms), skipping any that are toggled off ----
    std::vector<Segment> segments;
    if (m_showLabel && !m_labelText.empty()) {
        segments.push_back({m_labelText, m_textColorHex});
    }
    if (m_showFps) {
        segments.push_back({std::to_string(m_displayFps) + " FPS", m_textColorHex});
    }
    if (m_showFrameTime) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d ms", (int)std::lround(m_displayFrameMs));
        segments.push_back({buf, m_textColorHex});
    }

    const float badgeSize = m_textSize * 1.45f;
    const float dotRadius = m_textSize * 0.1f;

    // ---- 3) measure content width so the pill hugs its contents like the reference ----
    float contentWidth = 0.f;
    if (m_showLogo) contentWidth += badgeSize + m_itemGap;

    std::vector<float> segWidths;
    segWidths.reserve(segments.size());
    for (size_t i = 0; i < segments.size(); ++i) {
        float w = calcTextWidth(segments[i].text, m_textSize);
        segWidths.push_back(w);
        contentWidth += w;
        if (i + 1 < segments.size()) {
            contentWidth += m_itemGap + (dotRadius * 2.f) + m_itemGap;
        }
    }

    if (contentWidth <= 0.f) return; // nothing to show (everything toggled off)

    const float pillW = contentWidth + m_paddingX * 2.f;
    const float pillH = std::max(badgeSize, m_textSize + 8.f) + m_paddingY * 2.f;

    std::vector<PLModMenu_DrawCommand> cmds;
    std::list<std::string> stringStore; // keep .c_str() pointers alive until submitDrawCommands

    // ---- 4) capsule background (stadium shape: radius = half height = fully rounded ends) ----
    {
        PLModMenu_DrawCommand bg = {};
        bg.type = PL_DRAW_RECT_FILLED;
        bg.x = hudPosX;
        bg.y = hudPosY;
        bg.w = pillW;
        bg.h = pillH;
        bg.x3 = pillH / 2.f;
        int alpha = (int)(m_backgroundOpacity * 255.f);
        bg.color = ((uint32_t)alpha << 24) | (m_backgroundColorHex & 0x00FFFFFF);
        cmds.push_back(bg);
    }

    float cursorX = hudPosX + m_paddingX;
    const float centerY = hudPosY + pillH / 2.f;

    // ---- 5) logo badge: rounded-square + two-stroke checkmark ----
    if (m_showLogo) {
        PLModMenu_DrawCommand badge = {};
        badge.type = PL_DRAW_RECT_FILLED;
        badge.x = cursorX;
        badge.y = centerY - badgeSize / 2.f;
        badge.w = badgeSize;
        badge.h = badgeSize;
        badge.x3 = badgeSize * 0.32f;
        badge.color = m_logoColorHex;
        cmds.push_back(badge);

        // checkmark points, local to the badge box
        float bx = cursorX, by = centerY - badgeSize / 2.f, s = badgeSize;
        float ax_ = bx + s * 0.26f, ay_ = by + s * 0.52f; // start (left)
        float mx_ = bx + s * 0.43f, my_ = by + s * 0.70f; // middle (bottom of the check)
        float cx_ = bx + s * 0.76f, cy_ = by + s * 0.30f; // end (top right)
        float thickness = s * 0.14f;

        PLModMenu_DrawCommand seg1 = {};
        seg1.type = PL_DRAW_LINE;
        seg1.x = ax_; seg1.y = ay_;
        seg1.w = mx_ - ax_; seg1.h = my_ - ay_;
        seg1.size = thickness;
        seg1.color = m_logoCheckColorHex;
        cmds.push_back(seg1);

        PLModMenu_DrawCommand seg2 = {};
        seg2.type = PL_DRAW_LINE;
        seg2.x = mx_; seg2.y = my_;
        seg2.w = cx_ - mx_; seg2.h = cy_ - my_;
        seg2.size = thickness;
        seg2.color = m_logoCheckColorHex;
        cmds.push_back(seg2);

        // small filled joint so the two strokes read as one continuous rounded checkmark
        PLModMenu_DrawCommand joint = {};
        joint.type = PL_DRAW_CIRCLE_FILLED;
        joint.x = mx_; joint.y = my_;
        joint.size = thickness / 2.f;
        joint.color = m_logoCheckColorHex;
        cmds.push_back(joint);

        cursorX += badgeSize + m_itemGap;
    }

    // ---- 6) label / FPS / ms segments, each in its own sub-box so they sit side by side,
    //         with a small dot drawn between adjacent segments ----
    for (size_t i = 0; i < segments.size(); ++i) {
        stringStore.push_back(segments[i].text);

        PLModMenu_DrawCommand txt = {};
        txt.type = PL_DRAW_TEXT;
        txt.x = cursorX;
        txt.y = hudPosY;
        txt.w = segWidths[i];
        txt.h = pillH;
        txt.color = segments[i].color;
        txt.size = m_textSize;
        txt.text = stringStore.back().c_str();
        cmds.push_back(txt);

        cursorX += segWidths[i];

        if (i + 1 < segments.size()) {
            cursorX += m_itemGap;
            PLModMenu_DrawCommand dot = {};
            dot.type = PL_DRAW_CIRCLE_FILLED;
            dot.x = cursorX + dotRadius;
            dot.y = centerY;
            dot.size = dotRadius;
            int alpha = (int)(m_separatorOpacity * 255.f);
            dot.color = ((uint32_t)alpha << 24) | (m_separatorColorHex & 0x00FFFFFF);
            cmds.push_back(dot);
            cursorX += dotRadius * 2.f + m_itemGap;
        }
    }

    submitDrawCommands(moduleId, cmds);
}

void InfoOverlayModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    if (j.contains("hudPosX")) hudPosX = j["hudPosX"].get<float>();
    if (j.contains("hudPosY")) hudPosY = j["hudPosY"].get<float>();
    if (j.contains("isHudModule")) isHudModule = j["isHudModule"].get<bool>();

    if (j.contains("m_labelText")) m_labelText = j["m_labelText"].get<std::string>();
    if (j.contains("m_showLogo")) m_showLogo = j["m_showLogo"].get<bool>();
    if (j.contains("m_showLabel")) m_showLabel = j["m_showLabel"].get<bool>();
    if (j.contains("m_showFps")) m_showFps = j["m_showFps"].get<bool>();
    if (j.contains("m_showFrameTime")) m_showFrameTime = j["m_showFrameTime"].get<bool>();

    if (j.contains("m_textSize")) m_textSize = j["m_textSize"].get<float>();
    if (j.contains("m_paddingX")) m_paddingX = j["m_paddingX"].get<float>();
    if (j.contains("m_paddingY")) m_paddingY = j["m_paddingY"].get<float>();
    if (j.contains("m_itemGap")) m_itemGap = j["m_itemGap"].get<float>();

    if (j.contains("m_backgroundColorHex")) m_backgroundColorHex = j["m_backgroundColorHex"].get<uint32_t>();
    if (j.contains("m_backgroundOpacity")) m_backgroundOpacity = j["m_backgroundOpacity"].get<float>();
    if (j.contains("m_logoColorHex")) m_logoColorHex = j["m_logoColorHex"].get<uint32_t>();
    if (j.contains("m_logoCheckColorHex")) m_logoCheckColorHex = j["m_logoCheckColorHex"].get<uint32_t>();
    if (j.contains("m_textColorHex")) m_textColorHex = j["m_textColorHex"].get<uint32_t>();
    if (j.contains("m_separatorColorHex")) m_separatorColorHex = j["m_separatorColorHex"].get<uint32_t>();
    if (j.contains("m_separatorOpacity")) m_separatorOpacity = j["m_separatorOpacity"].get<float>();

    if (j.contains("m_fpsUpdateSpeed")) m_fpsUpdateSpeed = j["m_fpsUpdateSpeed"].get<float>();
}

void InfoOverlayModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["hudPosX"] = hudPosX;
    j["hudPosY"] = hudPosY;
    j["isHudModule"] = isHudModule;

    j["m_labelText"] = m_labelText;
    j["m_showLogo"] = m_showLogo;
    j["m_showLabel"] = m_showLabel;
    j["m_showFps"] = m_showFps;
    j["m_showFrameTime"] = m_showFrameTime;

    j["m_textSize"] = m_textSize;
    j["m_paddingX"] = m_paddingX;
    j["m_paddingY"] = m_paddingY;
    j["m_itemGap"] = m_itemGap;

    j["m_backgroundColorHex"] = m_backgroundColorHex;
    j["m_backgroundOpacity"] = m_backgroundOpacity;
    j["m_logoColorHex"] = m_logoColorHex;
    j["m_logoCheckColorHex"] = m_logoCheckColorHex;
    j["m_textColorHex"] = m_textColorHex;
    j["m_separatorColorHex"] = m_separatorColorHex;
    j["m_separatorOpacity"] = m_separatorOpacity;

    j["m_fpsUpdateSpeed"] = m_fpsUpdateSpeed;
}
