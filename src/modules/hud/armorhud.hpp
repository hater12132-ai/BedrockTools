#pragma once

#include "../Module.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

class ArmorHudModule : public Module {
public:
    ArmorHudModule();
    ~ArmorHudModule() override;

    void onInit() override;
    void onDisable() override;
    void onFrame() override;
    void onMenuRegistered() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    void renderNative(void* context, void* client);

private:
    struct SlotConfig {
        bool enabled;
        bool durability;
        float x;
        float y;
        float size;
    };

    struct SlotRuntime {
        std::atomic_bool hasItem{false};
        std::atomic_int damage{0};
        std::atomic_int maxDamage{0};
    };

    struct ConfigSnapshot {
        std::array<SlotConfig, 6> slots;
        bool hotbarBackground;
        bool showDamage;
        bool showRemaining;
        bool showMaxDurability;
        bool showPercentage;
        float durabilityTextSize;
        int durabilityTextPosition;
        float durabilityTextGap;
        std::uint32_t textColor;
        float gridSize;
        float gridGap;
        float snapThreshold;
        std::uint32_t snapFlags;
        // Capsule bar style (screenshot look)
        bool barStyle;
        float barPosX; // = hudPosX
        float barPosY; // = hudPosY
        float barIconSize;
        float barPadding;
        float barGap;
        float barRadius;
        std::uint32_t barColor;
        bool showGlow;
        std::uint32_t glowColor;
        float glowAmount;
        bool space;          // white stars drifting inside the card
        int starCount;
        float starSpeed;
        bool onlyEquipped;   // only show slots that currently have an item
    };

    ConfigSnapshot snapshotConfig() const;
    void clearRuntime();
    void submitEditorElements(const ConfigSnapshot& config);

    mutable std::mutex m_configMutex;
    std::array<SlotRuntime, 6> m_runtime;

    bool m_helmet = true;
    bool m_helmetDurability = true;
    float hudHelmetPosX = 24.0f;
    float hudHelmetPosY = 80.0f;
    float m_helmetSize = 48.0f;

    bool m_chestplate = true;
    bool m_chestplateDurability = true;
    float hudChestplatePosX = 24.0f;
    float hudChestplatePosY = 152.0f;
    float m_chestplateSize = 48.0f;

    bool m_leggings = true;
    bool m_leggingsDurability = true;
    float hudLeggingsPosX = 24.0f;
    float hudLeggingsPosY = 224.0f;
    float m_leggingsSize = 48.0f;

    bool m_boots = true;
    bool m_bootsDurability = true;
    float hudBootsPosX = 24.0f;
    float hudBootsPosY = 296.0f;
    float m_bootsSize = 48.0f;

    bool m_offhand = true;
    bool m_offhandDurability = true;
    float hudOffhandPosX = 24.0f;
    float hudOffhandPosY = 368.0f;
    float m_offhandSize = 48.0f;

    bool m_mainhand = true;
    bool m_mainhandDurability = true;
    float hudMainhandPosX = 24.0f;
    float hudMainhandPosY = 440.0f;
    float m_mainhandSize = 48.0f;

    bool m_hotbarBackground = true;
    bool m_showDamage = true;
    bool m_showRemaining = true;
    bool m_showMaxDurability = true;
    bool m_showPercentage = true;
    float m_durabilityTextSize = 14.0f;
    int m_durabilityTextPosition = 0;
    float m_durabilityTextGap = 4.0f;
    std::string m_textColor = "#FFFFFF";
    float m_gridSize = 16.0f;
    float m_gridGap = 4.0f;
    float m_snapThreshold = 12.0f;
    bool m_snapToGrid = true;
    bool m_snapToElements = true;
    bool m_snapToScreenCenter = true;

    // Horizontal black capsule bar (screenshot style)
    bool m_barStyle = true;
    // Standard keys so Levi HUD editor can drag the bar (same as PotionHUD)
    float hudPosX = 200.0f;
    float hudPosY = 40.0f;
    float m_barIconSize = 28.0f; // used when a slot has no size override
    float m_barPadding = 10.0f;
    float m_barGap = 6.0f;
    float m_barRadius = 14.0f;
    std::string m_barColor = "#FF000000"; // full black
    bool m_showGlow = false; // off by default → pure black, not cyan
    std::string m_glowColor = "#6640E0FF";
    float m_glowAmount = 0.35f;
    bool m_space = true;
    int m_starCount = 18;
    float m_starSpeed = 1.0f;
    bool m_onlyEquipped = true;
    bool isHudModule = true;

    struct Star {
        float x = 0.0f; // 0..1 inside card
        float y = 0.0f;
        float vx = 0.0f;
        float vy = 0.0f;
        float size = 1.0f;
        float phase = 0.0f;
    };
    std::vector<Star> m_stars;
    bool m_starsSeeded = false;
};
