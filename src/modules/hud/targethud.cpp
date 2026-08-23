// targethud.cpp

#include "targethud.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

#include <fmt/format.h>

namespace bedrocktools::modules::hud {

namespace {

// -----------------------------------------------------------------------
// Entity helpers
// -----------------------------------------------------------------------

std::string getEntityDisplayName(
    entt::registry& registry,
    entt::entity entity
) {
    if (auto* nameTag =
            registry.try_get<bedrocktools::components::NameTag>(entity)) {
        return nameTag->text;
    }

    return "Unknown";
}

std::pair<float, float> getEntityHealth(
    entt::registry& registry,
    entt::entity entity
) {
    if (auto* health =
            registry.try_get<bedrocktools::components::Health>(entity)) {
        return {health->current, health->max};
    }

    return {0.0f, 0.0f};
}

glm::vec3 getEntityEyePosition(
    entt::registry& registry,
    entt::entity entity
) {
    if (auto* transform =
            registry.try_get<bedrocktools::components::Transform>(entity)) {
        return transform->position +
               glm::vec3{
                   0.0f,
                   transform->eyeHeight,
                   0.0f
               };
    }

    return glm::vec3{0.0f};
}

std::uint64_t getEntityUniqueId(
    entt::registry& registry,
    entt::entity entity
) {
    if (auto* idComp =
            registry.try_get<bedrocktools::components::UniqueId>(entity)) {
        return idComp->value;
    }

    return static_cast<std::uint64_t>(
        entt::to_integral(entity)
    );
}

bool isLocalPlayer(
    entt::registry& registry,
    entt::entity entity
) {
    return registry.all_of<
        bedrocktools::components::LocalPlayerTag
    >(entity);
}

// -----------------------------------------------------------------------
// Simple ray/sphere intersection
// -----------------------------------------------------------------------

bool rayIntersectsSphere(
    const glm::vec3& rayOrigin,
    const glm::vec3& rayDirection,
    const glm::vec3& sphereCenter,
    float sphereRadius,
    float& outDistance
) {
    const glm::vec3 originToCenter =
        sphereCenter - rayOrigin;

    const float projection =
        glm::dot(originToCenter, rayDirection);

    if (projection < 0.0f) {
        return false;
    }

    const glm::vec3 closestPoint =
        rayOrigin + rayDirection * projection;

    const glm::vec3 difference =
        sphereCenter - closestPoint;

    const float distanceSquared =
        glm::dot(difference, difference);

    const float radiusSquared =
        sphereRadius * sphereRadius;

    if (distanceSquared > radiusSquared) {
        return false;
    }

    const float offset =
        std::sqrt(radiusSquared - distanceSquared);

    outDistance = projection - offset;

    return outDistance >= 0.0f;
}

} // namespace

// -----------------------------------------------------------------------
// Constructor / destructor
// -----------------------------------------------------------------------

TargetHud::TargetHud()
    : Module(
          "TargetHud",
          "Shows information about the entity under the crosshair"
      ),
      m_tickListener(
          [this](auto& event) {
              onLocalPlayerTick(event);
          }
      ) {
}

TargetHud::~TargetHud() = default;

// -----------------------------------------------------------------------
// Enable / disable
// -----------------------------------------------------------------------

void TargetHud::onEnable() {
    m_currentTarget.reset();
    m_tickListener.subscribe();
}

void TargetHud::onDisable() {
    m_tickListener.unsubscribe();
    m_currentTarget.reset();
}

// -----------------------------------------------------------------------
// Local player tick
// -----------------------------------------------------------------------

void TargetHud::onLocalPlayerTick(
    bedrocktools::events::LocalPlayerTickEvent& event
) {
    if (!event.player) {
        m_currentTarget.reset();
        return;
    }

    auto& registry =
        bedrocktools::getEntityRegistry();

    const glm::vec3 eyePosition =
        event.player->eyePosition();

    const glm::vec3 lookDirection =
        event.player->lookDirection();

    m_currentTarget =
        findTargetedEntity(
            registry,
            eyePosition,
            lookDirection,
            maxTargetDistance
        );
}

// -----------------------------------------------------------------------
// Find entity under crosshair
// -----------------------------------------------------------------------

std::optional<TargetInfo>
TargetHud::findTargetedEntity(
    entt::registry& registry,
    const glm::vec3& eyePosition,
    const glm::vec3& lookDirection,
    float maxDistance
) const {

    constexpr float kEntityHitRadius = 0.9f;

    std::optional<TargetInfo> bestTarget;
    float bestDistance = maxDistance;

    auto view =
        registry.view<
            bedrocktools::components::Transform
        >();

    for (auto entity : view) {

        // Don't target ourselves.
        if (isLocalPlayer(registry, entity)) {
            continue;
        }

        const glm::vec3 targetPosition =
            getEntityEyePosition(
                registry,
                entity
            );

        float hitDistance = 0.0f;

        if (!rayIntersectsSphere(
                eyePosition,
                lookDirection,
                targetPosition,
                kEntityHitRadius,
                hitDistance)) {
            continue;
        }

        if (hitDistance >= bestDistance) {
            continue;
        }

        const auto [health, maxHealth] =
            getEntityHealth(
                registry,
                entity
            );

        // Ignore entities without valid health.
        if (maxHealth <= 0.0f) {
            continue;
        }

        TargetInfo target;

        target.entity = entity;
        target.uniqueId =
            getEntityUniqueId(
                registry,
                entity
            );

        target.displayName =
            getEntityDisplayName(
                registry,
                entity
            );

        target.health = health;
        target.maxHealth = maxHealth;
        target.headPosition = targetPosition;

        bestDistance = hitDistance;
        bestTarget = target;
    }

    return bestTarget;
}

// -----------------------------------------------------------------------
// Per-frame update
// -----------------------------------------------------------------------

void TargetHud::onFrame() {

    if (!m_currentTarget.has_value()) {
        return;
    }

    drawTargetOverlay(
        *m_currentTarget
    );
}

// -----------------------------------------------------------------------
// HUD rendering
// -----------------------------------------------------------------------

void TargetHud::drawTargetOverlay(
    const TargetInfo& target
) const {

    const std::string cacheKey =
        fmt::format(
            "targethud_{:x}",
            target.uniqueId
        );

    const float healthFraction =
        target.maxHealth > 0.0f
            ? std::clamp(
                  target.health / target.maxHealth,
                  0.0f,
                  1.0f
              )
            : 0.0f;

    const float filledWidth =
        barWidth * healthFraction;

    const std::string fillColorHex =
        barColorHex;

    std::vector<pl::modmenu::DrawCommand> commands;

    // Background.
    commands.push_back(
        pl::modmenu::DrawCommand::rect(
            hudPosX,
            hudPosY,
            barWidth,
            barHeight,
            "404040",
            cacheKey + "_bg"
        )
    );

    // Health fill.
    if (filledWidth > 0.0f) {
        commands.push_back(
            pl::modmenu::DrawCommand::rect(
                hudPosX,
                hudPosY,
                filledWidth,
                barHeight,
                fillColorHex,
                cacheKey + "_fill"
            )
        );
    }

    // Gamertag.
    commands.push_back(
        pl::modmenu::DrawCommand::text(
            hudPosX,
            hudPosY - 14.0f,
            target.displayName,
            "FFFFFF",
            cacheKey + "_name"
        )
    );

    // Player head.
    commands.push_back(
        pl::modmenu::DrawCommand::playerHead(
            hudPosX - 18.0f,
            hudPosY - 16.0f,
            16.0f,
            16.0f,
            target.uniqueId,
            cacheKey
        )
    );

    pl::modmenu::submitDrawCommands(
        "Target HUD",
        commands
    );
}

} // namespace bedrocktools::modules::hud
