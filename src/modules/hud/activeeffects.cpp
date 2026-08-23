#include "activeeffects.hpp"
#include "modules/ModuleRegistry.hpp"
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/sdk/world/Actor.hpp>
#include <algorithm>
#include <iomanip>
#include <sstream>
#include <unordered_map>

static ActiveEffectsModule* g_effectsMod = nullptr;

// Bedrock effect-id -> display name. Just the public, documented list of Minecraft Bedrock
// status effect IDs (same ordering used in the protocol / wiki) - not derived from any
// memory offset, so it's safe to hardcode.
static const std::unordered_map<int, std::string>& effectNames() {
    static const std::unordered_map<int, std::string> table = {
        {1, "Speed"}, {2, "Slowness"}, {3, "Haste"}, {4, "Mining Fatigue"},
        {5, "Strength"}, {6, "Instant Health"}, {7, "Instant Damage"}, {8, "Jump Boost"},
        {9, "Nausea"}, {10, "Regeneration"}, {11, "Resistance"}, {12, "Fire Resistance"},
        {13, "Water Breathing"}, {14, "Invisibility"}, {15, "Blindness"}, {16, "Night Vision"},
        {17, "Hunger"}, {18, "Weakness"}, {19, "Poison"}, {20, "Wither"},
        {21, "Health Boost"}, {22, "Absorption"}, {23, "Saturation"}, {24, "Levitation"},
        {25, "Fatal Poison"}, {26, "Conduit Power"}, {27, "Slow Falling"}, {28, "Bad Omen"},
        {29, "Hero of the Village"}, {30, "Darkness"},
    };
    return table;
}

// Stand-in swatch colors, loosely themed after each effect's usual potion tint. These are
// NOT extracted from the game's real icon art (see the note in the chat reply) - just a
// reasonable, distinct color per row so effects are visually distinguishable at a glance.
static unsigned int effectAccentColor(int id) {
    switch (id) {
        case 1:  return 0xFF7FD8E8; // Speed
        case 2:  return 0xFF5A6B8C; // Slowness
        case 3:  return 0xFFD9A441; // Haste
        case 4:  return 0xFF4A3728; // Mining Fatigue
        case 5:  return 0xFFB0472F; // Strength
        case 6:  return 0xFFE0457B; // Instant Health
        case 7:  return 0xFF8B1A1A; // Instant Damage
        case 8:  return 0xFF4FBF6B; // Jump Boost
        case 9:  return 0xFF551D4A; // Nausea
        case 10: return 0xFFCC5CA6; // Regeneration
        case 11: return 0xFF8C3A3A; // Resistance
        case 12: return 0xFFE08A2E; // Fire Resistance
        case 13: return 0xFF2E5AA8; // Water Breathing
        case 14: return 0xFF9FB8C9; // Invisibility
        case 15: return 0xFF3A3A3A; // Blindness
        case 16: return 0xFF23368C; // Night Vision
        case 17: return 0xFF6B7A3A; // Hunger
        case 18: return 0xFF4A4A3A; // Weakness
        case 19: return 0xFF4E9331; // Poison
        case 20: return 0xFF2B2B2B; // Wither
        case 21: return 0xFFE0602E; // Health Boost
        case 22: return 0xFFE8C13C; // Absorption
        case 23: return 0xFFE0912F; // Saturation
        case 24: return 0xFFC7B8E8; // Levitation
        case 25: return 0xFF2E5A17; // Fatal Poison
        case 26: return 0xFF3CA6A6; // Conduit Power
        case 27: return 0xFFE8D89A; // Slow Falling
        case 28: return 0xFF5A2340; // Bad Omen
        case 29: return 0xFFE8C77A; // Hero of the Village
        case 30: return 0xFF1A1A1A; // Darkness
        default: return 0xFF808080;
    }
}

static std::string formatDuration(int ticks) {
    if (ticks < 0) ticks = 0;
    int totalSeconds = ticks / 20;
    int minutes = totalSeconds / 60;
    int seconds = totalSeconds % 60;
    std::ostringstream oss;
    oss << minutes << ":" << std::setfill('0') << std::setw(2) << seconds;
    return oss.str();
}

// Heuristic only: nothing in this SDK confirms the real "infinite duration" sentinel Bedrock
// uses internally (see refreshEffects() below - there's no live data feeding this yet at
// all). Once real duration ticks are available, treat a negative value or anything past
// about an hour as effectively infinite rather than counting down a wall of digits.
static bool isInfiniteDuration(int ticks) {
    return ticks < 0 || ticks >= 20 * 60 * 60;
}

static std::string toRoman(int amplifier) {
    static const char* numerals[] = {"I", "II", "III", "IV", "V", "VI", "VII", "VIII", "IX", "X"};
    int level = amplifier + 1;
    if (level >= 1 && level <= 10) return numerals[level - 1];
    return std::to_string(level);
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

// Stadium/"pill" shape: filled rect body + a filled circle capping each end. CircleFilled
// takes a center x/y and uses `size` as its diameter (matches the working usage in
// debugmenu.cpp's minimap dots).
static void drawPill(std::vector<PLModMenu_DrawCommand>& cmds, float x, float y, float w, float h, unsigned int argb) {
    float radius = h * 0.5f;
    float bodyW = std::max(0.0f, w - h);

    PLModMenu_DrawCommand body = {};
    body.type = PL_DRAW_RECT_FILLED;
    body.x = x + radius;
    body.y = y;
    body.w = bodyW;
    body.h = h;
    body.color = argb;
    cmds.push_back(body);

    PLModMenu_DrawCommand capLeft = {};
    capLeft.type = PL_DRAW_CIRCLE_FILLED;
    capLeft.x = x + radius;
    capLeft.y = y + radius;
    capLeft.size = h;
    capLeft.color = argb;
    cmds.push_back(capLeft);

    PLModMenu_DrawCommand capRight = capLeft;
    capRight.x = x + w - radius;
    cmds.push_back(capRight);
}

// Small vector potion-bottle glyph for the header (body + neck + a cap dot) - drawn with
// plain rect/circle primitives, not a bitmap, since no real icon art is wired into this
// project (see the chat reply for why).
static void drawPotionGlyph(std::vector<PLModMenu_DrawCommand>& cmds, float x, float y, float size, unsigned int argb) {
    float bodyW = size * 0.62f;
    float bodyH = size * 0.62f;
    float neckW = size * 0.28f;
    float neckH = size * 0.22f;

    PLModMenu_DrawCommand body = {};
    body.type = PL_DRAW_RECT_FILLED;
    body.x = x + (size - bodyW) * 0.5f;
    body.y = y + size - bodyH;
    body.w = bodyW;
    body.h = bodyH;
    body.color = argb;
    cmds.push_back(body);

    PLModMenu_DrawCommand neck = {};
    neck.type = PL_DRAW_RECT_FILLED;
    neck.x = x + (size - neckW) * 0.5f;
    neck.y = y + size - bodyH - neckH;
    neck.w = neckW;
    neck.h = neckH;
    neck.color = argb;
    cmds.push_back(neck);

    PLModMenu_DrawCommand cap = {};
    cap.type = PL_DRAW_CIRCLE_FILLED;
    cap.x = x + size * 0.5f;
    cap.y = y + size - bodyH - neckH;
    cap.size = neckW * 1.4f;
    cap.color = argb;
    cmds.push_back(cap);
}

ActiveEffectsModule::ActiveEffectsModule()
    : Module("Potions", "Lists your active status effects with an icon swatch, level, and remaining duration.") {
    g_effectsMod = this;
}

ActiveEffectsModule::~ActiveEffectsModule() {
    if (g_effectsMod == this) g_effectsMod = nullptr;
}

void ActiveEffectsModule::onInit() {
    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>([](auto& event) {
        if (g_effectsMod && g_effectsMod->enabled && event.player) {
            g_effectsMod->refreshEffects(event.player);
        }
    });
}

void ActiveEffectsModule::onEnable() {}

void ActiveEffectsModule::onDisable() {
    m_effects.clear();
}

void ActiveEffectsModule::refreshEffects(void* localPlayer) {
    (void)localPlayer;
    // --- STILL NOT WIRED UP ---
    // Same gap as before: BedrockTools' SDK has no offset yet for the Actor's active-effects
    // container, so there is no real MobEffectInstance list to walk here. m_effects only ever
    // contains whatever gets assigned to it in this function, and right now nothing does.
    // See the previous reply for what that offset needs to look like once someone locates it
    // via static analysis of the actual game binary - this function is the single place to
    // wire it in; everything downstream (icons, roman numerals, the infinite pill) already
    // works off of m_effects as soon as it's populated.
}

void ActiveEffectsModule::onFrame() {
    if (!enabled) return;
    if (m_effects.empty()) return;

    struct Row {
        std::string name;
        std::string value;   // formatted time, or the infinity glyph
        bool infinite;
        unsigned int color;
    };

    // Build every string up front into a reserve()'d, stable container before taking any
    // c_str() pointers - std::string uses small-string-optimization, so a vector growth
    // mid-loop could relocate a short string's backing buffer and dangle an earlier c_str().
    std::vector<Row> rows;
    rows.reserve(m_effects.size());
    for (auto& e : m_effects) {
        std::string label = e.name;
        if (m_showLevel) label += " " + toRoman(e.amplifier);
        bool inf = isInfiniteDuration(e.durationTicks);
        rows.push_back({std::move(label), inf ? "\xE2\x88\x9E" : formatDuration(e.durationTicks), inf, effectAccentColor(e.effectId)});
    }

    const std::string title = "Potions";
    const unsigned int headerColor = 0xFFCB6A2E;
    const unsigned int infPillColor = 0xFFE0912F;

    float padX = 12.0f;
    float iconSize = m_size * 0.85f;
    float headerIconSize = m_size;
    float headerH = m_size + 16.0f;
    float rowH = std::max(iconSize, m_size) + 14.0f;
    float iconGap = 10.0f;
    float valueGap = 18.0f;

    float boxW = padX * 2.0f + headerIconSize + iconGap + calcTextWidth(title, m_size);
    for (auto& row : rows) {
        float valueW = row.infinite ? (calcTextWidth(row.value, m_size) + m_size * 0.9f) // pill padding
                                     : calcTextWidth(row.value, m_size);
        float w = padX * 2.0f + (m_showIcons ? iconSize + iconGap : 0.0f)
                + calcTextWidth(row.name, m_size) + valueGap + valueW;
        boxW = std::max(boxW, w);
    }
    float boxH = headerH + rowH * rows.size() + 6.0f;

    std::vector<PLModMenu_DrawCommand> cmds;

    if (m_background) {
        PLModMenu_DrawCommand bg = {};
        bg.type = PL_DRAW_RECT_FILLED;
        bg.x = hudPosX;
        bg.y = hudPosY;
        bg.w = boxW;
        bg.h = boxH;
        int alpha = (int)(m_backgroundOpacity * 255.0f);
        bg.color = (static_cast<unsigned int>(alpha) << 24) | 0x000000;
        cmds.push_back(bg);
    }

    drawPotionGlyph(cmds, hudPosX + padX, hudPosY + (headerH - headerIconSize) * 0.5f, headerIconSize, headerColor);

    PLModMenu_DrawCommand titleCmd = {};
    titleCmd.type = PL_DRAW_TEXT;
    titleCmd.x = hudPosX + padX + headerIconSize + iconGap;
    titleCmd.y = hudPosY + (headerH - m_size) * 0.5f;
    titleCmd.w = boxW;
    titleCmd.h = m_size + 4.0f;
    titleCmd.color = 0xFFFFFFFF;
    titleCmd.size = m_size;
    titleCmd.text = title.c_str();
    cmds.push_back(titleCmd);

    float y = hudPosY + headerH;
    for (auto& row : rows) {
        float textX = hudPosX + padX;

        if (m_showIcons) {
            PLModMenu_DrawCommand icon = {};
            icon.type = PL_DRAW_RECT_FILLED;
            icon.x = textX;
            icon.y = y + (rowH - iconSize) * 0.5f;
            icon.w = iconSize;
            icon.h = iconSize;
            icon.color = 0xFF000000 | row.color;
            cmds.push_back(icon);
            textX += iconSize + iconGap;
        }

        PLModMenu_DrawCommand nameCmd = {};
        nameCmd.type = PL_DRAW_TEXT;
        nameCmd.x = textX;
        nameCmd.y = y + (rowH - m_size) * 0.5f;
        nameCmd.w = boxW;
        nameCmd.h = rowH;
        nameCmd.color = 0xFFFFFFFF;
        nameCmd.size = m_size;
        nameCmd.text = row.name.c_str();
        cmds.push_back(nameCmd);

        if (row.infinite) {
            float pillH = m_size + 8.0f;
            float pillW = calcTextWidth(row.value, m_size) + m_size * 0.9f;
            float pillX = hudPosX + boxW - padX - pillW;
            float pillY = y + (rowH - pillH) * 0.5f;
            drawPill(cmds, pillX, pillY, pillW, pillH, infPillColor);

            PLModMenu_DrawCommand valCmd = {};
            valCmd.type = PL_DRAW_TEXT;
            valCmd.x = pillX + (pillW - calcTextWidth(row.value, m_size)) * 0.5f;
            valCmd.y = pillY + (pillH - m_size) * 0.5f;
            valCmd.w = pillW;
            valCmd.h = pillH;
            valCmd.color = 0xFFFFFFFF;
            valCmd.size = m_size;
            valCmd.text = row.value.c_str();
            cmds.push_back(valCmd);
        } else {
            PLModMenu_DrawCommand valCmd = {};
            valCmd.type = PL_DRAW_TEXT;
            valCmd.x = hudPosX + boxW - padX - calcTextWidth(row.value, m_size);
            valCmd.y = y + (rowH - m_size) * 0.5f;
            valCmd.w = boxW;
            valCmd.h = rowH;
            valCmd.color = 0xFFD0D0D0;
            valCmd.size = m_size;
            valCmd.text = row.value.c_str();
            cmds.push_back(valCmd);
        }

        y += rowH;
    }

    submitDrawCommands(moduleId, cmds);
}

void ActiveEffectsModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    if (j.contains("hudPosX")) hudPosX = j["hudPosX"].get<float>();
    if (j.contains("hudPosY")) hudPosY = j["hudPosY"].get<float>();
    if (j.contains("isHudModule")) isHudModule = j["isHudModule"].get<bool>();
    if (j.contains("m_size")) m_size = j["m_size"].get<float>();
    if (j.contains("m_background")) m_background = j["m_background"].get<bool>();
    if (j.contains("m_backgroundOpacity")) m_backgroundOpacity = j["m_backgroundOpacity"].get<float>();
    if (j.contains("m_showLevel")) m_showLevel = j["m_showLevel"].get<bool>();
    if (j.contains("m_showIcons")) m_showIcons = j["m_showIcons"].get<bool>();
}

void ActiveEffectsModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["hudPosX"] = hudPosX;
    j["hudPosY"] = hudPosY;
    j["isHudModule"] = isHudModule;
    j["m_size"] = m_size;
    j["m_background"] = m_background;
    j["m_backgroundOpacity"] = m_backgroundOpacity;
    j["m_showLevel"] = m_showLevel;
    j["m_showIcons"] = m_showIcons;
}
