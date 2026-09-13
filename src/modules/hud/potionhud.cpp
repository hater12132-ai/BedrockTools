#include "potionhud.hpp"
#include "potionhud_assets.hpp"

#include "core/memory/Hooks.hpp"
#include "modules/ModuleRegistry.hpp"
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/input/MoveInput.hpp>
#include <bedrocktools/sdk/world/MobEffects.hpp>
#include <pl/ModMenu.hpp>
#include <pl/ModMenuConfig.hpp>
#include <pl/memory/Vtable.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <iterator>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr const char* MinecraftLibrary = "libminecraftpe.so";
constexpr std::size_t MaxEffects = 64;
constexpr float VanillaEffectSize = 16.0f;
constexpr int WarningSeconds = 5;
constexpr const char* GenericPotionImageId = "bedrocktools.potionhud.generic";
// Per-effect monochrome icons (registered in onInit).
constexpr const char* ImgSpeed = "bedrocktools.potionhud.speed";
constexpr const char* ImgSlowness = "bedrocktools.potionhud.slowness";
constexpr const char* ImgHaste = "bedrocktools.potionhud.haste";
constexpr const char* ImgMiningFatigue = "bedrocktools.potionhud.mining_fatigue";
constexpr const char* ImgStrength = "bedrocktools.potionhud.strength";
constexpr const char* ImgInstantHealth = "bedrocktools.potionhud.instant_health";
constexpr const char* ImgInstantDamage = "bedrocktools.potionhud.instant_damage";
constexpr const char* ImgJumpBoost = "bedrocktools.potionhud.jump_boost";
constexpr const char* ImgNausea = "bedrocktools.potionhud.nausea";
constexpr const char* ImgRegeneration = "bedrocktools.potionhud.regeneration";
constexpr const char* ImgResistance = "bedrocktools.potionhud.resistance";
constexpr const char* ImgFireResistance = "bedrocktools.potionhud.fire_resistance";
constexpr const char* ImgWaterBreathing = "bedrocktools.potionhud.water_breathing";
constexpr const char* ImgInvisibility = "bedrocktools.potionhud.invisibility";
constexpr const char* ImgBlindness = "bedrocktools.potionhud.blindness";
constexpr const char* ImgNightVision = "bedrocktools.potionhud.night_vision";
constexpr const char* ImgHunger = "bedrocktools.potionhud.hunger";
constexpr const char* ImgWeakness = "bedrocktools.potionhud.weakness";
constexpr const char* ImgPoison = "bedrocktools.potionhud.poison";
constexpr const char* ImgWither = "bedrocktools.potionhud.wither";
constexpr const char* ImgHealthBoost = "bedrocktools.potionhud.health_boost";
constexpr const char* ImgAbsorption = "bedrocktools.potionhud.absorption";
constexpr const char* ImgSaturation = "bedrocktools.potionhud.saturation";
constexpr const char* ImgLevitation = "bedrocktools.potionhud.levitation";
constexpr const char* ImgFatalPoison = "bedrocktools.potionhud.fatal_poison";
constexpr const char* ImgConduitPower = "bedrocktools.potionhud.conduit_power";
constexpr const char* ImgSlowFalling = "bedrocktools.potionhud.slow_falling";
constexpr const char* ImgBadOmen = "bedrocktools.potionhud.bad_omen";
constexpr const char* ImgVillageHero = "bedrocktools.potionhud.village_hero";
constexpr const char* ImgDarkness = "bedrocktools.potionhud.darkness";
constexpr const char* ImgTrialOmen = "bedrocktools.potionhud.trial_omen";
constexpr const char* ImgWindCharged = "bedrocktools.potionhud.wind_charged";
constexpr const char* ImgWeaving = "bedrocktools.potionhud.weaving";
constexpr const char* ImgOozing = "bedrocktools.potionhud.oozing";
constexpr const char* ImgInfested = "bedrocktools.potionhud.infested";
constexpr const char* ImgRaidOmen = "bedrocktools.potionhud.raid_omen";
constexpr const char* ImgBreathNautilus = "bedrocktools.potionhud.breath_nautilus";
struct RectangleArea {
    float x0;
    float x1;
    float y0;
    float y1;
};

struct UiVec2 {
    float x;
    float y;
};

struct Color {
    float r;
    float g;
    float b;
    float a;
};

struct ClientTexture {
    std::byte storage[24]{};
};

struct BedrockTextureData {
    ClientTexture clientTexture;
};

enum class ResourceFileSystem : int {
    UserPackage = 0
};

class ResourceLocation {
public:
    ResourceFileSystem fileSystem;
    std::string path;
    std::uint64_t pathHash;
    std::uint64_t fullHash;

    explicit ResourceLocation(std::string_view value)
        : fileSystem(ResourceFileSystem::UserPackage),
          path(value),
          pathHash(computeHash(path)),
          fullHash(pathHash ^ static_cast<std::uint64_t>(fileSystem)) {}

private:
    static std::uint64_t computeHash(std::string_view value) {
        constexpr std::uint64_t offset = 1469598103934665603ULL;
        constexpr std::uint64_t prime = 1099511628211ULL;
        std::uint64_t hash = offset;
        for (unsigned char ch : value) hash = static_cast<std::uint64_t>(ch) ^ (prime * hash);
        return hash;
    }
};

class TexturePtr {
public:
    std::shared_ptr<const BedrockTextureData> clientTexture;
    std::shared_ptr<ResourceLocation> resourceLocation;

    const ClientTexture& getClientTexture() const {
        static const ClientTexture empty{};
        return clientTexture ? clientTexture->clientTexture : empty;
    }
};

class HashedString {
public:
    std::uint64_t hash;
    std::string value;
    mutable const HashedString* lastMatch;

    explicit HashedString(const char* text)
        : hash(computeHash(text ? std::string_view(text) : std::string_view())),
          value(text ? text : ""),
          lastMatch(nullptr) {}

private:
    static std::uint64_t computeHash(std::string_view text) {
        if (text.empty()) return 0;
        constexpr std::uint64_t offset = 0xCBF29CE484222325ULL;
        constexpr std::uint64_t prime = 0x100000001B3ULL;
        std::uint64_t result = offset;
        for (char character : text) {
            result = static_cast<std::uint64_t>(static_cast<unsigned char>(character)) ^ (prime * result);
        }
        return result;
    }
};

struct RawEffect {
    std::uint32_t id;
    int duration;
    int amplifier;
    bool noCounter;
};

using HudMobEffectsRendererFn = void* (*)(void*, void*, void*, void*, int, void*);

HudMobEffectsRendererFn hudMobEffectsRendererOriginal = nullptr;
PotionHudModule* moduleInstance = nullptr;
bedrocktools::hooks::Handle hudMobEffectsRendererHook = nullptr;

void** getVtable(void* object) {
    return object ? *reinterpret_cast<void***>(object) : nullptr;
}

void* getLocalPlayer(void* client) {
    void** vtable = getVtable(client);
    if (!vtable || !vtable[bedrocktools::sdk::offsets::VTable::ClientInstanceGetLocalPlayer]) return nullptr;
    return reinterpret_cast<void* (*)(void*)>(vtable[bedrocktools::sdk::offsets::VTable::ClientInstanceGetLocalPlayer])(client);
}

MobEffectsComponent* getMobEffects(void* player) {
    if (!player) return nullptr;
    auto* context = reinterpret_cast<EntityContext*>(
        reinterpret_cast<std::uintptr_t>(player) + bedrocktools::sdk::offsets::Actor::mEntityContext);
    return context->tryGetComponent<MobEffectsComponent>();
}

bool copyEffects(MobEffectsComponent* component, std::vector<RawEffect>& out) {
    if (!component) return false;
    const auto begin = reinterpret_cast<std::uintptr_t>(component->begin);
    const auto end = reinterpret_cast<std::uintptr_t>(component->end);
    const auto capacity = reinterpret_cast<std::uintptr_t>(component->capacity);
    if (begin == 0 && end == 0 && capacity == 0) return true;
    if (!begin || !end || !capacity || end < begin || capacity < end) return false;
    const auto span = end - begin;
    const auto capacitySpan = capacity - begin;
    if (span % sizeof(MobEffectInstance) != 0 || capacitySpan % sizeof(MobEffectInstance) != 0) return false;
    const std::size_t count = span / sizeof(MobEffectInstance);
    const std::size_t capacityCount = capacitySpan / sizeof(MobEffectInstance);
    if (count > MaxEffects || capacityCount > MaxEffects * 4 || count > capacityCount) return false;
    out.clear();
    out.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto* effect = reinterpret_cast<const MobEffectInstance*>(begin + i * sizeof(MobEffectInstance));
        const auto id = static_cast<std::uint32_t>(effect->id);
        if (id == 0 || id > static_cast<std::uint32_t>(MobEffectType::BreathOfTheNautilus)) continue;
        if (effect->duration < -1 || (effect->duration == 0 && !effect->noCounter)) continue;
        out.push_back({id, effect->duration, std::clamp(effect->amplifier, 0, 255), effect->noCounter});
    }
    return true;
}

RectangleArea getFullClippingRectangle(void* context) {
    RectangleArea result{};
    void** vtable = getVtable(context);
    if (!vtable || !vtable[bedrocktools::sdk::offsets::VTable::MinecraftUIRenderContextGetFullClippingRectangle]) return result;
    using Fn = RectangleArea (*)(void*);
    return reinterpret_cast<Fn>(vtable[bedrocktools::sdk::offsets::VTable::MinecraftUIRenderContextGetFullClippingRectangle])(context);
}

bool validRectangle(const RectangleArea& area) {
    return std::isfinite(area.x0) && std::isfinite(area.x1) && std::isfinite(area.y0) && std::isfinite(area.y1) &&
           area.x1 > area.x0 && area.y1 > area.y0;
}

TexturePtr getTexture(void* context, const ResourceLocation& location) {
    void** vtable = getVtable(context);
    if (!vtable || !vtable[bedrocktools::sdk::offsets::VTable::MinecraftUIRenderContextGetTexture]) return {};
    using Fn = TexturePtr (*)(void*, const ResourceLocation&, bool);
    return reinterpret_cast<Fn>(vtable[bedrocktools::sdk::offsets::VTable::MinecraftUIRenderContextGetTexture])(context, location, false);
}

void drawImage(void* context, const ClientTexture& texture, const UiVec2& position, const UiVec2& size) {
    void** vtable = getVtable(context);
    if (!vtable || !vtable[bedrocktools::sdk::offsets::VTable::MinecraftUIRenderContextDrawImage]) return;
    using Fn = void (*)(void*, const ClientTexture&, const UiVec2&, const UiVec2&, const UiVec2&, const UiVec2&, bool);
    static constexpr UiVec2 uv{0.0f, 0.0f};
    static constexpr UiVec2 uvSize{1.0f, 1.0f};
    reinterpret_cast<Fn>(vtable[bedrocktools::sdk::offsets::VTable::MinecraftUIRenderContextDrawImage])(
        context, texture, position, size, uv, uvSize, false);
}

void flushImages(void* context) {
    void** vtable = getVtable(context);
    if (!vtable || !vtable[bedrocktools::sdk::offsets::VTable::MinecraftUIRenderContextFlushImages]) return;
    using Fn = void (*)(void*, const Color&, float, const HashedString&);
    static const HashedString material("ui_flush");
    static constexpr Color color{1.0f, 1.0f, 1.0f, 1.0f};
    reinterpret_cast<Fn>(vtable[bedrocktools::sdk::offsets::VTable::MinecraftUIRenderContextFlushImages])(
        context, color, 1.0f, material);
}

// Draws a solid UI rect in the same pass as effect icons so the card sits
// UNDER the native textures (mod-menu DrawCommands always composite on top).
void fillRectangle(void* context, const RectangleArea& area, const Color& color) {
    void** vtable = getVtable(context);
    if (!vtable || !vtable[bedrocktools::sdk::offsets::VTable::MinecraftUIRenderContextFillRectangle]) return;
    using Fn = void (*)(void*, const RectangleArea&, const Color&);
    reinterpret_cast<Fn>(vtable[bedrocktools::sdk::offsets::VTable::MinecraftUIRenderContextFillRectangle])(
        context, area, color);
}

Color colorFromArgb(std::uint32_t argb) {
    return Color{
        static_cast<float>((argb >> 16) & 0xFFu) / 255.0f,
        static_cast<float>((argb >> 8) & 0xFFu) / 255.0f,
        static_cast<float>(argb & 0xFFu) / 255.0f,
        static_cast<float>((argb >> 24) & 0xFFu) / 255.0f,
    };
}

std::uint32_t parseColor(const std::string& value, std::uint32_t fallback) {
    if (value.empty()) return fallback;
    const std::string hex = value[0] == '#' ? value.substr(1) : value;
    try {
        if (hex.size() == 6) return 0xFF000000u | static_cast<std::uint32_t>(std::stoul(hex, nullptr, 16));
        if (hex.size() == 8) return static_cast<std::uint32_t>(std::stoul(hex, nullptr, 16));
    } catch (...) {
    }
    return fallback;
}

std::string_view effectName(std::uint32_t id) {
    switch (static_cast<MobEffectType>(id)) {
        case MobEffectType::Speed: return "Speed";
        case MobEffectType::Slowness: return "Slowness";
        case MobEffectType::Haste: return "Haste";
        case MobEffectType::MiningFatigue: return "Mining Fatigue";
        case MobEffectType::Strength: return "Strength";
        case MobEffectType::InstantHealth: return "Instant Health";
        case MobEffectType::InstantDamage: return "Instant Damage";
        case MobEffectType::JumpBoost: return "Jump Boost";
        case MobEffectType::Nausea: return "Nausea";
        case MobEffectType::Regeneration: return "Regeneration";
        case MobEffectType::Resistance: return "Resistance";
        case MobEffectType::FireResistance: return "Fire Resistance";
        case MobEffectType::WaterBreathing: return "Water Breathing";
        case MobEffectType::Invisibility: return "Invisibility";
        case MobEffectType::Blindness: return "Blindness";
        case MobEffectType::NightVision: return "Night Vision";
        case MobEffectType::Hunger: return "Hunger";
        case MobEffectType::Weakness: return "Weakness";
        case MobEffectType::Poison: return "Poison";
        case MobEffectType::Wither: return "Wither";
        case MobEffectType::HealthBoost: return "Health Boost";
        case MobEffectType::Absorption: return "Absorption";
        case MobEffectType::Saturation: return "Saturation";
        case MobEffectType::Levitation: return "Levitation";
        case MobEffectType::FatalPoison: return "Fatal Poison";
        case MobEffectType::ConduitPower: return "Conduit Power";
        case MobEffectType::SlowFalling: return "Slow Falling";
        case MobEffectType::BadOmen: return "Bad Omen";
        case MobEffectType::VillageHero: return "Village Hero";
        case MobEffectType::Darkness: return "Darkness";
        case MobEffectType::TrialOmen: return "Trial Omen";
        case MobEffectType::WindCharged: return "Wind Charged";
        case MobEffectType::Weaving: return "Weaving";
        case MobEffectType::Oozing: return "Oozing";
        case MobEffectType::Infested: return "Infested";
        case MobEffectType::RaidOmen: return "Raid Omen";
        case MobEffectType::BreathOfTheNautilus: return "Breath of the Nautilus";
        default: return "Unknown Effect";
    }
}

std::string_view effectTexturePath(std::uint32_t id) {
    switch (static_cast<MobEffectType>(id)) {
        case MobEffectType::Speed: return "textures/ui/speed_effect";
        case MobEffectType::Slowness: return "textures/ui/slowness_effect";
        case MobEffectType::Haste: return "textures/ui/haste_effect";
        case MobEffectType::MiningFatigue: return "textures/ui/mining_fatigue_effect";
        case MobEffectType::Strength: return "textures/ui/strength_effect";
        case MobEffectType::InstantHealth: return "textures/ui/instant_health_effect";
        case MobEffectType::InstantDamage: return "textures/ui/instant_damage_effect";
        case MobEffectType::JumpBoost: return "textures/ui/jump_boost_effect";
        case MobEffectType::Nausea: return "textures/ui/nausea_effect";
        case MobEffectType::Regeneration: return "textures/ui/regeneration_effect";
        case MobEffectType::Resistance: return "textures/ui/resistance_effect";
        case MobEffectType::FireResistance: return "textures/ui/fire_resistance_effect";
        case MobEffectType::WaterBreathing: return "textures/ui/water_breathing_effect";
        case MobEffectType::Invisibility: return "textures/ui/invisibility_effect";
        case MobEffectType::Blindness: return "textures/ui/blindness_effect";
        case MobEffectType::NightVision: return "textures/ui/night_vision_effect";
        case MobEffectType::Hunger: return "textures/ui/hunger_effect";
        case MobEffectType::Weakness: return "textures/ui/weakness_effect";
        case MobEffectType::Poison: return "textures/ui/poison_effect";
        case MobEffectType::Wither: return "textures/ui/wither_effect";
        case MobEffectType::HealthBoost: return "textures/ui/health_boost_effect";
        case MobEffectType::Absorption: return "textures/ui/absorption_effect";
        case MobEffectType::Saturation: return "textures/ui/saturation_effect";
        case MobEffectType::Levitation: return "textures/ui/levitation_effect";
        case MobEffectType::FatalPoison: return "textures/ui/fatal_poison_effect";
        case MobEffectType::ConduitPower: return "textures/ui/conduit_power_effect";
        case MobEffectType::SlowFalling: return "textures/ui/slow_falling_effect";
        case MobEffectType::BadOmen: return "textures/ui/bad_omen_effect";
        case MobEffectType::VillageHero: return "textures/ui/village_hero_effect";
        case MobEffectType::Darkness: return "textures/ui/darkness_effect";
        case MobEffectType::TrialOmen: return "textures/ui/trial_omen_effect";
        case MobEffectType::WindCharged: return "textures/ui/wind_charged_effect";
        case MobEffectType::Weaving: return "textures/ui/weaving_effect";
        case MobEffectType::Oozing: return "textures/ui/oozing_effect";
        case MobEffectType::Infested: return "textures/ui/infested_effect";
        case MobEffectType::RaidOmen: return "textures/ui/raid_omen_effect";
        case MobEffectType::BreathOfTheNautilus: return "textures/ui/breath_of_the_nautilus_effect";
        default: return {};
    }
}

bool usesNativeTexture(std::uint32_t id) {
    return id != static_cast<std::uint32_t>(MobEffectType::InstantHealth) &&
           id != static_cast<std::uint32_t>(MobEffectType::InstantDamage) &&
           id != static_cast<std::uint32_t>(MobEffectType::Saturation);
}

const char* fallbackImageId(std::uint32_t id) {
    switch (static_cast<MobEffectType>(id)) {
        case MobEffectType::Speed: return ImgSpeed;
        case MobEffectType::Slowness: return ImgSlowness;
        case MobEffectType::Haste: return ImgHaste;
        case MobEffectType::MiningFatigue: return ImgMiningFatigue;
        case MobEffectType::Strength: return ImgStrength;
        case MobEffectType::InstantHealth: return ImgInstantHealth;
        case MobEffectType::InstantDamage: return ImgInstantDamage;
        case MobEffectType::JumpBoost: return ImgJumpBoost;
        case MobEffectType::Nausea: return ImgNausea;
        case MobEffectType::Regeneration: return ImgRegeneration;
        case MobEffectType::Resistance: return ImgResistance;
        case MobEffectType::FireResistance: return ImgFireResistance;
        case MobEffectType::WaterBreathing: return ImgWaterBreathing;
        case MobEffectType::Invisibility: return ImgInvisibility;
        case MobEffectType::Blindness: return ImgBlindness;
        case MobEffectType::NightVision: return ImgNightVision;
        case MobEffectType::Hunger: return ImgHunger;
        case MobEffectType::Weakness: return ImgWeakness;
        case MobEffectType::Poison: return ImgPoison;
        case MobEffectType::Wither: return ImgWither;
        case MobEffectType::HealthBoost: return ImgHealthBoost;
        case MobEffectType::Absorption: return ImgAbsorption;
        case MobEffectType::Saturation: return ImgSaturation;
        case MobEffectType::Levitation: return ImgLevitation;
        case MobEffectType::FatalPoison: return ImgFatalPoison;
        case MobEffectType::ConduitPower: return ImgConduitPower;
        case MobEffectType::SlowFalling: return ImgSlowFalling;
        case MobEffectType::BadOmen: return ImgBadOmen;
        case MobEffectType::VillageHero: return ImgVillageHero;
        case MobEffectType::Darkness: return ImgDarkness;
        case MobEffectType::TrialOmen: return ImgTrialOmen;
        case MobEffectType::WindCharged: return ImgWindCharged;
        case MobEffectType::Weaving: return ImgWeaving;
        case MobEffectType::Oozing: return ImgOozing;
        case MobEffectType::Infested: return ImgInfested;
        case MobEffectType::RaidOmen: return ImgRaidOmen;
        case MobEffectType::BreathOfTheNautilus: return ImgBreathNautilus;
        default: return GenericPotionImageId;
    }
}

std::string romanNumeral(int value) {
    if (value <= 0 || value > 3999) return std::to_string(value);
    static constexpr std::pair<int, std::string_view> table[] = {
        {1000, "M"}, {900, "CM"}, {500, "D"}, {400, "CD"}, {100, "C"}, {90, "XC"},
        {50, "L"}, {40, "XL"}, {10, "X"}, {9, "IX"}, {5, "V"}, {4, "IV"}, {1, "I"}
    };
    std::string result;
    for (const auto& [number, numeral] : table) {
        while (value >= number) {
            result += numeral;
            value -= number;
        }
    }
    return result;
}

std::string compactRoman(int value) {
    switch (value) {
        case 1: return "I";
        case 2: return "II";
        case 3: return "III";
        case 4: return "IV";
        case 5: return "V";
        default: return std::to_string(value);
    }
}

std::string formatDuration(int duration, bool noCounter) {
    if (noCounter || duration == -1) return "\xE2\x88\x9E";
    const int totalSeconds = std::max(0, duration / 20);
    const int minutes = totalSeconds / 60;
    const int seconds = totalSeconds % 60;
    std::string result = std::to_string(minutes) + ":";
    if (seconds < 10) result += '0';
    result += std::to_string(seconds);
    return result;
}

void* hudMobEffectsRendererDetour(void* renderer, void* context, void* client, void* owner, int pass, void* renderAabb) {
    if (moduleInstance && moduleInstance->enabled && moduleInstance->renderNative(context, client)) return nullptr;
    if (!hudMobEffectsRendererOriginal) return nullptr;
    return hudMobEffectsRendererOriginal(renderer, context, client, owner, pass, renderAabb);
}

}

PotionHudModule::PotionHudModule()
    : Module("PotionHUD", "Displays active potion effects with icons, names, amplifiers, and timers.") {
    moduleInstance = this;
}

PotionHudModule::~PotionHudModule() {
    if (hudMobEffectsRendererHook) {
        bedrocktools::hooks::remove(hudMobEffectsRendererHook);
        hudMobEffectsRendererHook = nullptr;
        hudMobEffectsRendererOriginal = nullptr;
    }
    if (moduleInstance == this) moduleInstance = nullptr;
}

void PotionHudModule::onInit() {
    // Unique monochrome icon per effect (drawn on top of the card).
    auto reg = [](const char* id, const auto& pixels, int w, int h) {
        pl::modmenu::registerImage(id, pixels, w, h);
    };
    reg(ImgSpeed, potionhud_assets::SpeedPixels, potionhud_assets::SpeedWidth, potionhud_assets::SpeedHeight);
    reg(ImgSlowness, potionhud_assets::SlownessPixels, potionhud_assets::SlownessWidth, potionhud_assets::SlownessHeight);
    reg(ImgHaste, potionhud_assets::HastePixels, potionhud_assets::HasteWidth, potionhud_assets::HasteHeight);
    reg(ImgMiningFatigue, potionhud_assets::MiningFatiguePixels, potionhud_assets::MiningFatigueWidth, potionhud_assets::MiningFatigueHeight);
    reg(ImgStrength, potionhud_assets::StrengthPixels, potionhud_assets::StrengthWidth, potionhud_assets::StrengthHeight);
    reg(ImgInstantHealth, potionhud_assets::InstantHealthPixels, potionhud_assets::InstantHealthWidth, potionhud_assets::InstantHealthHeight);
    reg(ImgInstantDamage, potionhud_assets::InstantDamagePixels, potionhud_assets::InstantDamageWidth, potionhud_assets::InstantDamageHeight);
    reg(ImgJumpBoost, potionhud_assets::JumpBoostPixels, potionhud_assets::JumpBoostWidth, potionhud_assets::JumpBoostHeight);
    reg(ImgNausea, potionhud_assets::NauseaPixels, potionhud_assets::NauseaWidth, potionhud_assets::NauseaHeight);
    reg(ImgRegeneration, potionhud_assets::RegenerationPixels, potionhud_assets::RegenerationWidth, potionhud_assets::RegenerationHeight);
    reg(ImgResistance, potionhud_assets::ResistancePixels, potionhud_assets::ResistanceWidth, potionhud_assets::ResistanceHeight);
    reg(ImgFireResistance, potionhud_assets::FireResistancePixels, potionhud_assets::FireResistanceWidth, potionhud_assets::FireResistanceHeight);
    reg(ImgWaterBreathing, potionhud_assets::WaterBreathingPixels, potionhud_assets::WaterBreathingWidth, potionhud_assets::WaterBreathingHeight);
    reg(ImgInvisibility, potionhud_assets::InvisibilityPixels, potionhud_assets::InvisibilityWidth, potionhud_assets::InvisibilityHeight);
    reg(ImgBlindness, potionhud_assets::BlindnessPixels, potionhud_assets::BlindnessWidth, potionhud_assets::BlindnessHeight);
    reg(ImgNightVision, potionhud_assets::NightVisionPixels, potionhud_assets::NightVisionWidth, potionhud_assets::NightVisionHeight);
    reg(ImgHunger, potionhud_assets::HungerPixels, potionhud_assets::HungerWidth, potionhud_assets::HungerHeight);
    reg(ImgWeakness, potionhud_assets::WeaknessPixels, potionhud_assets::WeaknessWidth, potionhud_assets::WeaknessHeight);
    reg(ImgPoison, potionhud_assets::PoisonPixels, potionhud_assets::PoisonWidth, potionhud_assets::PoisonHeight);
    reg(ImgWither, potionhud_assets::WitherPixels, potionhud_assets::WitherWidth, potionhud_assets::WitherHeight);
    reg(ImgHealthBoost, potionhud_assets::HealthBoostPixels, potionhud_assets::HealthBoostWidth, potionhud_assets::HealthBoostHeight);
    reg(ImgAbsorption, potionhud_assets::AbsorptionPixels, potionhud_assets::AbsorptionWidth, potionhud_assets::AbsorptionHeight);
    reg(ImgSaturation, potionhud_assets::SaturationPixels, potionhud_assets::SaturationWidth, potionhud_assets::SaturationHeight);
    reg(ImgLevitation, potionhud_assets::LevitationPixels, potionhud_assets::LevitationWidth, potionhud_assets::LevitationHeight);
    reg(ImgFatalPoison, potionhud_assets::FatalPoisonPixels, potionhud_assets::FatalPoisonWidth, potionhud_assets::FatalPoisonHeight);
    reg(ImgConduitPower, potionhud_assets::ConduitPowerPixels, potionhud_assets::ConduitPowerWidth, potionhud_assets::ConduitPowerHeight);
    reg(ImgSlowFalling, potionhud_assets::SlowFallingPixels, potionhud_assets::SlowFallingWidth, potionhud_assets::SlowFallingHeight);
    reg(ImgBadOmen, potionhud_assets::BadOmenPixels, potionhud_assets::BadOmenWidth, potionhud_assets::BadOmenHeight);
    reg(ImgVillageHero, potionhud_assets::VillageHeroPixels, potionhud_assets::VillageHeroWidth, potionhud_assets::VillageHeroHeight);
    reg(ImgDarkness, potionhud_assets::DarknessPixels, potionhud_assets::DarknessWidth, potionhud_assets::DarknessHeight);
    reg(ImgTrialOmen, potionhud_assets::TrialOmenPixels, potionhud_assets::TrialOmenWidth, potionhud_assets::TrialOmenHeight);
    reg(ImgWindCharged, potionhud_assets::WindChargedPixels, potionhud_assets::WindChargedWidth, potionhud_assets::WindChargedHeight);
    reg(ImgWeaving, potionhud_assets::WeavingPixels, potionhud_assets::WeavingWidth, potionhud_assets::WeavingHeight);
    reg(ImgOozing, potionhud_assets::OozingPixels, potionhud_assets::OozingWidth, potionhud_assets::OozingHeight);
    reg(ImgInfested, potionhud_assets::InfestedPixels, potionhud_assets::InfestedWidth, potionhud_assets::InfestedHeight);
    reg(ImgRaidOmen, potionhud_assets::RaidOmenPixels, potionhud_assets::RaidOmenWidth, potionhud_assets::RaidOmenHeight);
    reg(ImgBreathNautilus, potionhud_assets::BreathOfTheNautilusPixels, potionhud_assets::BreathOfTheNautilusWidth, potionhud_assets::BreathOfTheNautilusHeight);
    reg(GenericPotionImageId, potionhud_assets::GenericPotionPixels, potionhud_assets::GenericPotionWidth, potionhud_assets::GenericPotionHeight);

    const std::uintptr_t renderer = pl::memory::resolveVtableFunction(
        "21HudMobEffectsRenderer",
        bedrocktools::sdk::offsets::VTable::HudMobEffectsRendererRender,
        MinecraftLibrary);
    if (renderer && !hudMobEffectsRendererHook) {
        hudMobEffectsRendererHook = bedrocktools::hooks::install(
            reinterpret_cast<void*>(renderer),
            reinterpret_cast<void*>(hudMobEffectsRendererDetour),
            reinterpret_cast<void**>(&hudMobEffectsRendererOriginal));
    }
}

void PotionHudModule::onMenuRegistered() {
    using namespace pl::modmenu;
    ConfigSchemaBuilder schema;
    schema.defaultCategory("effects")
        .category("effects", "Effects", "Choose what PotionHUD displays")
        .category("layout", "Layout", "Size, order, spacing, and text placement")
        .category("text", "Text", "Timer and label appearance")
        .category("colors", "Colors", "Normal, expiring, and shadow colors")
        .category("editor", "HUD Editor", "Placement and snapping while editing the HUD");

    auto node = [](std::string key, std::string title, std::string category, ConfigControlTypeV2 type) {
        ConfigNodeV2 value;
        value.id = key;
        value.key = std::move(key);
        value.title = std::move(title);
        value.category = std::move(category);
        value.type = type;
        return value;
    };
    auto section = [&](const char* id, const char* title, const char* category) {
        auto value = node(id, title, category, ConfigControlTypeV2::Section);
        value.key.clear();
        schema.node(std::move(value));
    };
    auto toggle = [&](const char* key, const char* title, const char* category, const char* sectionId) {
        auto value = node(key, title, category, ConfigControlTypeV2::Toggle);
        value.section = sectionId;
        schema.node(std::move(value));
    };
    auto slider = [&](const char* key, const char* title, const char* category, const char* sectionId,
                      const char* min, const char* max, const char* step, const char* unit) {
        auto value = node(key, title, category, ConfigControlTypeV2::SliderFloat);
        value.section = sectionId;
        value.minValue = min;
        value.maxValue = max;
        value.step = step;
        value.unit = unit;
        schema.node(std::move(value));
    };

    section("display", "Display", "effects");
    toggle("m_showText", "Show Text", "effects", "display");
    auto title = node("m_showTitle", "Show Effect Name", "effects", ConfigControlTypeV2::Toggle);
    title.section = "display";
    title.visibleWhen = {{"m_showText", ConfigConditionOpV2::Truthy, {}}};
    schema.node(std::move(title));
    auto roman = node("m_useRoman", "Use Roman Numerals", "effects", ConfigControlTypeV2::Toggle);
    roman.section = "display";
    roman.visibleWhen = {{"m_showText", ConfigConditionOpV2::Truthy, {}}};
    schema.node(std::move(roman));
    auto romanFull = node("m_useRomanFull", "Roman Numerals Above V", "effects", ConfigControlTypeV2::Toggle);
    romanFull.section = "display";
    romanFull.visibleWhen = {
        {"m_showText", ConfigConditionOpV2::Truthy, {}},
        {"m_useRoman", ConfigConditionOpV2::Truthy, {}}
    };
    schema.node(std::move(romanFull));
    section("activation", "Shortcut", "effects");
    auto keybind = node("keybind", "Toggle Keybind", "effects", ConfigControlTypeV2::Keybind);
    keybind.section = "activation";
    schema.node(std::move(keybind));

    section("effect_layout", "Effect Stack", "layout");
    slider("m_uiScale", "UI Scale", "layout", "effect_layout", "0.5", "3", "0.05", "x");
    slider("m_spacing", "Row Spacing", "layout", "effect_layout", "0.25", "3", "0.05", "x");
    toggle("m_bottomUp", "Bottom Up", "layout", "effect_layout");
    section("text_layout", "Text Placement", "layout");
    auto side = node("m_textSide", "Text Side", "layout", ConfigControlTypeV2::Choice);
    side.section = "text_layout";
    side.choiceStyle = ConfigChoiceStyleV2::Segmented;
    side.options = {
        {"0", "Right", {}, {}, false, {}, false},
        {"1", "Left", {}, {}, false, {}, false}
    };
    side.defaultValue = "0";
    side.visibleWhen = {{"m_showText", ConfigConditionOpV2::Truthy, {}}};
    schema.node(std::move(side));
    auto textOffset = node("m_textOffsetX", "Distance From Icon", "layout", ConfigControlTypeV2::SliderFloat);
    textOffset.section = "text_layout";
    textOffset.minValue = "0";
    textOffset.maxValue = "20";
    textOffset.step = "0.5";
    textOffset.unit = " px";
    textOffset.visibleWhen = {{"m_showText", ConfigConditionOpV2::Truthy, {}}};
    schema.node(std::move(textOffset));

    section("text_style", "Text Appearance", "text");
    auto textSize = node("m_textSize", "Text Size", "text", ConfigControlTypeV2::SliderFloat);
    textSize.section = "text_style";
    textSize.minValue = "4";
    textSize.maxValue = "20";
    textSize.step = "0.5";
    textSize.unit = " px";
    textSize.visibleWhen = {{"m_showText", ConfigConditionOpV2::Truthy, {}}};
    schema.node(std::move(textSize));
    auto shadow = node("m_textShadow", "Text Shadow", "text", ConfigControlTypeV2::Toggle);
    shadow.section = "text_style";
    shadow.visibleWhen = {{"m_showText", ConfigConditionOpV2::Truthy, {}}};
    schema.node(std::move(shadow));
    auto shadowOffset = node("m_shadowOffset", "Shadow Offset", "text", ConfigControlTypeV2::SliderFloat);
    shadowOffset.section = "text_style";
    shadowOffset.minValue = "0.25";
    shadowOffset.maxValue = "5";
    shadowOffset.step = "0.25";
    shadowOffset.unit = " px";
    shadowOffset.visibleWhen = {
        {"m_showText", ConfigConditionOpV2::Truthy, {}},
        {"m_textShadow", ConfigConditionOpV2::Truthy, {}}
    };
    schema.node(std::move(shadowOffset));

    section("text_colors", "Text Colors", "colors");
    auto mainColor = node("m_mainColor", "Main Color", "colors", ConfigControlTypeV2::Color);
    mainColor.section = "text_colors";
    mainColor.defaultValue = "#FFFFFFFF";
    mainColor.visibleWhen = {{"m_showText", ConfigConditionOpV2::Truthy, {}}};
    schema.node(std::move(mainColor));
    auto lowColor = node("m_lowColor", "Effect About To Expire", "colors", ConfigControlTypeV2::Color);
    lowColor.section = "text_colors";
    lowColor.defaultValue = "#FFFF4040";
    lowColor.visibleWhen = {{"m_showText", ConfigConditionOpV2::Truthy, {}}};
    schema.node(std::move(lowColor));
    auto shadowColor = node("m_shadowColor", "Shadow Color", "colors", ConfigControlTypeV2::Color);
    shadowColor.section = "text_colors";
    shadowColor.defaultValue = "#8C000000";
    shadowColor.visibleWhen = {
        {"m_showText", ConfigConditionOpV2::Truthy, {}},
        {"m_textShadow", ConfigConditionOpV2::Truthy, {}}
    };
    schema.node(std::move(shadowColor));

    auto help = node("editor_help", "Move PotionHUD In The HUD Editor", "editor", ConfigControlTypeV2::Info);
    help.key.clear();
    help.description = "The editor moves the whole active-effect stack as one HUD element.";
    schema.node(std::move(help));
    section("snapping", "Snapping", "editor");
    auto snapping = node("snap_targets", "Snap To", "editor", ConfigControlTypeV2::ToggleGroup);
    snapping.key.clear();
    snapping.section = "snapping";
    snapping.choiceStyle = ConfigChoiceStyleV2::Chips;
    snapping.options = {
        {"grid", "Grid", {}, "m_snapToGrid", false, {}, false},
        {"elements", "Other Elements", {}, "m_snapToElements", false, {}, false},
        {"center", "Screen Center", {}, "m_snapToScreenCenter", false, {}, false}
    };
    schema.node(std::move(snapping));
    slider("m_gridSize", "Grid Size", "editor", "snapping", "1", "100", "1", " px");
    slider("m_gridGap", "Gap Between Elements", "editor", "snapping", "0", "100", "1", " px");
    slider("m_snapThreshold", "Snap Distance", "editor", "snapping", "1", "100", "1", " px");

    schema.category("card", "Card", "Panel background, size, and rounding");
    section("card_panel", "Panel", "card");
    toggle("m_showCard", "Show Card", "card", "card_panel");
    toggle("m_showHeader", "Show Header", "card", "card_panel");
    auto cardColor = node("m_cardColor", "Card Color", "card", ConfigControlTypeV2::Color);
    cardColor.section = "card_panel";
    cardColor.defaultValue = "#FF0A0A0E";
    cardColor.visibleWhen = {{"m_showCard", ConfigConditionOpV2::Truthy, {}}};
    schema.node(std::move(cardColor));
    section("card_size", "Size & Rounding", "card");
    auto cardWidth = node("m_cardWidth", "Card Width", "card", ConfigControlTypeV2::SliderFloat);
    cardWidth.section = "card_size";
    cardWidth.minValue = "0";
    cardWidth.maxValue = "400";
    cardWidth.step = "1";
    cardWidth.unit = " px";
    cardWidth.description = "0 = auto-fit content";
    cardWidth.visibleWhen = {{"m_showCard", ConfigConditionOpV2::Truthy, {}}};
    schema.node(std::move(cardWidth));
    auto cardHeight = node("m_cardHeight", "Card Height", "card", ConfigControlTypeV2::SliderFloat);
    cardHeight.section = "card_size";
    cardHeight.minValue = "0";
    cardHeight.maxValue = "600";
    cardHeight.step = "1";
    cardHeight.unit = " px";
    cardHeight.description = "0 = auto-fit content";
    cardHeight.visibleWhen = {{"m_showCard", ConfigConditionOpV2::Truthy, {}}};
    schema.node(std::move(cardHeight));
    slider("m_cardRadius", "Corner Rounding", "card", "card_size", "0", "48", "0.5", " px");
    slider("m_cardPadding", "Inner Padding", "card", "card_size", "0", "40", "0.5", " px");

    schema.category("capsules", "Capsules", "Grey-black pills behind header, effects, and timers");
    section("capsule_header", "Active Potions Header", "capsules");
    toggle("m_showHeaderCapsule", "Header Capsule", "capsules", "capsule_header");
    auto headerCapColor = node("m_headerCapsuleColor", "Header Capsule Color", "capsules", ConfigControlTypeV2::Color);
    headerCapColor.section = "capsule_header";
    headerCapColor.defaultValue = "#FF1A1A20";
    headerCapColor.visibleWhen = {{"m_showHeaderCapsule", ConfigConditionOpV2::Truthy, {}}};
    schema.node(std::move(headerCapColor));
    slider("m_headerCapsuleRadius", "Header Capsule Rounding", "capsules", "capsule_header", "0", "24", "0.5", " px");
    section("capsule_effect", "Effect Row", "capsules");
    toggle("m_showEffectCapsule", "Effect Capsule", "capsules", "capsule_effect");
    auto effectCapColor = node("m_effectCapsuleColor", "Effect Capsule Color", "capsules", ConfigControlTypeV2::Color);
    effectCapColor.section = "capsule_effect";
    effectCapColor.defaultValue = "#FF1A1A20";
    effectCapColor.visibleWhen = {{"m_showEffectCapsule", ConfigConditionOpV2::Truthy, {}}};
    schema.node(std::move(effectCapColor));
    slider("m_effectCapsuleRadius", "Effect Capsule Rounding", "capsules", "capsule_effect", "0", "24", "0.5", " px");
    section("capsule_timer", "Timer", "capsules");
    toggle("m_showTimerCapsule", "Timer Capsule", "capsules", "capsule_timer");
    auto timerCapColor = node("m_timerCapsuleColor", "Timer Capsule Color", "capsules", ConfigControlTypeV2::Color);
    timerCapColor.section = "capsule_timer";
    timerCapColor.defaultValue = "#FF1A1A20";
    timerCapColor.visibleWhen = {{"m_showTimerCapsule", ConfigConditionOpV2::Truthy, {}}};
    schema.node(std::move(timerCapColor));
    slider("m_timerCapsuleRadius", "Timer Capsule Rounding", "capsules", "capsule_timer", "0", "24", "0.5", " px");
    slider("m_rowGap", "Gap Between Rows", "capsules", "capsule_effect", "0", "20", "0.5", " px");

    pl::modmenu::setConfigSchemaJson(moduleId, schema.toJson());
}

void PotionHudModule::onDisable() {
    clearRuntime();
    pl::modmenu::submitDrawCommands(moduleId, std::span<const pl::modmenu::DrawCommand>{});
    pl::modmenu::submitHudEditorElements(moduleId, std::span<const pl::modmenu::HudEditorElement>{});
}

PotionHudModule::ConfigSnapshot PotionHudModule::snapshotConfig() const {
    std::lock_guard lock(m_configMutex);
    return {
        hudPosX,
        hudPosY,
        m_uiScale,
        m_spacing,
        m_bottomUp,
        m_showText,
        m_showTitle,
        m_useRoman,
        m_useRomanFull,
        m_textSize,
        m_textOffsetX,
        m_textSide,
        m_textShadow,
        m_shadowOffset,
        parseColor(m_mainColor, 0xFFFFFFFFu),
        parseColor(m_lowColor, 0xFFFF4040u),
        parseColor(m_shadowColor, 0x8C000000u),
        m_gridSize,
        m_gridGap,
        m_snapThreshold,
        (m_snapToGrid ? pl::modmenu::HudSnapGrid : pl::modmenu::HudSnapNone) |
            (m_snapToElements ? pl::modmenu::HudSnapElements : pl::modmenu::HudSnapNone) |
            (m_snapToScreenCenter ? pl::modmenu::HudSnapScreenCenter : pl::modmenu::HudSnapNone),
        m_showCard,
        parseColor(m_cardColor, 0xFF0A0A0Eu),
        m_cardRadius,
        m_cardPadding,
        m_cardWidth,
        m_cardHeight,
        m_showHeader,
        m_singleLineRow,
        m_showHeaderCapsule,
        parseColor(m_headerCapsuleColor, 0xFF1A1A20u),
        m_headerCapsuleRadius,
        m_showEffectCapsule,
        parseColor(m_effectCapsuleColor, 0xFF1A1A20u),
        m_effectCapsuleRadius,
        m_showTimerCapsule,
        parseColor(m_timerCapsuleColor, 0xFF1A1A20u),
        m_timerCapsuleRadius,
        m_showRowCapsule,
        parseColor(m_rowCapsuleColor, 0x26FFFFFFu),
        m_iconOpacity,
        m_rowGap,
        m_showOutline,
        parseColor(m_outlineColor, 0x66C8C8C8u),
        m_outlineThickness,
        m_blurAmount,
        m_animate,
        m_animationDurationMs
    };
}

std::vector<PotionHudModule::RuntimeEffect> PotionHudModule::snapshotRuntime(float& surfaceScale) const {
    std::lock_guard lock(m_runtimeMutex);
    surfaceScale = m_surfaceScale;
    return m_runtimeEffects;
}

void PotionHudModule::clearRuntime() {
    std::lock_guard lock(m_runtimeMutex);
    m_runtimeEffects.clear();
    m_surfaceScale = 1.0f;
    m_runtimeValid.store(false, std::memory_order_release);
}

float PotionHudModule::iconSurfaceSize(const ConfigSnapshot& config, float surfaceScale) {
    return std::max(1.0f, VanillaEffectSize * config.uiScale * std::max(0.1f, surfaceScale));
}

std::string PotionHudModule::titleForEffect(const RuntimeEffect& effect, const ConfigSnapshot& config) {
    const int level = effect.amplifier + 1;
    std::string result(effectName(effect.id));
    result += ' ';
    if (!config.useRoman) result += std::to_string(level);
    else if (config.useRomanFull) result += romanNumeral(level);
    else result += compactRoman(level);
    return result;
}

float PotionHudModule::rowSurfaceHeight(const ConfigSnapshot& config, float surfaceScale) {
    const float icon = iconSurfaceSize(config, surfaceScale);
    if (!config.showText) return icon;
    const float textSize = config.textSize * config.uiScale * surfaceScale;
    const float timerSize = textSize * 0.8f;
    // Single-line mode (name + timer on one row) keeps rows compact like the reference.
    const float textHeight = !config.showTitle
        ? timerSize
        : (config.singleLineRow ? textSize : textSize + timerSize * 1.15f);
    return std::max(icon, std::max(1.0f, textHeight));
}

float PotionHudModule::textSurfaceWidth(const ConfigSnapshot& config, const std::vector<RuntimeEffect>& effects, float surfaceScale) {
    if (!config.showText || effects.empty()) return 0.0f;
    const float textSize = config.textSize * config.uiScale * surfaceScale;
    const float timerSize = textSize * 0.8f;
    float width = timerSize * 5.0f * 0.56f;
    if (config.showTitle) {
        for (const auto& effect : effects) {
            width = std::max(width, static_cast<float>(titleForEffect(effect, config).size()) * textSize * 0.56f);
        }
    }
    return std::max(1.0f, width + surfaceScale * config.uiScale * 2.0f);
}

void PotionHudModule::submitEditorElement(const ConfigSnapshot& config, const std::vector<RuntimeEffect>& effects, float surfaceScale) {
    const float icon = iconSurfaceSize(config, surfaceScale);
    const float textWidth = textSurfaceWidth(config, effects, surfaceScale);
    const float gap = config.showText ? config.textOffsetX * config.uiScale * surfaceScale : 0.0f;
    const float rowHeight = rowSurfaceHeight(config, surfaceScale);
    const float rowStride = rowHeight * std::max(0.25f, config.spacing);
    const std::size_t count = effects.size();
    const float padding = config.cardPadding * config.uiScale * surfaceScale;
    const float textSize = std::max(1.0f, config.textSize * config.uiScale * surfaceScale);
    const float headerIconSize = config.showHeader ? textSize * 1.25f : 0.0f;
    const float headerHeight = (config.showHeader && count > 0)
        ? std::max(headerIconSize, textSize) + padding * 0.85f
        : 0.0f;

    pl::modmenu::HudEditorElement element;
    element.elementId = "bedrocktools.potionhud.effects";
    element.displayName = "PotionHUD";
    element.positionKeyX = "hudPosX";
    element.positionKeyY = "hudPosY";
    element.x = config.hudPosX;
    element.y = config.hudPosY;
    element.width = std::max(1.0f, icon + (config.showText && !effects.empty() ? gap + textWidth : 0.0f));
    // Match the drawn card: header + one stride per effect + padding.
    element.height = std::max(1.0f, headerHeight + static_cast<float>(count) * rowStride + padding * 2.0f);
    element.gridSize = config.gridSize;
    element.gridGap = config.gridGap;
    element.snapThreshold = config.snapThreshold;
    element.snapFlags = config.snapFlags;
    pl::modmenu::submitHudEditorElements(moduleId, std::span<const pl::modmenu::HudEditorElement>(&element, 1));
}

bool PotionHudModule::renderNative(void* context, void* client) {
    auto fail = [&]() {
        clearRuntime();
        return false;
    };
    if (!context || !client) return fail();
    void* player = getLocalPlayer(client);
    if (!player) return fail();
    MobEffectsComponent* component = getMobEffects(player);
    if (!component) return fail();

    std::vector<RawEffect> rawEffects;
    if (!copyEffects(component, rawEffects)) return fail();
    std::vector<RuntimeEffect> effects;
    effects.reserve(rawEffects.size());
    for (const auto& effect : rawEffects) effects.push_back({effect.id, effect.duration, effect.amplifier, effect.noCounter, false});

    const pl::modmenu::HudSurfaceSize surface = pl::modmenu::getHudSurfaceSize();
    const RectangleArea full = getFullClippingRectangle(context);
    if (surface.width <= 0.0f || surface.height <= 0.0f || !validRectangle(full)) return fail();

    const float uiWidth = full.x1 - full.x0;
    const float uiHeight = full.y1 - full.y0;
    const float scaleX = surface.width / uiWidth;
    const float scaleY = surface.height / uiHeight;
    if (!std::isfinite(scaleX) || !std::isfinite(scaleY) || scaleX <= 0.0f || scaleY <= 0.0f) return fail();
    const float surfaceScale = std::min(scaleX, scaleY);
    const ConfigSnapshot config = snapshotConfig();
    const float icon = iconSurfaceSize(config, surfaceScale);
    const float textWidth = textSurfaceWidth(config, effects, surfaceScale);
    const float gap = config.showText ? config.textOffsetX * config.uiScale * surfaceScale : 0.0f;
    const float iconSurfaceX = config.hudPosX + (config.showText && config.textSide == 1 ? textWidth + gap : 0.0f);
    const float rowHeight = rowSurfaceHeight(config, surfaceScale);
    const float rowStride = rowHeight * std::max(0.25f, config.spacing);
    const float textSize = std::max(1.0f, config.textSize * config.uiScale * surfaceScale);
    const float padding = config.cardPadding * config.uiScale * surfaceScale;
    // Same header offset used by the card/draw-command path so native icons
    // sit inside the card, below the "Potions" header.
    const float headerIconSize = config.showHeader ? textSize * 1.25f : 0.0f;
    const float headerHeight = (config.showHeader && !effects.empty())
        ? std::max(headerIconSize, textSize) + padding * 0.85f
        : 0.0f;
    const float rowStartY = config.hudPosY + headerHeight;
    const float contentWidth = icon + (config.showText ? gap + textWidth : 0.0f);
    const float cardInnerWidth = std::max(contentWidth,
        config.showHeader ? headerIconSize + gap + static_cast<float>(std::string("Active Potions").size()) * textSize * 0.56f : 0.0f);
    // Full vertical span: one rowStride slot per effect.
    const std::size_t rowCount = effects.size();
    const float contentHeight = static_cast<float>(rowCount) * rowStride;

    // Draw the card background in THIS pass (under the icons). Mod-menu
    // DrawCommands always composite above the game HUD, so a card drawn
    // there covers native effect textures. Native fill + native icons keep
    // real coloured icons visible inside the panel.
    if (config.showCard && !effects.empty()) {
        const float cardX = config.hudPosX - padding;
        const float cardY = config.hudPosY - padding;
        const float autoW = cardInnerWidth + padding * 2.0f;
        // Always size to content; fixed config height is a minimum only.
        const float autoH = headerHeight + contentHeight + padding * 2.0f;
        const float cardW = (config.cardWidth > 0.5f)
            ? std::max(autoW, config.cardWidth * config.uiScale * surfaceScale)
            : autoW;
        const float cardH = (config.cardHeight > 0.5f)
            ? std::max(autoH, config.cardHeight * config.uiScale * surfaceScale)
            : autoH;
        RectangleArea cardArea{
            full.x0 + cardX / scaleX,
            full.x0 + (cardX + cardW) / scaleX,
            full.y0 + cardY / scaleY,
            full.y0 + (cardY + cardH) / scaleY,
        };
        if (validRectangle(cardArea)) {
            fillRectangle(context, cardArea, colorFromArgb(config.cardColor));
        }
    }

    // Custom monochrome icons are drawn on the overlay. Skip vanilla
    // effect textures entirely so they never peek through the card.
    for (auto& effect : effects) {
        effect.nativeIcon = false;
    }
    (void)rowStartY;
    (void)iconSurfaceX;
    (void)scaleX;
    (void)scaleY;
    (void)full;
    (void)icon;
    (void)rowStride;

    {
        std::lock_guard lock(m_runtimeMutex);
        m_runtimeEffects = std::move(effects);
        m_surfaceScale = surfaceScale;
    }
    m_runtimeValid.store(true, std::memory_order_release);
    return true;
}

void PotionHudModule::onFrame() {
    if (!enabled) return;
    const ConfigSnapshot config = snapshotConfig();
    float surfaceScale = 1.0f;
    std::vector<RuntimeEffect> effects;
    if (m_runtimeValid.load(std::memory_order_acquire)) effects = snapshotRuntime(surfaceScale);
    submitEditorElement(config, effects, surfaceScale);

    const float icon = iconSurfaceSize(config, surfaceScale);
    const float textWidth = textSurfaceWidth(config, effects, surfaceScale);
    const float gap = config.showText ? config.textOffsetX * config.uiScale * surfaceScale : 0.0f;
    const float rowHeight = rowSurfaceHeight(config, surfaceScale);
    const float rowStride = rowHeight * std::max(0.25f, config.spacing);
    const float textSize = std::max(1.0f, config.textSize * config.uiScale * surfaceScale);
    const float timerSize = std::max(1.0f, textSize * 0.8f);
    const float shadowOffset = config.shadowOffset * config.uiScale * surfaceScale;
    const float padding = config.cardPadding * config.uiScale * surfaceScale;
    const bool hasEffects = !effects.empty();

    // Card/header block - purely additive on top of the existing layout math
    // above. Content width/height reuse the exact same measurements
    // submitEditorElement() already uses, so the card always wraps the real
    // content instead of an independently-guessed size.
    static const std::string headerText = "Active Potions";
    const float headerTextWidth = static_cast<float>(headerText.size()) * textSize * 0.56f;
    const float headerIconSize = config.showHeader ? textSize * 1.25f : 0.0f;
    const float headerHeight = config.showHeader && hasEffects ? std::max(headerIconSize, textSize) + padding * 0.85f : 0.0f;

    const float contentWidth = icon + (config.showText ? gap + textWidth : 0.0f);
    const float cardInnerWidth = std::max(contentWidth, config.showHeader ? headerIconSize + gap + headerTextWidth : 0.0f);
    // Each effect gets a full rowStride slot so icons/text centered in the
    // row never spill past the card bottom when spacing ≠ 1.
    const std::size_t rowCount = effects.size();
    const float contentHeight = static_cast<float>(rowCount) * rowStride;

    const float cardX = config.hudPosX - padding;
    const float cardY = config.hudPosY - padding;
    const float autoCardWidth = cardInnerWidth + padding * 2.0f;
    const float autoCardHeight = headerHeight + contentHeight + padding * 2.0f;
    // 0 = auto-fit content; fixed values are a MINIMUM so more effects
    // always expand the card instead of clipping outside it.
    const float cardWidth = (config.cardWidth > 0.5f)
        ? std::max(autoCardWidth, config.cardWidth * config.uiScale * surfaceScale)
        : autoCardWidth;
    const float cardHeight = (config.cardHeight > 0.5f)
        ? std::max(autoCardHeight, config.cardHeight * config.uiScale * surfaceScale)
        : autoCardHeight;

    // Rows start below the header (if any); the card itself stays anchored
    // at the original hudPosX/hudPosY so existing HUD-editor snap points
    // don't shift.
    const float rowStartY = config.hudPosY + headerHeight;
    const float iconX = config.hudPosX + (config.showText && config.textSide == 1 ? textWidth + gap : 0.0f);
    const float cardRadiusPx = config.cardRadius * config.uiScale * surfaceScale;
    const float outlineThicknessPx = config.outlineThickness * config.uiScale * surfaceScale;

    // Animation bookkeeping: remember when each effect id first appeared so
    // rows can fade/slide in instead of popping in solid. Keyed by effect
    // id, which is safe here because the local player can only have one
    // active instance of a given effect type at a time.
    const auto nowMs = []() -> std::int64_t {
        using namespace std::chrono;
        return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
    };
    const std::int64_t now = nowMs();
    if (config.animate) {
        for (const auto& effect : effects) {
            m_effectFirstSeenMs.try_emplace(effect.id, now);
        }
        for (auto it = m_effectFirstSeenMs.begin(); it != m_effectFirstSeenMs.end();) {
            const bool stillActive = std::any_of(effects.begin(), effects.end(),
                [&](const RuntimeEffect& e) { return e.id == it->first; });
            it = stillActive ? std::next(it) : m_effectFirstSeenMs.erase(it);
        }
    } else if (!m_effectFirstSeenMs.empty()) {
        m_effectFirstSeenMs.clear();
    }

    auto animProgress = [&](std::uint32_t effectId) -> float {
        if (!config.animate || config.animationDurationMs <= 0.0f) return 1.0f;
        const auto it = m_effectFirstSeenMs.find(effectId);
        if (it == m_effectFirstSeenMs.end()) return 1.0f;
        const float t = static_cast<float>(now - it->second) / config.animationDurationMs;
        const float clamped = std::clamp(t, 0.0f, 1.0f);
        return 1.0f - (1.0f - clamped) * (1.0f - clamped); // ease-out
    };

    auto scaleAlpha = [](std::uint32_t color, float factor) -> std::uint32_t {
        const std::uint32_t alpha = (color >> 24) & 0xFFu;
        const std::uint32_t scaled = static_cast<std::uint32_t>(std::clamp(static_cast<float>(alpha) * factor, 0.0f, 255.0f));
        return (scaled << 24) | (color & 0x00FFFFFFu);
    };

    std::vector<pl::modmenu::DrawCommand> commands;
    commands.reserve(effects.size() * 8 + 8);

    auto addText = [&](float x, float y, float widthMode, float size, std::uint32_t color, std::string text) {
        pl::modmenu::DrawCommand command;
        command.type = pl::modmenu::DrawCommandType::Text;
        command.x = x;
        command.y = y;
        command.w = widthMode;
        command.size = size;
        command.color = color;
        command.text = std::move(text);
        commands.push_back(std::move(command));
    };

    auto addRect = [&](float x, float y, float w, float h, float radius, std::uint32_t color) {
        pl::modmenu::DrawCommand rect;
        rect.type = pl::modmenu::DrawCommandType::RectFilled;
        rect.x = x;
        rect.y = y;
        rect.w = w;
        rect.h = h;
        rect.x3 = radius;
        rect.color = color;
        commands.push_back(rect);
    };

    if (config.showCard && hasEffects) {
        // Opaque panel fill MUST be drawn here (mod-menu overlay). The native
        // fillRectangle underlay is unreliable across builds; without this
        // the card disappears and only text remains.
        if (config.blurAmount > 0.001f) {
            constexpr int layers = 3;
            for (int i = layers; i >= 1; --i) {
                const float spread = (padding * 0.6f) * config.blurAmount * (static_cast<float>(i) / layers);
                const float layerAlpha = (config.blurAmount * 0.12f) / static_cast<float>(i);
                addRect(cardX - spread, cardY - spread, cardWidth + spread * 2.0f, cardHeight + spread * 2.0f,
                        cardRadiusPx + spread, scaleAlpha(0xFF000000u, layerAlpha));
            }
        }

        if (config.showOutline && outlineThicknessPx > 0.01f) {
            addRect(cardX - outlineThicknessPx, cardY - outlineThicknessPx,
                    cardWidth + outlineThicknessPx * 2.0f, cardHeight + outlineThicknessPx * 2.0f,
                    cardRadiusPx + outlineThicknessPx, config.outlineColor);
        }

        // Force fully opaque panel (ignore low alpha in config so the card
        // never goes translucent by accident).
        const std::uint32_t solidCard = 0xFF000000u | (config.cardColor & 0x00FFFFFFu);
        addRect(cardX, cardY, cardWidth, cardHeight, cardRadiusPx, solidCard);
    }

    if (config.showHeader && hasEffects) {
        // Header capsule behind icon + "Active Potions"
        const float headerCapPadX = padding * 0.45f;
        const float headerCapPadY = padding * 0.25f;
        const float headerCapW = headerIconSize + gap + headerTextWidth + headerCapPadX * 2.0f;
        const float headerCapH = std::max(headerIconSize, textSize) + headerCapPadY * 2.0f;
        const float headerCapX = config.hudPosX - headerCapPadX;
        const float headerCapY = config.hudPosY - headerCapPadY;
        if (config.showHeaderCapsule) {
            const float headerCapRadius = config.headerCapsuleRadius * config.uiScale * surfaceScale;
            addRect(headerCapX, headerCapY, headerCapW, headerCapH, headerCapRadius, config.headerCapsuleColor);
        }

        pl::modmenu::DrawCommand headerIcon;
        headerIcon.type = pl::modmenu::DrawCommandType::Image;
        headerIcon.x = config.hudPosX;
        headerIcon.y = config.hudPosY + (headerCapH - headerIconSize) * 0.5f - headerCapPadY;
        // Keep icon aligned to original hud pos when capsule pads outward.
        headerIcon.y = config.hudPosY;
        headerIcon.w = headerIconSize;
        headerIcon.h = headerIconSize;
        headerIcon.color = 0xFFFFFFFFu;
        headerIcon.imageId = GenericPotionImageId;
        commands.push_back(headerIcon);

        const float headerTextY = config.hudPosY + headerIconSize * 0.5f + textSize * 0.35f;
        addText(config.hudPosX + headerIconSize + gap, headerTextY, 0.0f, textSize, config.mainColor, headerText);
    }

    for (std::size_t row = 0; row < effects.size(); ++row) {
        const std::size_t source = config.bottomUp ? effects.size() - 1 - row : row;
        const RuntimeEffect& effect = effects[source];
        const float rowY = rowStartY + static_cast<float>(row) * rowStride;

        // Entrance: ease-out fade + slide up when the effect first appears.
        const float anim = animProgress(effect.id);
        const float animatedRowY = rowY + (1.0f - anim) * (padding * 1.1f);

        // Low-time state (≤ WarningSeconds): red text, shake, then fade out.
        const int remainingSeconds = (effect.noCounter || effect.duration < 0)
            ? -1
            : effect.duration / 20;
        const bool expiring = remainingSeconds >= 0 && remainingSeconds <= WarningSeconds;
        // Gentle horizontal sway when expiring — slow and small so text stays readable.
        float shakeX = 0.0f;
        float expireFade = 1.0f;
        if (config.animate && expiring) {
            const float urgency = 1.0f - static_cast<float>(remainingSeconds) / static_cast<float>(WarningSeconds);
            // ~0.4–0.9 px at default scale; only a little stronger near 0s
            const float shakeAmp = (0.35f + 0.55f * urgency) * config.uiScale * surfaceScale;
            // ~2 Hz — slow enough to read through
            shakeX = std::sin(static_cast<float>(now) * 0.012f + static_cast<float>(effect.id) * 1.7f) * shakeAmp;
            // Final second: fade out before the effect disappears
            if (remainingSeconds <= 1) {
                const int ticksLeft = std::max(0, effect.duration);
                expireFade = std::clamp(static_cast<float>(ticksLeft) / 20.0f, 0.0f, 1.0f);
            }
        }
        const float rowAlpha = anim * expireFade;

        const float rowGapPx = config.rowGap * config.uiScale * surfaceScale;
        const float capsuleHeight = std::max(1.0f, rowStride - rowGapPx);
        const float capsuleY = animatedRowY + (rowStride - capsuleHeight) * 0.5f;
        const float contentRight = config.hudPosX - padding + cardWidth - padding;
        const std::string timer = formatDuration(effect.duration, effect.noCounter);
        const std::string title = config.showTitle ? titleForEffect(effect, config) : std::string{};
        const float titleWidth = static_cast<float>(title.size()) * textSize * 0.56f;
        const float timerWidth = static_cast<float>(timer.size()) * timerSize * 0.62f;

        // Effect capsule = icon + name only (not the full row).
        const bool useEffectCap = config.showEffectCapsule || config.showRowCapsule;
        const float effectCapPadX = padding * 0.35f;
        const float effectCapX = iconX - effectCapPadX + shakeX;
        const float effectCapW = icon + gap + titleWidth + effectCapPadX * 2.0f;
        const float effectCapRadius = (config.showEffectCapsule ? config.effectCapsuleRadius : config.cardRadius * 0.7f)
            * config.uiScale * surfaceScale;
        const std::uint32_t effectCapColor = config.showEffectCapsule
            ? config.effectCapsuleColor
            : config.rowCapsuleColor;
        if (useEffectCap) {
            addRect(effectCapX, capsuleY, effectCapW, capsuleHeight, effectCapRadius,
                    scaleAlpha(effectCapColor, rowAlpha));
        }

        // Timer capsule (small pill on the right).
        const float timerCapPadX = padding * 0.35f;
        const float timerCapPadY = std::max(2.0f, capsuleHeight * 0.12f);
        const float timerCapW = timerWidth + timerCapPadX * 2.0f;
        const float timerCapH = std::max(timerSize + timerCapPadY * 2.0f, capsuleHeight * 0.75f);
        const float timerCapX = contentRight - timerCapW + shakeX;
        const float timerCapY = animatedRowY + (rowStride - timerCapH) * 0.5f;
        const float timerCapRadius = config.timerCapsuleRadius * config.uiScale * surfaceScale;
        if (config.showTimerCapsule && config.showText) {
            addRect(timerCapX, timerCapY, timerCapW, timerCapH, timerCapRadius,
                    scaleAlpha(config.timerCapsuleColor, rowAlpha));
        }

        // Icon on top of effect capsule / card.
        if (!effect.nativeIcon || config.showCard) {
            const std::uint8_t iconAlpha = static_cast<std::uint8_t>(
                std::clamp(config.iconOpacity * rowAlpha, 0.0f, 1.0f) * 255.0f);
            pl::modmenu::DrawCommand image;
            image.type = pl::modmenu::DrawCommandType::Image;
            image.x = iconX + shakeX;
            image.y = animatedRowY + (rowStride - icon) * 0.5f;
            image.w = icon;
            image.h = icon;
            image.color = (static_cast<std::uint32_t>(iconAlpha) << 24) | 0x00FFFFFFu;
            image.imageId = fallbackImageId(effect.id);
            commands.push_back(std::move(image));
        }

        if (!config.showText) continue;
        const bool left = config.textSide == 1;
        const float textX = (left ? config.hudPosX + textWidth : iconX + icon + gap) + shakeX;
        const float widthMode = left ? -1.0f : 0.0f;
        const std::uint32_t effectColor = scaleAlpha(expiring ? config.lowColor : config.mainColor, rowAlpha);
        const std::uint32_t shadowColor = scaleAlpha(config.shadowColor, rowAlpha);

        float timerPulse = 1.0f;
        float timerSizeAnim = timerSize;
        if (config.animate && !effect.noCounter && effect.duration >= 0) {
            const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(now) * 0.012f);
            timerPulse = expiring ? (0.75f + 0.25f * pulse) : (0.92f + 0.08f * pulse);
            if (expiring) timerSizeAnim = timerSize * (1.0f + 0.06f * pulse);
        }
        const std::uint32_t timerColor = scaleAlpha(expiring ? config.lowColor : config.mainColor, rowAlpha * timerPulse);
        // Center timer text inside its capsule when enabled; otherwise right-align to card.
        const float timerTextX = config.showTimerCapsule
            ? (timerCapX + timerCapW - timerCapPadX)
            : contentRight;
        const float lineY = animatedRowY + rowStride * 0.5f + textSize * 0.35f;
        const float timerLineY = config.showTimerCapsule
            ? (timerCapY + timerCapH * 0.5f + timerSizeAnim * 0.35f)
            : lineY;

        if (config.showTitle && config.singleLineRow) {
            if (config.textShadow) {
                addText(textX + shadowOffset, lineY + shadowOffset, widthMode, textSize, shadowColor, title);
                addText(timerTextX + shadowOffset, timerLineY + shadowOffset, -1.0f, timerSizeAnim, shadowColor, timer);
            }
            addText(textX, lineY, widthMode, textSize, effectColor, title);
            addText(timerTextX, timerLineY, -1.0f, timerSizeAnim, timerColor, timer);
        } else if (config.showTitle) {
            const float titleY = animatedRowY + textSize;
            const float timerY = animatedRowY + textSize + timerSize * 1.05f;
            if (config.textShadow) {
                addText(textX + shadowOffset, titleY + shadowOffset, widthMode, textSize, shadowColor, title);
                addText(textX + shadowOffset, timerY + shadowOffset, widthMode, timerSizeAnim, shadowColor, timer);
            }
            addText(textX, titleY, widthMode, textSize, effectColor, title);
            addText(textX, timerY, widthMode, timerSizeAnim, timerColor, timer);
        } else {
            const float timerY = animatedRowY + icon * 0.5f + timerSize * 0.35f;
            if (config.textShadow)
                addText(textX + shadowOffset, timerY + shadowOffset, widthMode, timerSizeAnim, shadowColor, timer);
            addText(textX, timerY, widthMode, timerSizeAnim, timerColor, timer);
        }
    }

    pl::modmenu::submitDrawCommands(moduleId, commands);
}

void PotionHudModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    std::lock_guard lock(m_configMutex);
    if (j.contains("hudPosX")) hudPosX = std::clamp(j["hudPosX"].get<float>(), 0.0f, 4000.0f);
    if (j.contains("hudPosY")) hudPosY = std::clamp(j["hudPosY"].get<float>(), 0.0f, 4000.0f);
    if (j.contains("m_uiScale")) m_uiScale = std::clamp(j["m_uiScale"].get<float>(), 0.5f, 3.0f);
    if (j.contains("m_spacing")) m_spacing = std::clamp(j["m_spacing"].get<float>(), 0.25f, 3.0f);
    if (j.contains("m_bottomUp")) m_bottomUp = j["m_bottomUp"].get<bool>();
    if (j.contains("m_showText")) m_showText = j["m_showText"].get<bool>();
    if (j.contains("m_showTitle")) m_showTitle = j["m_showTitle"].get<bool>();
    if (j.contains("m_useRoman")) m_useRoman = j["m_useRoman"].get<bool>();
    if (j.contains("m_useRomanFull")) m_useRomanFull = j["m_useRomanFull"].get<bool>();
    if (j.contains("m_textSize")) m_textSize = std::clamp(j["m_textSize"].get<float>(), 4.0f, 20.0f);
    if (j.contains("m_textOffsetX")) m_textOffsetX = std::clamp(j["m_textOffsetX"].get<float>(), 0.0f, 20.0f);
    if (j.contains("m_textSide")) {
        try {
            std::string value = j["m_textSide"].get<std::string>();
            const std::size_t separator = value.find(',');
            if (separator != std::string::npos) value.resize(separator);
            m_textSide = std::clamp(std::stoi(value), 0, 1);
        } catch (...) {
        }
    }
    if (j.contains("m_textShadow")) m_textShadow = j["m_textShadow"].get<bool>();
    if (j.contains("m_shadowOffset")) m_shadowOffset = std::clamp(j["m_shadowOffset"].get<float>(), 0.25f, 5.0f);
    if (j.contains("m_mainColor")) m_mainColor = j["m_mainColor"].get<std::string>();
    if (j.contains("m_lowColor")) m_lowColor = j["m_lowColor"].get<std::string>();
    if (j.contains("m_shadowColor")) m_shadowColor = j["m_shadowColor"].get<std::string>();
    if (j.contains("m_gridSize")) m_gridSize = std::clamp(j["m_gridSize"].get<float>(), 1.0f, 100.0f);
    if (j.contains("m_gridGap")) m_gridGap = std::clamp(j["m_gridGap"].get<float>(), 0.0f, 100.0f);
    if (j.contains("m_snapThreshold")) m_snapThreshold = std::clamp(j["m_snapThreshold"].get<float>(), 1.0f, 100.0f);
    if (j.contains("m_snapToGrid")) m_snapToGrid = j["m_snapToGrid"].get<bool>();
    if (j.contains("m_snapToElements")) m_snapToElements = j["m_snapToElements"].get<bool>();
    if (j.contains("m_snapToScreenCenter")) m_snapToScreenCenter = j["m_snapToScreenCenter"].get<bool>();
    if (j.contains("m_showCard")) m_showCard = j["m_showCard"].get<bool>();
    if (j.contains("m_cardColor")) m_cardColor = j["m_cardColor"].get<std::string>();
    if (j.contains("m_cardRadius")) m_cardRadius = std::clamp(j["m_cardRadius"].get<float>(), 0.0f, 48.0f);
    if (j.contains("m_cardPadding")) m_cardPadding = std::clamp(j["m_cardPadding"].get<float>(), 0.0f, 40.0f);
    if (j.contains("m_cardWidth")) m_cardWidth = std::clamp(j["m_cardWidth"].get<float>(), 0.0f, 400.0f);
    if (j.contains("m_cardHeight")) m_cardHeight = std::clamp(j["m_cardHeight"].get<float>(), 0.0f, 600.0f);
    if (j.contains("m_showHeader")) m_showHeader = j["m_showHeader"].get<bool>();
    if (j.contains("m_singleLineRow")) m_singleLineRow = j["m_singleLineRow"].get<bool>();
    if (j.contains("m_showHeaderCapsule")) m_showHeaderCapsule = j["m_showHeaderCapsule"].get<bool>();
    if (j.contains("m_headerCapsuleColor")) m_headerCapsuleColor = j["m_headerCapsuleColor"].get<std::string>();
    if (j.contains("m_headerCapsuleRadius")) m_headerCapsuleRadius = std::clamp(j["m_headerCapsuleRadius"].get<float>(), 0.0f, 24.0f);
    if (j.contains("m_showEffectCapsule")) m_showEffectCapsule = j["m_showEffectCapsule"].get<bool>();
    if (j.contains("m_effectCapsuleColor")) m_effectCapsuleColor = j["m_effectCapsuleColor"].get<std::string>();
    if (j.contains("m_effectCapsuleRadius")) m_effectCapsuleRadius = std::clamp(j["m_effectCapsuleRadius"].get<float>(), 0.0f, 24.0f);
    if (j.contains("m_showTimerCapsule")) m_showTimerCapsule = j["m_showTimerCapsule"].get<bool>();
    if (j.contains("m_timerCapsuleColor")) m_timerCapsuleColor = j["m_timerCapsuleColor"].get<std::string>();
    if (j.contains("m_timerCapsuleRadius")) m_timerCapsuleRadius = std::clamp(j["m_timerCapsuleRadius"].get<float>(), 0.0f, 24.0f);
    if (j.contains("m_showRowCapsule")) m_showRowCapsule = j["m_showRowCapsule"].get<bool>();
    if (j.contains("m_rowCapsuleColor")) m_rowCapsuleColor = j["m_rowCapsuleColor"].get<std::string>();
    if (j.contains("m_iconOpacity")) m_iconOpacity = std::clamp(j["m_iconOpacity"].get<float>(), 0.0f, 1.0f);
    if (j.contains("m_rowGap")) m_rowGap = std::clamp(j["m_rowGap"].get<float>(), 0.0f, 40.0f);
    if (j.contains("m_showOutline")) m_showOutline = j["m_showOutline"].get<bool>();
    if (j.contains("m_outlineColor")) m_outlineColor = j["m_outlineColor"].get<std::string>();
    if (j.contains("m_outlineThickness")) m_outlineThickness = std::clamp(j["m_outlineThickness"].get<float>(), 0.0f, 10.0f);
    if (j.contains("m_blurAmount")) m_blurAmount = std::clamp(j["m_blurAmount"].get<float>(), 0.0f, 1.0f);
    if (j.contains("m_animate")) m_animate = j["m_animate"].get<bool>();
    if (j.contains("m_animationDurationMs")) m_animationDurationMs = std::clamp(j["m_animationDurationMs"].get<float>(), 0.0f, 2000.0f);
}

void PotionHudModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    std::lock_guard lock(m_configMutex);
    j["isHudModule"] = true;
    j["hudPosX"] = hudPosX;
    j["hudPosY"] = hudPosY;
    j["m_uiScale"] = m_uiScale;
    j["m_spacing"] = m_spacing;
    j["m_bottomUp"] = m_bottomUp;
    j["m_showText"] = m_showText;
    j["m_showTitle"] = m_showTitle;
    j["m_useRoman"] = m_useRoman;
    j["m_useRomanFull"] = m_useRomanFull;
    j["m_textSize"] = m_textSize;
    j["m_textOffsetX"] = m_textOffsetX;
    j["m_textSide"] = std::to_string(m_textSide) + ",Right,Left";
    j["m_textShadow"] = m_textShadow;
    j["m_shadowOffset"] = m_shadowOffset;
    j["m_mainColor"] = m_mainColor;
    j["m_lowColor"] = m_lowColor;
    j["m_shadowColor"] = m_shadowColor;
    j["m_gridSize"] = m_gridSize;
    j["m_gridGap"] = m_gridGap;
    j["m_snapThreshold"] = m_snapThreshold;
    j["m_snapToGrid"] = m_snapToGrid;
    j["m_snapToElements"] = m_snapToElements;
    j["m_snapToScreenCenter"] = m_snapToScreenCenter;
    j["m_showCard"] = m_showCard;
    j["m_cardColor"] = m_cardColor;
    j["m_cardRadius"] = m_cardRadius;
    j["m_cardPadding"] = m_cardPadding;
    j["m_cardWidth"] = m_cardWidth;
    j["m_cardHeight"] = m_cardHeight;
    j["m_showHeader"] = m_showHeader;
    j["m_singleLineRow"] = m_singleLineRow;
    j["m_showHeaderCapsule"] = m_showHeaderCapsule;
    j["m_headerCapsuleColor"] = m_headerCapsuleColor;
    j["m_headerCapsuleRadius"] = m_headerCapsuleRadius;
    j["m_showEffectCapsule"] = m_showEffectCapsule;
    j["m_effectCapsuleColor"] = m_effectCapsuleColor;
    j["m_effectCapsuleRadius"] = m_effectCapsuleRadius;
    j["m_showTimerCapsule"] = m_showTimerCapsule;
    j["m_timerCapsuleColor"] = m_timerCapsuleColor;
    j["m_timerCapsuleRadius"] = m_timerCapsuleRadius;
    j["m_showRowCapsule"] = m_showRowCapsule;
    j["m_rowCapsuleColor"] = m_rowCapsuleColor;
    j["m_iconOpacity"] = m_iconOpacity;
    j["m_rowGap"] = m_rowGap;
    j["m_showOutline"] = m_showOutline;
    j["m_outlineColor"] = m_outlineColor;
    j["m_outlineThickness"] = m_outlineThickness;
    j["m_blurAmount"] = m_blurAmount;
    j["m_animate"] = m_animate;
    j["m_animationDurationMs"] = m_animationDurationMs;
}
