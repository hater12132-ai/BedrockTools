#pragma once

#include "../Module.hpp"
#include <bedrocktools/sdk/Types.hpp>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

class WorldParticlesModule : public Module {
public:
    enum class Mode : int {
        Snow = 0,
        Hearts = 1,
        Stars = 2,
        Orbs = 3,
        Storm = 4,
        Snowflake = 5,
        Dollar = 6,
        Pumpkin = 7,
        Multi = 8,
        Glowfly = 9
    };

    WorldParticlesModule();
    ~WorldParticlesModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    Mode mode = Mode::Snow;
    int density = 80;
    float radius = 18.0f;
    float fallSpeed = 1.0f;
    float particleSize = 0.12f;
    float opacity = 0.85f;
    std::string colorHex = "#FFFFFFFF";
    bool forceTint = false;
    float wind = 0.35f;

    struct Particle {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        float vx = 0.0f;
        float vy = 0.0f;
        float vz = 0.0f;
        float life = 0.0f;
        float maxLife = 1.0f;
        float size = 0.1f;
        float phase = 0.0f;
        int style = 0;
        std::uint32_t color = 0xFFFFFFFFu;
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
