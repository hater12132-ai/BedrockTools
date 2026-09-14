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
        mStrHash = computeHash(mStr);
    }

private:
    static uint64_t computeHash(const std::string& str) {
        if (str.empty()) return 0;
        constexpr uint64_t kOffset = 0xCBF29CE484222325ULL;
        constexpr uint64_t kPrime = 0x100000001B3ULL;
        uint64_t hash = kOffset;
        for (size_t i = 0; i < str.size(); ++i) {
            hash = static_cast<uint64_t>(static_cast<unsigned char>(str[i])) ^ (kPrime * hash);
        }
        return hash;
    }
};

struct MaterialPtr {
    void* sharedPtrData[2]{nullptr, nullptr};

    MaterialPtr() = default;
    MaterialPtr(const MaterialPtr&) = delete;
    MaterialPtr& operator=(const MaterialPtr&) = delete;

    MaterialPtr(MaterialPtr&& other) noexcept
        : sharedPtrData{other.sharedPtrData[0], other.sharedPtrData[1]} {
        other.sharedPtrData[0] = nullptr;
        other.sharedPtrData[1] = nullptr;
    }

    MaterialPtr& operator=(MaterialPtr&& other) noexcept {
        if (this != &other) {
            sharedPtrData[0] = other.sharedPtrData[0];
            sharedPtrData[1] = other.sharedPtrData[1];
            other.sharedPtrData[0] = nullptr;
            other.sharedPtrData[1] = nullptr;
        }
        return *this;
    }

    ~MaterialPtr() {}

    explicit operator bool() const {
        return sharedPtrData[0] != nullptr;
    }
};

static WorldParticlesModule* g_mod = nullptr;
static uint32_t s_rngState = 0xA341316Cu;

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
    } catch (...) {
    }
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
    if (s_matSelection) return;
    if (!s_renderMaterialGroup) return;
    if (!s_matSelection) s_matSelection = getMaterial("selection_box");
}

static std::uint32_t styleColor(WorldParticlesModule::Mode mode, int style, float phase) {
    switch (mode) {
        case WorldParticlesModule::Mode::Hearts: return 0xFFFF5A8Au;
        case WorldParticlesModule::Mode::Stars: return 0xFFFFF2AAu;
        case WorldParticlesModule::Mode::Orbs: return 0xFF7AD7FFu;
        case WorldParticlesModule::Mode::Storm: return 0xFFB0C4DEu;
        case WorldParticlesModule::Mode::Snowflake: return 0xFFE8F6FFu;
        case WorldParticlesModule::Mode::Dollar: return 0xFF40E070u;
        case WorldParticlesModule::Mode::Pumpkin: return 0xFFFF8C20u;
        case WorldParticlesModule::Mode::Glowfly: {
            const float t = 0.5f + 0.5f * std::sin(phase);
            const std::uint8_t a = static_cast<std::uint8_t>(120.0f + 135.0f * t);
            return (static_cast<std::uint32_t>(a) << 24) | 0x00FFE060u;
        }
        case WorldParticlesModule::Mode::Multi: {
            static const std::uint32_t palette[] = {
                0xFFFFFFFFu, 0xFFFF5A8Au, 0xFFFFF2AAu, 0xFF7AD7FFu,
                0xFF40E070u, 0xFFFF8C20u, 0xFFB0C4DEu
            };
            return palette[style % 7];
        }
        case WorldParticlesModule::Mode::Snow:
        default:
            return 0xFFE8F4FFu;
    }
}

static void spawnParticle(WorldParticlesModule* mod, float px, float py, float pz) {
    WorldParticlesModule::Particle p;
    const float r = std::max(2.0f, mod->radius);
    p.x = px + randf(-r, r);
    p.z = pz + randf(-r, r);
    p.y = py + randf(1.0f, r * 0.75f);

    const float wind = mod->wind;
    const float speed = std::max(0.05f, mod->fallSpeed);

    switch (mod->mode) {
        case WorldParticlesModule::Mode::Glowfly:
            p.vx = randf(-0.04f, 0.04f) * wind;
            p.vy = randf(-0.02f, 0.05f);
            p.vz = randf(-0.04f, 0.04f) * wind;
            p.maxLife = randf(2.5f, 6.0f);
            p.size = mod->particleSize * randf(0.6f, 1.4f);
            break;
        case WorldParticlesModule::Mode::Orbs:
            p.vx = randf(-0.03f, 0.03f);
            p.vy = randf(0.01f, 0.06f);
            p.vz = randf(-0.03f, 0.03f);
            p.maxLife = randf(2.0f, 5.0f);
            p.size = mod->particleSize * randf(0.8f, 1.6f);
            break;
        case WorldParticlesModule::Mode::Storm:
            p.vx = randf(-0.15f, 0.15f) * wind;
            p.vy = -randf(0.12f, 0.28f) * speed;
            p.vz = randf(-0.15f, 0.15f) * wind;
            p.maxLife = randf(1.0f, 2.5f);
            p.size = mod->particleSize * randf(0.5f, 1.1f);
            break;
        case WorldParticlesModule::Mode::Stars:
            p.vx = randf(-0.02f, 0.02f);
            p.vy = -randf(0.01f, 0.05f) * speed;
            p.vz = randf(-0.02f, 0.02f);
            p.maxLife = randf(1.5f, 4.0f);
            p.size = mod->particleSize * randf(0.5f, 1.2f);
            break;
        default:
            p.vx = randf(-0.06f, 0.06f) * wind;
            p.vy = -randf(0.04f, 0.12f) * speed;
            p.vz = randf(-0.06f, 0.06f) * wind;
            p.maxLife = randf(1.5f, 4.0f);
            p.size = mod->particleSize * randf(0.7f, 1.3f);
            break;
    }

    p.life = p.maxLife;
    p.phase = randf(0.0f, 6.28f);
    p.style = static_cast<int>(randf(0.0f, 6.99f));
    p.color = styleColor(mod->mode, p.style, p.phase);
    if (mod->forceTint) {
        p.color = parseColor(mod->colorHex, p.color);
    }
    mod->particles.push_back(p);
}

static void tickParticles(void* player) {
    if (!g_mod || !g_mod->enabled || !player) return;

    auto* actor = reinterpret_cast<bedrocktools::sdk::Actor*>(player);
    const bedrocktools::sdk::Vec3 pos = actor->position();

    std::lock_guard<std::mutex> lock(g_mod->particlesMutex);
    auto& list = g_mod->particles;

    for (size_t i = 0; i < list.size(); ++i) {
        auto& p = list[i];
        p.x += p.vx;
        p.y += p.vy;
        p.z += p.vz;
        p.phase += 0.08f;
        p.life -= 0.05f;

        if (g_mod->mode == WorldParticlesModule::Mode::Glowfly ||
            g_mod->mode == WorldParticlesModule::Mode::Orbs) {
            p.vx += std::sin(p.phase) * 0.002f;
            p.vz += std::cos(p.phase * 0.7f) * 0.002f;
            p.vx *= 0.98f;
            p.vz *= 0.98f;
        }
    }

    size_t write = 0;
    for (size_t i = 0; i < list.size(); ++i) {
        if (list[i].life > 0.0f) {
            if (write != i) list[write] = list[i];
            ++write;
        }
    }
    list.resize(write);

    const int target = std::clamp(g_mod->density, 4, 400);
    int spawnBudget = std::max(1, target / 12);
    while (static_cast<int>(list.size()) < target && spawnBudget-- > 0) {
        spawnParticle(g_mod, pos.x, pos.y, pos.z);
    }
}

static void renderLevelHook(void* self, void* screenContext, void* a3) {
    if (s_renderLevelOrig) {
        s_renderLevelOrig(self, screenContext, a3);
    }

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
    colorHolder[0] = 1.0f;
    colorHolder[1] = 1.0f;
    colorHolder[2] = 1.0f;
    colorHolder[3] = 1.0f;

    const int verts = static_cast<int>(snapshot.size()) * 4;
    s_tessBegin(tessellator, nullptr, 4, verts, 0);

    const float opacity = std::clamp(g_mod->opacity, 0.05f, 1.0f);
    for (size_t i = 0; i < snapshot.size(); ++i) {
        const WorldParticlesModule::Particle& p = snapshot[i];
        float lifeT = std::clamp(p.life / std::max(0.01f, p.maxLife), 0.0f, 1.0f);
        float a = opacity * lifeT;
        if (g_mod->mode == WorldParticlesModule::Mode::Glowfly) {
            a *= 0.45f + 0.55f * (0.5f + 0.5f * std::sin(p.phase * 2.0f));
        }

        const float cr = ((p.color >> 16) & 0xFF) / 255.0f;
        const float cg = ((p.color >> 8) & 0xFF) / 255.0f;
        const float cb = (p.color & 0xFF) / 255.0f;
        s_tessColor(tessellator, cr, cg, cb, a);

        const float s = p.size;
        const float x = p.x - camX;
        const float y = p.y - camY;
        const float z = p.z - camZ;

        s_tessVertex(tessellator, x - s, y, z);
        s_tessVertex(tessellator, x + s, y, z);
        s_tessVertex(tessellator, x, y - s, z);
        s_tessVertex(tessellator, x, y + s, z);
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
    : Module("WorldParticles", "Ambient world particles around you (snow, glowflies, hearts, stars).") {
    g_mod = this;
}

WorldParticlesModule::~WorldParticlesModule() {
    if (g_mod == this) g_mod = nullptr;
}

void WorldParticlesModule::onInit() {
    uintptr_t addr = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::RenderLevel);
    if (addr != 0) {
        m_patchTarget = reinterpret_cast<void*>(addr);
    }

    uintptr_t tb = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::TessellatorBegin);
    if (tb) {
        m_tessBeginAddr = reinterpret_cast<void*>(tb);
        s_tessBegin = reinterpret_cast<Tessellator_begin_t>(tb);
    }

    uintptr_t tc = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::TessellatorColor);
    if (tc) {
        m_tessColorAddr = reinterpret_cast<void*>(tc);
        s_tessColor = reinterpret_cast<Tessellator_color_t>(tc);
    }

    uintptr_t tv = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::TessellatorVertex);
    if (tv) {
        m_tessVertexAddr = reinterpret_cast<void*>(tv);
        s_tessVertex = reinterpret_cast<Tessellator_vertex_t>(tv);
    }

    uintptr_t rm = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::MeshHelpersRenderMeshImmediately2);
    if (rm) {
        m_renderMeshAddr = reinterpret_cast<void*>(rm);
        s_renderMesh = reinterpret_cast<MeshHelpers_renderMeshImmediately_t>(rm);
    } else {
        uintptr_t rm5 = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::MeshHelpersRenderMeshImmediately);
        if (rm5) s_renderMesh = reinterpret_cast<MeshHelpers_renderMeshImmediately_t>(rm5);
    }

    uintptr_t rmg = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::RenderMaterialGroupCommon);
    if (rmg) {
        m_renderMaterialGroupAddr = reinterpret_cast<void*>(rmg);
        uintptr_t groupAddr = resolveADRP(reinterpret_cast<uint32_t*>(rmg), 2, 0);
        if (groupAddr) {
            s_renderMaterialGroup = groupAddr + bedrocktools::sdk::offsets::MaterialGroup::mRenderMaterialGroupOffset;
        }
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

void WorldParticlesModule::onEnable() {
    applyPatch();
}

void WorldParticlesModule::onDisable() {
    std::lock_guard<std::mutex> lock(particlesMutex);
    particles.clear();
}

void WorldParticlesModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);

    if (j.contains("mode")) {
        try {
            if (j["mode"].is_number_integer()) {
                mode = static_cast<Mode>(std::clamp(j["mode"].get<int>(), 0, 9));
            } else if (j["mode"].is_number_float()) {
                mode = static_cast<Mode>(std::clamp(static_cast<int>(j["mode"].get<float>()), 0, 9));
            } else {
                std::string v = j["mode"].get<std::string>();
                const auto comma = v.find(',');
                if (comma != std::string::npos) v.resize(comma);
                mode = static_cast<Mode>(std::clamp(std::stoi(v), 0, 9));
            }
        } catch (...) {
        }
    }

    if (j.contains("density")) {
        if (j["density"].is_number_integer()) density = std::clamp(j["density"].get<int>(), 4, 400);
        else if (j["density"].is_number_float()) density = std::clamp(static_cast<int>(j["density"].get<float>()), 4, 400);
    }
    if (j.contains("radius")) radius = std::clamp(j["radius"].get<float>(), 2.0f, 64.0f);
    if (j.contains("fallSpeed")) fallSpeed = std::clamp(j["fallSpeed"].get<float>(), 0.05f, 5.0f);
    if (j.contains("particleSize")) particleSize = std::clamp(j["particleSize"].get<float>(), 0.02f, 1.0f);
    if (j.contains("opacity")) opacity = std::clamp(j["opacity"].get<float>(), 0.05f, 1.0f);
    if (j.contains("colorHex")) colorHex = j["colorHex"].get<std::string>();
    if (j.contains("forceTint")) forceTint = j["forceTint"].get<bool>();
    if (j.contains("rainbow")) forceTint = j["rainbow"].get<bool>();
    if (j.contains("wind")) wind = std::clamp(j["wind"].get<float>(), 0.0f, 3.0f);
}

void WorldParticlesModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["mode"] = std::to_string(static_cast<int>(mode)) + ",Snow,Hearts,Stars,Orbs,Storm,Snowflake,Dollar,Pumpkin,Multi,Glowfly";
    j["density"] = density;
    j["radius"] = radius;
    j["fallSpeed"] = fallSpeed;
    j["particleSize"] = particleSize;
    j["opacity"] = opacity;
    j["colorHex"] = colorHex;
    j["forceTint"] = forceTint;
    j["wind"] = wind;
}
