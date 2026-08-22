#include "infooverlay.hpp"
#include "modules/ModuleRegistry.hpp"
#include "core/memory/Hooks.hpp"
#include <bedrocktools/Version.hpp>
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/world/Actor.hpp>
#include <algorithm>
#include <sstream>

// Same signature + offset PingCounterModule uses to read the RakNet connector's
// average ping. Multiple modules independently hooking the same signature is an
// established pattern elsewhere in this codebase (e.g. GetFov is hooked by both
// Zoom and other visual modules), so this does not conflict with PingCounterModule
// being enabled at the same time.
static void (*_infoOverlayRaknetUpdate_orig)(void* _this);
static InfoOverlayModule* g_infoOverlayMod = nullptr;

static void _infoOverlayRaknetUpdate_hook(void* _this) {
    if (_infoOverlayRaknetUpdate_orig) _infoOverlayRaknetUpdate_orig(_this);
    if (g_infoOverlayMod && g_infoOverlayMod->enabled) {
        int avgPing = *(int*)((uintptr_t)_this + bedrocktools::sdk::offsets::RakNetConnector::mAvgPing);
        if (avgPing >= 0) g_infoOverlayMod->updatePing(avgPing);
    }
}

static float calcTextWidth(const std::string& text, float size) {
    float width = 0;
    for (char c : text) {
        if (c == 'i' || c == 'l' || c == '1' || c == ':' || c == '.' || c == ' ') width += size * 0.3f;
        else if (c == 'm' || c == 'w' || c == 'M' || c == 'W') width += size * 0.8f;
        else width += size * 0.58f;
    }
    return width;
}

// Draws a stadium/"pill" shape: a filled rect body with a filled circle capping
// each end. CircleFilled here takes a center x/y and uses `size` as its diameter,
// matching the working usage in debugmenu.cpp's minimap dots.
static void drawPill(std::vector<PLModMenu_DrawCommand>& cmds, float x, float y, float w, float h, unsigned int rgb, int alpha) {
    unsigned int color = (static_cast<unsigned int>(alpha) << 24) | (rgb & 0x00FFFFFF);
    float radius = h * 0.5f;
    float bodyW = std::max(0.0f, w - h);

    PLModMenu_DrawCommand body = {};
    body.type = PL_DRAW_RECT_FILLED;
    body.x = x + radius;
    body.y = y;
    body.w = bodyW;
    body.h = h;
    body.color = color;
    cmds.push_back(body);

    PLModMenu_DrawCommand capLeft = {};
    capLeft.type = PL_DRAW_CIRCLE_FILLED;
    capLeft.x = x + radius;
    capLeft.y = y + radius;
    capLeft.size = h;
    capLeft.color = color;
    cmds.push_back(capLeft);

    PLModMenu_DrawCommand capRight = capLeft;
    capRight.x = x + w - radius;
    cmds.push_back(capRight);
}

InfoOverlayModule::InfoOverlayModule()
    : Module("Info Overlay", "Shows an fps/ping bar and your coordinates, styled like a rounded pill HUD.") {
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
            bedrocktools::hooks::install((void*)addr, (void*)_infoOverlayRaknetUpdate_hook, (void**)&_infoOverlayRaknetUpdate_orig);
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

    // ModuleRegistry::onFrame() is itself driven by the FrameEvent (one call per real
    // rendered frame, via eglSwapBuffers), so counting calls to this function directly
    // gives an accurate fps reading without a second subscription.
    ++m_frameAccumulator;
    auto now = std::chrono::steady_clock::now();
    std::chrono::duration<double> elapsed = now - m_fpsWindowStart;
    if (elapsed.count() >= 1.0) {
        m_fps = static_cast<int>(m_frameAccumulator / elapsed.count());
        m_frameAccumulator = 0;
        m_fpsWindowStart = now;
    }

    std::ostringstream row1;
    row1 << bedrocktools::Name << "  \xE2\x80\xA2  " << m_fps << " fps  \xE2\x80\xA2  " << m_ping << " ms";
    const std::string line1 = row1.str();

    std::ostringstream row2;
    row2 << "X " << (int)m_currentPos.x << "   Y " << (int)m_currentPos.y << "   Z " << (int)m_currentPos.z;
    const std::string line2 = row2.str();

    std::vector<PLModMenu_DrawCommand> cmds;
    float padX = 12.0f;
    float rowH = m_size + 10.0f;
    float gap = 6.0f;

    float w1 = calcTextWidth(line1, m_size) + padX * 2.0f;
    int alpha = (int)(m_backgroundOpacity * 255.0f);

    if (m_background) drawPill(cmds, hudPosX, hudPosY, w1, rowH, m_pillColor, alpha);

    PLModMenu_DrawCommand text1 = {};
    text1.type = PL_DRAW_TEXT;
    text1.x = hudPosX + padX;
    text1.y = hudPosY + (rowH - m_size) * 0.5f;
    text1.w = w1;
    text1.h = rowH;
    text1.color = 0xFFFFFFFF;
    text1.size = m_size;
    text1.text = line1.c_str();
    cmds.push_back(text1);

    if (m_showCoords) {
        float row2Y = hudPosY + rowH + gap;
        float w2 = calcTextWidth(line2, m_size) + padX * 2.0f;

        if (m_background) drawPill(cmds, hudPosX, row2Y, w2, rowH, m_pillColor, alpha);

        PLModMenu_DrawCommand text2 = {};
        text2.type = PL_DRAW_TEXT;
        text2.x = hudPosX + padX;
        text2.y = row2Y + (rowH - m_size) * 0.5f;
        text2.w = w2;
        text2.h = rowH;
        text2.color = 0xFFFFFFFF;
        text2.size = m_size;
        text2.text = line2.c_str();
        cmds.push_back(text2);
    }

    submitDrawCommands(moduleId, cmds);
}

void InfoOverlayModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    if (j.contains("hudPosX")) hudPosX = j["hudPosX"].get<float>();
    if (j.contains("hudPosY")) hudPosY = j["hudPosY"].get<float>();
    if (j.contains("isHudModule")) isHudModule = j["isHudModule"].get<bool>();
    if (j.contains("m_size")) m_size = j["m_size"].get<float>();
    if (j.contains("m_background")) m_background = j["m_background"].get<bool>();
    if (j.contains("m_backgroundOpacity")) m_backgroundOpacity = j["m_backgroundOpacity"].get<float>();
    if (j.contains("m_showCoords")) m_showCoords = j["m_showCoords"].get<bool>();
}

void InfoOverlayModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["hudPosX"] = hudPosX;
    j["hudPosY"] = hudPosY;
    j["isHudModule"] = isHudModule;
    j["m_size"] = m_size;
    j["m_background"] = m_background;
    j["m_backgroundOpacity"] = m_backgroundOpacity;
    j["m_showCoords"] = m_showCoords;
}
