#include "hitoutline.hpp"
#include "modules/ModuleRegistry.hpp"

#include <bedrocktools/sdk/world/Actor.hpp>

#include <algorithm>
#include <android/log.h>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#define HITOUTLINE_LOG(...) __android_log_print(ANDROID_LOG_WARN, "BT-HitOutline", __VA_ARGS__)

HitOutlineModule* g_hitOutlineInstance = nullptr;

namespace {

std::int64_t nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

} // namespace

HitOutlineModule::HitOutlineModule()
    : Module("Hit Outline", "Outlines any entity (players, cows, pigs, sheep, mobs) briefly when you hit it. Occluded by terrain - not an ESP.") {
    g_hitOutlineInstance = this;
}

HitOutlineModule::~HitOutlineModule() {
    if (m_attackSubscription) bedrocktools::events::bus().unsubscribe(m_attackSubscription);
    if (g_hitOutlineInstance == this) g_hitOutlineInstance = nullptr;
}

void HitOutlineModule::onInit() {
    // Fires for every GameMode::attack() call - players AND mobs, since
    // AttackEvent::target is a plain sdk::Actor*, not Player*.
    m_attackSubscription = bedrocktools::events::bus().subscribe<bedrocktools::events::AttackEvent>(
        [this](auto& event) {
            HITOUTLINE_LOG("AttackEvent fired, target=%p", static_cast<void*>(event.target));
            if (!enabled || !event.target) return;
            void* ctx = event.target->entityContext();
            HITOUTLINE_LOG("tracking hit, entityContext=%p, enabled=%d", ctx, enabled);
            trackHit(ctx);
        }
    );

    // No render hook to install here - glintcolor.cpp's existing
    // ActorShaderManagerSetupShaderParametersActorGlint hook calls into
    // shouldOutline() directly via g_hitOutlineInstance.
}

void HitOutlineModule::onEnable() {}
void HitOutlineModule::onDisable() {}

void HitOutlineModule::onFrame() {
    const std::int64_t t = nowMs();
    bool anyActive = false;
    for (auto& hit : m_hits) {
        if (hit.entityContext) {
            if (hit.expiresAtMs <= t) {
                hit.entityContext = nullptr;
            } else {
                anyActive = true;
            }
        }
    }

    if (anyActive) drawDebugBanner();
}

void HitOutlineModule::drawDebugBanner() {
    // TEMPORARY - see the comment on this method in hitoutline.hpp. Big,
    // impossible-to-miss banner so this is a yes/no visual check, not
    // something you have to squint at.
    std::vector<PLModMenu_DrawCommand> cmds;

    PLModMenu_DrawCommand bgCmd = {};
    bgCmd.type = PL_DRAW_RECT_FILLED;
    bgCmd.x = 40.0f;
    bgCmd.y = 40.0f;
    bgCmd.w = 500.0f;
    bgCmd.h = 60.0f;
    bgCmd.color = 0xCCFF00FFu; // bright magenta, impossible to confuse with normal UI
    cmds.push_back(bgCmd);

    PLModMenu_DrawCommand txtCmd = {};
    txtCmd.type = PL_DRAW_TEXT;
    txtCmd.x = 50.0f;
    txtCmd.y = 55.0f;
    txtCmd.w = 480.0f;
    txtCmd.h = 40.0f;
    txtCmd.color = 0xFF000000u;
    txtCmd.size = 30.0f;
    static const char* debugText = "HITOUTLINE DEBUG: ATTACKEVENT FIRED";
    txtCmd.text = debugText;
    cmds.push_back(txtCmd);

    submitDrawCommands(moduleId, cmds);
}

void HitOutlineModule::trackHit(void* entityContext) {
    if (!entityContext) return;

    const std::int64_t expiresAt = nowMs() + m_durationMs.load(std::memory_order_relaxed);

    // Refresh if this entity is already tracked (e.g. hitting the same cow
    // repeatedly - don't burn a new slot every swing).
    for (auto& hit : m_hits) {
        if (hit.entityContext == entityContext) {
            hit.expiresAtMs = expiresAt;
            return;
        }
    }

    // Otherwise take the next ring-buffer slot (round-robins, so hitting more
    // than kMaxTracked entities in one window just evicts the oldest).
    m_hits[m_nextSlot] = Hit{entityContext, expiresAt};
    m_nextSlot = (m_nextSlot + 1) % kMaxTracked;
}

bool HitOutlineModule::shouldOutline(void* entityContext, bedrocktools::sdk::Color& outColor) const {
    if (!enabled || !entityContext) return false;

    const std::int64_t t = nowMs();
    bool hit = false;
    for (const auto& h : m_hits) {
        if (h.entityContext == entityContext && h.expiresAtMs > t) {
            hit = true;
            break;
        }
    }
    if (!hit) return false;

    const std::uint32_t rgb = m_colorRGB.load(std::memory_order_relaxed);
    const float opacity = std::clamp(m_opacity.load(std::memory_order_relaxed), 0.0f, 1.0f);
    outColor = {
        static_cast<float>((rgb >> 16) & 0xFFu) / 255.0f,
        static_cast<float>((rgb >> 8) & 0xFFu) / 255.0f,
        static_cast<float>(rgb & 0xFFu) / 255.0f,
        opacity
    };
    return true;
}

void HitOutlineModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    if (j.contains("color") && j["color"].is_string()) {
        const std::string value = j["color"].get<std::string>();
        if (!value.empty() && value[0] == '#') {
            try {
                m_colorRGB.store(static_cast<std::uint32_t>(std::stoul(value.substr(1), nullptr, 16)), std::memory_order_relaxed);
            } catch (...) {}
        }
    }
    if (j.contains("opacity")) {
        try { m_opacity.store(std::clamp(j["opacity"].get<float>(), 0.0f, 1.0f), std::memory_order_relaxed); } catch (...) {}
    }
    if (j.contains("durationMs")) m_durationMs.store(j["durationMs"].get<int>(), std::memory_order_relaxed);
}

void HitOutlineModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    char color[8];
    std::snprintf(color, sizeof(color), "#%06X", m_colorRGB.load(std::memory_order_relaxed) & 0xFFFFFFu);
    j["color"] = std::string(color);
    j["opacity"] = m_opacity.load(std::memory_order_relaxed);
    j["durationMs"] = m_durationMs.load(std::memory_order_relaxed);
}
