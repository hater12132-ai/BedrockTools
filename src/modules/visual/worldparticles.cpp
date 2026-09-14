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

typedef void (*Tessellator_begin_t)(void* tessellator, void* debugCallback, int primitiveMode, int vertexCount, int noIndices);
typedef void (*Tessellator_color_t)(void* tessellator, float r, float g, float b, float a);
typedef void (*Tessellator_vertex_t)(void* tessellator, float x, float y, float z);
typedef void (*MeshHelpers_renderMeshImmediately_t)(void* screenContext, void* tessellator, void* material, char* pad);

struct HashedString {
    uint64_t mStrHash;
    std::string mStr;
    mutable const HashedString* mLastMatch;

    HashedString() : mStrHash(0), mStr(), mLastMatch(nullptr) {}
    explicit HashedString(const char* str) : mLastMatch(nullptr) {
        mStr = str ? str : "";
        constexpr uint64_t kOffset = 0xCBF29CE484222325ULL;
        constexpr uint64_t kPrime = 0x100000001B3ULL;
        uint64_t hash = kOffset;
        for (size_t i = 0; i < mStr.size(); ++i)
            hash = static_cast<uint64_t>(static_cast<unsigned char>(mStr[i])) ^ (kPrime * hash);
        mStrHash = hash;
    }
};

struct MaterialPtr {
    void* sharedPtrData[2]{nullptr, nullptr};
    MaterialPtr() = default;
    MaterialPtr(const MaterialPtr&) = delete;
    MaterialPtr& operator=(const MaterialPtr&) = delete;
    MaterialPtr(MaterialPtr&& o) noexcept : sharedPtrData{o.sharedPtrData[0], o.sharedPtrData[1]} {
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
static uint32_t s_rngState = 0xC0FFEEu;

static Tessellator_begin_t s_tessBegin = nullptr;
static Tessellator_color_t s_tessColor = nullptr;
static Tessellator_vertex_t s_tessVertex = nullptr;
static MeshHelpers_renderMeshImmediately_t s_renderMesh = nullptr;
static MaterialPtr s_matSelection;
static uintptr_t s_renderMaterialGroup = 0;
static void (*s_renderLevelOrig)(void* self, void* screenContext, void* a3) = nullptr;

static float randf(float lo, float hi) {
    s_rngState = s_rngState * 1664525u + 1013904223u;
    const float t = static_cast<float>((s_rngState >> 8) & 0xFFFFFFu) / static_cast<float>(0xFFFFFFu);
    return lo + (hi - lo) * t;
}

static std::uint32_t parseColor(const std::string& value, std::uint32_t fallback = 0xFFFFFFFFu) {
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

// SoupVisuals-style preset tints per mode
static std::uint32_t modeColor(WorldParticlesModule::Mode mode, int style, float phase) {
    switch (mode) {
        case WorldParticlesModule::Mode::Hearts: return 0xFFFF4D7Au;
        case WorldParticlesModule::Mode::Bloom: return 0xFFFFC8E0u;
        case WorldParticlesModule::Mode::Blink: return 0xFFE0F0FFu;
        case WorldParticlesModule::Mode::Dollar: return 0xFF3DDC84u;
        case WorldParticlesModule::Mode::Flame: return 0xFFFF8A30u;
        case WorldParticlesModule::Mode::Snowflake: return 0xFFE8F6FFu;
        case WorldParticlesModule::Mode::Virus: return 0xFF7CFF4Au;
        case WorldParticlesModule::Mode::Firefly: {
            const float t = 0.5f + 0.5f * std::sin(phase * 3.0f);
            const std::uint8_t a = static_cast<std::uint8_t>(140.0f + 115.0f * t);
            return (static_cast<std::uint32_t>(a) << 24) | 0x00FFE45Au;
        }
        case WorldParticlesModule::Mode::Network: return 0xFF6AD4FFu;
        case WorldParticlesModule::Mode::Cube: return 0xFFB08CFFu;
        case WorldParticlesModule::Mode::Pyramid: return 0xFFFFB14Au;
        case WorldParticlesModule::Mode::Multi: {
            static const std::uint32_t palette[] = {
                0xFFFFFFFFu, 0xFFFF4D7Au, 0xFFFFC8E0u, 0xFF3DDC84u,
                0xFFFF8A30u, 0xFF6AD4FFu, 0xFFB08CFFu, 0xFFE8F6FFu
            };
            return palette[style % 8];
        }
        case WorldParticlesModule::Mode::Stars:
        default:
            return 0xFFFFF6C8u;
    }
}

// Soup AbstractParticle.initMotion(Fall / Fly / Emerge)
static void initMotion(WorldParticlesModule::Particle& p, WorldParticlesModule::Physics phys, float speedMul) {
    switch (phys) {
        case WorldParticlesModule::Physics::Fall:
            // motionX=0, motionY in [-0.2, -0.05], motionZ=0
            p.vx = 0.0f;
            p.vy = randf(-0.20f, -0.05f) * speedMul;
            p.vz = 0.0f;
            break;
        case WorldParticlesModule::Physics::Emerge:
            // XZ [-0.2,0.2], Y [0.4,0.7]
            p.vx = randf(-0.20f, 0.20f) * speedMul;
            p.vy = randf(0.40f, 0.70f) * speedMul;
            p.vz = randf(-0.20f, 0.20f) * speedMul;
            break;
        case WorldParticlesModule::Physics::Fly:
        default:
            // XZ [-0.4,0.4], Y [-0.1,0.1]
            p.vx = randf(-0.40f, 0.40f) * speedMul;
            p.vy = randf(-0.10f, 0.10f) * speedMul;
            p.vz = randf(-0.40f, 0.40f) * speedMul;
            break;
    }
}

static void spawnOne(WorldParticlesModule* mod, float px, float py, float pz, bool asFirefly) {
    WorldParticlesModule::Particle p;
    const float radius = std::max(2.0f, mod->spawnRadius);
    const float height = std::max(1.0f, mod->spawnHeight);

    // Soup-style: horizontal disc + vertical band around player
    const float ang = randf(0.0f, 6.2831853f);
    const float dist = std::sqrt(randf(0.0f, 1.0f)) * radius;
    p.x = px + std::cos(ang) * dist;
    p.z = pz + std::sin(ang) * dist;
    p.y = py + randf(-height * 0.25f, height);

    p.isFirefly = asFirefly || mod->mode == WorldParticlesModule::Mode::Firefly;

    WorldParticlesModule::Physics phys = mod->physics;
    if (p.isFirefly) phys = WorldParticlesModule::Physics::Fly;

    const float speedMul = std::max(0.15f, mod->speed);
    initMotion(p, phys, speedMul);

    if (p.isFirefly) {
        p.vx *= 0.35f;
        p.vy = randf(-0.03f, 0.05f) * speedMul;
        p.vz *= 0.35f;
        p.maxLife = randf(60.0f, 140.0f); // ticks
        p.size = mod->fireflyScale * randf(0.7f, 1.3f);
        p.trail.reserve(static_cast<size_t>(std::max(2, mod->trailLength)));
    } else {
        p.maxLife = randf(40.0f, 110.0f); // ticks
        p.size = mod->particleSize * randf(0.7f, 1.4f);
    }

    p.life = p.maxLife;
    p.phase = randf(0.0f, 6.28f);
    p.rot = randf(0.0f, 6.28f);
    p.rotSpeed = randf(-0.08f, 0.08f);
    p.style = static_cast<int>(randf(0.0f, 7.99f));
    p.color = modeColor(mod->mode, p.style, p.phase);
    if (mod->forceTint) p.color = parseColor(mod->colorHex, p.color);

    mod->particles.push_back(p);
}

static void tickParticles(void* player) {
    if (!g_mod || !g_mod->enabled || !player) return;

    auto* actor = reinterpret_cast<bedrocktools::sdk::Actor*>(player);
    const auto pos = actor->position();

    std::lock_guard<std::mutex> lock(g_mod->particlesMutex);
    auto& list = g_mod->particles;

    // Soup motion values are per game tick.
    for (size_t i = 0; i < list.size(); ++i) {
        auto& p = list[i];
        p.x += p.vx;
        p.y += p.vy;
        p.z += p.vz;
        p.phase += 0.12f;
        p.rot += p.rotSpeed;
        p.life -= 1.0f;

        if (p.isFirefly) {
            p.vx += std::sin(p.phase) * 0.008f;
            p.vz += std::cos(p.phase * 0.85f) * 0.008f;
            p.vx *= 0.96f;
            p.vz *= 0.96f;
            p.vy *= 0.98f;

            const int maxTrail = std::max(2, g_mod->trailLength);
            WorldParticlesModule::TrailPoint tp{p.x, p.y, p.z};
            p.trail.push_back(tp);
            if (static_cast<int>(p.trail.size()) > maxTrail) {
                p.trail.erase(p.trail.begin(),
                    p.trail.begin() + (static_cast<int>(p.trail.size()) - maxTrail));
            }
        } else if (g_mod->physics == WorldParticlesModule::Physics::Fall) {
            p.vy -= 0.003f;
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

    const bool fireflyMode = g_mod->mode == WorldParticlesModule::Mode::Firefly;
    const int regularTarget = fireflyMode ? 0 : std::clamp(g_mod->density, 0, 300);
    const int fireflyTarget = (fireflyMode || g_mod->mode == WorldParticlesModule::Mode::Multi)
        ? std::clamp(g_mod->fireflyCount, 0, 120)
        : 0;

    int regular = 0, flies = 0;
    for (const auto& p : list) {
        if (p.isFirefly) ++flies;
        else ++regular;
    }

    int budget = 12;
    while (regular < regularTarget && budget-- > 0) {
        spawnOne(g_mod, pos.x, pos.y, pos.z, false);
        ++regular;
    }
    budget = 8;
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

    // Count line segments: each particle = 2 lines (cross), firefly trail segments, network links
    int segments = static_cast<int>(snapshot.size()) * 2;
    for (const auto& p : snapshot) {
        if (p.trail.size() > 1) segments += static_cast<int>(p.trail.size()) - 1;
    }
    if (g_mod->mode == WorldParticlesModule::Mode::Network || g_mod->mode == WorldParticlesModule::Mode::Multi) {
        segments += static_cast<int>(snapshot.size()) * std::max(1, g_mod->maxLinks);
    }

    s_tessBegin(tessellator, nullptr, 4 /* lines */, segments * 2, 0);

    const float opacity = std::clamp(g_mod->opacity, 0.05f, 1.0f);
    const float linkDist = std::max(1.0f, g_mod->linkDistance);
    const float linkDistSq = linkDist * linkDist;
    const int maxLinks = std::max(0, g_mod->maxLinks);

    auto emitLine = [&](float x0, float y0, float z0, float x1, float y1, float z1) {
        s_tessVertex(tessellator, x0 - camX, y0 - camY, z0 - camZ);
        s_tessVertex(tessellator, x1 - camX, y1 - camY, z1 - camZ);
    };

    for (size_t i = 0; i < snapshot.size(); ++i) {
        const auto& p = snapshot[i];
        float lifeT = std::clamp(p.life / std::max(1.0f, p.maxLife), 0.0f, 1.0f);
        // Fade in first 10%, fade out last 25%
        float fade = 1.0f;
        if (lifeT > 0.9f) fade = (1.0f - lifeT) / 0.1f;
        else if (lifeT < 0.25f) fade = lifeT / 0.25f;
        float a = opacity * fade;
        if (p.isFirefly) a *= 0.5f + 0.5f * (0.5f + 0.5f * std::sin(p.phase * 3.0f));

        const float cr = ((p.color >> 16) & 0xFF) / 255.0f;
        const float cg = ((p.color >> 8) & 0xFF) / 255.0f;
        const float cb = (p.color & 0xFF) / 255.0f;
        s_tessColor(tessellator, cr, cg, cb, a);

        const float s = p.size;
        // Rotated cross for sparkle
        const float c = std::cos(p.rot);
        const float sn = std::sin(p.rot);
        emitLine(p.x - s * c, p.y, p.z - s * sn, p.x + s * c, p.y, p.z + s * sn);
        emitLine(p.x - s * sn, p.y - s * 0.6f, p.z + s * c, p.x + s * sn, p.y + s * 0.6f, p.z - s * c);

        // Firefly trail
        if (p.trail.size() > 1) {
            for (size_t t = 1; t < p.trail.size(); ++t) {
                const float ta = a * (static_cast<float>(t) / static_cast<float>(p.trail.size()));
                s_tessColor(tessellator, cr, cg, cb, ta * 0.7f);
                const auto& a0 = p.trail[t - 1];
                const auto& a1 = p.trail[t];
                emitLine(a0.x, a0.y, a0.z, a1.x, a1.y, a1.z);
            }
        }

        // Network links to nearby particles
        if ((g_mod->mode == WorldParticlesModule::Mode::Network || g_mod->mode == WorldParticlesModule::Mode::Multi) && maxLinks > 0) {
            int links = 0;
            for (size_t j = i + 1; j < snapshot.size() && links < maxLinks; ++j) {
                const auto& q = snapshot[j];
                const float dx = p.x - q.x, dy = p.y - q.y, dz = p.z - q.z;
                const float d2 = dx * dx + dy * dy + dz * dz;
                if (d2 <= linkDistSq) {
                    const float la = a * 0.35f * (1.0f - std::sqrt(d2) / linkDist);
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
    : Module("WorldParticles", "SoupVisuals-style ambient particles (stars, fireflies, network, fall/fly/emerge).") {
    g_mod = this;
}

WorldParticlesModule::~WorldParticlesModule() {
    if (g_mod == this) g_mod = nullptr;
}

void WorldParticlesModule::onInit() {
    uintptr_t addr = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::RenderLevel);
    if (addr != 0) m_patchTarget = reinterpret_cast<void*>(addr);

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
    auto readMode = [&](const char* key, int maxV, int& out) {
        if (!j.contains(key)) return;
        try {
            if (j[key].is_number_integer()) out = std::clamp(j[key].get<int>(), 0, maxV);
            else if (j[key].is_number_float()) out = std::clamp(static_cast<int>(j[key].get<float>()), 0, maxV);
            else {
                std::string v = j[key].get<std::string>();
                const auto c = v.find(',');
                if (c != std::string::npos) v.resize(c);
                out = std::clamp(std::stoi(v), 0, maxV);
            }
        } catch (...) {}
    };
    int m = static_cast<int>(mode), p = static_cast<int>(physics);
    readMode("mode", 12, m);
    readMode("physics", 2, p);
    mode = static_cast<Mode>(m);
    physics = static_cast<Physics>(p);

    if (j.contains("density")) {
        if (j["density"].is_number()) density = std::clamp(static_cast<int>(j["density"].get<float>()), 0, 300);
    }
    if (j.contains("spawnRadius")) spawnRadius = std::clamp(j["spawnRadius"].get<float>(), 2.0f, 64.0f);
    if (j.contains("radius")) spawnRadius = std::clamp(j["radius"].get<float>(), 2.0f, 64.0f); // legacy
    if (j.contains("spawnHeight")) spawnHeight = std::clamp(j["spawnHeight"].get<float>(), 1.0f, 32.0f);
    if (j.contains("particleSize")) particleSize = std::clamp(j["particleSize"].get<float>(), 0.02f, 1.5f);
    if (j.contains("opacity")) opacity = std::clamp(j["opacity"].get<float>(), 0.05f, 1.0f);
    if (j.contains("speed")) speed = std::clamp(j["speed"].get<float>(), 0.1f, 3.0f);
    if (j.contains("fallSpeed")) speed = std::clamp(j["fallSpeed"].get<float>(), 0.1f, 3.0f); // legacy
    if (j.contains("colorHex")) colorHex = j["colorHex"].get<std::string>();
    if (j.contains("forceTint")) forceTint = j["forceTint"].get<bool>();
    if (j.contains("fireflyCount")) fireflyCount = std::clamp(static_cast<int>(j["fireflyCount"].get<float>()), 0, 120);
    if (j.contains("fireflyScale")) fireflyScale = std::clamp(j["fireflyScale"].get<float>(), 0.05f, 1.5f);
    if (j.contains("trailLength")) trailLength = std::clamp(static_cast<int>(j["trailLength"].get<float>()), 2, 24);
    if (j.contains("linkDistance")) linkDistance = std::clamp(j["linkDistance"].get<float>(), 1.0f, 16.0f);
    if (j.contains("maxLinks")) maxLinks = std::clamp(static_cast<int>(j["maxLinks"].get<float>()), 0, 8);
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
}
