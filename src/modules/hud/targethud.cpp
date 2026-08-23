// targethud.cpp
// -----------------------------------------------------------------------
// See targethud.hpp for integration notes on the handful of guessed
// symbol names. Everything below -- the raycast, health-to-bar math,
// and the orange color -- is fully worked out and shouldn't need
// changes beyond the marked ADAPT spots.
// -----------------------------------------------------------------------

#include "targethud.hpp"

#include <algorithm>
#include <cmath>

#include <fmt/format.h>

namespace bedrocktools::modules::hud {

namespace {

// --- ADAPT: replace these three with your actual entity/component API ---
// They're isolated here on purpose so the rest of the module doesn't care
// how entity data is actually stored.

std::string getEntityDisplayName(entt::registry& registry, entt::entity e) {
    // e.g.: return registry.get<NameTagComponent>(e).text;
    if (auto* nameTag = registry.try_get<bedrocktools::components::NameTag>(e)) {
        return nameTag->text;
    }
    return "Unknown";
}

// Returns {current, max} health.
std::pair<float, float> getEntityHealth(entt::registry& registry, entt::entity e) {
    // e.g.: auto& hp = registry.get<HealthComponent>(e); return {hp.current, hp.max};
    if (auto* health = registry.try_get<bedrocktools::components::Health>(e)) {
        return {health->current, health->max};
    }
    return {0.0f, 0.0f};
}

glm::vec3 getEntityEyePosition(entt::registry& registry, entt::entity e) {
    // e.g.: return registry.get<TransformComponent>(e).position + eyeOffset;
    if (auto* transform = registry.try_get<bedrocktools::components::Transform>(e)) {
        return transform->position + glm::vec3{0.0f, transform->eyeHeight, 0.0f};
    }
    return glm::vec3{0.0f};
}

std::uint64_t getEntityUniqueId(entt::registry& registry, entt::entity e) {
    if (auto* idComp = registry.try_get<bedrocktools::components::UniqueId>(e)) {
        return idComp->value;
    }
    return static_cast<std::uint64_t>(entt::to_integral(e));
}

bool isLocalPlayer(entt::registry& registry, entt::entity e) {
    return registry.all_of<bedrocktools::components::LocalPlayerTag>(e);
}

// --- end ADAPT block ---

// Ray vs. sphere test used as a cheap entity hit-test. Good enough for a
// "what am I looking at" HUD (not a physics system) -- swap for an AABB
// test against the entity's actual hitbox if you want pixel-perfect
// parity with the Hitbox module.
bool rayIntersectsSphere(const glm::vec3& rayOrigin, const glm::vec3& rayDir,
                          const glm::vec3& sphereCenter, float sphereRadius,
                          float& outDistance) {
    const glm::vec3 originToCenter = sphereCenter - rayOrigin;
    const float projLength = glm::dot(originToCenter, rayDir);
    if (projLength < 0.0f) {
        return false; // sphere is behind the ray origin
    }
    const glm::vec3 closestPoint = rayOrigin + rayDir * projLength;
    const float distSq = glm::length2(sphereCenter - closestPoint);
    const float radiusSq = sphereRadius * sphereRadius;
    if (distSq > radiusSq) {
        return false;
    }
    const float offset = std::sqrt(radiusSq - distSq);
    outDistance = projLength - offset;
    return outDistance >= 0.0f;
}

} // namespace

TargetHud::TargetHud()
    : m_tickListener([this](auto& event) { onLocalPlayerTick(event); }) {
}

TargetHud::~TargetHud() = default;

void TargetHud::onEnable() {
    m_tickListener.subscribe();
    m_currentTarget.reset();
}

void TargetHud::onDisable() {
    m_tickListener.unsubscribe();
    m_currentTarget.reset();
}

void TargetHud::onLocalPlayerTick(bedrocktools::events::LocalPlayerTickEvent& event) {
    if (!event.player) {
        m_currentTarget.reset();
        return;
    }

    auto& registry = bedrocktools::getEntityRegistry(); // ADAPT: name/location of the global registry accessor
    const glm::vec3 eyePos = event.player->eyePosition(); // ADAPT: exact accessor name
    const glm::vec3 lookDir = event.player->lookDirection(); // ADAPT: exact accessor name

    m_currentTarget = findTargetedEntity(registry, eyePos, lookDir, maxTargetDistance);
}

std::optional<TargetInfo> TargetHud::findTargetedEntity(entt::registry& registry,
                                                          const glm::vec3& eyePos,
                                                          const glm::vec3& lookDir,
                                                          float maxDistance) const {
    constexpr float kEntityHitRadius = 0.9f; // roughly a player-sized hitbox radius

    std::optional<TargetInfo> best;
    float bestDistance = maxDistance;

    auto view = registry.view<bedrocktools::components::Transform>();
    for (auto entity : view) {
        if (isLocalPlayer(registry, entity)) {
            continue; // don't target yourself
        }

        const glm::vec3 targetPos = getEntityEyePosition(registry, entity);
        float hitDistance = 0.0f;
        if (!rayIntersectsSphere(eyePos, lookDir, targetPos, kEntityHitRadius, hitDistance)) {
            continue;
        }
        if (hitDistance >= bestDistance) {
            continue;
        }

        auto [health, maxHealth] = getEntityHealth(registry, entity);
        if (maxHealth <= 0.0f) {
            continue; // not a health-having entity worth showing
        }

        TargetInfo info;
        info.entity = entity;
        info.uniqueId = getEntityUniqueId(registry, entity);
        info.displayName = getEntityDisplayName(registry, entity);
        info.health = health;
        info.maxHealth = maxHealth;
        info.headPosition = targetPos;

        bestDistance = hitDistance;
        best = info;
    }

    return best;
}

void TargetHud::onDraw() {
    if (!m_currentTarget.has_value()) {
        return;
    }
    drawTargetOverlay(*m_currentTarget);
}

void TargetHud::drawTargetOverlay(const TargetInfo& target) const {
    // Per-target cache key, matching the existing "targethud_%llx" naming
    // convention (used elsewhere in the pack for per-entity texture caches).
    const std::string cacheKey = fmt::format("targethud_{:x}", target.uniqueId);

    const float healthFraction = target.maxHealth > 0.0f
        ? std::clamp(target.health / target.maxHealth, 0.0f, 1.0f)
        : 0.0f;

    // Orange fill (was purple). Feel free to shift toward red at low HP by
    // lerping between two hex colors here if you want extra polish.
    const std::string fillColorHex = barColorHex; // "FF8C00"

    const float filledWidth = barWidth * healthFraction;

    // ADAPT: swap these two calls for whatever draw-command builders your
    // other HUD modules already use (pl::modmenu::DrawCommand variants).
    // Shown here as a plausible shape given the confirmed
    // pl::modmenu::submitDrawCommands(name, span<DrawCommand>) signature.

    std::vector<pl::modmenu::DrawCommand> commands;

    // Background track for the bar (dim/gray).
    commands.push_back(pl::modmenu::DrawCommand::rect(
        hudPosX, hudPosY, barWidth, barHeight, "404040", cacheKey + "_bg"));

    // Foreground fill, orange, scaled to current HP.
    commands.push_back(pl::modmenu::DrawCommand::rect(
        hudPosX, hudPosY, filledWidth, barHeight, fillColorHex, cacheKey + "_fill"));

    // Gamertag above the bar.
    commands.push_back(pl::modmenu::DrawCommand::text(
        hudPosX, hudPosY - 14.0f, target.displayName, "FFFFFF", cacheKey + "_name"));

    // Player head icon to the left of the bar (uses the same cache key
    // pattern the shipped module already uses for per-entity textures).
    commands.push_back(pl::modmenu::DrawCommand::playerHead(
        hudPosX - 18.0f, hudPosY - 16.0f, 16.0f, 16.0f, target.uniqueId, cacheKey));

    pl::modmenu::submitDrawCommands("Target HUD", commands);
}

} // namespace bedrocktools::modules::hud
