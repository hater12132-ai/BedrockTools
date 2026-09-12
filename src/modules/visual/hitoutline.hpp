#pragma once

#include "../Module.hpp"
#include <bedrocktools/events/AttackEvent.hpp>
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/sdk/Types.hpp>

#include <atomic>
#include <cstdint>
#include <array>

// Tracks recently-hit entities (players AND mobs - cows, pigs, sheep, zombies,
// anything with an Actor) so ActorShaderManagerSetupShaderParametersActorGlint's
// hook (installed by glintcolor.cpp) can tint them while the window is active.
//
// This module does NOT install its own render hook - glintcolor.cpp already
// owns ActorShaderManagerSetupShaderParametersActorGlint, and that same hook
// already receives both `entityContext` and `actor` per call, which is all
// the identity we need. Installing a second detour on the same address would
// conflict, so instead glintcolor.cpp's hook calls HitOutlineModule::shouldOutline()
// directly via g_hitOutlineInstance before it does its own glint-color logic.
class HitOutlineModule : public Module {
public:
    HitOutlineModule();
    ~HitOutlineModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void onFrame() override;

    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    // Called from glintcolor.cpp's setupActorShaderParametersGlintHook.
    // entityContext should be the same pointer AttackEvent::target->entityContext()
    // produces. Returns true + writes outColor if this entity was hit recently.
    bool shouldOutline(void* entityContext, bedrocktools::sdk::Color& outColor) const;

private:
    struct Hit {
        void* entityContext = nullptr;
        std::int64_t expiresAtMs = 0;
    };

    static constexpr std::size_t kMaxTracked = 16; // several mobs at once, e.g. hitting a flock of sheep

    std::array<Hit, kMaxTracked> m_hits{};
    std::size_t m_nextSlot = 0;

    std::atomic<std::uint32_t> m_colorRGB{0xFFFFFFu}; // 0xRRGGBB, white matches the video
    std::atomic<float> m_opacity{1.0f};
    std::atomic<int> m_durationMs{300};

    bedrocktools::events::Subscription m_attackSubscription = 0;

    // TEMPORARY DIAGNOSTIC - draws an unmissable on-screen banner whenever a
    // hit is tracked, using the same proven HUD draw path as ReachCounter/
    // TargetHud. This is completely independent of the glint-hook render
    // path. If you see this banner, AttackEvent + tracking are fine and the
    // bug is isolated to the glint/entityContext side. If you never see it,
    // the AttackEvent signature itself is dead on your game version. Remove
    // once the real bug is found - it's noisy on purpose.
    std::int64_t m_lastDebugBannerAtMs = 0;
    void drawDebugBanner();

    void trackHit(void* entityContext);
};

// Set/cleared in the ctor/dtor, same pattern as g_glintColor in glintcolor.cpp.
// glintcolor.cpp reads this to consult shouldOutline() without a second hook.
extern HitOutlineModule* g_hitOutlineInstance;
