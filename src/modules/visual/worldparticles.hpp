#pragma once

#include "../Module.hpp"
#include <bedrocktools/sdk/Types.hpp>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

// SoupVisuals-style AmbientParticles for BedrockTools.
// Modes + Fall/Fly/Emerge physics inspired by SoupVisuals AmbientParticles.
class WorldParticlesModule : public Module {
public:
    // Matches SoupVisuals ambient mode names (subset that works as line sparkles).
    enum class Mode : int {
        Stars = 0,
        Hearts = 1,
        Bloom = 2,
        Blink = 3,
        Dollar = 4,
        Flame = 5,
        Snowflake = 6,
        Virus = 7,
        Firefly = 8,
        Network = 9,
        Cube = 10,
        Pyramid = 11,
        Multi = 12
    };

    enum class Physics : int {
        Fall = 0,
        Fly = 1,
        Emerge = 2
    };

    WorldParticlesModule();
    ~WorldParticlesModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    Mode mode = Mode::Stars;
    Physics physics = Physics::Fall;
    int density = 60;           // regular particle count target
    float spawnRadius = 25.0f;  // Soup default
    float spawnHeight = 4.0f;   // Soup default
    float particleSize = 0.15f;
    float opacity = 0.9f;
    float speed = 1.0f;
    std::string colorHex = "#FFFFFFFF";
    bool forceTint = false;
    // Firefly
    int fireflyCount = 25;
    float fireflyScale = 0.2f;
    int trailLength = 8;
    // Network
    float linkDistance = 5.0f;
    int maxLinks = 3;

    struct TrailPoint {
        float x = 0, y = 0, z = 0;
    };

    struct Particle {
        float x = 0, y = 0, z = 0;
        float vx = 0, vy = 0, vz = 0;
        float life = 0;
        float maxLife = 1;
        float size = 0.1f;
        float phase = 0;
        float rot = 0;
        float rotSpeed = 0;
        int style = 0;
        std::uint32_t color = 0xFFFFFFFFu;
        bool isFirefly = false;
        std::vector<TrailPoint> trail;
    };

    std::mutex particlesMutex;
    std::vector<Particle> particles;

private:
    bool m_patched = false;
    void* m_patchTarget = nullptr;
    void* m_tessBeginAddr = nullptr;
    void* m_tessColorAddr = nullptr;
    void* m_tessVertexAddr = nullptr;
    void* m_renderMeshAddr = nullptr;
    void* m_renderMaterialGroupAddr = nullptr;

    void applyPatch();
};
