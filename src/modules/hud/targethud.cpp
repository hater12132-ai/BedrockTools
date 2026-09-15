#include "targethud.hpp"

#include "modules/ModuleRegistry.hpp"

#include <bedrocktools/events/AttackEvent.hpp>
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/events/LocalPlayerTickEvent.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Memory.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/world/Actor.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace {

static TargetHudModule* g_mod = nullptr;
static float g_localX = 0, g_localY = 0, g_localZ = 0;
static void* g_localPlayer = nullptr;

using GetRuntimeActorList_t = std::vector<void*> (*)(void*);
using ActorIsPlayer_t = bool (*)(void*);
using ActorGetNameTag_t = std::string (*)(void*);

static GetRuntimeActorList_t s_getRuntimeActorList = nullptr;
static ActorIsPlayer_t s_actorIsPlayer = nullptr;
static ActorGetNameTag_t s_actorGetNameTag = nullptr;

constexpr int HEAD_TEX_SIZE = 64;
using HeadPixels = std::array<uint8_t, HEAD_TEX_SIZE * HEAD_TEX_SIZE * 4>;

static std::uint32_t parseColor(const std::string& value, std::uint32_t fallback) {
    if (value.empty()) return fallback;
    const std::string hex = value[0] == '#' ? value.substr(1) : value;
    try {
        if (hex.size() == 6) return 0xFF000000u | static_cast<std::uint32_t>(std::stoul(hex, nullptr, 16));
        if (hex.size() == 8) return static_cast<std::uint32_t>(std::stoul(hex, nullptr, 16));
    } catch (...) {}
    return fallback;
}

static std::uint32_t withAlpha(std::uint32_t color, float a) {
    a = std::clamp(a, 0.0f, 1.0f);
    const auto aa = static_cast<std::uint32_t>(std::clamp(((color >> 24) & 0xFFu) * a, 0.0f, 255.0f));
    return (aa << 24) | (color & 0x00FFFFFFu);
}

static std::string cleanName(const std::string& input) {
    std::string output;
    output.reserve(input.size());
    for (std::size_t i = 0; i < input.size();) {
        const auto c = static_cast<unsigned char>(input[i]);
        if (c == 0xC2 && i + 1 < input.size() && static_cast<unsigned char>(input[i + 1]) == 0xA7) {
            i += 2;
            if (i < input.size()) ++i;
            continue;
        }
        if (c == 0xA7) {
            i += std::min<std::size_t>(2, input.size() - i);
            continue;
        }
        if (c >= 0x20 && c != 0x7F) output.push_back(input[i]);
        ++i;
    }
    auto first = output.find_first_not_of(' ');
    if (first == std::string::npos) return {};
    auto last = output.find_last_not_of(' ');
    output = output.substr(first, last - first + 1);
    if (output.size() > 48) output.resize(48);
    return output;
}

static std::string readName(void* actor) {
    if (!actor || reinterpret_cast<std::uintptr_t>(actor) < 0x1000) return "Entity";
    if (s_actorGetNameTag) {
        try {
            auto n = cleanName(s_actorGetNameTag(actor));
            if (!n.empty()) return n;
        } catch (...) {}
    }
    try {
        auto* player = reinterpret_cast<bedrocktools::sdk::Player*>(actor);
        auto n = cleanName(player->name());
        if (!n.empty()) return n;
    } catch (...) {}
    return "Entity";
}

static bool isPlayerActor(void* actor) {
    if (!actor) return false;
    if (s_actorIsPlayer) {
        try { return s_actorIsPlayer(actor); } catch (...) {}
    }
    return false;
}

static const void* getSkinImageFromActor(void* actor) {
    if (!actor) return nullptr;
    auto skinRef = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(actor) + bedrocktools::sdk::offsets::Player::mSkin);
    if (!skinRef) return nullptr;
    auto threadOwner = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(skinRef) + bedrocktools::sdk::offsets::SerializedSkinRef::mSkinImpl);
    if (!threadOwner) return nullptr;
    auto skinImpl = reinterpret_cast<uintptr_t>(threadOwner) + bedrocktools::sdk::offsets::ThreadOwner::mObject;
    auto image = reinterpret_cast<const void*>(skinImpl + bedrocktools::sdk::offsets::SerializedSkinImpl::mSkinImage);
    if (*reinterpret_cast<const bool*>(skinImpl + bedrocktools::sdk::offsets::SerializedSkinImpl::mIsPersona)) {
        auto begin = *reinterpret_cast<const uintptr_t*>(skinImpl + bedrocktools::sdk::offsets::SerializedSkinImpl::mSkinAnimatedImages);
        auto end = *reinterpret_cast<const uintptr_t*>(skinImpl + bedrocktools::sdk::offsets::SerializedSkinImpl::mSkinAnimatedImages + sizeof(uintptr_t));
        if (begin && end >= begin && end - begin <= bedrocktools::sdk::offsets::AnimatedImageData::Size * 64) {
            for (auto entry = begin; entry < end; entry += bedrocktools::sdk::offsets::AnimatedImageData::Size) {
                auto type = *reinterpret_cast<const uint32_t*>(entry + bedrocktools::sdk::offsets::AnimatedImageData::mType);
                if (type == 2 || type == 3) {
                    image = reinterpret_cast<const void*>(entry + bedrocktools::sdk::offsets::AnimatedImageData::mImage);
                    if (type == 3) break;
                }
            }
        }
    }
    return image;
}

static bool extractHeadFromActor(void* actor, HeadPixels& out) {
    auto image = getSkinImageFromActor(actor);
    if (!image) return false;
    auto imageAddr = reinterpret_cast<uintptr_t>(image);
    auto width = *reinterpret_cast<const uint32_t*>(imageAddr + bedrocktools::sdk::offsets::SkinImage::mWidth);
    auto height = *reinterpret_cast<const uint32_t*>(imageAddr + bedrocktools::sdk::offsets::SkinImage::mHeight);
    auto pixels = *reinterpret_cast<const uint8_t* const*>(imageAddr + bedrocktools::sdk::offsets::Image::mBytesOffset);
    if (!pixels || width < 64 || width > 256 || height < 32 || height > 256 || width % 64 != 0 || height % 32 != 0)
        return false;

    const auto scale = width / 64;
    const int upScale = HEAD_TEX_SIZE / 8;
    out.fill(0);

    auto copyLayer = [&](int skinX, bool overlay) {
        for (int y = 0; y < 8; ++y) {
            for (int x = 0; x < 8; ++x) {
                int srcX = skinX + x * static_cast<int>(scale) + static_cast<int>(scale / 2);
                int srcY = 8 * static_cast<int>(scale) + y * static_cast<int>(scale) + static_cast<int>(scale / 2);
                if (srcX < 0 || srcY < 0 || srcX >= static_cast<int>(width) || srcY >= static_cast<int>(height)) continue;
                const auto* src = pixels + (srcY * static_cast<int>(width) + srcX) * 4;
                if (overlay && src[3] == 0) continue;
                for (int sy = 0; sy < upScale; ++sy) {
                    for (int sx = 0; sx < upScale; ++sx) {
                        auto* dst = out.data() + ((y * upScale + sy) * HEAD_TEX_SIZE + x * upScale + sx) * 4;
                        dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = 255;
                    }
                }
            }
        }
    };
    copyLayer(8, false);
    copyLayer(40, true);
    return true;
}

static std::string headKeyFor(void* actor, const std::string& name) {
    std::ostringstream oss;
    oss << "bedrocktools.targethud.head." << name << "."
        << std::hex << reinterpret_cast<std::uintptr_t>(actor);
    return oss.str();
}

static bool actorStillAlive(void* actor) {
    if (!actor || !g_localPlayer || !s_getRuntimeActorList) return false;
    try {
        auto* level = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(g_localPlayer) + bedrocktools::sdk::offsets::Actor::mLevel);
        if (!level) return false;
        auto* actorManager = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(level) + bedrocktools::sdk::offsets::Level::mActorManager);
        if (!actorManager) return false;
        auto list = s_getRuntimeActorList(actorManager);
        for (void* a : list) {
            if (a == actor) return true;
        }
    } catch (...) {}
    return false;
}

static float lerp(float a, float b, float t) {
    return a + (b - a) * std::clamp(t, 0.0f, 1.0f);
}

} // namespace

float TargetHudModule::textWidth(const std::string& text, float size) {
    float w = 0.0f;
    for (char c : text) {
        if (c == 'i' || c == 'l' || c == '1' || c == ':' || c == '.' || c == ' ' || c == '/')
            w += size * 0.30f;
        else if (c == 'm' || c == 'w' || c == 'M' || c == 'W')
            w += size * 0.80f;
        else
            w += size * 0.55f;
    }
    return w;
}

TargetHudModule::TargetHudModule()
    : Module("TargetHUD", "SoupVisuals-style target card with head, HP and absorption bars.") {
    g_mod = this;
}

TargetHudModule::~TargetHudModule() {
    if (g_mod == this) g_mod = nullptr;
}

void TargetHudModule::onInit() {
    s_getRuntimeActorList = reinterpret_cast<GetRuntimeActorList_t>(
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorManagerList));
    s_actorIsPlayer = reinterpret_cast<ActorIsPlayer_t>(
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorIsPlayer));
    s_actorGetNameTag = reinterpret_cast<ActorGetNameTag_t>(
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorGetNameTag));

    bedrocktools::events::bus().subscribe<bedrocktools::events::AttackEvent>([](auto& event) {
        if (g_mod && g_mod->enabled && event.target)
            g_mod->onAttackTarget(event.target);
    });
    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>([](auto& event) {
        if (!event.player) return;
        g_localPlayer = event.player;
        try {
            auto* actor = reinterpret_cast<bedrocktools::sdk::Actor*>(event.player);
            const auto pos = actor->position();
            g_localX = pos.x; g_localY = pos.y; g_localZ = pos.z;
        } catch (...) {}

        if (!g_mod || !g_mod->enabled) return;
        std::lock_guard lock(g_mod->m_mutex);
        auto& t = g_mod->m_target;
        if (!t.valid || t.dead || !t.actorPtr) return;

        if (!actorStillAlive(t.actorPtr)) {
            t.health = 0.0f;
            t.absorption = 0.0f;
            t.dead = true;
            t.diedAt = std::chrono::steady_clock::now();
            t.actorPtr = nullptr;
            return;
        }

        try {
            auto* actor = reinterpret_cast<bedrocktools::sdk::Actor*>(t.actorPtr);
            const auto pos = actor->position();
            t.posX = pos.x; t.posY = pos.y; t.posZ = pos.z;
            auto n = readName(t.actorPtr);
            if (!n.empty() && n != "Entity") t.name = n;
        } catch (...) {
            t.health = 0.0f;
            t.dead = true;
            t.diedAt = std::chrono::steady_clock::now();
            t.actorPtr = nullptr;
        }
    });
}

void TargetHudModule::onEnable() {}

void TargetHudModule::onDisable() {
    std::lock_guard lock(m_mutex);
    m_target = {};
    m_anim = 0.0f;
}

void TargetHudModule::onAttackTarget(void* actorPtr) {
    if (!actorPtr || reinterpret_cast<std::uintptr_t>(actorPtr) < 0x1000) return;

    float px = 0, py = 0, pz = 0;
    try {
        auto* actor = reinterpret_cast<bedrocktools::sdk::Actor*>(actorPtr);
        const auto pos = actor->position();
        px = pos.x; py = pos.y; pz = pos.z;
    } catch (...) {
        return;
    }

    const bool player = isPlayerActor(actorPtr);
    const std::string name = readName(actorPtr);
    const std::uintptr_t id = reinterpret_cast<std::uintptr_t>(actorPtr);

    std::string headKey;
    bool hasHead = false;
    if (showHeads && player) {
        HeadPixels head{};
        if (extractHeadFromActor(actorPtr, head)) {
            headKey = headKeyFor(actorPtr, name);
            pl::modmenu::registerImage(headKey, head, HEAD_TEX_SIZE, HEAD_TEX_SIZE);
            hasHead = true;
        }
    }

    std::lock_guard lock(m_mutex);
    const bool same = m_target.valid && m_target.actorId == id && !m_target.dead;

    m_target.actorPtr = actorPtr;
    m_target.actorId = id;
    m_target.name = name.empty() ? (player ? "Player" : "Entity") : name;
    m_target.kind = player ? "Player" : "Entity";
    m_target.isPlayer = player;
    m_target.headKey = headKey;
    m_target.hasHead = hasHead;
    m_target.posX = px;
    m_target.posY = py;
    m_target.posZ = pz;
    m_target.lastHit = std::chrono::steady_clock::now();
    m_target.valid = true;
    m_target.dead = false;

    if (!same) {
        m_target.health = 20.0f;
        m_target.maxHealth = 20.0f;
        m_target.absorption = 0.0f;
        m_target.displayHealth = 20.0f;
        m_target.displayAbsorption = 0.0f;
    } else {
        const float dmg = std::max(0.25f, damagePerHit);
        float remain = dmg;
        if (m_target.absorption > 0.0f) {
            const float used = std::min(m_target.absorption, remain);
            m_target.absorption -= used;
            remain -= used;
        }
        if (remain > 0.0f)
            m_target.health = std::max(0.0f, m_target.health - remain);
        if (m_target.health <= 0.0f) {
            m_target.health = 0.0f;
            m_target.absorption = 0.0f;
            m_target.dead = true;
            m_target.diedAt = std::chrono::steady_clock::now();
            m_target.actorPtr = nullptr;
        }
    }
}

void TargetHudModule::onFrame() {
    if (!enabled) {
        pl::modmenu::submitDrawCommands(moduleId, std::span<const pl::modmenu::DrawCommand>{});
        return;
    }

    TargetState snap;
    {
        std::lock_guard lock(m_mutex);
        if (m_target.valid) {
            m_target.displayHealth = lerp(m_target.displayHealth, m_target.health, 0.18f);
            m_target.displayAbsorption = lerp(m_target.displayAbsorption, m_target.absorption, 0.18f);
            if (std::fabs(m_target.displayHealth - m_target.health) < 0.02f)
                m_target.displayHealth = m_target.health;
            if (std::fabs(m_target.displayAbsorption - m_target.absorption) < 0.02f)
                m_target.displayAbsorption = m_target.absorption;
        }
        snap = m_target;
    }

    const auto now = std::chrono::steady_clock::now();
    float want = 0.0f;
    if (snap.valid) {
        if (snap.dead) {
            const double sinceDeath = std::chrono::duration<double>(now - snap.diedAt).count();
            if (sinceDeath < static_cast<double>(deathFadeTime)) {
                want = 1.0f - static_cast<float>(sinceDeath / std::max(0.15, static_cast<double>(deathFadeTime)));
                want = std::clamp(want, 0.0f, 1.0f);
            } else {
                std::lock_guard lock(m_mutex);
                m_target.valid = false;
                snap.valid = false;
                want = 0.0f;
            }
        } else {
            const double age = std::chrono::duration<double>(now - snap.lastHit).count();
            if (age <= static_cast<double>(liveTime)) want = 1.0f;
            else {
                std::lock_guard lock(m_mutex);
                m_target.valid = false;
                snap.valid = false;
            }
        }
    }

    const float step = std::clamp(animationSpeed, 0.3f, 4.0f) * 0.14f;
    if (want > m_anim) m_anim = std::min(want, m_anim + step);
    else m_anim = std::max(want, m_anim - step * 1.2f);
    if (snap.valid && snap.dead) m_anim = want;

    if (m_anim < 0.02f) {
        pl::modmenu::submitDrawCommands(moduleId, std::span<const pl::modmenu::DrawCommand>{});
        return;
    }

    if (hudPosX < 0.0f) hudPosX = 60.0f;
    if (hudPosY < 0.0f) hudPosY = 40.0f;

    const float fade = m_anim;
    const float pop = 0.94f + 0.06f * m_anim;
    const float s = std::clamp(scale, 0.5f, 2.5f) * pop;
    const float mainW = cardWidth * s;
    const float mainH = cardHeight * s;
    const float rad = cornerRadius * s;
    const float x = hudPosX;
    const float y = hudPosY;

    std::vector<pl::modmenu::DrawCommand> cmds;
    cmds.reserve(28);
    static thread_local std::vector<std::string> texts;
    texts.clear();

    auto rect = [&](float rx, float ry, float rw, float rh, float r, std::uint32_t col) {
        if (rw <= 0.5f || rh <= 0.5f) return;
        pl::modmenu::DrawCommand c{};
        c.type = pl::modmenu::DrawCommandType::RectFilled;
        c.x = rx; c.y = ry; c.w = rw; c.h = rh; c.x3 = r; c.color = col;
        cmds.push_back(c);
    };
    auto text = [&](float tx, float ty, float size, std::uint32_t col, std::string str) {
        texts.push_back(std::move(str));
        pl::modmenu::DrawCommand c{};
        c.type = pl::modmenu::DrawCommandType::Text;
        c.x = tx; c.y = ty; c.w = mainW; c.h = size + 4.0f;
        c.size = size; c.color = col;
        c.text = texts.back().c_str();
        cmds.push_back(c);
    };

    const int ba = static_cast<int>(std::clamp(backAlpha, 0.0f, 1.0f) * 255.0f * fade);
    rect(x, y, mainW, mainH, rad, (static_cast<std::uint32_t>(ba) << 24) | 0x000D0D12u);

    const float headPad = 8.0f * s;
    const float headSize = mainH - headPad * 2.0f;
    const float headX = x + headPad;
    const float headY = y + headPad;
    rect(headX, headY, headSize, headSize, 6.0f * s, withAlpha(0xFF2A2A32u, fade));

    if (snap.hasHead && !snap.headKey.empty()) {
        texts.push_back(snap.headKey);
        pl::modmenu::DrawCommand hc{};
        hc.type = pl::modmenu::DrawCommandType::Image;
        hc.x = headX + 2.0f * s;
        hc.y = headY + 2.0f * s;
        hc.w = headSize - 4.0f * s;
        hc.h = headSize - 4.0f * s;
        hc.text = texts.back().c_str();
        hc.color = withAlpha(0xFFFFFFFFu, fade);
        cmds.push_back(hc);
    } else {
        const float face = headSize * 0.72f;
        const float faceX = headX + (headSize - face) * 0.5f;
        const float faceY = headY + (headSize - face) * 0.5f;
        rect(faceX, faceY, face, face, 3.0f * s, withAlpha(0xFFD8D8D8u, fade));
        const float eyeW = face * 0.18f, eyeH = face * 0.14f, eyeY = faceY + face * 0.32f;
        rect(faceX + face * 0.18f, eyeY, eyeW, eyeH, 1.0f * s, withAlpha(0xFF1A1A1Au, fade));
        rect(faceX + face * 0.62f, eyeY, eyeW, eyeH, 1.0f * s, withAlpha(0xFF1A1A1Au, fade));
        rect(faceX + face * 0.22f, faceY + face * 0.62f, face * 0.56f, face * 0.10f, 1.0f * s, withAlpha(0xFF1A1A1Au, fade));
    }

    const float textLeft = headX + headSize + 10.0f * s;
    text(textLeft, y + 10.0f * s, 15.0f * s, withAlpha(0xFFFFFFFFu, fade), snap.name);

    if (showPlayerTag) {
        const float tagSize = 10.0f * s;
        const float tagW = textWidth(snap.kind, tagSize);
        text(x + mainW - 12.0f * s - tagW, y + 12.0f * s, tagSize, withAlpha(0xFF9A9AA8u, fade), snap.kind);
    }

    {
        std::ostringstream hp;
        hp << "HP / " << std::fixed << std::setprecision(1) << snap.displayHealth;
        if (snap.displayAbsorption > 0.05f)
            hp << "  +" << std::setprecision(1) << snap.displayAbsorption;
        text(textLeft, y + 28.0f * s, 11.0f * s, withAlpha(0xFFB0B0BCu, fade), hp.str());
    }

    const float barX = textLeft;
    const float barH = 5.5f * s;
    const float barY = y + mainH - 14.0f * s;
    const float barW = mainW - (textLeft - x) - 12.0f * s;
    const float maxH = std::max(1.0f, snap.maxHealth);

    rect(barX, barY, barW, barH, barH * 0.5f, withAlpha(0xFF1E1E28u, fade));

    const float hpRatio = std::clamp(snap.displayHealth / maxH, 0.0f, 1.0f);
    const float absRatio = std::clamp(snap.displayAbsorption / maxH, 0.0f, 1.0f - hpRatio);

    if (hpRatio > 0.001f) {
        rect(barX, barY, barW * hpRatio, barH, barH * 0.5f,
             withAlpha(parseColor(barColor, 0xFF8B5CFFu), fade));
    }
    if (absRatio > 0.001f) {
        rect(barX + barW * hpRatio, barY, barW * absRatio, barH, barH * 0.5f,
             withAlpha(parseColor(absorptionColor, 0xFFFFC93Au), fade));
    }

    if (showArmorRow) {
        const float rowGap = 6.0f * s;
        const float rowY = y + mainH + rowGap;
        const float rowH = 28.0f * s;
        const float icon = 16.0f * s;
        const float slotGap = 8.0f * s;
        const int slots = 5;
        const float rowW = slots * icon + (slots + 1) * slotGap + 8.0f * s;
        const float rowX = x + (mainW - rowW) * 0.5f;

        rect(rowX, rowY, rowW, rowH, 10.0f * s, withAlpha(0xFF0D0D12u, fade * 0.95f));
        const std::uint32_t iconCol = withAlpha(0xFF3DE0F0u, fade);
        const std::uint32_t barGreen = withAlpha(0xFF3DFF7Au, fade);
        const std::uint32_t barOrange = withAlpha(0xFFFF9A3Du, fade);

        for (int i = 0; i < slots; ++i) {
            const float ix = rowX + slotGap + i * (icon + slotGap);
            const float iy = rowY + 4.0f * s;
            if (i == 0) {
                rect(ix + icon * 0.4f, iy, icon * 0.2f, icon * 0.75f, 1.0f * s, iconCol);
                rect(ix + icon * 0.25f, iy + icon * 0.7f, icon * 0.5f, icon * 0.15f, 1.0f * s, iconCol);
            } else if (i == 1) {
                rect(ix + icon * 0.15f, iy + icon * 0.15f, icon * 0.7f, icon * 0.55f, 2.0f * s, iconCol);
            } else if (i == 2) {
                rect(ix + icon * 0.2f, iy + icon * 0.1f, icon * 0.6f, icon * 0.7f, 2.0f * s, iconCol);
            } else if (i == 3) {
                rect(ix + icon * 0.2f, iy + icon * 0.15f, icon * 0.25f, icon * 0.7f, 1.5f * s, iconCol);
                rect(ix + icon * 0.55f, iy + icon * 0.15f, icon * 0.25f, icon * 0.7f, 1.5f * s, iconCol);
            } else {
                rect(ix + icon * 0.15f, iy + icon * 0.45f, icon * 0.3f, icon * 0.4f, 1.5f * s, iconCol);
                rect(ix + icon * 0.55f, iy + icon * 0.45f, icon * 0.3f, icon * 0.4f, 1.5f * s, iconCol);
            }
            const float dw = icon * 0.7f;
            const float dx = ix + (icon - dw) * 0.5f;
            const float dy = rowY + rowH - 5.0f * s;
            rect(dx, dy, dw, 2.0f * s, 1.0f * s, (i == 1 || i == 4) ? barOrange : barGreen);
        }
    }

    pl::modmenu::submitDrawCommands(moduleId, cmds);
}

void TargetHudModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    if (j.contains("hudPosX")) hudPosX = j["hudPosX"].get<float>();
    if (j.contains("hudPosY")) hudPosY = j["hudPosY"].get<float>();
    if (j.contains("isHudModule")) isHudModule = j["isHudModule"].get<bool>();
    if (j.contains("scale")) scale = std::clamp(j["scale"].get<float>(), 0.5f, 2.5f);
    if (j.contains("backAlpha")) backAlpha = std::clamp(j["backAlpha"].get<float>(), 0.0f, 1.0f);
    if (j.contains("liveTime")) liveTime = std::clamp(j["liveTime"].get<float>(), 0.5f, 15.0f);
    if (j.contains("deathFadeTime")) deathFadeTime = std::clamp(j["deathFadeTime"].get<float>(), 0.3f, 5.0f);
    if (j.contains("animationSpeed")) animationSpeed = std::clamp(j["animationSpeed"].get<float>(), 0.3f, 4.0f);
    if (j.contains("cardWidth")) cardWidth = std::clamp(j["cardWidth"].get<float>(), 120.0f, 360.0f);
    if (j.contains("cardHeight")) cardHeight = std::clamp(j["cardHeight"].get<float>(), 40.0f, 120.0f);
    if (j.contains("cornerRadius")) cornerRadius = std::clamp(j["cornerRadius"].get<float>(), 0.0f, 28.0f);
    if (j.contains("barColor")) barColor = j["barColor"].get<std::string>();
    if (j.contains("absorptionColor")) absorptionColor = j["absorptionColor"].get<std::string>();
    if (j.contains("showArmorRow")) showArmorRow = j["showArmorRow"].get<bool>();
    if (j.contains("showPlayerTag")) showPlayerTag = j["showPlayerTag"].get<bool>();
    if (j.contains("showHeads")) showHeads = j["showHeads"].get<bool>();
    if (j.contains("damagePerHit")) damagePerHit = std::clamp(j["damagePerHit"].get<float>(), 0.25f, 20.0f);
}

void TargetHudModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["hudPosX"] = hudPosX;
    j["hudPosY"] = hudPosY;
    j["isHudModule"] = true;
    j["scale"] = scale;
    j["backAlpha"] = backAlpha;
    j["liveTime"] = liveTime;
    j["deathFadeTime"] = deathFadeTime;
    j["animationSpeed"] = animationSpeed;
    j["cardWidth"] = cardWidth;
    j["cardHeight"] = cardHeight;
    j["cornerRadius"] = cornerRadius;
    j["barColor"] = barColor;
    j["absorptionColor"] = absorptionColor;
    j["showArmorRow"] = showArmorRow;
    j["showPlayerTag"] = showPlayerTag;
    j["showHeads"] = showHeads;
    j["damagePerHit"] = damagePerHit;
}
