#pragma once

#include "../Module.hpp"
#include <bedrocktools/sdk/Types.hpp>
#include <string>
#include <vector>
#include <mutex>
#include <cstdint>

class WorldParticlesModule : public Module {
public:
    enum class Mode : int {
        Snow = 0,       // normal falling snow
        Hearts = 1,     // heart particles
        Stars = 2,      // star twinkles
        Orbs = 3,       // floating orbs
        Storm = 4,      // heavy storm flakes
        Snowflake = 5,  // larger flakes
        Dollar = 6,     // green money-style
        Pumpkin = 7,    // orange halloween
        Multi = 8,      // mix of styles
        Glowfly = 9     // fireflies
    };

    WorldParticlesModule();
    ~WorldParticlesModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void onMenuRegistered() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    Mode mode = Mode::Snow;
    int density = 80;          // target live particles
    float radius = 18.0f;      // spawn radius around player
    float fallSpeed = 1.0f;    // vertical speed multiplier
    float particleSize = 0.12f;
    float opacity = 0.85f;
    std::string colorHex = "#FFFFFFFF";
    bool rainbow = false;
    float wind = 0.35f;

    struct Particle {
        float x = 0, y = 0, z = 0;
        float vx = 0, vy = 0, vz = 0;
        float life = 0;
        float maxLife = 1;
        float size = 0.1f;
        float phase = 0;
        int style = 0; // sub-style for multi mode
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
