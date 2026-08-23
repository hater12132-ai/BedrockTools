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

enum class SegmentIcon { Brand, Fps, Signal, Coords, Bps };

struct Segment {
    SegmentIcon icon;
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

// Line endpoint is (x + w, y + h); size is stroke thickness. Mirrors the
// semantics CompassModule already relies on for its tick marks.
static void addLine(std::vector<PLModMenu_DrawCommand>& cmds,
                    float x0, float y0, float x1, float y1,
                    float thickness, unsigned int color) {
    PLModMenu_DrawCommand c = {};
    c.type = PL_DRAW_LINE;
    c.x = x0;
    c.y = y0;
    c.w = x1 - x0;
    c.h = y1 - y0;
    c.size = thickness;
    c.color = color;
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

// Stylized "W" brand mark, drawn as a single zigzag stroke so it reads
// cleanly at small HUD sizes.
static void drawBrandIcon(std::vector<PLModMenu_DrawCommand>& cmds,
                          float cx, float cy, float size, unsigned int color) {
    const float thick = std::max(1.6f, size * 0.075f);
    const float halfW = size * 0.20f;
    const float top = cy - size * 0.14f;
    const float bottom = cy + size * 0.14f;
    const float midTop = cy - size * 0.02f;

    addLine(cmds, cx - halfW, top,    cx - halfW * 0.5f, bottom, thick, color);
    addLine(cmds, cx - halfW * 0.5f, bottom, cx, midTop,          thick, color);
    addLine(cmds, cx, midTop,        cx + halfW * 0.5f, bottom,   thick, color);
    addLine(cmds, cx + halfW * 0.5f, bottom, cx + halfW, top,     thick, color);
}

// Small trending-up line chart, matching the reference design's FPS glyph
// more closely than plain bars do.
static void drawFpsIcon(std::vector<PLModMenu_DrawCommand>& cmds,
                        float cx, float cy, float size, unsigned int color) {
    const float thick = std::max(1.6f, size * 0.075f);
    const float x0 = cx - size * 0.30f, y0 = cy + size * 0.10f;
    const float x1 = cx - size * 0.08f, y1 = cy - size * 0.06f;
    const float x2 = cx + size * 0.10f, y2 = cy + size * 0.18f;
    const float x3 = cx + size * 0.32f, y3 = cy - size * 0.26f;

    addLine(cmds, x0, y0, x1, y1, thick, color);
    addLine(cmds, x1, y1, x2, y2, thick, color);
    addLine(cmds, x2, y2, x3, y3, thick, color);

    // Little arrowhead at the trend's tip.
    addLine(cmds, x3, y3, x3 - size * 0.11f, y3, thick, color);
    addLine(cmds, x3, y3, x3, y3 + size * 0.11f, thick, color);
}

// Ascending network/signal bars.
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

// Diagonal double-headed arrow, used for the coordinates readout instead of a
// crosshair so it reads as "position/movement" the way the reference does.
static void drawCoordsIcon(std::vector<PLModMenu_DrawCommand>& cmds,
                           float cx, float cy, float size, unsigned int color) {
    const float thick = std::max(1.6f, size * 0.08f);
    const float reach = size * 0.28f;
    const float headLen = size * 0.13f;

    const float x0 = cx - reach, y0 = cy + reach;
    const float x1 = cx + reach, y1 = cy - reach;

    addLine(cmds, x0, y0, x1, y1, thick, color);

    // Arrowhead at the top-right end.
    addLine(cmds, x1, y1, x1 - headLen, y1, thick, color);
    addLine(cmds, x1, y1, x1, y1 + headLen, thick, color);

    // Arrowhead at the bottom-left end.
    addLine(cmds, x0, y0, x0 + headLen, y0, thick, color);
    addLine(cmds, x0, y0, x0, y0 - headLen, thick, color);
}

// Three stacked bars of increasing length, read as a compact data/throughput
// glyph for the blocks-per-second readout.
static void drawBpsIcon(std::vector<PLModMenu_DrawCommand>& cmds,
                        float cx, float cy, float size, unsigned int color) {
    const float barH = std::max(1.6f, size * 0.09f);
    const float gap = size * 0.13f;
    const float widths[] = { 0.32f, 0.50f, 0.68f };
    const float left = cx - size * 0.34f;
    const float top = cy - (gap * 2.0f + barH * 1.5f) * 0.5f;

    for (int i = 0; i < 3; ++i) {
        addRect(cmds, left, top + i * gap, size * widths[i], barH, color);
    }
}

static void drawIcon(std::vector<PLModMenu_DrawCommand>& cmds, SegmentIcon icon,
                     float cx, float cy, float size, unsigned int color) {
    switch (icon) {
        case SegmentIcon::Brand:  drawBrandIcon(cmds, cx, cy, size, color); break;
        case SegmentIcon::Fps:    drawFpsIcon(cmds, cx, cy, size, color); break;
        case SegmentIcon::Signal: drawSignalIcon(cmds, cx, cy, size, color); break;
        case SegmentIcon::Coords: drawCoordsIcon(cmds, cx, cy, size, color); break;
        case SegmentIcon::Bps:    drawBpsIcon(cmds, cx, cy, size, color); break;
    }
}

static void drawDivider(std::vector<PLModMenu_DrawCommand>& cmds,
                        float x, float y, unsigned int color) {
    addCircle(cmds, x, y, 4.0f, color);
}

static float measureSegment(const Segment& seg, float icon, float iconGap, float textSize) {
    return icon + iconGap + calcTextWidth(seg.text, textSize);
}

// Draws one or more segments merged into a single glass pill, separated by a
// small "•" divider - this is what produces the "brand • fps • ping" look
// from the reference screenshot instead of three separate pills.
static float drawSegmentPill(std::vector<PLModMenu_DrawCommand>& cmds,
                             float x, float y, float h,
                             const std::vector<Segment>& segments,
                             float padding, float icon, float iconGap, float segmentGap,
                             float textSize, unsigned int panelColor, unsigned int accentColor,
                             unsigned int textColor, float iconOpacity,
                             float backgroundOpacity, float glowOpacity, bool blur, bool background) {
    if (segments.empty()) return 0.0f;

    float innerW = 0.0f;
    for (size_t i = 0; i < segments.size(); ++i) {
        innerW += measureSegment(segments[i], icon, iconGap, textSize);
        if (i + 1 < segments.size()) innerW += segmentGap;
    }
    const float totalW = padding * 2.0f + innerW;

    if (background) {
        drawGlassPill(cmds, x, y, totalW, h, panelColor, accentColor, backgroundOpacity, glowOpacity, blur);
    }

    const float cy = y + h * 0.5f;
    float cursor = x + padding;
    const unsigned int glyphColor = multiplyAlpha(textColor, 0.95f);

    for (size_t i = 0; i < segments.size(); ++i) {
        const float iconCX = cursor + icon * 0.5f;
        drawIconBubble(cmds, iconCX, cy, icon, accentColor, iconOpacity);
        drawIcon(cmds, segments[i].icon, iconCX, cy, icon, glyphColor);

        const float textX = cursor + icon + iconGap;
        const float textW = calcTextWidth(segments[i].text, textSize);
        addText(cmds, textX, y + (h - textSize) * 0.5f, textW + 4.0f, h, textSize, textColor, segments[i].text);

        cursor = textX + textW;
        if (i + 1 < segments.size()) {
            drawDivider(cmds, cursor + segmentGap * 0.5f, cy, multiplyAlpha(accentColor, 0.55f));
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
    fpsText << m_fps << " fps";

    std::ostringstream pingText;
    pingText << m_ping << " ms";

    std::ostringstream coordsText;
    coordsText << "x-" << x << " y-" << y << " z-" << z;

    std::ostringstream bpsText;
    bpsText << std::fixed << std::setprecision(1) << m_bps << " bps";

    const std::string fpsString = fpsText.str();
    const std::string pingString = pingText.str();
    const std::string coordsString = coordsText.str();
    const std::string bpsString = bpsText.str();

    // Screenshot-matched hierarchy:
    // [brand • fps • ping]                       <- one merged pill
    // [coords]        [bps]                       <- two independent pills
    // Independent panels below still hide cleanly if a metric is turned off.
    std::vector<PLModMenu_DrawCommand> cmds;

    const float topH = m_panelHeight;
    const float rowH = m_panelHeight;
    const float icon = m_iconSize;
    const float textSize = m_size;

    const float topY = hudPosY;

    // Row 1: brand / fps / ping merged into a single pill -------------------
    std::vector<Segment> topSegments;
    if (m_showBrand) topSegments.push_back({SegmentIcon::Brand, m_brand});
    if (m_showFps)   topSegments.push_back({SegmentIcon::Fps, fpsString});
    if (m_showPing)  topSegments.push_back({SegmentIcon::Signal, pingString});

    if (!topSegments.empty()) {
        drawSegmentPill(cmds, hudPosX, topY, topH, topSegments,
                        m_panelPadding, icon, m_iconGap, m_segmentGap,
                        textSize, m_panelColor, m_accentColor, m_textColor,
                        m_iconOpacity, m_backgroundOpacity, m_glowOpacity, m_blur, m_background);
    }

    // Row 2: coordinates and bps as two independent pills --------------------
    const float rowY = topY + topH + m_rowGap;
    float rowCursor = hudPosX;

    if (m_showCoords) {
        std::vector<Segment> coordsSegments{{SegmentIcon::Coords, coordsString}};
        const float w = drawSegmentPill(cmds, rowCursor, rowY, rowH, coordsSegments,
                                        m_panelPadding, icon, m_iconGap, m_segmentGap,
                                        textSize, m_panelColor, m_accentColor, m_textColor,
                                        m_iconOpacity, m_backgroundOpacity, m_glowOpacity, m_blur, m_background);
        rowCursor += w + m_panelSpacing;
    }

    if (m_showBps) {
        std::vector<Segment> bpsSegments{{SegmentIcon::Bps, bpsString}};
        drawSegmentPill(cmds, rowCursor, rowY, rowH, bpsSegments,
                        m_panelPadding, icon, m_iconGap, m_segmentGap,
                        textSize, m_panelColor, m_accentColor, m_textColor,
                        m_iconOpacity, m_backgroundOpacity, m_glowOpacity, m_blur, m_background);
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
    if (j.contains("m_panelColor")) m_panelColor = j["m_panelColor"].get<unsigned int>();
    if (j.contains("m_accentColor")) m_accentColor = j["m_accentColor"].get<unsigned int>();
    if (j.contains("m_textColor")) m_textColor = j["m_textColor"].get<unsigned int>();
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
    j["m_panelColor"] = m_panelColor;
    j["m_accentColor"] = m_accentColor;
    j["m_textColor"] = m_textColor;
    j["m_mutedColor"] = m_mutedColor;

    j["m_brand"] = m_brand;
    j["m_showBrand"] = m_showBrand;
    j["m_showFps"] = m_showFps;
    j["m_showPing"] = m_showPing;
    j["m_showCoords"] = m_showCoords;
    j["m_showBps"] = m_showBps;
}
