#pragma once

#include "ColorMatrix.h"

#include <array>
#include <filesystem>
#include <string>

namespace pv {

enum class CrosshairStyle : int {
    Dot = 0,
    Cross,
    Circle,
    CrossDot,
    TShape,
    CircleDot,
    Count
};

struct CrosshairSettings {
    bool enabled = false;
    int style = static_cast<int>(CrosshairStyle::CrossDot);
    float size = 10.0f;          // arm length / circle radius, px
    float thickness = 2.0f;      // px
    float gap = 4.0f;            // px
    std::array<float, 4> color{0.10f, 1.00f, 0.55f, 1.00f};
    bool outline = true;
    float outlineThickness = 1.0f;
    float offsetX = 0.0f;
    float offsetY = 0.0f;
    int anchor = 1;  // 0 = monitor center, 1 = center of the game window (handles windowed / title bars)
};

struct SharpenSettings {
    bool enabled = false;
    float strength = 0.65f;
    float clamp = 0.06f;
};

struct VignetteSettings {
    bool enabled = false;
    float strength = 0.55f;
    float radius = 0.55f;
    float softness = 0.45f;
};

struct ZoomSettings {
    bool enabled = false;
    float factor = 2.5f;
    float radius = 200.0f;  // px
    int holdKey = VK_XBUTTON2;
    std::array<float, 4> border{0.13f, 0.82f, 0.72f, 0.90f};
};

struct Profile {
    std::string name;
    ColorParams color;
    SharpenSettings sharpen;
    VignetteSettings vignette;
    CrosshairSettings crosshair;
    ZoomSettings zoom;
};

inline constexpr int kProfileCount = 4;

struct AppConfig {
    AppConfig();

    bool masterEnabled = true;
    int activeProfile = 0;
    std::string wallpaperPath;  // menu background image, UTF-8
    float wallpaperDim = 0.55f;
    float wallpaperBlurPanels = 0.55f;  // panel tint opacity over the blurred wallpaper
    float wallpaperBlur = 0.6f;         // 0..1 frosted glass strength
    std::array<float, 3> accent{0.91f, 0.34f, 0.23f};
    int saturationEngine = 0;  // 0 = GPU driver (zero cost, falls back to DWM), 1 = DWM matrix (exact Rec.709)
    int vibranceMode = 0;      // 0 = GPU driver (zero cost), 1 = capture shader (exact, costs FPS)
    std::array<Profile, kProfileCount> profiles;

    [[nodiscard]] Profile& Active() noexcept { return profiles[static_cast<size_t>(activeProfile)]; }
    [[nodiscard]] const Profile& Active() const noexcept { return profiles[static_cast<size_t>(activeProfile)]; }

    // default contents of a slot (first run and "Reset")
    [[nodiscard]] static Profile DefaultProfile(int slot);
};

[[nodiscard]] std::filesystem::path DefaultConfigPath();
bool LoadConfig(const std::filesystem::path& path, AppConfig& cfg, std::string& error);
bool SaveConfig(const std::filesystem::path& path, const AppConfig& cfg, std::string& error);

}  // namespace pv
