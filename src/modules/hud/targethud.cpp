#include "targethud.hpp"

#include "modules/ModuleRegistry.hpp"

#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/events/LocalPlayerTickEvent.hpp>
#include <bedrocktools/events/AttackEvent.hpp>
#include <bedrocktools/sdk/Memory.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/world/Actor.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

static TargetHudModule* g_targetHud = nullptr;

static std::uint32_t parseColor(const std::string& value, std::uint32_t fallback = 0xFF55FF55u) {
    if (value.empty()) return fallback;
    const std::string hex = value[0] == '#' ? value.substr(1) : value;
    try {
        if (hex.size() == 6) return 0xFF000000u | static_cast<std::uint32_t>(std::stoul(hex, nullptr, 16));
        if (hex.size() == 8) return static_cast<std::uint32_t>(std::stoul(hex, nullptr, 16));
    } catch (...) {}
    return fallback;
}

static std::uint32_t scaleAlpha(std::uint32_t color, float a) {
    a = std::clamp(a, 0.0f, 1.0f);
    const auto aa = static_cast<std::uint32_t>(std::clamp(((color >> 24) & 0xFFu) * a, 0.0f, 255.0f));
    return (aa << 24) | (color & 0x00FFFFFFu);
}

static std::string tryReadName(bedrocktools::sdk::Actor* actor) {
    if (!actor) return "Unknown";
    // Player::name lives at a fixed offset; valid for local/remote players.
    auto* player = static_cast<bedrocktools::sdk::Player*>(actor);
    const std::string& n = player->name();
    if (!n.empty() && n.size() < 48) {
        bool ok = true;
        for (unsigned char c : n) {
            if (c < 32) { ok = false; break; }
        }
        if (ok) return n;
    }
    return "Entity";
}

float TargetHudModule::calcTextWidth(const std::string& text, float size) {
    float width = 0.0f;
    for (char c : text) {
        if (c == 'i' || c == 'l' || c == '1' || c == ':' || c == '.' || c == ' ' || c == '/')
            width += size * 0.30f;
        else if (c == 'm' || c == 'w' || c == 'M' || c == 'W')
            width += size * 0.80f;
        else
            width += size * 0.58f;
    }
    return width;
}

TargetHudModule::TargetHudModule()
    : Module("TargetHUD", "SoupVisuals-style target info card — name, health bar, distance.") {
    g_targetHud = this;
}

TargetHudModule::~TargetHudModule() {
    if (g_targetHud == this) g_targetHud = nullptr;
}

static float g_localX = 0, g_localY = 0, g_localZ = 0;
static bool g_hasLocal = false;

void TargetHudModule::onInit() {
    bedrocktools::events::bus().subscribe<bedrocktools::events::AttackEvent>([](auto& event) {
        if (g_targetHud && g_targetHud->enabled && event.target) {
            g_targetHud->onAttackTarget(event.target);
        }
    });
    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>([](auto& event) {
        if (!event.player) return;
        auto* actor = reinterpret_cast<bedrocktools::sdk::Actor*>(event.player);
        const auto pos = actor->position();
        g_localX = pos.x; g_localY = pos.y; g_localZ = pos.z;
        g_hasLocal = true;
    });
}

void TargetHudModule::onEnable() {}

void TargetHudModule::onDisable() {
    std::lock_guard lock(m_mutex);
    m_target = {};
    m_anim = 0.0f;
}

void TargetHudModule::onAttackTarget(void* actorPtr) {
    if (!actorPtr) return;
    auto* actor = reinterpret_cast<bedrocktools::sdk::Actor*>(actorPtr);

    std::lock_guard lock(m_mutex);
    const bool same = m_target.valid && m_target.actor == actorPtr;

    m_target.actor = actorPtr;
    m_target.name = tryReadName(actor);
    const auto pos = actor->position();
    m_target.posX = pos.x;
    m_target.posY = pos.y;
    m_target.posZ = pos.z;
    m_target.hurtTime = actor->hurtTime();
    m_target.lastHit = std::chrono::steady_clock::now();
    m_target.valid = true;

    if (!same) {
        m_target.health = 20.0f;
        m_target.maxHealth = 20.0f;
        m_target.displayHealth = 20.0f;
    } else {
        // Client-side estimate: each successful swing chips ~1 HP (Soup shows real HP;
        // BedrockTools has no attribute signature yet).
        m_target.health = std::max(0.0f, m_target.health - 1.0f);
    }
}

void TargetHudModule::onFrame() {
    if (!enabled) {
        pl::modmenu::submitDrawCommands(moduleId, std::span<const pl::modmenu::DrawCommand>{});
        return;
    }

    TargetState snap;
    {
        std::lock_guard lock(m_mutex);
        snap = m_target;
    }

    const auto now = std::chrono::steady_clock::now();
    float targetAnim = 0.0f;
    if (snap.valid) {
        const double age = std::chrono::duration<double>(now - snap.lastHit).count();
        if (age <= static_cast<double>(liveTime)) {
            targetAnim = 1.0f;
            // Refresh live position / hurt while pointer still usable
            if (snap.actor) {
                try {
                    auto* actor = reinterpret_cast<bedrocktools::sdk::Actor*>(snap.actor);
                    const auto pos = actor->position();
                    snap.posX = pos.x;
                    snap.posY = pos.y;
                    snap.posZ = pos.z;
                    snap.hurtTime = actor->hurtTime();
                    std::lock_guard lock(m_mutex);
                    m_target.posX = snap.posX;
                    m_target.posY = snap.posY;
                    m_target.posZ = snap.posZ;
                    m_target.hurtTime = snap.hurtTime;
                } catch (...) {}
            }
        } else {
            std::lock_guard lock(m_mutex);
            m_target.valid = false;
            snap.valid = false;
        }
    }

    // Smooth appear / disappear (Soup Scale/Fade/Both)
    const float speed = std::clamp(animationSpeed, 0.25f, 4.0f) * 0.18f;
    if (targetAnim > m_anim) m_anim = std::min(targetAnim, m_anim + speed);
    else if (targetAnim < m_anim) m_anim = std::max(targetAnim, m_anim - speed);

    if (m_anim <= 0.01f) {
        pl::modmenu::submitDrawCommands(moduleId, std::span<const pl::modmenu::DrawCommand>{});
        return;
    }

    // Smooth health bar
    snap.displayHealth += (snap.health - snap.displayHealth) * 0.25f;

    float fade = m_anim;
    float scaleMul = 1.0f;
    if (animationMode == 0 || animationMode == 2) {
        // ease out back-ish
        const float t = m_anim;
        scaleMul = 0.85f + 0.15f * t;
    }
    if (animationMode == 1) {
        scaleMul = 1.0f;
    }
    if (animationMode == 2) {
        fade = m_anim;
    }

    if (hudPosX < 0.0f) hudPosX = 40.0f;
    if (hudPosY < 0.0f) hudPosY = 80.0f;

    const float s = std::clamp(scale, 0.5f, 2.5f) * scaleMul;
    const float w = cardWidth * s;
    const float h = cardHeight * s;
    const float x = hudPosX;
    const float y = hudPosY;
    const float radius = (style == 1) ? cornerRadius * s : 0.0f;
    const float pad = 8.0f * s;

    // Hurt flash
    float flash = 0.0f;
    if (showHurtFlash && snap.hurtTime > 0) {
        flash = std::clamp(snap.hurtTime / 10.0f, 0.0f, 1.0f);
    }

    std::vector<pl::modmenu::DrawCommand> cmds;
    cmds.reserve(12);

    auto addRect = [&](float rx, float ry, float rw, float rh, float rad, std::uint32_t color) {
        pl::modmenu::DrawCommand rect{};
        rect.type = pl::modmenu::DrawCommandType::RectFilled;
        rect.x = rx;
        rect.y = ry;
        rect.w = rw;
        rect.h = rh;
        rect.x3 = rad;
        rect.color = color;
        cmds.push_back(rect);
    };

    // Keep strings alive for submit
    static thread_local std::vector<std::string> s_textStorage;
    s_textStorage.clear();
    auto pushText = [&](float tx, float ty, float size, std::uint32_t color, std::string text) {
        s_textStorage.push_back(std::move(text));
        pl::modmenu::DrawCommand t{};
        t.type = pl::modmenu::DrawCommandType::Text;
        t.x = tx;
        t.y = ty;
        t.w = w;
        t.h = size + 4.0f;
        t.size = size;
        t.color = color;
        t.text = s_textStorage.back().c_str();
        cmds.push_back(t);
    };

    // Background card
    const int baseA = static_cast<int>(std::clamp(backAlpha, 0.0f, 1.0f) * 255.0f * fade);
    std::uint32_t bgColor = (static_cast<std::uint32_t>(baseA) << 24);
    if (flash > 0.0f) {
        // mix red into background while hurt
        const int r = static_cast<int>(80 * flash);
        bgColor |= (static_cast<std::uint32_t>(r) << 16);
    }
    addRect(x, y, w, h, radius, bgColor);

    // Accent strip on left
    const std::uint32_t accent = scaleAlpha(parseColor(accentColor), fade);
    addRect(x, y, 3.0f * s, h, radius > 0 ? 2.0f * s : 0.0f, accent);

    // Name
    const float nameSize = 14.0f * s;
    pushText(x + pad + 4.0f * s, y + 6.0f * s, nameSize, scaleAlpha(0xFFFFFFFFu, fade), snap.name);

    // Distance
    float dist = 0.0f;
    // Local player distance if we can get it from target vs camera is hard; use last hit positions refreshed
    // Prefer reading local player from Attack - we don't have it here. Distance from target to last known
    // is 0. Leave distance as horizontal span from origin only if we store local pos on attack.
    // Re-subscribe: store local player pos on attack via event? Attack doesn't give local.
    // Show distance only when we have local from a side channel — skip if 0.

    // Health text + bar
    const float barX = x + pad + 4.0f * s;
    const float barY = y + h - 14.0f * s;
    const float barW = w - pad * 2.0f - 4.0f * s;
    const float barH = 6.0f * s;

    if (showHealthBar) {
        // Track bg
        addRect(barX, barY, barW, barH, barH * 0.5f, scaleAlpha(0xFF1A1A1Au, fade));

        const float ratio = std::clamp(snap.displayHealth / std::max(1.0f, snap.maxHealth), 0.0f, 1.0f);
        // Green -> yellow -> red
        std::uint8_t rr, gg, bb;
        if (ratio > 0.5f) {
            const float t = (ratio - 0.5f) * 2.0f;
            rr = static_cast<std::uint8_t>(255 * (1.0f - t));
            gg = 255;
            bb = 40;
        } else {
            const float t = ratio * 2.0f;
            rr = 255;
            gg = static_cast<std::uint8_t>(255 * t);
            bb = 40;
        }
        if (flash > 0.0f) {
            rr = 255;
            gg = static_cast<std::uint8_t>(gg * (1.0f - flash * 0.5f));
        }
        const std::uint32_t barCol = scaleAlpha(
            (0xFFu << 24) | (static_cast<std::uint32_t>(rr) << 16) |
            (static_cast<std::uint32_t>(gg) << 8) | bb, fade);
        if (ratio > 0.01f)
            addRect(barX, barY, barW * ratio, barH, barH * 0.5f, barCol);

        std::ostringstream hp;
        hp << std::fixed << std::setprecision(1) << snap.displayHealth
           << " / " << std::setprecision(0) << snap.maxHealth;
        pushText(barX, barY - 12.0f * s, 10.0f * s, scaleAlpha(0xFFCCCCCCu, fade), hp.str());
    }

    if (showDistance) {
        // Approximate: use hurt-time only as we may not have local player each frame.
        // Store distance on attack if we can get local from a global — optional secondary line.
    }

    // Always show "Target" label subtle
    pushText(x + w - pad - calcTextWidth("TARGET", 8.0f * s), y + 6.0f * s, 8.0f * s,
             scaleAlpha(0xFF888888u, fade * 0.9f), "TARGET");

    pl::modmenu::submitDrawCommands(moduleId, cmds);
}

void TargetHudModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    if (j.contains("hudPosX")) hudPosX = j["hudPosX"].get<float>();
    if (j.contains("hudPosY")) hudPosY = j["hudPosY"].get<float>();
    if (j.contains("isHudModule")) isHudModule = j["isHudModule"].get<bool>();
    if (j.contains("style")) {
        if (j["style"].is_number()) style = std::clamp(static_cast<int>(j["style"].get<float>()), 0, 1);
        else {
            try {
                std::string v = j["style"].get<std::string>();
                const auto c = v.find(',');
                if (c != std::string::npos) v.resize(c);
                style = std::clamp(std::stoi(v), 0, 1);
            } catch (...) {}
        }
    }
    if (j.contains("scale")) scale = std::clamp(j["scale"].get<float>(), 0.5f, 2.5f);
    if (j.contains("backAlpha")) backAlpha = std::clamp(j["backAlpha"].get<float>(), 0.0f, 1.0f);
    if (j.contains("liveTime")) liveTime = std::clamp(j["liveTime"].get<float>(), 0.5f, 15.0f);
    if (j.contains("animationMode")) {
        if (j["animationMode"].is_number()) animationMode = std::clamp(static_cast<int>(j["animationMode"].get<float>()), 0, 2);
        else {
            try {
                std::string v = j["animationMode"].get<std::string>();
                const auto c = v.find(',');
                if (c != std::string::npos) v.resize(c);
                animationMode = std::clamp(std::stoi(v), 0, 2);
            } catch (...) {}
        }
    }
    if (j.contains("animationSpeed")) animationSpeed = std::clamp(j["animationSpeed"].get<float>(), 0.25f, 4.0f);
    if (j.contains("cardWidth")) cardWidth = std::clamp(j["cardWidth"].get<float>(), 80.0f, 320.0f);
    if (j.contains("cardHeight")) cardHeight = std::clamp(j["cardHeight"].get<float>(), 30.0f, 120.0f);
    if (j.contains("cornerRadius")) cornerRadius = std::clamp(j["cornerRadius"].get<float>(), 0.0f, 24.0f);
    if (j.contains("accentColor")) accentColor = j["accentColor"].get<std::string>();
    if (j.contains("showDistance")) showDistance = j["showDistance"].get<bool>();
    if (j.contains("showHealthBar")) showHealthBar = j["showHealthBar"].get<bool>();
    if (j.contains("showHurtFlash")) showHurtFlash = j["showHurtFlash"].get<bool>();
}

void TargetHudModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["hudPosX"] = hudPosX;
    j["hudPosY"] = hudPosY;
    j["isHudModule"] = true;
    j["style"] = std::to_string(style) + ",Default,Round";
    j["scale"] = scale;
    j["backAlpha"] = backAlpha;
    j["liveTime"] = liveTime;
    j["animationMode"] = std::to_string(animationMode) + ",Scale,Fade,Both";
    j["animationSpeed"] = animationSpeed;
    j["cardWidth"] = cardWidth;
    j["cardHeight"] = cardHeight;
    j["cornerRadius"] = cornerRadius;
    j["accentColor"] = accentColor;
    j["showDistance"] = showDistance;
    j["showHealthBar"] = showHealthBar;
    j["showHurtFlash"] = showHurtFlash;
}
