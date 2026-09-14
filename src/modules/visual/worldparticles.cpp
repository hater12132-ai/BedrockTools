#include "worldparticles.hpp"

#include "core/memory/Hooks.hpp"

#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Memory.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/world/Actor.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace {

typedef void (*Tessellator_begin_t)(void*, void*, int, int, int);
typedef void (*Tessellator_color_t)(void*, float, float, float, float);
typedef void (*Tessellator_vertex_t)(void*, float, float, float);
typedef void (*MeshHelpers_renderMeshImmediately_t)(void*, void*, void*, char*);

struct HashedString {
    uint64_t mStrHash = 0;
    std::string mStr;
    mutable const HashedString* mLastMatch = nullptr;
    HashedString() = default;
    explicit HashedString(const char* str) {
        mStr = str ? str : "";
        constexpr uint64_t kOffset = 0xCBF29CE484222325ULL;
        constexpr uint64_t kPrime = 0x100000001B3ULL;
        uint64_t hash = kOffset;
        for (unsigned char ch : mStr)
            hash = static_cast<uint64_t>(ch) ^ (kPrime * hash);
        mStrHash = hash;
    }
};

struct MaterialPtr {
    void* sharedPtrData[2]{nullptr, nullptr};
    MaterialPtr() = default;
    MaterialPtr(const MaterialPtr&) = delete;
    MaterialPtr& operator=(const MaterialPtr&) = delete;
    MaterialPtr(MaterialPtr&& o) noexcept {
        sharedPtrData[0] = o.sharedPtrData[0];
        sharedPtrData[1] = o.sharedPtrData[1];
        o.sharedPtrData[0] = o.sharedPtrData[1] = nullptr;
    }
    MaterialPtr& operator=(MaterialPtr&& o) noexcept {
        if (this != &o) {
            sharedPtrData[0] = o.sharedPtrData[0];
            sharedPtrData[1] = o.sharedPtrData[1];
            o.sharedPtrData[0] = o.sharedPtrData[1] = nullptr;
        }
        return *this;
    }
    ~MaterialPtr() {}
    explicit operator bool() const { return sharedPtrData[0] != nullptr; }
};

static WorldParticlesModule* g_mod = nullptr;
static uint32_t s_rng = 0x91A2B3C4u;

static Tessellator_begin_t s_tessBegin = nullptr;
static Tessellator_color_t s_tessColor = nullptr;
static Tessellator_vertex_t s_tessVertex = nullptr;
static MeshHelpers_renderMeshImmediately_t s_renderMesh = nullptr;
static MaterialPtr s_matSelection;
static uintptr_t s_renderMaterialGroup = 0;
static void (*s_renderLevelOrig)(void*, void*, void*) = nullptr;

static float randf(float lo, float hi) {
    s_rng = s_rng * 1664525u + 1013904223u;
    const float t = static_cast<float>((s_rng >> 8) & 0xFFFFFFu) / static_cast<float>(0xFFFFFFu);
    return lo + (hi - lo) * t;
}

static std::uint32_t parseColor(const std::string& value, std::uint32_t fallback) {
    if (value.empty()) return fallback;
    const std::string hex = value[0] == '#' ? value.substr(1) : value;
    try {
        if (hex.size() == 6) return 0xFF000000u | static_cast<std::uint32_t>(std::stoul(hex, nullptr, 16));
        if (hex.size() == 8) return static_cast<std::uint32_t>(std::stoul(hex, nullptr, 16));
    } catch (...) {}
    return fallback;
}

static uintptr_t resolveADRP(uint32_t* insns, size_t count, uint32_t targetReg) {
    for (size_t i = 0; i < count; i++) {
        uint32_t insn = insns[i];
        if ((insn & 0x1F) != targetReg) continue;
        if ((insn & 0x9F000000) == 0x90000000) {
            uintptr_t page = ((uintptr_t)&insns[i] & ~0xFFFULL)
                + ((int64_t)((uint64_t)((insn >> 3) & 0x1FFFFC | (insn >> 29) & 3) << 43) >> 31);
            for (size_t j = i + 1; j < count; j++) {
                uint32_t add = insns[j];
                if ((add & 0xFF000000) == 0x91000000 &&
                    ((add >> 5) & 0x1F) == targetReg &&
                    (add & 0x1F) == targetReg) {
                    uint32_t imm12 = (add >> 10) & 0xFFF;
                    if (add & 0x400000) imm12 <<= 12;
                    return page + imm12;
                }
                if ((add & 0x1F) == targetReg) break;
            }
        }
        if ((insn & 0x9F000000) == 0x10000000) {
            int64_t imm = (int64_t)((uint64_t)((insn >> 3) & 0x1FFFFC | (insn >> 29)) << 43) >> 43;
            return (uintptr_t)&insns[i] + imm;
        }
    }
    return 0;
}

static MaterialPtr getMaterial(const char* name) {
    if (!s_renderMaterialGroup) return {};
    HashedString hs(name);
    void** vtable = *reinterpret_cast<void***>(s_renderMaterialGroup);
    if (!vtable || !vtable[2]) return {};
    using getMat_t = MaterialPtr(*)(void*, const HashedString*);
    return reinterpret_cast<getMat_t>(vtable[2])((void*)s_renderMaterialGroup, &hs);
}

static void ensureMaterials() {
    if (s_matSelection || !s_renderMaterialGroup) return;
    s_matSelection = getMaterial("selection_box");
}

static std::uint32_t modeColor(WorldParticlesModule::Mode mode, int style, float phase) {
    switch (mode) {
        case WorldParticlesModule::Mode::Hearts:     return 0xFFFF3D6Eu;
        case WorldParticlesModule::Mode::Bloom:      return 0xFFFFB8D4u;
        case WorldParticlesModule::Mode::Blink:      return 0xFFF0F8FFu;
        case WorldParticlesModule::Mode::Dollar:     return 0xFF2EE86Au;
        case WorldParticlesModule::Mode::Flame:      return 0xFFFF7A1Au;
        case WorldParticlesModule::Mode::Snowflake:  return 0xFFDCF4FFu;
        case WorldParticlesModule::Mode::Virus:      return 0xFF6CFF3Au;
        case WorldParticlesModule::Mode::Firefly: {
            // Warm yellow-green pulse
            const float t = 0.55f + 0.45f * std::sin(phase * 2.7f);
            const std::uint8_t a = static_cast<std::uint8_t>(160.0f + 95.0f * t);
            return (static_cast<std::uint32_t>(a) << 24) | 0x00FFE84Au;
        }
        case WorldParticlesModule::Mode::Network:    return 0xFF5CC8FFu;
        case WorldParticlesModule::Mode::Cube:       return 0xFFB08CFFu;
        case WorldParticlesModule::Mode::Pyramid:    return 0xFFFFA83Au;
        case WorldParticlesModule::Mode::Multi: {
            static const std::uint32_t pal[] = {
                0xFFFFF4B0u, 0xFFFF3D6Eu, 0xFFFFB8D4u, 0xFF2EE86Au,
                0xFFFF7A1Au, 0xFF5CC8FFu, 0xFFB08CFFu, 0xFFDCF4FFu
            };
            return pal[style & 7];
        }
        case WorldParticlesModule::Mode::Stars:
        default:
            return 0xFFFFF2A8u;
    }
}

// Soup AbstractParticle.initMotion
static void initMotion(WorldParticlesModule::Particle& p, WorldParticlesModule::Physics phys, float speedMul) {
    switch (phys) {
        case WorldParticlesModule::Physics::Fall:
            p.vx = 0.0f;
            p.vy = randf(-0.18f, -0.04f) * speedMul;
            p.vz = 0.0f;
            break;
        case WorldParticlesModule::Physics::Emerge:
            p.vx = randf(-0.18f, 0.18f) * speedMul;
            p.vy = randf(0.35f, 0.65f) * speedMul;
            p.vz = randf(-0.18f, 0.18f) * speedMul;
            break;
        case WorldParticlesModule::Physics::Fly:
        default:
            p.vx = randf(-0.35f, 0.35f) * speedMul;
            p.vy = randf(-0.08f, 0.08f) * speedMul;
            p.vz = randf(-0.35f, 0.35f) * speedMul;
            break;
    }
}

static void spawnOne(WorldParticlesModule* mod, float px, float py, float pz, bool asFirefly) {
    WorldParticlesModule::Particle p;
    const float radius = std::max(2.0f, mod->spawnRadius);
    const float height = std::max(1.0f, mod->spawnHeight);

    // Uniform disc around player
    const float ang = randf(0.0f, 6.2831853f);
    const float dist = std::sqrt(randf(0.15f, 1.0f)) * radius;
    p.x = px + std::cos(ang) * dist;
    p.z = pz + std::sin(ang) * dist;
    p.y = py + randf(-height * 0.2f, height);

    p.isFirefly = asFirefly || mod->mode == WorldParticlesModule::Mode::Firefly;

    WorldParticlesModule::Physics phys = mod->physics;
    if (p.isFirefly) phys = WorldParticlesModule::Physics::Fly;

    const float speedMul = std::max(0.2f, mod->speed);
    initMotion(p, phys, speedMul);

    if (p.isFirefly) {
        // Gentle wander — glowing dots that slowly float
        p.vx = randf(-0.12f, 0.12f) * speedMul;
        p.vy = randf(-0.04f, 0.06f) * speedMul;
        p.vz = randf(-0.12f, 0.12f) * speedMul;
        p.maxLife = randf(80.0f, 160.0f);
        p.size = mod->fireflyScale * randf(0.85f, 1.25f);
        p.trail.reserve(static_cast<size_t>(std::max(3, mod->trailLength)));
    } else {
        p.maxLife = randf(45.0f, 120.0f);
        p.size = mod->particleSize * randf(0.75f, 1.35f);
    }

    p.life = p.maxLife;
    p.phase = randf(0.0f, 6.28f);
    p.rot = randf(0.0f, 6.28f);
    p.rotSpeed = randf(-0.12f, 0.12f);
    p.style = static_cast<int>(randf(0.0f, 7.99f));
    p.color = modeColor(mod->mode, p.style, p.phase);
    if (mod->forceTint) p.color = parseColor(mod->colorHex, p.color);

    mod->particles.push_back(std::move(p));
}

static void tickParticles(void* player) {
    if (!g_mod || !g_mod->enabled || !player) return;

    auto* actor = reinterpret_cast<bedrocktools::sdk::Actor*>(player);
    const auto pos = actor->position();

    std::lock_guard<std::mutex> lock(g_mod->particlesMutex);
    auto& list = g_mod->particles;

    for (size_t i = 0; i < list.size(); ++i) {
        auto& p = list[i];
        p.x += p.vx;
        p.y += p.vy;
        p.z += p.vz;
        p.phase += 0.15f;
        p.rot += p.rotSpeed;
        p.life -= 1.0f;

        if (p.isFirefly) {
            // Organic wandering (Soup firefly feel)
            p.vx += std::sin(p.phase * 1.3f) * 0.012f;
            p.vz += std::cos(p.phase * 0.9f) * 0.012f;
            p.vy += std::sin(p.phase * 0.55f + 1.7f) * 0.006f;
            p.vx *= 0.94f;
            p.vy *= 0.95f;
            p.vz *= 0.94f;
            // Soft speed clamp so they never zip
            const float spd = std::sqrt(p.vx * p.vx + p.vy * p.vy + p.vz * p.vz);
            if (spd > 0.22f) {
                const float s = 0.22f / spd;
                p.vx *= s; p.vy *= s; p.vz *= s;
            }

            const int maxTrail = std::max(3, g_mod->trailLength);
            p.trail.push_back({p.x, p.y, p.z});
            if (static_cast<int>(p.trail.size()) > maxTrail) {
                p.trail.erase(p.trail.begin(),
                    p.trail.begin() + (static_cast<int>(p.trail.size()) - maxTrail));
            }
        } else if (g_mod->physics == WorldParticlesModule::Physics::Fall) {
            p.vy -= 0.004f;
        } else if (g_mod->physics == WorldParticlesModule::Physics::Fly) {
            p.vx *= 0.995f;
            p.vz *= 0.995f;
        }
    }

    size_t write = 0;
    for (size_t i = 0; i < list.size(); ++i) {
        if (list[i].life > 0.0f) {
            if (write != i) list[write] = std::move(list[i]);
            ++write;
        }
    }
    list.resize(write);

    const bool fireflyOnly = g_mod->mode == WorldParticlesModule::Mode::Firefly;
    const int regularTarget = fireflyOnly ? 0 : std::clamp(g_mod->density, 0, 250);
    const int fireflyTarget =
        (fireflyOnly || g_mod->mode == WorldParticlesModule::Mode::Multi)
            ? std::clamp(g_mod->fireflyCount, 0, 100)
            : 0;

    int regular = 0, flies = 0;
    for (const auto& p : list) {
        if (p.isFirefly) ++flies;
        else ++regular;
    }

    int budget = 14;
    while (regular < regularTarget && budget-- > 0) {
        spawnOne(g_mod, pos.x, pos.y, pos.z, false);
        ++regular;
    }
    budget = 10;
    while (flies < fireflyTarget && budget-- > 0) {
        spawnOne(g_mod, pos.x, pos.y, pos.z, true);
        ++flies;
    }
}

static void renderLevelHook(void* self, void* screenContext, void* a3) {
    if (s_renderLevelOrig) s_renderLevelOrig(self, screenContext, a3);

    if (!g_mod || !g_mod->enabled) return;
    if (!s_tessBegin || !s_tessColor || !s_tessVertex || !s_renderMesh) return;
    if (!screenContext || reinterpret_cast<uintptr_t>(screenContext) < 0x1000) return;

    std::vector<WorldParticlesModule::Particle> snapshot;
    {
        std::lock_guard<std::mutex> lock(g_mod->particlesMutex);
        snapshot = g_mod->particles;
    }
    if (snapshot.empty()) return;

    uintptr_t tessellatorPtr = *reinterpret_cast<uintptr_t*>(
        reinterpret_cast<uintptr_t>(screenContext) + bedrocktools::sdk::offsets::ScreenContext::mTessellator);
    if (!tessellatorPtr || tessellatorPtr < 0x1000) return;
    void* tessellator = reinterpret_cast<void*>(tessellatorPtr);

    uintptr_t lrpPtr = *reinterpret_cast<uintptr_t*>(
        reinterpret_cast<uintptr_t>(self) + bedrocktools::sdk::offsets::LevelRenderer::mLevelRendererPlayer);
    if (!lrpPtr || lrpPtr < 0x1000) return;

    const float camX = *reinterpret_cast<float*>(lrpPtr + bedrocktools::sdk::offsets::LevelRendererPlayer::mCamPos);
    const float camY = *reinterpret_cast<float*>(lrpPtr + bedrocktools::sdk::offsets::LevelRendererPlayer::mCamPos + 4);
    const float camZ = *reinterpret_cast<float*>(lrpPtr + bedrocktools::sdk::offsets::LevelRendererPlayer::mCamPos + 8);

    ensureMaterials();
    void* mat = s_matSelection
        ? reinterpret_cast<void*>(&s_matSelection)
        : reinterpret_cast<void*>(lrpPtr + bedrocktools::sdk::offsets::LevelRendererPlayer::mSelectionOverlayMaterial);

    uintptr_t colorHolderPtr = *reinterpret_cast<uintptr_t*>(
        reinterpret_cast<uintptr_t>(screenContext) + bedrocktools::sdk::offsets::ScreenContext::mColorHolder);
    if (!colorHolderPtr || colorHolderPtr < 0x1000) return;
    float* colorHolder = reinterpret_cast<float*>(colorHolderPtr);
    float saved[4] = {colorHolder[0], colorHolder[1], colorHolder[2], colorHolder[3]};
    colorHolder[0] = colorHolder[1] = colorHolder[2] = colorHolder[3] = 1.0f;

    // Budget: glowing dot = several radial lines (core + glow rings) + trails + network
    // 8 spokes * 3 rings = 24 lines per particle worst case
    int segs = static_cast<int>(snapshot.size()) * 28;
    for (const auto& p : snapshot)
        if (p.trail.size() > 1) segs += static_cast<int>(p.trail.size()) - 1;
    if (g_mod->mode == WorldParticlesModule::Mode::Network || g_mod->mode == WorldParticlesModule::Mode::Multi)
        segs += static_cast<int>(snapshot.size()) * std::max(1, g_mod->maxLinks);

    s_tessBegin(tessellator, nullptr, 4, segs * 2, 0);

    const float opacity = std::clamp(g_mod->opacity, 0.05f, 1.0f);
    const float glow = std::clamp(g_mod->glowStrength, 0.0f, 1.5f);
    const float linkDist = std::max(1.0f, g_mod->linkDistance);
    const float linkDistSq = linkDist * linkDist;
    const int maxLinks = std::max(0, g_mod->maxLinks);

    auto emitLine = [&](float x0, float y0, float z0, float x1, float y1, float z1) {
        s_tessVertex(tessellator, x0 - camX, y0 - camY, z0 - camZ);
        s_tessVertex(tessellator, x1 - camX, y1 - camY, z1 - camZ);
    };

    // Draw a soft glowing disc approximated by radial spokes + rings
    auto emitGlowDot = [&](float x, float y, float z, float size, float cr, float cg, float cb, float a, float pulse) {
        const float core = size * (0.35f + 0.15f * pulse);
        const float mid  = size * (0.85f + 0.25f * pulse);
        const float outer = size * (1.6f + 0.5f * pulse) * (0.7f + 0.3f * glow);

        // Bright core cross
        s_tessColor(tessellator, 1.0f, 1.0f, 1.0f, a * 0.95f);
        emitLine(x - core, y, z, x + core, y, z);
        emitLine(x, y - core, z, x, y + core, z);
        emitLine(x - core * 0.7f, y, z - core * 0.7f, x + core * 0.7f, y, z + core * 0.7f);
        emitLine(x - core * 0.7f, y, z + core * 0.7f, x + core * 0.7f, y, z - core * 0.7f);

        // Mid ring (tinted)
        s_tessColor(tessellator, cr, cg, cb, a * 0.55f * glow);
        constexpr int spokes = 8;
        for (int i = 0; i < spokes; ++i) {
            const float a0 = (6.2831853f * static_cast<float>(i)) / static_cast<float>(spokes);
            const float a1 = (6.2831853f * static_cast<float>(i + 1)) / static_cast<float>(spokes);
            emitLine(x + std::cos(a0) * mid, y + std::sin(a0) * mid * 0.35f, z + std::sin(a0) * mid,
                     x + std::cos(a1) * mid, y + std::sin(a1) * mid * 0.35f, z + std::sin(a1) * mid);
        }

        // Soft outer glow
        s_tessColor(tessellator, cr, cg, cb, a * 0.22f * glow);
        for (int i = 0; i < spokes; ++i) {
            const float ang = (6.2831853f * static_cast<float>(i)) / static_cast<float>(spokes);
            const float cx = std::cos(ang), cz = std::sin(ang);
            emitLine(x, y, z, x + cx * outer, y, z + cz * outer);
            emitLine(x, y, z, x, y + std::sin(ang) * outer * 0.55f, z);
        }
    };

    for (size_t i = 0; i < snapshot.size(); ++i) {
        const auto& p = snapshot[i];
        const float lifeT = std::clamp(p.life / std::max(1.0f, p.maxLife), 0.0f, 1.0f);
        float fade = 1.0f;
        if (lifeT > 0.85f) fade = (1.0f - lifeT) / 0.15f;
        else if (lifeT < 0.15f) fade = lifeT / 0.15f;

        const float pulse = p.isFirefly
            ? (0.55f + 0.45f * std::sin(p.phase * 2.7f))
            : (0.85f + 0.15f * std::sin(p.phase));

        float a = opacity * fade * (p.isFirefly ? (0.65f + 0.35f * pulse) : 1.0f);

        const float cr = ((p.color >> 16) & 0xFF) / 255.0f;
        const float cg = ((p.color >> 8) & 0xFF) / 255.0f;
        const float cb = (p.color & 0xFF) / 255.0f;

        if (p.isFirefly) {
            // Glowing flying dot
            emitGlowDot(p.x, p.y, p.z, p.size, cr, cg, cb, a, pulse);

            // Smooth trail
            if (p.trail.size() > 1) {
                for (size_t t = 1; t < p.trail.size(); ++t) {
                    const float ta = a * (static_cast<float>(t) / static_cast<float>(p.trail.size())) * 0.55f;
                    s_tessColor(tessellator, cr, cg, cb, ta);
                    const auto& a0 = p.trail[t - 1];
                    const auto& a1 = p.trail[t];
                    emitLine(a0.x, a0.y, a0.z, a1.x, a1.y, a1.z);
                }
            }
        } else {
            // Regular ambient sparkle — rotated diamond / star
            const float s = p.size * (0.7f + 0.3f * pulse);
            const float c = std::cos(p.rot);
            const float sn = std::sin(p.rot);

            s_tessColor(tessellator, cr, cg, cb, a);
            emitLine(p.x - s * c, p.y, p.z - s * sn, p.x + s * c, p.y, p.z + s * sn);
            emitLine(p.x - s * sn, p.y - s * 0.65f, p.z + s * c,
                     p.x + s * sn, p.y + s * 0.65f, p.z - s * c);

            // Soft glow halo
            s_tessColor(tessellator, cr, cg, cb, a * 0.25f * glow);
            const float hs = s * 1.8f;
            emitLine(p.x - hs * c, p.y, p.z - hs * sn, p.x + hs * c, p.y, p.z + hs * sn);
            emitLine(p.x - hs * sn, p.y, p.z + hs * c, p.x + hs * sn, p.y, p.z - hs * c);

            // Mode accents
            if (g_mod->mode == WorldParticlesModule::Mode::Hearts ||
                g_mod->mode == WorldParticlesModule::Mode::Bloom) {
                s_tessColor(tessellator, cr, cg, cb, a * 0.5f);
                emitLine(p.x, p.y - s * 0.4f, p.z, p.x - s * 0.55f, p.y + s * 0.25f, p.z);
                emitLine(p.x, p.y - s * 0.4f, p.z, p.x + s * 0.55f, p.y + s * 0.25f, p.z);
            } else if (g_mod->mode == WorldParticlesModule::Mode::Cube ||
                       g_mod->mode == WorldParticlesModule::Mode::Pyramid) {
                s_tessColor(tessellator, cr, cg, cb, a * 0.7f);
                const float q = s * 0.7f;
                emitLine(p.x - q, p.y - q, p.z, p.x + q, p.y - q, p.z);
                emitLine(p.x + q, p.y - q, p.z, p.x + q, p.y + q, p.z);
                emitLine(p.x + q, p.y + q, p.z, p.x - q, p.y + q, p.z);
                emitLine(p.x - q, p.y + q, p.z, p.x - q, p.y - q, p.z);
            } else if (g_mod->mode == WorldParticlesModule::Mode::Snowflake) {
                s_tessColor(tessellator, cr, cg, cb, a * 0.65f);
                for (int k = 0; k < 6; ++k) {
                    const float ang = p.rot + (6.2831853f * static_cast<float>(k)) / 6.0f;
                    emitLine(p.x, p.y, p.z,
                             p.x + std::cos(ang) * s, p.y, p.z + std::sin(ang) * s);
                }
            }
        }

        // Network links
        if ((g_mod->mode == WorldParticlesModule::Mode::Network ||
             g_mod->mode == WorldParticlesModule::Mode::Multi) && maxLinks > 0) {
            int links = 0;
            for (size_t j = i + 1; j < snapshot.size() && links < maxLinks; ++j) {
                const auto& q = snapshot[j];
                const float dx = p.x - q.x, dy = p.y - q.y, dz = p.z - q.z;
                const float d2 = dx * dx + dy * dy + dz * dz;
                if (d2 <= linkDistSq) {
                    const float la = a * 0.28f * (1.0f - std::sqrt(d2) / linkDist);
                    s_tessColor(tessellator, cr, cg, cb, la);
                    emitLine(p.x, p.y, p.z, q.x, q.y, q.z);
                    ++links;
                }
            }
        }
    }

    char pad[0x58];
    std::memset(pad, 0, sizeof(pad));
    s_renderMesh(screenContext, tessellator, mat, pad);

    colorHolder[0] = saved[0];
    colorHolder[1] = saved[1];
    colorHolder[2] = saved[2];
    colorHolder[3] = saved[3];
}

} // namespace

WorldParticlesModule::WorldParticlesModule()
    : Module("AmbientParticles", "SoupVisuals-style ambient particles — glowing fireflies, stars, network, bloom.") {
    g_mod = this;
}

WorldParticlesModule::~WorldParticlesModule() {
    if (g_mod == this) g_mod = nullptr;
}

void WorldParticlesModule::onInit() {
    if (auto addr = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::RenderLevel))
        m_patchTarget = reinterpret_cast<void*>(addr);

    if (auto tb = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::TessellatorBegin)) {
        m_tessBeginAddr = reinterpret_cast<void*>(tb);
        s_tessBegin = reinterpret_cast<Tessellator_begin_t>(tb);
    }
    if (auto tc = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::TessellatorColor)) {
        m_tessColorAddr = reinterpret_cast<void*>(tc);
        s_tessColor = reinterpret_cast<Tessellator_color_t>(tc);
    }
    if (auto tv = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::TessellatorVertex)) {
        m_tessVertexAddr = reinterpret_cast<void*>(tv);
        s_tessVertex = reinterpret_cast<Tessellator_vertex_t>(tv);
    }
    if (auto rm = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::MeshHelpersRenderMeshImmediately2)) {
        m_renderMeshAddr = reinterpret_cast<void*>(rm);
        s_renderMesh = reinterpret_cast<MeshHelpers_renderMeshImmediately_t>(rm);
    } else if (auto rm5 = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::MeshHelpersRenderMeshImmediately)) {
        s_renderMesh = reinterpret_cast<MeshHelpers_renderMeshImmediately_t>(rm5);
    }
    if (auto rmg = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::RenderMaterialGroupCommon)) {
        m_renderMaterialGroupAddr = reinterpret_cast<void*>(rmg);
        uintptr_t groupAddr = resolveADRP(reinterpret_cast<uint32_t*>(rmg), 2, 0);
        if (groupAddr)
            s_renderMaterialGroup = groupAddr + bedrocktools::sdk::offsets::MaterialGroup::mRenderMaterialGroupOffset;
    }

    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>(
        [](auto& event) { tickParticles(event.player); });
}

void WorldParticlesModule::applyPatch() {
    if (m_patched || !m_patchTarget) return;
    bedrocktools::hooks::install(m_patchTarget, reinterpret_cast<void*>(renderLevelHook),
                                 reinterpret_cast<void**>(&s_renderLevelOrig));
    m_patched = true;
}

void WorldParticlesModule::onEnable() { applyPatch(); }

void WorldParticlesModule::onDisable() {
    std::lock_guard<std::mutex> lock(particlesMutex);
    particles.clear();
}

void WorldParticlesModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    auto readChoice = [&](const char* key, int maxV) -> int {
        if (!j.contains(key)) return -1;
        try {
            if (j[key].is_number_integer()) return std::clamp(j[key].get<int>(), 0, maxV);
            if (j[key].is_number_float()) return std::clamp(static_cast<int>(j[key].get<float>()), 0, maxV);
            std::string v = j[key].get<std::string>();
            const auto c = v.find(',');
            if (c != std::string::npos) v.resize(c);
            return std::clamp(std::stoi(v), 0, maxV);
        } catch (...) { return -1; }
    };
    if (int m = readChoice("mode", 12); m >= 0) mode = static_cast<Mode>(m);
    if (int p = readChoice("physics", 2); p >= 0) physics = static_cast<Physics>(p);

    if (j.contains("density")) density = std::clamp(static_cast<int>(j["density"].get<float>()), 0, 250);
    if (j.contains("spawnRadius")) spawnRadius = std::clamp(j["spawnRadius"].get<float>(), 2.0f, 64.0f);
    if (j.contains("radius")) spawnRadius = std::clamp(j["radius"].get<float>(), 2.0f, 64.0f);
    if (j.contains("spawnHeight")) spawnHeight = std::clamp(j["spawnHeight"].get<float>(), 1.0f, 32.0f);
    if (j.contains("particleSize")) particleSize = std::clamp(j["particleSize"].get<float>(), 0.02f, 1.5f);
    if (j.contains("opacity")) opacity = std::clamp(j["opacity"].get<float>(), 0.05f, 1.0f);
    if (j.contains("speed")) speed = std::clamp(j["speed"].get<float>(), 0.1f, 3.0f);
    if (j.contains("fallSpeed")) speed = std::clamp(j["fallSpeed"].get<float>(), 0.1f, 3.0f);
    if (j.contains("colorHex")) colorHex = j["colorHex"].get<std::string>();
    if (j.contains("forceTint")) forceTint = j["forceTint"].get<bool>();
    if (j.contains("fireflyCount")) fireflyCount = std::clamp(static_cast<int>(j["fireflyCount"].get<float>()), 0, 100);
    if (j.contains("fireflyScale")) fireflyScale = std::clamp(j["fireflyScale"].get<float>(), 0.05f, 1.5f);
    if (j.contains("trailLength")) trailLength = std::clamp(static_cast<int>(j["trailLength"].get<float>()), 3, 32);
    if (j.contains("linkDistance")) linkDistance = std::clamp(j["linkDistance"].get<float>(), 1.0f, 16.0f);
    if (j.contains("maxLinks")) maxLinks = std::clamp(static_cast<int>(j["maxLinks"].get<float>()), 0, 8);
    if (j.contains("glowStrength")) glowStrength = std::clamp(j["glowStrength"].get<float>(), 0.0f, 1.5f);
}

void WorldParticlesModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["mode"] = std::to_string(static_cast<int>(mode)) + ",Stars,Hearts,Bloom,Blink,Dollar,Flame,Snowflake,Virus,Firefly,Network,Cube,Pyramid,Multi";
    j["physics"] = std::to_string(static_cast<int>(physics)) + ",Fall,Fly,Emerge";
    j["density"] = density;
    j["spawnRadius"] = spawnRadius;
    j["spawnHeight"] = spawnHeight;
    j["particleSize"] = particleSize;
    j["opacity"] = opacity;
    j["speed"] = speed;
    j["colorHex"] = colorHex;
    j["forceTint"] = forceTint;
    j["fireflyCount"] = fireflyCount;
    j["fireflyScale"] = fireflyScale;
    j["trailLength"] = trailLength;
    j["linkDistance"] = linkDistance;
    j["maxLinks"] = maxLinks;
    j["glowStrength"] = glowStrength;
}
