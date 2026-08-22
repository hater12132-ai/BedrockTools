#include "infooverlay.hpp"
#include "modules/ModuleRegistry.hpp"
#include "core/memory/Hooks.hpp"
#include <bedrocktools/Version.hpp>
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/world/Actor.hpp>
#include <algorithm>
#include <cmath>
#include <sstream>
#include <vector>

// Same signature + offset PingCounterModule uses to read the RakNet connector's
// average ping. This keeps the overlay independent from PingCounterModule.
static void (*_infoOverlayRaknetUpdate_orig)(void* _this);
static InfoOverlayModule* g_infoOverlayMod = nullptr;

static void _infoOverlayRaknetUpdate_hook(void* _this) {
    if (_infoOverlayRaknetUpdate_orig) _infoOverlayRaknetUpdate_orig(_this);
    if (g_infoOverlayMod && g_infoOverlayMod->enabled) {
        int avgPing = *(int*)((uintptr_t)_this + bedrocktools::sdk::offsets::RakNetConnector::mAvgPing);
        if (avgPing >= 0) g_infoOverlayMod->updatePing(avgPing);
    }
}

namespace {

// The draw API exposed by BedrockTools does not expose a real Gaussian blur
// primitive. We therefore build a convincing "glass blur" from several very
// low-alpha expanded pills behind the main panel. On mobile this is much
// cheaper than a framebuffer blur while still producing the soft purple halo
// visible in the reference design.
static unsigned int withAlpha(unsigned int rgb, int alpha) {
    alpha = std::clamp(alpha, 0, 255);
    return (static_cast<unsigned int>(alpha) << 24) | (rgb & 0x00FFFFFF);
}

static unsigned int multiplyAlpha(unsigned int rgb, float opacity) {
    return withAlpha(rgb, static_cast<int>(std::round(std::clamp(opacity, 0.0f, 1.0f) * 255.0f)));
}

static float calcTextWidth(const std::string& text, float size) {
    float width = 0.0f;
    for (unsigned char c : text) {
        if (c == 'i' || c == 'l' || c == '1' || c == ':' || c == '.' || c == ' ')
            width += size * 0.30f;
        else if (c == 'm' || c == 'w' || c == 'M' || c == 'W')
            width += size * 0.80f;
        else
            width += size * 0.58f;
    }
    return width;
}

static void addRect(std::vector<PLModMenu_DrawCommand>& cmds,
                    float x, float y, float w, float h, unsigned int color) {
    PLModMenu_DrawCommand c = {};
    c.type = PL_DRAW_RECT_FILLED;
    c.x = x;
    c.y = y;
    c.w = std::max(0.0f, w);
    c.h = std::max(0.0f, h);
    c.color = color;
    cmds.push_back(c);
}

static void addCircle(std::vector<PLModMenu_DrawCommand>& cmds,
                      float cx, float cy, float diameter, unsigned int color) {
    PLModMenu_DrawCommand c = {};
    c.type = PL_DRAW_CIRCLE_FILLED;
    c.x = cx;
    c.y = cy;
    c.size = diameter;
    c.color = color;
    cmds.push_back(c);
}

static void addText(std::vector<PLModMenu_DrawCommand>& cmds,
                    float x, float y, float w, float h, float size,
                    unsigned int color, const std::string& text) {
    PLModMenu_DrawCommand c = {};
    c.type = PL_DRAW_TEXT;
    c.x = x;
    c.y = y;
    c.w = w;
    c.h = h;
    c.color = color;
    c.size = size;
    c.text = text.c_str();
    cmds.push_back(c);
}

static void drawPill(std::vector<PLModMenu_DrawCommand>& cmds,
                     float x, float y, float w, float h,
                     unsigned int rgb, unsigned int color) {
    const float radius = h * 0.5f;
    const float bodyW = std::max(0.0f, w - h);

    addRect(cmds, x + radius, y, bodyW, h, color);
    addCircle(cmds, x + radius, y + radius, h, color);
    addCircle(cmds, x + w - radius, y + radius, h, color);
}

static void drawGlassPill(std::vector<PLModMenu_DrawCommand>& cmds,
                          float x, float y, float w, float h,
                          unsigned int panelColor,
                          unsigned int accentColor,
                          float backgroundOpacity,
                          float glowOpacity,
                          bool blur) {
    // Soft outer purple halo. Multiple layers are intentionally subtle.
    if (blur) {
        const float spreads[] = { 7.0f, 4.0f, 2.0f };
        const float opacities[] = { 0.025f, 0.045f, 0.070f };
        for (int i = 0; i < 3; ++i) {
            drawPill(cmds,
                     x - spreads[i], y - spreads[i],
                     w + spreads[i] * 2.0f,
                     h + spreads[i] * 2.0f,
                     accentColor,
                     multiplyAlpha(accentColor, glowOpacity * (opacities[i] / 0.07f)));
        }
    }

    // Dark translucent glass body.
    drawPill(cmds, x, y, w, h, panelColor, multiplyAlpha(panelColor, backgroundOpacity));

    // Very thin purple inner highlight. It is deliberately offset toward the
    // top so the panel reads like glass instead of a flat black rectangle.
    const float highlightH = std::max(2.0f, h * 0.055f);
    drawPill(cmds,
             x + 2.0f, y + 2.0f,
             std::max(0.0f, w - 4.0f), highlightH + 4.0f,
             accentColor,
             multiplyAlpha(accentColor, 0.10f));
}

static void drawIconBubble(std::vector<PLModMenu_DrawCommand>& cmds,
                           float cx, float cy, float size,
                           unsigned int accentColor, float opacity) {
    addCircle(cmds, cx, cy, size, multiplyAlpha(accentColor, opacity * 0.20f));
    addCircle(cmds, cx, cy, size * 0.72f, multiplyAlpha(accentColor, opacity));
}

// Small custom icon: three ascending FPS bars.
static void drawFpsIcon(std::vector<PLModMenu_DrawCommand>& cmds,
                        float cx, float cy, float size, unsigned int color) {
    const float barW = size * 0.105f;
    const float gap = size * 0.075f;
    const float h1 = size * 0.27f;
    const float h2 = size * 0.46f;
    const float h3 = size * 0.67f;
    const float left = cx - (barW * 1.5f + gap);

    addRect(cmds, left,            cy + size * 0.33f, barW, h1, color);
    addRect(cmds, left + barW+gap, cy + size * 0.14f, barW, h2, color);
    addRect(cmds, left + 2*(barW+gap), cy - size * 0.08f, barW, h3, color);
}

// Small custom icon: four ascending network/signal bars.
static void drawSignalIcon(std::vector<PLModMenu_DrawCommand>& cmds,
                           float cx, float cy, float size, unsigned int color) {
    const float barW = size * 0.10f;
    const float gap = size * 0.075f;
    const float heights[] = { 0.22f, 0.36f, 0.51f, 0.68f };
    const float totalW = 4.0f * barW + 3.0f * gap;
    const float left = cx - totalW * 0.5f;

    for (int i = 0; i < 4; ++i) {
        const float h = size * heights[i];
        addRect(cmds,
                left + i * (barW + gap),
                cy + size * 0.32f - h,
                barW, h,
                color);
    }
}

// Minimal coordinate/crosshair icon built entirely from circles/rectangles so
// it works with the same draw primitives already used by this module.
static void drawCoordsIcon(std::vector<PLModMenu_DrawCommand>& cmds,
                           float cx, float cy, float size, unsigned int color) {
    const float arm = size * 0.25f;
    const float thick = std::max(2.0f, size * 0.075f);
    addRect(cmds, cx - arm, cy - thick * 0.5f, arm * 2.0f, thick, color);
    addRect(cmds, cx - thick * 0.5f, cy - arm, thick, arm * 2.0f, color);
    addCircle(cmds, cx, cy, size * 0.20f, color);
}

static void drawIconDivider(std::vector<PLModMenu_DrawCommand>& cmds,
                            float x, float y, unsigned int color) {
    addCircle(cmds, x, y, 4.0f, color);
}

} // namespace

InfoOverlayModule::InfoOverlayModule()
    : Module("Info Overlay", "Clean purple glass FPS, ping and coordinates HUD.") {
    g_infoOverlayMod = this;
}

InfoOverlayModule::~InfoOverlayModule() {
    if (g_infoOverlayMod == this) g_infoOverlayMod = nullptr;
}

void InfoOverlayModule::updatePing(int ping) {
    m_ping = ping;
}

void InfoOverlayModule::onInit() {
    if (!m_pingHooked) {
        uintptr_t addr = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::RaknetUpdate);
        if (addr != 0) {
            bedrocktools::hooks::install((void*)addr,
                                         (void*)_infoOverlayRaknetUpdate_hook,
                                         (void**)&_infoOverlayRaknetUpdate_orig);
            m_pingHooked = true;
        }
    }

    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>([](auto& event) {
        if (g_infoOverlayMod && g_infoOverlayMod->enabled && event.player) {
            g_infoOverlayMod->m_currentPos = event.player->position();
        }
    });
}

void InfoOverlayModule::onEnable() {
    m_fps = 0;
    m_frameAccumulator = 0;
    m_fpsWindowStart = std::chrono::steady_clock::now();
}

void InfoOverlayModule::onDisable() {
    m_fps = 0;
    m_frameAccumulator = 0;
}

void InfoOverlayModule::onFrame() {
    if (!enabled) return;

    ++m_frameAccumulator;
    auto now = std::chrono::steady_clock::now();
    std::chrono::duration<double> elapsed = now - m_fpsWindowStart;
    if (elapsed.count() >= 1.0) {
        m_fps = static_cast<int>(m_frameAccumulator / elapsed.count());
        m_frameAccumulator = 0;
        m_fpsWindowStart = now;
    }

    const int x = static_cast<int>(std::round(m_currentPos.x));
    const int y = static_cast<int>(std::round(m_currentPos.y));
    const int z = static_cast<int>(std::round(m_currentPos.z));

    std::ostringstream fpsText;
    fpsText << m_fps << " fps";

    std::ostringstream pingText;
    pingText << m_ping << " ms";

    std::ostringstream coordsText;
    coordsText << "x-" << x << "  y-" << y << "  z-" << z;

    const std::string fpsString = fpsText.str();
    const std::string pingString = pingText.str();
    const std::string coordsString = coordsText.str();

    // Screenshot-inspired hierarchy:
    // [brand] • [fps] • [ping]
    // [coords]                 [blocks/sec placeholder]
    // The panels are independent, so hiding one metric doesn't leave ugly gaps.
    std::vector<PLModMenu_DrawCommand> cmds;

    const float topH = m_panelHeight;
    const float rowH = m_panelHeight;
    const float icon = m_iconSize;
    const float textSize = m_size;
    const float innerY = m_panelPadding;

    float cursorX = hudPosX;
    const float topY = hudPosY;

    // Brand panel ------------------------------------------------------------
    if (m_showBrand) {
        const float brandW = m_panelPadding * 2.0f + icon + m_iconGap + calcTextWidth(m_brand, textSize);
        if (m_background) {
            drawGlassPill(cmds, cursorX, topY, brandW, topH,
                          m_panelColor, m_accentColor,
                          m_backgroundOpacity, m_glowOpacity, m_blur);
        }

        const float iconCX = cursorX + m_panelPadding + icon * 0.5f;
        const float iconCY = topY + topH * 0.5f;
        drawIconBubble(cmds, iconCX, iconCY, icon, m_accentColor, m_iconOpacity);
        // Stylized E mark.
        addRect(cmds, iconCX - icon * 0.18f, iconCY - icon * 0.22f,
                icon * 0.10f, icon * 0.44f, multiplyAlpha(m_textColor, 0.95f));
        addRect(cmds, iconCX - icon * 0.08f, iconCY - icon * 0.22f,
                icon * 0.22f, icon * 0.07f, multiplyAlpha(m_textColor, 0.95f));
        addRect(cmds, iconCX - icon * 0.08f, iconCY - icon * 0.035f,
                icon * 0.16f, icon * 0.07f, multiplyAlpha(m_textColor, 0.95f));
        addRect(cmds, iconCX - icon * 0.08f, iconCY + icon * 0.15f,
                icon * 0.22f, icon * 0.07f, multiplyAlpha(m_textColor, 0.95f));

        addText(cmds,
                cursorX + m_panelPadding + icon + m_iconGap,
                topY + (topH - textSize) * 0.5f,
                brandW - m_panelPadding * 2.0f - icon - m_iconGap,
                topH,
                textSize,
                m_textColor,
                m_brand);

        cursorX += brandW + m_panelSpacing;
    }

    // FPS panel --------------------------------------------------------------
    if (m_showFps) {
        const float fpsW = m_panelPadding * 2.0f + icon + m_iconGap + calcTextWidth(fpsString, textSize);
        if (m_background) {
            drawGlassPill(cmds, cursorX, topY, fpsW, topH,
                          m_panelColor, m_accentColor,
                          m_backgroundOpacity, m_glowOpacity, m_blur);
        }

        const float iconCX = cursorX + m_panelPadding + icon * 0.5f;
        const float iconCY = topY + topH * 0.5f;
        drawIconBubble(cmds, iconCX, iconCY, icon, m_accentColor, m_iconOpacity);
        drawFpsIcon(cmds, iconCX, iconCY, icon, multiplyAlpha(m_textColor, 0.95f));

        addText(cmds,
                cursorX + m_panelPadding + icon + m_iconGap,
                topY + (topH - textSize) * 0.5f,
                fpsW - m_panelPadding * 2.0f - icon - m_iconGap,
                topH,
                textSize,
                m_textColor,
                fpsString);

        cursorX += fpsW + m_panelSpacing;
    }

    // Ping panel -------------------------------------------------------------
    if (m_showPing) {
        const float pingW = m_panelPadding * 2.0f + icon + m_iconGap + calcTextWidth(pingString, textSize);
        if (m_background) {
            drawGlassPill(cmds, cursorX, topY, pingW, topH,
                          m_panelColor, m_accentColor,
                          m_backgroundOpacity, m_glowOpacity, m_blur);
        }

        const float iconCX = cursorX + m_panelPadding + icon * 0.5f;
        const float iconCY = topY + topH * 0.5f;
        drawIconBubble(cmds, iconCX, iconCY, icon, m_accentColor, m_iconOpacity);
        drawSignalIcon(cmds, iconCX, iconCY, icon, multiplyAlpha(m_textColor, 0.95f));

        addText(cmds,
                cursorX + m_panelPadding + icon + m_iconGap,
                topY + (topH - textSize) * 0.5f,
                pingW - m_panelPadding * 2.0f - icon - m_iconGap,
                topH,
                textSize,
                m_textColor,
                pingString);
    }

    // Coordinates panel -----------------------------------------------------
    if (m_showCoords) {
        const float coordsY = topY + topH + m_rowGap;
        const float coordsW = m_panelPadding * 2.0f + icon + m_iconGap + calcTextWidth(coordsString, textSize);

        if (m_background) {
            drawGlassPill(cmds, hudPosX, coordsY, coordsW, rowH,
                          m_panelColor, m_accentColor,
                          m_backgroundOpacity, m_glowOpacity, m_blur);
        }

        const float iconCX = hudPosX + m_panelPadding + icon * 0.5f;
        const float iconCY = coordsY + rowH * 0.5f;
        drawIconBubble(cmds, iconCX, iconCY, icon, m_accentColor, m_iconOpacity);
        drawCoordsIcon(cmds, iconCX, iconCY, icon, multiplyAlpha(m_textColor, 0.95f));

        addText(cmds,
                hudPosX + m_panelPadding + icon + m_iconGap,
                coordsY + (rowH - textSize) * 0.5f,
                coordsW - m_panelPadding * 2.0f - icon - m_iconGap,
                rowH,
                textSize,
                m_textColor,
                coordsString);
    }

    submitDrawCommands(moduleId, cmds);
}

void InfoOverlayModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);

    if (j.contains("hudPosX")) hudPosX = j["hudPosX"].get<float>();
    if (j.contains("hudPosY")) hudPosY = j["hudPosY"].get<float>();
    if (j.contains("isHudModule")) isHudModule = j["isHudModule"].get<bool>();

    if (j.contains("m_size")) m_size = j["m_size"].get<float>();
    if (j.contains("m_panelHeight")) m_panelHeight = j["m_panelHeight"].get<float>();
    if (j.contains("m_rowGap")) m_rowGap = j["m_rowGap"].get<float>();
    if (j.contains("m_panelRadius")) m_panelRadius = j["m_panelRadius"].get<float>();
    if (j.contains("m_panelPadding")) m_panelPadding = j["m_panelPadding"].get<float>();
    if (j.contains("m_iconSize")) m_iconSize = j["m_iconSize"].get<float>();
    if (j.contains("m_iconGap")) m_iconGap = j["m_iconGap"].get<float>();
    if (j.contains("m_panelSpacing")) m_panelSpacing = j["m_panelSpacing"].get<float>();

    if (j.contains("m_background")) m_background = j["m_background"].get<bool>();
    if (j.contains("m_blur")) m_blur = j["m_blur"].get<bool>();
    if (j.contains("m_backgroundOpacity")) m_backgroundOpacity = j["m_backgroundOpacity"].get<float>();
    if (j.contains("m_glowOpacity")) m_glowOpacity = j["m_glowOpacity"].get<float>();
    if (j.contains("m_iconOpacity")) m_iconOpacity = j["m_iconOpacity"].get<float>();
    if (j.contains("m_panelColor")) m_panelColor = j["m_panelColor"].get<unsigned int>();
    if (j.contains("m_accentColor")) m_accentColor = j["m_accentColor"].get<unsigned int>();
    if (j.contains("m_textColor")) m_textColor = j["m_textColor"].get<unsigned int>();
    if (j.contains("m_mutedColor")) m_mutedColor = j["m_mutedColor"].get<unsigned int>();

    if (j.contains("m_brand")) m_brand = j["m_brand"].get<std::string>();
    if (j.contains("m_showBrand")) m_showBrand = j["m_showBrand"].get<bool>();
    if (j.contains("m_showFps")) m_showFps = j["m_showFps"].get<bool>();
    if (j.contains("m_showPing")) m_showPing = j["m_showPing"].get<bool>();
    if (j.contains("m_showCoords")) m_showCoords = j["m_showCoords"].get<bool>();
}

void InfoOverlayModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);

    j["hudPosX"] = hudPosX;
    j["hudPosY"] = hudPosY;
    j["isHudModule"] = isHudModule;

    j["m_size"] = m_size;
    j["m_panelHeight"] = m_panelHeight;
    j["m_rowGap"] = m_rowGap;
    j["m_panelRadius"] = m_panelRadius;
    j["m_panelPadding"] = m_panelPadding;
    j["m_iconSize"] = m_iconSize;
    j["m_iconGap"] = m_iconGap;
    j["m_panelSpacing"] = m_panelSpacing;

    j["m_background"] = m_background;
    j["m_blur"] = m_blur;
    j["m_backgroundOpacity"] = m_backgroundOpacity;
    j["m_glowOpacity"] = m_glowOpacity;
    j["m_iconOpacity"] = m_iconOpacity;
    j["m_panelColor"] = m_panelColor;
    j["m_accentColor"] = m_accentColor;
    j["m_textColor"] = m_textColor;
    j["m_mutedColor"] = m_mutedColor;

    j["m_brand"] = m_brand;
    j["m_showBrand"] = m_showBrand;
    j["m_showFps"] = m_showFps;
    j["m_showPing"] = m_showPing;
    j["m_showCoords"] = m_showCoords;
}
