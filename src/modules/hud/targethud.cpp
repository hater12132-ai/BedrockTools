#include "targethud.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include <fmt/format.h>

namespace bedrocktools::modules::hud {

namespace {

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

// NOTE: guessed component name. Point this at whatever your
// registry actually uses to mark "this entity is a player"
// (as opposed to a mob). If you track mob type via an id/name
// component instead, branch on that here and return
// "A " + mobTypeName.
std::string getEntityCategoryLabel(
    entt::registry& registry,
    entt::entity entity
) {
    if (registry.all_of<bedrocktools::components::PlayerTag>(entity)) {
        return "A Player";
    }
    return "An Entity";
}

// Matches the "16,0" comma-decimal style from the reference
// screenshot. Drop the std::replace call if you want a plain
// period instead.
std::string formatHealthValue(float value) {
    std::string text = fmt::format("{:.1f}", value);
    std::replace(text.begin(), text.end(), '.', ',');
    return text;
}

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

void TargetHud::onEnable() {
    m_currentTarget.reset();
    m_tickListener.subscribe();
}

void TargetHud::onDisable() {
    m_tickListener.unsubscribe();
    m_currentTarget.reset();
}

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
        target.categoryLabel =
            getEntityCategoryLabel(
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

void TargetHud::onFrame() {
    if (!m_currentTarget.has_value()) {
        return;
    }

    drawTargetOverlay(
        *m_currentTarget
    );
}

void TargetHud::drawTargetOverlay(
    const TargetInfo& target
) const {
    const std::string cacheKey =
        fmt::format(
            "targethud_{:x}",
            target.uniqueId
        );

    // --- Layout constants -------------------------------------------------
    // Panel sits above the bar and holds the head icon, gamertag,
    // HP line, and category label. The bar itself is the thin strip
    // below the panel. Tweak these against your actual hudPosX/hudPosY
    // anchor if the panel ends up off-screen at small resolutions.
    constexpr float kHeadSize = 32.0f;
    constexpr float kPanelPaddingX = 8.0f;
    constexpr float kPanelHeight = 40.0f;
    constexpr float kPanelGap = 4.0f; // gap between panel bottom and bar

    const float panelWidth = std::max(barWidth, 150.0f);
    const float panelX = hudPosX;
    const float panelY = hudPosY - kPanelHeight - kPanelGap;

    const float headX = panelX + kPanelPaddingX;
    const float headY = panelY + (kPanelHeight - kHeadSize) * 0.5f;

    const float textX = headX + kHeadSize + 8.0f;
    const float nameY = panelY + 6.0f;
    const float hpY = panelY + 22.0f;

    // Bar fill now comes from barColorHex (defaults to purple in
    // targethud.hpp) rather than being hardcoded here.
    constexpr const char* kBarTrackColor = "2B2B2E";
    constexpr const char* kPanelColor = "1C1C22";
    constexpr const char* kNameColor = "FFFFFF";
    constexpr const char* kSubTextColor = "AFAFAF";

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

    std::vector<pl::modmenu::DrawCommand> commands;

    // Panel background (head / gamertag / HP / category)
    commands.push_back(
        pl::modmenu::DrawCommand::rect(
            panelX,
            panelY,
            panelWidth,
            kPanelHeight,
            kPanelColor,
            cacheKey + "_panel"
        )
    );

    // Player head
    commands.push_back(
        pl::modmenu::DrawCommand::playerHead(
            headX,
            headY,
            kHeadSize,
            kHeadSize,
            target.uniqueId,
            cacheKey
        )
    );

    // Gamertag
    commands.push_back(
        pl::modmenu::DrawCommand::text(
            textX,
            nameY,
            target.displayName,
            kNameColor,
            cacheKey + "_name"
        )
    );

    // HP line, e.g. "HP / 16,0"
    commands.push_back(
        pl::modmenu::DrawCommand::text(
            textX,
            hpY,
            fmt::format("HP / {}", formatHealthValue(target.health)),
            kSubTextColor,
            cacheKey + "_hp"
        )
    );

    // Category label, right-aligned in the panel
    // (subtract an estimate for text width if your DrawCommand::text
    // doesn't support a right-align flag directly)
    commands.push_back(
        pl::modmenu::DrawCommand::text(
            panelX + panelWidth - 70.0f,
            nameY,
            target.categoryLabel,
            kSubTextColor,
            cacheKey + "_category"
        )
    );

    // Bar track (background)
    commands.push_back(
        pl::modmenu::DrawCommand::rect(
            hudPosX,
            hudPosY,
            barWidth,
            barHeight,
            kBarTrackColor,
            cacheKey + "_bg"
        )
    );

    // Bar fill (purple)
    if (filledWidth > 0.0f) {
        commands.push_back(
            pl::modmenu::DrawCommand::rect(
                hudPosX,
                hudPosY,
                filledWidth,
                barHeight,
                barColorHex,
                cacheKey + "_fill"
            )
        );
    }

    pl::modmenu::submitDrawCommands(
        "Target HUD",
        commands
    );
}

} // namespace bedrocktools::modules::hud
