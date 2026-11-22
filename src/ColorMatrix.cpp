#include "ColorMatrix.h"

#include <magnification.h>

#include <algorithm>
#include <cmath>

namespace pv {

namespace {

struct DcDeleter {
    void operator()(HDC dc) const noexcept {
        if (dc) DeleteDC(dc);
    }
};
using UniqueDC = std::unique_ptr<std::remove_pointer_t<HDC>, DcDeleter>;

UniqueDC OpenDisplayDC(const std::wstring& name) noexcept {
    return UniqueDC(CreateDCW(nullptr, name.c_str(), nullptr, nullptr));
}

ColorParams MakeParams(float sat, float vib, float exp, float con, float bri, float gam, float sh,
                       float r = 1.0f, float g = 1.0f, float b = 1.0f) {
    ColorParams p;
    p.saturation = sat;
    p.vibrance = vib;
    p.exposure = exp;
    p.contrast = con;
    p.brightness = bri;
    p.gamma = gam;
    p.shadowBoost = sh;
    p.gainR = r;
    p.gainG = g;
    p.gainB = b;
    return p;
}

const ColorPreset kPresets[] = {
    {"Neutral", ColorParams{}},
    {"Tactical Clarity", MakeParams(1.40f, 0.0f, 0.05f, 1.10f, 0.0f, 1.12f, 0.35f)},
    {"Night Vision", MakeParams(0.0f, 0.0f, 0.45f, 1.20f, 0.0f, 1.35f, 0.55f, 0.25f, 1.20f, 0.30f)},
    {"Thermal", MakeParams(1.75f, 0.0f, 0.0f, 1.45f, -0.03f, 0.95f, 0.10f, 1.15f, 0.92f, 0.85f)},
    {"Daylight", MakeParams(1.60f, 0.0f, 0.10f, 1.05f, 0.0f, 1.0f, 0.0f)},
};
static_assert(std::size(kPresets) == static_cast<size_t>(PresetId::Count));

}  // namespace

void ClampColorParams(ColorParams& p) noexcept {
    p.saturation = std::clamp(p.saturation, 0.0f, 4.0f);
    p.vibrance = std::clamp(p.vibrance, -1.0f, 1.0f);
    p.exposure = std::clamp(p.exposure, -3.0f, 3.0f);
    p.contrast = std::clamp(p.contrast, 0.25f, 3.0f);
    p.brightness = std::clamp(p.brightness, -0.5f, 0.5f);
    p.gamma = std::clamp(p.gamma, 0.4f, 2.8f);
    p.shadowBoost = std::clamp(p.shadowBoost, 0.0f, 1.0f);
    p.gainR = std::clamp(p.gainR, 0.0f, 2.0f);
    p.gainG = std::clamp(p.gainG, 0.0f, 2.0f);
    p.gainB = std::clamp(p.gainB, 0.0f, 2.0f);
}

Mat5 Identity() noexcept {
    Mat5 m{};
    for (size_t i = 0; i < 5; ++i) m[i][i] = 1.0f;
    return m;
}

Mat5 Multiply(const Mat5& a, const Mat5& b) noexcept {
    Mat5 r{};
    for (size_t i = 0; i < 5; ++i)
        for (size_t j = 0; j < 5; ++j)
            for (size_t k = 0; k < 5; ++k) r[i][j] += a[i][k] * b[k][j];
    return r;
}

// out = Y + s * (in - Y); Rec.709 luma is preserved
Mat5 SaturationMatrix(float s) noexcept {
    const float luma[3] = {kLumaR, kLumaG, kLumaB};
    Mat5 m = Identity();
    for (size_t i = 0; i < 3; ++i)
        for (size_t j = 0; j < 3; ++j) m[i][j] = (1.0f - s) * luma[i] + (i == j ? s : 0.0f);
    return m;
}

Mat5 ChannelGainMatrix(float r, float g, float b) noexcept {
    Mat5 m = Identity();
    m[0][0] = r;
    m[1][1] = g;
    m[2][2] = b;
    return m;
}

Mat5 ExposureMatrix(float ev) noexcept {
    const float k = std::exp2(ev);
    return ChannelGainMatrix(k, k, k);
}

Mat5 ContrastBrightnessMatrix(float contrast, float brightness) noexcept {
    Mat5 m = Identity();
    const float offset = 0.5f * (1.0f - contrast) + brightness;
    for (size_t i = 0; i < 3; ++i) {
        m[i][i] = contrast;
        m[4][i] = offset;
    }
    return m;
}

Mat5 InvertMatrix() noexcept {
    Mat5 m = Identity();
    for (size_t i = 0; i < 3; ++i) {
        m[i][i] = -1.0f;
        m[4][i] = 1.0f;
    }
    return m;
}

Mat5 BuildColorMatrix(const ColorParams& p) noexcept {
    Mat5 m = ExposureMatrix(p.exposure);
    m = Multiply(m, SaturationMatrix(p.saturation));
    m = Multiply(m, ChannelGainMatrix(p.gainR, p.gainG, p.gainB));
    m = Multiply(m, ContrastBrightnessMatrix(p.contrast, p.brightness));
    if (p.invert) m = Multiply(m, InvertMatrix());
    return m;
}

std::array<float, 3> TransformColor(const Mat5& m, float r, float g, float b) noexcept {
    const float in[5] = {r, g, b, 1.0f, 1.0f};
    std::array<float, 3> out{};
    for (size_t j = 0; j < 3; ++j) {
        float sum = 0.0f;
        for (size_t i = 0; i < 5; ++i) sum += in[i] * m[i][j];
        out[j] = std::clamp(sum, 0.0f, 1.0f);
    }
    return out;
}

std::span<const ColorPreset> GetColorPresets() noexcept { return kPresets; }

bool IsIdentity(const Mat5& m) noexcept {
    const Mat5 id = Identity();
    for (size_t i = 0; i < 5; ++i)
        for (size_t j = 0; j < 5; ++j)
            if (std::fabs(m[i][j] - id[i][j]) > 1e-5f) return false;
    return true;
}

MagnificationColor::~MagnificationColor() { Reset(); }

bool MagnificationColor::Apply(const Mat5& m) noexcept {
    if (IsIdentity(m)) {
        Reset();
        return true;
    }
    if (!m_initialized) m_initialized = MagInitialize() != FALSE;
    if (!m_initialized) return false;
    MAGCOLOREFFECT effect{};
    for (size_t i = 0; i < 5; ++i)
        for (size_t j = 0; j < 5; ++j) effect.transform[i][j] = m[i][j];
    return MagSetFullscreenColorEffect(&effect) != FALSE;
}

void MagnificationColor::Reset() noexcept {
    if (!m_initialized) return;
    MAGCOLOREFFECT effect{};
    const Mat5 id = Identity();
    for (size_t i = 0; i < 5; ++i)
        for (size_t j = 0; j < 5; ++j) effect.transform[i][j] = id[i][j];
    MagSetFullscreenColorEffect(&effect);
    MagUninitialize();
    m_initialized = false;
}

GammaRamp::GammaRamp() { CaptureOriginals(); }

GammaRamp::~GammaRamp() { Restore(); }

void GammaRamp::CaptureOriginals() {
    m_displays.clear();
    for (DWORD i = 0;; ++i) {
        DISPLAY_DEVICEW dd{};
        dd.cb = sizeof(dd);
        if (!EnumDisplayDevicesW(nullptr, i, &dd, 0)) break;
        if (!(dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)) continue;

        Display d;
        d.deviceName = dd.DeviceName;
        UniqueDC dc = OpenDisplayDC(d.deviceName);
        if (!dc || !GetDeviceGammaRamp(dc.get(), d.original.data())) {
            for (size_t ch = 0; ch < 3; ++ch)
                for (size_t k = 0; k < 256; ++k) d.original[ch * 256 + k] = static_cast<WORD>(k * 257);
        }
        m_displays.push_back(std::move(d));
    }
}

float GammaRamp::Curve(float x, float gamma, float shadowBoost) noexcept {
    x = std::clamp(x, 0.0f, 1.0f);
    float y = std::pow(x, 1.0f / std::max(gamma, 0.05f));
    const float inv = 1.0f - y;
    y += 2.0f * shadowBoost * y * inv * inv;  // monotonic for shadowBoost <= 1
    return std::clamp(y, 0.0f, 1.0f);
}

float GammaRamp::Channel(float x, const ColorParams& p, int channel, bool includeLinear) noexcept {
    if (includeLinear) {
        const float gains[3] = {p.gainR, p.gainG, p.gainB};
        x *= std::exp2(p.exposure) * gains[channel];
        x = (x - 0.5f) * p.contrast + 0.5f + p.brightness;
        x = std::clamp(x, 0.0f, 1.0f);
        if (p.invert) x = 1.0f - x;
    }
    return Curve(x, p.gamma, p.shadowBoost);
}

bool GammaRamp::IsNeutral(const ColorParams& p, bool includeLinear) noexcept {
    const bool curve = std::fabs(p.gamma - 1.0f) < 1e-4f && p.shadowBoost < 1e-4f;
    if (!includeLinear) return curve;
    return curve && std::fabs(p.exposure) < 1e-4f && std::fabs(p.contrast - 1.0f) < 1e-4f &&
           std::fabs(p.brightness) < 1e-4f && std::fabs(p.gainR - 1.0f) < 1e-4f &&
           std::fabs(p.gainG - 1.0f) < 1e-4f && std::fabs(p.gainB - 1.0f) < 1e-4f && !p.invert;
}

bool GammaRamp::Apply(const ColorParams& p, bool includeLinear) noexcept {
    if (IsNeutral(p, includeLinear)) {
        Restore();
        return true;
    }
    bool ok = !m_displays.empty();
    for (const Display& d : m_displays) {
        UniqueDC dc = OpenDisplayDC(d.deviceName);
        if (!dc) {
            ok = false;
            continue;
        }
        std::array<WORD, 768> ramp{};
        for (size_t ch = 0; ch < 3; ++ch) {
            for (size_t k = 0; k < 256; ++k) {
                const float y = Channel(k / 255.0f, p, static_cast<int>(ch), includeLinear) * 255.0f;
                const size_t i0 = static_cast<size_t>(y);
                const size_t i1 = std::min<size_t>(i0 + 1, 255);
                const float f = y - static_cast<float>(i0);
                // compose with the monitor's original (calibrated) ramp
                const float v = d.original[ch * 256 + i0] * (1.0f - f) + d.original[ch * 256 + i1] * f;
                ramp[ch * 256 + k] = static_cast<WORD>(std::clamp(std::lround(v), 0L, 65535L));
            }
        }
        if (SetDeviceGammaRamp(dc.get(), ramp.data()))
            m_modified = true;
        else
            ok = false;
    }
    return ok;
}

void GammaRamp::Restore() noexcept {
    if (!m_modified) return;
    for (const Display& d : m_displays) {
        UniqueDC dc = OpenDisplayDC(d.deviceName);
        if (dc) SetDeviceGammaRamp(dc.get(), const_cast<WORD*>(d.original.data()));
    }
    m_modified = false;
}

}  // namespace pv
