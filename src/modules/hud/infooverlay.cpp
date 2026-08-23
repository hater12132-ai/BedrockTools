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
#include <cstdio>
#include <iomanip>
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

// Same technique BreakIndicatorModule uses to derive per-block break progress.
// We only care about the moment progress crosses 1.0 (a block finished
// breaking), which we use to derive a rolling blocks-per-second rate.
static float (*_infoOverlayDestroyProgress_orig)(void* _this, void* block);

static float _infoOverlayDestroyProgress_hook(void* _this, void* block) {
    float increment = 0.0f;
    if (_infoOverlayDestroyProgress_orig) {
        increment = _infoOverlayDestroyProgress_orig(_this, block);
    }
    if (g_infoOverlayMod && g_infoOverlayMod->enabled) {
        g_infoOverlayMod->trackBlockProgress(block, increment);
    }
    return increment;
}

namespace {

enum class SegmentIcon { None, Brand };

struct Segment {
    SegmentIcon icon = SegmentIcon::None;
    std::string text;
};

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

// BreakIndicatorModule's color settings (barColorHex / textColorHex /
// outlineColorHex) are stored as "#RRGGBB" strings, not raw numbers, and
// those are the ones that actually render as color pickers in the mod menu.
// The previous version of this file stored colors as plain JSON integers,
// which is almost certainly why the color settings weren't behaving like
// color pickers. Matching that exact convention here.
static bool loadHexColor(const nlohmann::json& j, const char* key, unsigned int& outRgb) {
    if (!j.contains(key) || !j[key].is_string()) return false;
    const std::string hexStr = j[key].get<std::string>();
    if (hexStr.empty() || hexStr[0] != '#') return false;
    try {
        outRgb = static_cast<unsigned int>(std::stoul(hexStr.substr(1), nullptr, 16)) & 0x00FFFFFFu;
        return true;
    } catch (...) {
        return false;
    }
}

static std::string saveHexColor(unsigned int rgb) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "#%06X", rgb & 0x00FFFFFFu);
    return std::string(buf);
}

// NOTE: this is only an estimate - the real font metrics run slightly wider
// than a flat per-character guess can capture. We deliberately overestimate
// (rather than match exactly) and add a flat safety margin, because an
// undersized estimate here means the next icon/divider gets placed on top of
// this text instead of after it.
static float calcTextWidth(const std::string& text, float size) {
    float width = 0.0f;
    for (unsigned char c : text) {
        if (c == 'i' || c == 'l' || c == '1' || c == ':' || c == '.' || c == ' ')
            width += size * 0.34f;
        else if (c == 'm' || c == 'w' || c == 'M' || c == 'W')
            width += size * 0.86f;
        else
            width += size * 0.64f;
    }
    return width + size * 0.35f; // flat safety margin per string
}

// Every draw call below snaps to whole pixels. Fractional x/y (and a
// fractional font size on text) is the classic cause of blurry output in
// bitmap/atlas-based immediate-mode renderers like this one - the previous
// version fed it accumulated sums of size*0.34f etc. and never rounded
// anything, which is very likely what you were seeing.
static float px(float v) { return std::round(v); }

static void addRect(std::vector<PLModMenu_DrawCommand>& cmds,
                    float x, float y, float w, float h, unsigned int color) {
    PLModMenu_DrawCommand c = {};
    c.type = PL_DRAW_RECT_FILLED;
    c.x = px(x);
    c.y = px(y);
    c.w = px(std::max(0.0f, w));
    c.h = px(std::max(0.0f, h));
    c.color = color;
    cmds.push_back(c);
}

// Native rounded rect via the x3 corner-radius field - the same mechanism
// BreakIndicatorModule and KeystrokesModule already use for rounded panels.
// A radius of h/2 on both ends produces a true stadium/capsule shape in a
// single draw call, instead of gluing a rect to two circles by hand.
static void addRoundedRect(std::vector<PLModMenu_DrawCommand>& cmds,
                           float x, float y, float w, float h,
                           float radius, unsigned int color) {
    PLModMenu_DrawCommand c = {};
    c.type = PL_DRAW_RECT_FILLED;
    c.x = px(x);
    c.y = px(y);
    c.w = px(std::max(0.0f, w));
    c.h = px(std::max(0.0f, h));
    c.x3 = px(std::clamp(radius, 0.0f, std::max(0.0f, w) * 0.5f));
    c.color = color;
    cmds.push_back(c);
}

static void addCircle(std::vector<PLModMenu_DrawCommand>& cmds,
                      float cx, float cy, float diameter, unsigned int color) {
    PLModMenu_DrawCommand c = {};
    c.type = PL_DRAW_CIRCLE_FILLED;
    c.x = px(cx);
    c.y = px(cy);
    c.size = px(diameter);
    c.color = color;
    cmds.push_back(c);
}

static void addText(std::vector<PLModMenu_DrawCommand>& cmds,
                    float x, float y, float w, float h, float size,
                    unsigned int color, const std::string& text) {
    PLModMenu_DrawCommand c = {};
    c.type = PL_DRAW_TEXT;
    c.x = px(x);
    c.y = px(y);
    c.w = px(w);
    c.h = px(h);
    c.color = color;
    c.size = px(size); // whole-pixel font size - fractional sizes are what smear text
    c.text = text.c_str();
    cmds.push_back(c);
}

// Line endpoint is (x + w, y + h); size is stroke thickness. Mirrors the
// semantics CompassModule already relies on for its tick marks.
static void addLine(std::vector<PLModMenu_DrawCommand>& cmds,
                    float x0, float y0, float x1, float y1,
                    float thickness, unsigned int color) {
    PLModMenu_DrawCommand c = {};
    const float rx0 = px(x0), ry0 = px(y0), rx1 = px(x1), ry1 = px(y1);
    c.type = PL_DRAW_LINE;
    c.x = rx0;
    c.y = ry0;
    c.w = rx1 - rx0;
    c.h = ry1 - ry0;
    c.size = std::max(1.0f, px(thickness));
    c.color = color;
    cmds.push_back(c);
}

static void drawFlatPill(std::vector<PLModMenu_DrawCommand>& cmds,
                         float x, float y, float w, float h,
                         unsigned int panelColor, float backgroundOpacity) {
    // Reference panel is a single flat, solid capsule - no glow halo, no
    // glass sheen. One draw call, full-height corner radius.
    addRoundedRect(cmds, x, y, w, h, h * 0.5f, multiplyAlpha(panelColor, backgroundOpacity));
}

// Bold monogram echoing the reference's angular sticker-style brand mark: a
// dark outline stroke underneath a solid fill. This is a stylised
// approximation, not a vector trace - straight-line/rect primitives can't
// reproduce the source artwork's curves exactly, but the silhouette, weight
// and outline treatment match.
static void drawBrandLogo(std::vector<PLModMenu_DrawCommand>& cmds,
                          float cx, float cy, float size,
                          unsigned int fillColor, unsigned int outlineColor) {
    const float outline = std::max(2.2f, size * 0.13f);
    const float stroke  = std::max(1.6f, size * 0.085f);

    const float stemX = cx - size * 0.34f;
    const float top    = cy - size * 0.30f;
    const float bottom = cy + size * 0.30f;
    const float mid     = cy - size * 0.02f;
    const float bowlR   = cx - size * 0.02f;

    const float rSegs[5][4] = {
        { stemX, top,    stemX, bottom },              // stem
        { stemX, top,    bowlR, top },                 // bowl top
        { bowlR, top,    bowlR, mid },                 // bowl right
        { bowlR, mid,    stemX, mid },                 // bowl bottom
        { stemX, mid,    bowlR, bottom },               // kicking leg
    };

    const float vx0 = cx + size * 0.04f, vy0 = cy + size * 0.08f;
    const float vx1 = cx + size * 0.22f, vy1 = cy + size * 0.34f;
    const float vx2 = cx + size * 0.48f, vy2 = cy - size * 0.32f;

    // Dark outline pass first (wider stroke), then the fill on top - the
    // same order that produces the sticker look in the reference.
    for (auto& s : rSegs) addLine(cmds, s[0], s[1], s[2], s[3], outline, outlineColor);
    addLine(cmds, vx0, vy0, vx1, vy1, outline, outlineColor);
    addLine(cmds, vx1, vy1, vx2, vy2, outline, outlineColor);

    for (auto& s : rSegs) addLine(cmds, s[0], s[1], s[2], s[3], stroke, fillColor);
    addLine(cmds, vx0, vy0, vx1, vy1, stroke, fillColor);
    addLine(cmds, vx1, vy1, vx2, vy2, stroke, fillColor);
}

static void drawDivider(std::vector<PLModMenu_DrawCommand>& cmds,
                        float x, float y, unsigned int color) {
    addCircle(cmds, x, y, 4.0f, color);
}

static float measureSegment(const Segment& seg, float icon, float iconGap, float textSize) {
    const float iconW = (seg.icon != SegmentIcon::None) ? (icon + iconGap) : 0.0f;
    return iconW + calcTextWidth(seg.text, textSize);
}

// Draws one or more segments merged into a single flat pill, separated by a
// small "•" divider - this is what produces the "brand • 2251 FPS" look from
// the reference screenshot. Only segments that carry an icon (the brand)
// reserve space/draw a glyph; plain-text segments sit flush after the dot,
// matching the reference exactly.
static float drawSegmentPill(std::vector<PLModMenu_DrawCommand>& cmds,
                             float x, float y, float h,
                             const std::vector<Segment>& segments,
                             float padding, float icon, float iconGap, float segmentGap,
                             float textSize, unsigned int panelColor,
                             unsigned int logoColor, unsigned int logoOutlineColor,
                             unsigned int textColor, unsigned int dotColor,
                             float backgroundOpacity, bool background) {
    if (segments.empty()) return 0.0f;

    float innerW = 0.0f;
    for (size_t i = 0; i < segments.size(); ++i) {
        innerW += measureSegment(segments[i], icon, iconGap, textSize);
        if (i + 1 < segments.size()) innerW += segmentGap;
    }
    const float totalW = padding * 2.0f + innerW;

    if (background) {
        drawFlatPill(cmds, x, y, totalW, h, panelColor, backgroundOpacity);
    }

    const float cy = y + h * 0.5f;
    float cursor = x + padding;

    for (size_t i = 0; i < segments.size(); ++i) {
        float textX = cursor;
        if (segments[i].icon != SegmentIcon::None) {
            const float iconCX = cursor + icon * 0.5f;
            drawBrandLogo(cmds, iconCX, cy, icon, logoColor, logoOutlineColor);
            textX = cursor + icon + iconGap;
        }

        const float textW = calcTextWidth(segments[i].text, textSize);
        addText(cmds, textX, y + (h - textSize) * 0.5f, textW + 4.0f, h, textSize, textColor, segments[i].text);

        cursor = textX + textW;
        if (i + 1 < segments.size()) {
            drawDivider(cmds, cursor + segmentGap * 0.5f, cy, dotColor);
            cursor += segmentGap;
        }
    }

    return totalW;
}

} // namespace

InfoOverlayModule::InfoOverlayModule()
    : Module("Info Overlay", "Clean purple glass FPS, ping, coordinates and bps HUD.") {
    g_infoOverlayMod = this;
}

InfoOverlayModule::~InfoOverlayModule() {
    if (g_infoOverlayMod == this) g_infoOverlayMod = nullptr;
}

void InfoOverlayModule::updatePing(int ping) {
    m_ping = ping;
}

void InfoOverlayModule::trackBlockProgress(void* block, float increment) {
    auto now = std::chrono::steady_clock::now();

    if (m_breakBlock == nullptr) m_breakLastUpdate = now;

    float elapsedMs = std::chrono::duration_cast<std::chrono::duration<float, std::milli>>(now - m_breakLastUpdate).count();

    if (elapsedMs > 150.0f || m_breakBlock != block || m_breakProgress >= 1.0f) {
        m_breakProgress = 0.0f;
        elapsedMs = 0.0f;
    }

    if (increment > 0.0f) {
        m_breakProgress += elapsedMs * (increment / 50.0f);
    }

    if (m_breakProgress >= 1.0f) {
        m_breakProgress = 1.0f;
        m_breakTimestamps.push_back(now);
    }

    m_breakBlock = block;
    m_breakLastUpdate = now;
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

    if (!m_breakHooked) {
        uintptr_t addr = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::GetDestroyProgress);
        if (addr != 0) {
            bedrocktools::hooks::install((void*)addr,
                                         (void*)_infoOverlayDestroyProgress_hook,
                                         (void**)&_infoOverlayDestroyProgress_orig);
            m_breakHooked = true;
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
    m_breakTimestamps.clear();
    m_bps = 0.0f;
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

    while (!m_breakTimestamps.empty() &&
           std::chrono::duration<float>(now - m_breakTimestamps.front()).count() > kBpsWindowSeconds) {
        m_breakTimestamps.pop_front();
    }
    m_bps = static_cast<float>(m_breakTimestamps.size()) / kBpsWindowSeconds;

    const int x = static_cast<int>(std::round(m_currentPos.x));
    const int y = static_cast<int>(std::round(m_currentPos.y));
    const int z = static_cast<int>(std::round(m_currentPos.z));

    std::ostringstream fpsText;
    fpsText << m_fps << " FPS";

    std::ostringstream pingText;
    pingText << m_ping << " MS";

    std::ostringstream coordsText;
    coordsText << "x-" << x << " y-" << y << " z-" << z;

    std::ostringstream bpsText;
    bpsText << std::fixed << std::setprecision(1) << m_bps << " BPS";

    const std::string fpsString = fpsText.str();
    const std::string pingString = pingText.str();
    const std::string coordsString = coordsText.str();
    const std::string bpsString = bpsText.str();

    // Reference-matched hierarchy: a single flat, solid capsule per row.
    // Only the brand carries a mark - every other segment is plain text
    // after a small dot divider, exactly as in the reference.
    std::vector<PLModMenu_DrawCommand> cmds;

    const float topH = m_panelHeight;
    const float rowH = m_panelHeight;
    const float icon = m_iconSize;
    const float textSize = m_size;

    const float topY = hudPosY;
    const unsigned int outlineColor = multiplyAlpha(m_panelColor, 1.0f);

    // Row 1: brand / fps / ping merged into a single pill -------------------
    std::vector<Segment> topSegments;
    if (m_showBrand) topSegments.push_back({SegmentIcon::Brand, m_brand});
    if (m_showFps)   topSegments.push_back({SegmentIcon::None, fpsString});
    if (m_showPing)  topSegments.push_back({SegmentIcon::None, pingString});

    if (!topSegments.empty()) {
        drawSegmentPill(cmds, hudPosX, topY, topH, topSegments,
                        m_panelPadding, icon, m_iconGap, m_segmentGap,
                        textSize, m_panelColor, m_accentColor, outlineColor,
                        m_textColor, m_dotColor, m_backgroundOpacity, m_background);
    }

    // Row 2: coordinates and bps as two independent pills --------------------
    const float rowY = topY + topH + m_rowGap;
    float rowCursor = hudPosX;

    if (m_showCoords) {
        std::vector<Segment> coordsSegments{{SegmentIcon::None, coordsString}};
        const float w = drawSegmentPill(cmds, rowCursor, rowY, rowH, coordsSegments,
                                        m_panelPadding, icon, m_iconGap, m_segmentGap,
                                        textSize, m_panelColor, m_accentColor, outlineColor,
                                        m_textColor, m_dotColor, m_backgroundOpacity, m_background);
        rowCursor += w + m_panelSpacing;
    }

    if (m_showBps) {
        std::vector<Segment> bpsSegments{{SegmentIcon::None, bpsString}};
        drawSegmentPill(cmds, rowCursor, rowY, rowH, bpsSegments,
                        m_panelPadding, icon, m_iconGap, m_segmentGap,
                        textSize, m_panelColor, m_accentColor, outlineColor,
                        m_textColor, m_dotColor, m_backgroundOpacity, m_background);
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
    if (j.contains("m_segmentGap")) m_segmentGap = j["m_segmentGap"].get<float>();

    if (j.contains("m_background")) m_background = j["m_background"].get<bool>();
    if (j.contains("m_blur")) m_blur = j["m_blur"].get<bool>();
    if (j.contains("m_backgroundOpacity")) m_backgroundOpacity = j["m_backgroundOpacity"].get<float>();
    if (j.contains("m_glowOpacity")) m_glowOpacity = j["m_glowOpacity"].get<float>();
    if (j.contains("m_iconOpacity")) m_iconOpacity = j["m_iconOpacity"].get<float>();

    // Hex-string color settings (the format the mod menu renders as an
    // actual color picker). Old numeric fields are still accepted as a
    // fallback so configs saved by the previous version of this file still
    // load correctly.
    if (!loadHexColor(j, "capsuleColorHex", m_panelColor) && j.contains("m_panelColor"))
        m_panelColor = j["m_panelColor"].get<unsigned int>();
    if (!loadHexColor(j, "logoColorHex", m_accentColor) && j.contains("m_accentColor"))
        m_accentColor = j["m_accentColor"].get<unsigned int>();
    if (!loadHexColor(j, "textColorHex", m_textColor) && j.contains("m_textColor"))
        m_textColor = j["m_textColor"].get<unsigned int>();
    if (!loadHexColor(j, "dotColorHex", m_dotColor) && j.contains("m_dotColor"))
        m_dotColor = j["m_dotColor"].get<unsigned int>();
    if (j.contains("m_mutedColor")) m_mutedColor = j["m_mutedColor"].get<unsigned int>();

    if (j.contains("m_brand")) m_brand = j["m_brand"].get<std::string>();
    if (j.contains("m_showBrand")) m_showBrand = j["m_showBrand"].get<bool>();
    if (j.contains("m_showFps")) m_showFps = j["m_showFps"].get<bool>();
    if (j.contains("m_showPing")) m_showPing = j["m_showPing"].get<bool>();
    if (j.contains("m_showCoords")) m_showCoords = j["m_showCoords"].get<bool>();
    if (j.contains("m_showBps")) m_showBps = j["m_showBps"].get<bool>();
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
    j["m_segmentGap"] = m_segmentGap;

    j["m_background"] = m_background;
    j["m_blur"] = m_blur;
    j["m_backgroundOpacity"] = m_backgroundOpacity;
    j["m_glowOpacity"] = m_glowOpacity;
    j["m_iconOpacity"] = m_iconOpacity;

    j["capsuleColorHex"] = saveHexColor(m_panelColor);
    j["logoColorHex"] = saveHexColor(m_accentColor);
    j["textColorHex"] = saveHexColor(m_textColor);
    j["dotColorHex"] = saveHexColor(m_dotColor);
    j["m_mutedColor"] = m_mutedColor;

    j["m_brand"] = m_brand;
    j["m_showBrand"] = m_showBrand;
    j["m_showFps"] = m_showFps;
    j["m_showPing"] = m_showPing;
    j["m_showCoords"] = m_showCoords;
    j["m_showBps"] = m_showBps;
}
