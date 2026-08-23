#include "targethud.hpp"
#include "modules/ModuleRegistry.hpp"
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <sstream>

// Same three signatures skinstealer.cpp already uses to find and validate whatever
// entity is currently under the local player's crosshair.
using LevelGetHitResultFn = void* (*)(void*);
using HitResultGetEntityFn = void* (*)(void*);
using ActorIsPlayerFn = bool (*)(void*);
using ActorGetNameTagFn = std::string (*)(void*);

static LevelGetHitResultFn s_getHitResult = nullptr;
static HitResultGetEntityFn s_getEntity = nullptr;
static ActorIsPlayerFn s_isPlayer = nullptr;
static ActorGetNameTagFn s_getNameTag = nullptr;

static TargetHudModule* g_targetHud = nullptr;
static void* g_localPlayerNative = nullptr;
static void* g_lastTargetActor = nullptr;

constexpr int HEAD_TEX_SIZE = 64;
using HeadPixels = std::array<uint8_t, HEAD_TEX_SIZE * HEAD_TEX_SIZE * 4>;

// --- Everything below this line to extractHeadFromActor() is the same head-render
// pipeline tablist.cpp already uses (strip skin layers -> upscale the 8x8 head region
// with its hat-layer overlay blended in). Duplicated locally rather than shared across
// files since tablist.cpp keeps these as translation-unit-local statics too. ---

static std::string cleanPlayerName(const std::string& input) {
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
    if (output.size() > 128) output.resize(128);
    return output;
}

static std::string getActorName(void* actor) {
    if (!actor) return {};
    if (s_getNameTag) {
        auto name = cleanPlayerName(s_getNameTag(actor));
        if (!name.empty()) return name;
    }
    auto* filteredName = reinterpret_cast<const std::string*>(
        reinterpret_cast<uintptr_t>(actor) + bedrocktools::sdk::offsets::Actor::mFilteredNameTag);
    if (filteredName && !filteredName->empty() && filteredName->size() <= 256) {
        auto name = cleanPlayerName(*filteredName);
        if (!name.empty()) return name;
    }
    auto* playerName = reinterpret_cast<const std::string*>(
        reinterpret_cast<uintptr_t>(actor) + bedrocktools::sdk::offsets::Player::mName);
    if (playerName && !playerName->empty() && playerName->size() <= 256) {
        return cleanPlayerName(*playerName);
    }
    return {};
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

    if (!pixels || width < 64 || width > 256 || height < 32 || height > 256 || width % 64 != 0 || height % 32 != 0) {
        return false;
    }

    const auto scale = width / 64;
    const int upScale = HEAD_TEX_SIZE / 8;
    out.fill(0);

    auto copyLayer = [&](int skinX, bool overlay) {
        for (int y = 0; y < 8; ++y) {
            for (int x = 0; x < 8; ++x) {
                int srcX = skinX + x * static_cast<int>(scale) + static_cast<int>(scale / 2);
                int srcY = 8 * static_cast<int>(scale) + y * static_cast<int>(scale) + static_cast<int>(scale / 2);
                const auto* src = pixels + (static_cast<std::size_t>(srcY) * width + srcX) * 4;
                for (int sy = 0; sy < upScale; ++sy) {
                    for (int sx = 0; sx < upScale; ++sx) {
                        auto* dst = out.data() + ((y * upScale + sy) * HEAD_TEX_SIZE + x * upScale + sx) * 4;
                        if (!overlay) { std::memcpy(dst, src, 4); continue; }
                        const auto alpha = src[3];
                        if (alpha == 0) continue;
                        if (alpha == 255) { std::memcpy(dst, src, 4); continue; }
                        float a = alpha / 255.0f;
                        float inverse = 1.0f - a;
                        dst[0] = static_cast<uint8_t>(src[0] * a + dst[0] * inverse + 0.5f);
                        dst[1] = static_cast<uint8_t>(src[1] * a + dst[1] * inverse + 0.5f);
                        dst[2] = static_cast<uint8_t>(src[2] * a + dst[2] * inverse + 0.5f);
                        dst[3] = 255;
                    }
                }
            }
        }
    };

    copyLayer(8 * static_cast<int>(scale), false);
    copyLayer(40 * static_cast<int>(scale), true);
    return true;
}

static std::string imageKeyFor(void* actor) {
    char buffer[48];
    std::snprintf(buffer, sizeof(buffer), "targethud_%llx", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(actor)));
    return buffer;
}

static float calcTextWidth(const std::string& text, float size) {
    float width = 0;
    for (char c : text) {
        if (c == 'i' || c == 'l' || c == '1' || c == ':' || c == '.' || c == ' ') width += size * 0.3f;
        else if (c == 'm' || c == 'w' || c == 'M' || c == 'W') width += size * 0.8f;
        else width += size * 0.58f;
    }
    return width;
}

TargetHudModule::TargetHudModule()
    : Module("Target HUD", "Shows the head, gamertag, and health bar of the player under your crosshair.") {
    g_targetHud = this;
}

TargetHudModule::~TargetHudModule() {
    if (g_targetHud == this) g_targetHud = nullptr;
}

void TargetHudModule::onInit() {
    uintptr_t addr = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::LevelGetHitResult);
    if (addr) s_getHitResult = reinterpret_cast<LevelGetHitResultFn>(addr);

    addr = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::HitResultGetEntity);
    if (addr) s_getEntity = reinterpret_cast<HitResultGetEntityFn>(addr);

    addr = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorIsPlayer);
    if (addr) s_isPlayer = reinterpret_cast<ActorIsPlayerFn>(addr);

    addr = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorGetNameTag);
    if (addr) s_getNameTag = reinterpret_cast<ActorGetNameTagFn>(addr);

    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>([](auto& event) {
        g_localPlayerNative = event.player;
        if (g_targetHud && g_targetHud->enabled) g_targetHud->onLocalPlayerTick(event.player);
    });
}

void TargetHudModule::onEnable() {
    m_hasTarget = false;
    g_lastTargetActor = nullptr;
}

void TargetHudModule::onDisable() {
    m_hasTarget = false;
    g_lastTargetActor = nullptr;
}

void TargetHudModule::onLocalPlayerTick(void* localPlayer) {
    if (!localPlayer || !s_getHitResult || !s_getEntity || !s_isPlayer) {
        m_hasTarget = false;
        return;
    }

    void* level = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(localPlayer) + bedrocktools::sdk::offsets::Actor::mLevel);
    if (!level) { m_hasTarget = false; return; }

    void* hit = s_getHitResult(level);
    void* entity = hit ? s_getEntity(hit) : nullptr;

    if (!entity || entity == localPlayer || !s_isPlayer(entity)) {
        m_hasTarget = false;
        g_lastTargetActor = nullptr;
        return;
    }

    m_hasTarget = true;
    m_targetName = getActorName(entity);
    refreshHealth(entity);

    // Only redo the (comparatively expensive) pixel extraction + re-registration when
    // the target actually changes, not every tick.
    if (entity != g_lastTargetActor) {
        g_lastTargetActor = entity;
        m_imageKey = imageKeyFor(entity);
        if (m_showHead) {
            HeadPixels head{};
            if (extractHeadFromActor(entity, head)) {
                pl::modmenu::registerImage(m_imageKey, head, HEAD_TEX_SIZE, HEAD_TEX_SIZE);
            }
        }
    }
}

void TargetHudModule::refreshHealth(void* targetActor) {
    (void)targetActor;
    // --- NOT WIRED UP YET ---
    // No offset for the local player's own health exists anywhere in this SDK either
    // (checked include/bedrocktools/sdk/offsets/), so there's genuinely nothing to read
    // yet for a target's health. Worth knowing before you go looking for it: newer
    // Bedrock versions moved health off the legacy synced-actor-data list (the kind
    // ActorDataIds::FuseTime uses in tnttimer.cpp) and onto an attribute component keyed
    // by the string "minecraft:health" (min/current/max), per the protocol docs. So the
    // FuseTime-style DataItem lookup won't get you there - what's actually needed is an
    // offset to that Actor's attribute-instance container, which isn't in this codebase.
    //
    // Once you have it, set m_currentHealth / m_maxHealth here and onFrame() will start
    // drawing the bar fill automatically - it already checks m_maxHealth > 0 before
    // drawing anything so it won't show a fake number in the meantime.
}

void TargetHudModule::onFrame() {
    if (!enabled) return;
    if (!m_hasTarget || m_targetName.empty()) return;

    const std::string playerTag = "Player";
    const float headSize = 48.0f;
    const float padX = 12.0f;
    const float padY = 10.0f;
    const float gap = 10.0f;

    float textBlockW = std::max(calcTextWidth(m_targetName, m_nameSize), 90.0f) + calcTextWidth(playerTag, m_nameSize * 0.6f) + 20.0f;
    float cardW = padX * 2.0f + (m_showHead ? headSize + gap : 0.0f) + textBlockW;
    cardW = std::max(cardW, 220.0f);
    float cardH = padY * 2.0f + headSize;

    std::vector<PLModMenu_DrawCommand> cmds;

    if (m_background) {
        PLModMenu_DrawCommand bg = {};
        bg.type = PL_DRAW_RECT_FILLED;
        bg.x = hudPosX;
        bg.y = hudPosY;
        bg.w = cardW;
        bg.h = cardH;
        int alpha = (int)(m_backgroundOpacity * 255.0f);
        bg.color = (alpha << 24) | 0x101014;
        cmds.push_back(bg);
    }

    float contentX = hudPosX + padX;

    if (m_showHead && !m_imageKey.empty()) {
        PLModMenu_DrawCommand headCmd = {};
        headCmd.type = PL_DRAW_IMAGE;
        headCmd.x = contentX;
        headCmd.y = hudPosY + padY;
        headCmd.w = headSize;
        headCmd.h = headSize;
        headCmd.color = 0xFFFFFFFF;
        headCmd.imageId = m_imageKey;
        cmds.push_back(headCmd);
        contentX += headSize + gap;
    }

    PLModMenu_DrawCommand nameCmd = {};
    nameCmd.type = PL_DRAW_TEXT;
    nameCmd.x = contentX;
    nameCmd.y = hudPosY + padY - 2.0f;
    nameCmd.w = textBlockW;
    nameCmd.h = m_nameSize;
    nameCmd.color = 0xFFFFFFFF;
    nameCmd.size = m_nameSize;
    nameCmd.text = m_targetName.c_str();
    cmds.push_back(nameCmd);

    PLModMenu_DrawCommand tagCmd = {};
    tagCmd.type = PL_DRAW_TEXT;
    tagCmd.x = contentX + calcTextWidth(m_targetName, m_nameSize) + 14.0f;
    tagCmd.y = hudPosY + padY - 1.0f;
    tagCmd.w = 120.0f;
    tagCmd.h = m_nameSize * 0.6f;
    tagCmd.color = 0xFFA0A0A0;
    tagCmd.size = m_nameSize * 0.6f;
    tagCmd.text = playerTag.c_str();
    cmds.push_back(tagCmd);

    std::ostringstream hpLabelStream;
    if (m_maxHealth > 0.0f) {
        hpLabelStream << "HP / " << std::fixed << std::setprecision(1) << m_currentHealth;
    } else {
        hpLabelStream << "HP / --";
    }
    const std::string hpLabel = hpLabelStream.str();

    float barY = hudPosY + padY + m_nameSize + 8.0f;
    float barW = textBlockW;
    float barH = 6.0f;

    PLModMenu_DrawCommand hpLabelCmd = {};
    hpLabelCmd.type = PL_DRAW_TEXT;
    hpLabelCmd.x = contentX;
    hpLabelCmd.y = barY - m_nameSize * 0.55f;
    hpLabelCmd.w = barW;
    hpLabelCmd.h = m_nameSize * 0.5f;
    hpLabelCmd.color = 0xFFB0B0B0;
    hpLabelCmd.size = m_nameSize * 0.5f;
    hpLabelCmd.text = hpLabel.c_str();
    cmds.push_back(hpLabelCmd);

    PLModMenu_DrawCommand barTrack = {};
    barTrack.type = PL_DRAW_RECT_FILLED;
    barTrack.x = contentX;
    barTrack.y = barY;
    barTrack.w = barW;
    barTrack.h = barH;
    barTrack.color = 0x60000000;
    cmds.push_back(barTrack);

    if (m_maxHealth > 0.0f) {
        float pct = std::clamp(m_currentHealth / m_maxHealth, 0.0f, 1.0f);
        PLModMenu_DrawCommand barFill = {};
        barFill.type = PL_DRAW_RECT_FILLED;
        barFill.x = contentX;
        barFill.y = barY;
        barFill.w = barW * pct;
        barFill.h = barH;
        barFill.color = 0xFF000000 | (m_barColor & 0x00FFFFFF);
        cmds.push_back(barFill);
    }

    submitDrawCommands(moduleId, cmds);
}

void TargetHudModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    if (j.contains("hudPosX")) hudPosX = j["hudPosX"].get<float>();
    if (j.contains("hudPosY")) hudPosY = j["hudPosY"].get<float>();
    if (j.contains("isHudModule")) isHudModule = j["isHudModule"].get<bool>();
    if (j.contains("m_nameSize")) m_nameSize = j["m_nameSize"].get<float>();
    if (j.contains("m_showHead")) m_showHead = j["m_showHead"].get<bool>();
    if (j.contains("m_background")) m_background = j["m_background"].get<bool>();
    if (j.contains("m_backgroundOpacity")) m_backgroundOpacity = j["m_backgroundOpacity"].get<float>();
    if (j.contains("m_barColor")) m_barColor = j["m_barColor"].get<uint32_t>();
}

void TargetHudModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["hudPosX"] = hudPosX;
    j["hudPosY"] = hudPosY;
    j["isHudModule"] = isHudModule;
    j["m_nameSize"] = m_nameSize;
    j["m_showHead"] = m_showHead;
    j["m_background"] = m_background;
    j["m_backgroundOpacity"] = m_backgroundOpacity;
    j["m_barColor"] = m_barColor;
}
