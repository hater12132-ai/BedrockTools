#pragma once

#include "../Module.hpp"
#include <bedrocktools/sdk/Types.hpp>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

// SoupVisuals AmbientParticles recreation for BedrockTools.
class WorldParticlesModule : public Module {
public:
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

    Mode mode = Mode::Firefly;
    Physics physics = Physics::Fly;
    int density = 50;
    float spawnRadius = 20.0f;
    float spawnHeight = 5.0f;
    float particleSize = 0.18f;
    float opacity = 0.95f;
    float speed = 1.0f;
    std::string colorHex = "#FFFFFFFF";
    bool forceTint = false;
    int fireflyCount = 30;
    float fireflyScale = 0.22f;
    int trailLength = 12;
    float linkDistance = 5.0f;
    int maxLinks = 3;
    float glowStrength = 0.85f;

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
