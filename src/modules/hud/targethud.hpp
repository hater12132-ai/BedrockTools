#pragma once

// targethud.hpp
// -----------------------------------------------------------------------
// HUD module: shows the head, gamertag, and an HP bar for whichever
// entity is currently under the local player's crosshair.
//
// Integration notes (verify against your actual include/bedrocktools
// headers -- these are the only guessed identifiers in this file; the
// raycast, health math, color, and layout logic is self-contained and
// does not depend on any of these guesses):
//
//   1. `bedrocktools::Module`  -- assumed base class every module derives
//      from (onEnable/onDisable/onDraw virtuals). Rename if different.
//   2. `bedrocktools::events::RuntimeListener<...>` /
//      `LocalPlayerTickEvent` -- taken verbatim from the README example,
//      should be correct as-is.
//   3. `pl::modmenu::DrawCommand` / `pl::modmenu::submitDrawCommands` --
//      confirmed present in the shipped binary (symbols
//      `pl::modmenu::registerModule(ModuleInfo const&)` and
//      `pl::modmenu::submitDrawCommands(string_view, span<DrawCommand>)`),
//      but the exact builder helpers (text/rect) are guessed in the .cpp
//      -- swap them for whatever an existing HUD module (Reach Counter,
//      Combo Display) already calls.
//   4. Entity access goes through `entt::registry` since the project
//      depends on `entt`. `getEntityDisplayName` / `getEntityHealth` /
//      `getEntityEyePosition` in the .cpp are the three points where you
//      plug in your actual components, or delegate to whatever
//      Hitbox/Reach Counter already use to resolve entities.
// -----------------------------------------------------------------------

#include <bedrocktools/BedrockTools.hpp>
#include "../Module.hpp"

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace bedrocktools::modules::hud {

struct TargetInfo {
    entt::entity entity{entt::null};
    std::uint64_t uniqueId = 0;
    std::string displayName;
    float health = 0.0f;
    float maxHealth = 0.0f;
    glm::vec3 headPosition{0.0f};
};

class TargetHud : public bedrocktools::Module {
public:
    TargetHud();
    ~TargetHud() override;

    void onEnable() override;
    void onDisable() override;

    // Called once per render frame while the module is enabled.
    void onDraw() override;

private:
    void onLocalPlayerTick(bedrocktools::events::LocalPlayerTickEvent& event);

    // Casts a ray from the local player's eyes along their view direction
    // and returns the closest living entity it hits within maxDistance.
    std::optional<TargetInfo> findTargetedEntity(entt::registry& registry,
                                                   const glm::vec3& eyePos,
                                                   const glm::vec3& lookDir,
                                                   float maxDistance) const;

    void drawTargetOverlay(const TargetInfo& target) const;

    // --- config ---
    // Property names follow the barColorHex / hudPosX / hudPosY
    // convention already present elsewhere in the pack.
    std::string barColorHex = "FF8C00"; // orange (was purple)
    float barWidth = 120.0f;
    float barHeight = 6.0f;
    float hudPosX = 0.0f;  // offset from crosshair center
    float hudPosY = 40.0f;
    float maxTargetDistance = 32.0f; // blocks

    // --- per-frame cached state ---
    std::optional<TargetInfo> m_currentTarget;

    bedrocktools::events::RuntimeListener<bedrocktools::events::LocalPlayerTickEvent> m_tickListener;
};

} // namespace bedrocktools::modules::hud
