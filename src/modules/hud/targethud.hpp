#pragma once

#include "../Module.hpp"
#include <bedrocktools/BedrockTools.hpp>

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

class TargetHud : public ::Module {
public:
    TargetHud();
    ~TargetHud() override;

    void onEnable() override;
    void onDisable() override;
    void onFrame() override;

private:
    void onLocalPlayerTick(
        bedrocktools::events::LocalPlayerTickEvent& event
    );

    std::optional<TargetInfo> findTargetedEntity(
        entt::registry& registry,
        const glm::vec3& eyePosition,
        const glm::vec3& lookDirection,
        float maxDistance
    ) const;

    void drawTargetOverlay(
        const TargetInfo& target
    ) const;

    std::string barColorHex = "FF8C00";

    float barWidth = 120.0f;
    float barHeight = 6.0f;

    float hudPosX = 0.0f;
    float hudPosY = 40.0f;

    float maxTargetDistance = 32.0f;

    std::optional<TargetInfo> m_currentTarget;

    bedrocktools::events::RuntimeListener<
        bedrocktools::events::LocalPlayerTickEvent
    > m_tickListener;
};

} // namespace bedrocktools::modules::hud
