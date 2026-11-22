#pragma once

#include <windows.h>

#include <array>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace pv {

// Magnification API layout: [r g b a 1] * M, row = input channel, row 4 = offset.
using Mat5 = std::array<std::array<float, 5>, 5>;

inline constexpr float kLumaR = 0.2126f;
inline constexpr float kLumaG = 0.7152f;
inline constexpr float kLumaB = 0.0722f;

struct ColorParams {
    float saturation = 1.0f;   // GPU driver or DWM matrix
    float vibrance = 0.0f;     // -1..1, shader (needs capture)
    float exposure = 0.0f;
    float contrast = 1.0f;
    float brightness = 0.0f;
    float gamma = 1.0f;
    float shadowBoost = 0.0f;
    float gainR = 1.0f;
    float gainG = 1.0f;
    float gainB = 1.0f;
    bool invert = false;

    bool operator==(const ColorParams&) const = default;
};

void ClampColorParams(ColorParams& p) noexcept;

Mat5 Identity() noexcept;
bool IsIdentity(const Mat5& m) noexcept;
Mat5 Multiply(const Mat5& a, const Mat5& b) noexcept;
Mat5 SaturationMatrix(float s) noexcept;
Mat5 ChannelGainMatrix(float r, float g, float b) noexcept;
Mat5 ExposureMatrix(float ev) noexcept;
Mat5 ContrastBrightnessMatrix(float contrast, float brightness) noexcept;
Mat5 InvertMatrix() noexcept;
Mat5 BuildColorMatrix(const ColorParams& p) noexcept;
std::array<float, 3> TransformColor(const Mat5& m, float r, float g, float b) noexcept;

enum class PresetId : int { Neutral = 0, TacticalClarity, NightVisionGreen, ThermalContrast, DaylightVibrance, Count };

struct ColorPreset {
    const char* name;
    ColorParams params;
};

std::span<const ColorPreset> GetColorPresets() noexcept;

// DWM full-screen color effect. Only active while the matrix is not identity:
// a live color effect makes DWM re-compose the whole screen every frame, which
// takes games off their fast present path, so it is released as soon as possible.
class MagnificationColor {
public:
    MagnificationColor() = default;
    ~MagnificationColor();
    MagnificationColor(const MagnificationColor&) = delete;
    MagnificationColor& operator=(const MagnificationColor&) = delete;

    bool Active() const noexcept { return m_initialized; }
    bool Apply(const Mat5& m) noexcept;
    void Reset() noexcept;

private:
    bool m_initialized = false;
};

// Hardware gamma LUT (applied at scan-out, zero cost). Holds every per-channel
// adjustment: exposure, RGB gains, contrast, brightness, invert, gamma, shadows.
class GammaRamp {
public:
    GammaRamp();
    ~GammaRamp();
    GammaRamp(const GammaRamp&) = delete;
    GammaRamp& operator=(const GammaRamp&) = delete;

    // includeLinear=false applies only gamma + shadows (fallback when Windows rejects the full curve)
    bool Apply(const ColorParams& p, bool includeLinear) noexcept;
    void Restore() noexcept;

    static float Curve(float x, float gamma, float shadowBoost) noexcept;
    static float Channel(float x, const ColorParams& p, int channel, bool includeLinear) noexcept;
    static bool IsNeutral(const ColorParams& p, bool includeLinear) noexcept;

private:
    struct Display {
        std::wstring deviceName;
        std::array<WORD, 768> original{};
    };
    void CaptureOriginals();

    std::vector<Display> m_displays;
    bool m_modified = false;
};

}  // namespace pv
