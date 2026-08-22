#include "activeeffects.hpp"
#include "modules/ModuleRegistry.hpp"
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/sdk/world/Actor.hpp>
#include <algorithm>
#include <iomanip>
#include <sstream>
#include <unordered_map>

static ActiveEffectsModule* g_effectsMod = nullptr;

// Bedrock effect-id -> display name. This table is just the public, documented list of
// Minecraft Bedrock status effect IDs (same ordering used in the protocol / wiki) - it
// is not derived from any memory offset, so it's safe to hardcode.
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

static std::string formatDuration(int ticks) {
    if (ticks < 0) ticks = 0;
    int totalSeconds = ticks / 20;
    int minutes = totalSeconds / 60;
    int seconds = totalSeconds % 60;
    std::ostringstream oss;
    oss << minutes << ":" << std::setfill('0') << std::setw(2) << seconds;
    return oss.str();
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

ActiveEffectsModule::ActiveEffectsModule()
    : Module("Active Effects", "Lists your active status effects with their level and remaining duration.") {
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
    // --- NOT WIRED UP YET ---
    // BedrockTools' current SDK (include/bedrocktools/sdk/offsets/) has no offset for the
    // Actor's active-effects container (the MobEffectInstance list), unlike position/
    // rotation/hurt-time which already have entries in offsets::Actor. So there's nothing
    // real to read yet: m_effects is only ever populated by whatever you assign to it here.
    //
    // What's needed once you (or someone maintaining offsets for your game build) locates
    // it: an offset to the effect-data component on Actor - conceptually a small container
    // of entries shaped like { int effectId; int amplifier; int durationTicks; }, similar
    // in spirit to how mStateVectorComponent/mActorRotationComponent already expose other
    // per-actor components. That has to come from static analysis (Ghidra/IDA) of the
    // specific libminecraftpe.so this build targets - I don't have that binary or its
    // layout memorized, so I'm not going to guess a plausible-looking offset here; a wrong
    // one would silently read garbage memory instead of failing loudly.
    //
    // Once you have it, replace this function's body with something like:
    //
    //   m_effects.clear();
    //   void* effectData = /* field<void*>(localPlayer, offsets::Actor::mEffectData) */;
    //   for (each active MobEffectInstance in effectData) {
    //       auto it = effectNames().find(entry.effectId);
    //       if (it == effectNames().end()) continue;
    //       m_effects.push_back({ it->second, entry.amplifier, entry.durationTicks });
    //   }
}

void ActiveEffectsModule::onFrame() {
    if (!enabled) return;
    if (m_effects.empty()) return;

    // Build every string up front (into a reserve()'d, stable container) before taking any
    // c_str() pointers - std::string uses small-string-optimization, so a vector growth
    // mid-loop could relocate a short string's backing buffer and dangle an earlier c_str().
    struct Row { std::string label; std::string time; };
    std::vector<Row> rows;
    rows.reserve(m_effects.size());
    for (auto& e : m_effects) {
        std::string label = e.name;
        if (m_showLevel) label += " " + toRoman(e.amplifier);
        rows.push_back({std::move(label), formatDuration(e.durationTicks)});
    }

    const std::string title = "Active Effects";
    float rowH = m_size + 10.0f;
    float titleH = m_size + 8.0f;
    float padX = 10.0f;

    float boxW = calcTextWidth(title, m_size) + padX * 2.0f;
    for (auto& row : rows) {
        float w = calcTextWidth(row.label, m_size) + calcTextWidth(row.time, m_size) + padX * 2.0f + 24.0f;
        boxW = std::max(boxW, w);
    }
    float boxH = titleH + rowH * rows.size() + 6.0f;

    std::vector<PLModMenu_DrawCommand> cmds;

    if (m_background) {
        PLModMenu_DrawCommand bg = {};
        bg.type = PL_DRAW_RECT_FILLED;
        bg.x = hudPosX;
        bg.y = hudPosY;
        bg.w = boxW;
        bg.h = boxH;
        int alpha = (int)(m_backgroundOpacity * 255.0f);
        bg.color = (alpha << 24) | 0x000000;
        cmds.push_back(bg);
    }

    PLModMenu_DrawCommand titleCmd = {};
    titleCmd.type = PL_DRAW_TEXT;
    titleCmd.x = hudPosX + padX;
    titleCmd.y = hudPosY + 4.0f;
    titleCmd.w = boxW;
    titleCmd.h = titleH;
    titleCmd.color = 0xFFA0A6FF;
    titleCmd.size = m_size;
    titleCmd.text = title.c_str();
    cmds.push_back(titleCmd);

    float y = hudPosY + titleH;
    for (auto& row : rows) {
        PLModMenu_DrawCommand nameCmd = {};
        nameCmd.type = PL_DRAW_TEXT;
        nameCmd.x = hudPosX + padX;
        nameCmd.y = y + 4.0f;
        nameCmd.w = boxW;
        nameCmd.h = rowH;
        nameCmd.color = 0xFFFFFFFF;
        nameCmd.size = m_size;
        nameCmd.text = row.label.c_str();
        cmds.push_back(nameCmd);

        PLModMenu_DrawCommand timeCmd = {};
        timeCmd.type = PL_DRAW_TEXT;
        timeCmd.x = hudPosX + boxW - calcTextWidth(row.time, m_size) - padX;
        timeCmd.y = y + 4.0f;
        timeCmd.w = boxW;
        timeCmd.h = rowH;
        timeCmd.color = 0xFFCFCFCF;
        timeCmd.size = m_size;
        timeCmd.text = row.time.c_str();
        cmds.push_back(timeCmd);

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
}
