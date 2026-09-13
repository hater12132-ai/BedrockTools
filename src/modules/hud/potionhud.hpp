#pragma once

#include "../Module.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

class PotionHudModule : public Module {
public:
    PotionHudModule();
    ~PotionHudModule() override;

    void onInit() override;
    void onDisable() override;
    void onFrame() override;
    void onMenuRegistered() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    bool renderNative(void* context, void* client);

private:
    struct RuntimeEffect {
        std::uint32_t id;
        int duration;
        int amplifier;
        bool noCounter;
        bool nativeIcon;
    };

    struct ConfigSnapshot {
        float hudPosX;
        float hudPosY;
        float uiScale;
        float spacing;
        bool bottomUp;
        bool showText;
        bool showTitle;
        bool useRoman;
        bool useRomanFull;
        float textSize;
        float textOffsetX;
        int textSide;
        bool textShadow;
        float shadowOffset;
        std::uint32_t mainColor;
        std::uint32_t lowColor;
        std::uint32_t shadowColor;
        float gridSize;
        float gridGap;
        float snapThreshold;
        std::uint32_t snapFlags;
        bool showCard;
        std::uint32_t cardColor;
        float cardRadius;
        float cardPadding;
        float cardWidth;   // 0 = auto (fit content)
        float cardHeight;  // 0 = auto (fit content)
        bool showHeader;
        bool singleLineRow;
        bool showHeaderCapsule;
        std::uint32_t headerCapsuleColor;
        float headerCapsuleRadius;
        float headerCapsuleWidth;   // 0 = auto
        float headerCapsuleHeight;  // 0 = auto
        bool showEffectCapsule;
        std::uint32_t effectCapsuleColor;
        float effectCapsuleRadius;
        float effectCapsuleWidth;   // 0 = auto
        float effectCapsuleHeight;  // 0 = auto
        bool showTimerCapsule;
        std::uint32_t timerCapsuleColor;
        float timerCapsuleRadius;
        float timerCapsuleWidth;    // 0 = auto
        float timerCapsuleHeight;   // 0 = auto
        // Legacy alias kept for snapshot compatibility with older configs.
        bool showRowCapsule;
        std::uint32_t rowCapsuleColor;
        bool showIcons;
        float iconSizeScale;
        float iconOpacity;
        float rowGap;
        bool showOutline;
        std::uint32_t outlineColor;
        float outlineThickness;
        float blurAmount;
        bool animate;
        float animationDurationMs;
    };

    ConfigSnapshot snapshotConfig() const;
    std::vector<RuntimeEffect> snapshotRuntime(float& surfaceScale) const;
    void clearRuntime();
    void submitEditorElement(const ConfigSnapshot& config, const std::vector<RuntimeEffect>& effects, float surfaceScale);
    static float iconSurfaceSize(const ConfigSnapshot& config, float surfaceScale);
    static float rowSurfaceHeight(const ConfigSnapshot& config, float surfaceScale);
    static float textSurfaceWidth(const ConfigSnapshot& config, const std::vector<RuntimeEffect>& effects, float surfaceScale);
    static std::string titleForEffect(const RuntimeEffect& effect, const ConfigSnapshot& config);

    mutable std::mutex m_configMutex;
    mutable std::mutex m_runtimeMutex;
    std::vector<RuntimeEffect> m_runtimeEffects;
    float m_surfaceScale = 1.0f;
    std::atomic_bool m_runtimeValid{false};

    float hudPosX = 24.0f;
    float hudPosY = 80.0f;
    float m_uiScale = 1.0f;
    float m_spacing = 1.0f;
    bool m_bottomUp = false;
    bool m_showText = true;
    bool m_showTitle = true;
    bool m_useRoman = true;
    bool m_useRomanFull = true;
    float m_textSize = 8.0f;
    float m_textOffsetX = 2.0f;
    int m_textSide = 0;
    bool m_textShadow = true;
    float m_shadowOffset = 1.0f;
    std::string m_mainColor = "#FFFFFFFF";
    std::string m_lowColor = "#FFFF4040";
    std::string m_shadowColor = "#8C000000";
    float m_gridSize = 16.0f;
    float m_gridGap = 4.0f;
    float m_snapThreshold = 12.0f;
    bool m_snapToGrid = true;
    bool m_snapToElements = true;
    bool m_snapToScreenCenter = true;

    // Card/header styling to match the clean dark "Potions" panel:
    // dark rounded background, potion icon + "Potions" header, name +
    // duration on one line (∞ for permanent effects). Purely additive -
    // existing layout options above still work when showCard/showHeader
    // are turned off.
    bool m_showCard = true;
    std::string m_cardColor = "#FF0A0A0E"; // fully opaque near-black
    float m_cardRadius = 12.0f;
    float m_cardPadding = 12.0f;
    float m_cardWidth = 0.0f;   // 0 = auto-fit content
    float m_cardHeight = 0.0f;  // 0 = auto-fit content
    bool m_showHeader = true;
    bool m_singleLineRow = true;

    // Three independent grey-black capsules (all configurable):
    // 1) header "Active Potions"  2) effect icon+name  3) timer
    bool m_showHeaderCapsule = true;
    std::string m_headerCapsuleColor = "#FF1A1A20";
    float m_headerCapsuleRadius = 8.0f;
    float m_headerCapsuleWidth = 0.0f;   // 0 = auto-fit
    float m_headerCapsuleHeight = 0.0f;  // 0 = auto-fit
    bool m_showEffectCapsule = true;
    std::string m_effectCapsuleColor = "#FF1A1A20";
    float m_effectCapsuleRadius = 8.0f;
    float m_effectCapsuleWidth = 0.0f;
    float m_effectCapsuleHeight = 0.0f;
    bool m_showTimerCapsule = true;
    std::string m_timerCapsuleColor = "#FF1A1A20";
    float m_timerCapsuleRadius = 6.0f;
    float m_timerCapsuleWidth = 0.0f;
    float m_timerCapsuleHeight = 0.0f;
    // Legacy single-row capsule (maps to effect capsule if enabled in old configs).
    bool m_showRowCapsule = false;
    std::string m_rowCapsuleColor = "#26FFFFFF";
    bool m_showIcons = true;
    float m_iconSizeScale = 1.0f; // multiplier on base 16px effect icons
    float m_iconOpacity = 1.0f;
    float m_rowGap = 4.0f;

    // Smooth 0→1 reveal for icon show/hide (text slides with it).
    float m_iconReveal = 1.0f;

    // Outline - there's no dedicated stroke/border draw type in this API,
    // so this uses the same "bigger rect behind, smaller rect in front"
    // trick breakindicator.cpp already uses elsewhere in this codebase.
    bool m_showOutline = false;
    std::string m_outlineColor = "#66C8C8C8";
    float m_outlineThickness = 1.5f;

    // Soft glow around the card edge (layered fainter copies). Kept subtle
    // so the panel stays clean and solid like the reference.
    float m_blurAmount = 0.15f;

    // New effects fade + slide in instead of popping in solid.
    bool m_animate = true;
    float m_animationDurationMs = 320.0f; // entrance slide/fade duration
    std::unordered_map<std::uint32_t, std::int64_t> m_effectFirstSeenMs;
};
