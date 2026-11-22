#include "Config.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <system_error>

namespace pv {

using nlohmann::json;

namespace {

constexpr int kConfigVersion = 3;
constexpr const char* kBindKeys[] = {"menu", "panic", "toggleAll", "profile1", "profile2", "profile3",
                                     "profile4", "nextProfile", "toggleCrosshair", "toggleShaders", "compare"};
static_assert(std::size(kBindKeys) == static_cast<size_t>(kBindCount));


json Write(const ColorParams& p) {
    return json{
        {"saturation", p.saturation}, {"vibrance", p.vibrance}, {"exposure", p.exposure}, {"contrast", p.contrast},
        {"brightness", p.brightness}, {"gamma", p.gamma},     {"shadowBoost", p.shadowBoost},
        {"gainR", p.gainR},         {"gainG", p.gainG},       {"gainB", p.gainB},
        {"invert", p.invert},
    };
}

json Write(const SharpenSettings& s) {
    return json{{"enabled", s.enabled}, {"strength", s.strength}, {"clamp", s.clamp}};
}

json Write(const VignetteSettings& v) {
    return json{{"enabled", v.enabled}, {"strength", v.strength}, {"radius", v.radius}, {"softness", v.softness}};
}

json Write(const CrosshairSettings& c) {
    return json{
        {"enabled", c.enabled}, {"style", c.style},     {"size", c.size},
        {"thickness", c.thickness}, {"gap", c.gap},     {"color", c.color},
        {"outline", c.outline}, {"outlineThickness", c.outlineThickness},
        {"offsetX", c.offsetX}, {"offsetY", c.offsetY}, {"anchor", c.anchor},
        {"rotation", c.rotation}, {"dotSize", c.dotSize}, {"outlineColor", c.outlineColor},
        {"armTop", c.armTop}, {"armBottom", c.armBottom}, {"armLeft", c.armLeft}, {"armRight", c.armRight},
        {"rainbow", c.rainbow}, {"rainbowSpeed", c.rainbowSpeed}, {"hideWhileZoom", c.hideWhileZoom},
        {"hideWhileRmb", c.hideWhileRmb}, {"imagePath", c.imagePath}, {"imageScale", c.imageScale},
        {"imageTint", c.imageTint},
    };
}

json Write(const ZoomSettings& z) {
    return json{{"enabled", z.enabled}, {"factor", z.factor}, {"radius", z.radius},
                {"holdKey", z.holdKey}, {"border", z.border}};
}

json Write(const Profile& p) {
    return json{
        {"name", p.name},
        {"color", Write(p.color)},
        {"sharpen", Write(p.sharpen)},
        {"vignette", Write(p.vignette)},
        {"crosshair", Write(p.crosshair)},
        {"zoom", Write(p.zoom)},
        {"autoExe", p.autoExe},
    };
}

// readers: current values act as defaults
template <typename T>
void ReadField(const json& j, const char* key, T& out) {
    if (auto it = j.find(key); it != j.end() && !it->is_null()) {
        try {
            out = it->get<T>();
        } catch (const json::exception&) {
            // wrong type, keep the default
        }
    }
}

void Read(const json& j, ColorParams& p) {
    if (!j.is_object()) return;
    ReadField(j, "saturation", p.saturation);
    ReadField(j, "vibrance", p.vibrance);
    ReadField(j, "exposure", p.exposure);
    ReadField(j, "contrast", p.contrast);
    ReadField(j, "brightness", p.brightness);
    ReadField(j, "gamma", p.gamma);
    ReadField(j, "shadowBoost", p.shadowBoost);
    ReadField(j, "gainR", p.gainR);
    ReadField(j, "gainG", p.gainG);
    ReadField(j, "gainB", p.gainB);
    ReadField(j, "invert", p.invert);
    ClampColorParams(p);
}

void Read(const json& j, SharpenSettings& s) {
    if (!j.is_object()) return;
    ReadField(j, "enabled", s.enabled);
    ReadField(j, "strength", s.strength);
    ReadField(j, "clamp", s.clamp);
    s.strength = std::clamp(s.strength, 0.0f, 2.0f);
    s.clamp = std::clamp(s.clamp, 0.005f, 0.5f);
}

void Read(const json& j, VignetteSettings& v) {
    if (!j.is_object()) return;
    ReadField(j, "enabled", v.enabled);
    ReadField(j, "strength", v.strength);
    ReadField(j, "radius", v.radius);
    ReadField(j, "softness", v.softness);
    v.strength = std::clamp(v.strength, 0.0f, 1.0f);
    v.radius = std::clamp(v.radius, 0.0f, 1.0f);
    v.softness = std::clamp(v.softness, 0.01f, 1.0f);
}

void Read(const json& j, CrosshairSettings& c) {
    if (!j.is_object()) return;
    ReadField(j, "enabled", c.enabled);
    ReadField(j, "style", c.style);
    ReadField(j, "size", c.size);
    ReadField(j, "thickness", c.thickness);
    ReadField(j, "gap", c.gap);
    ReadField(j, "color", c.color);
    ReadField(j, "outline", c.outline);
    ReadField(j, "outlineThickness", c.outlineThickness);
    ReadField(j, "offsetX", c.offsetX);
    ReadField(j, "offsetY", c.offsetY);
    ReadField(j, "anchor", c.anchor);
    ReadField(j, "rotation", c.rotation);
    ReadField(j, "dotSize", c.dotSize);
    ReadField(j, "outlineColor", c.outlineColor);
    ReadField(j, "armTop", c.armTop);
    ReadField(j, "armBottom", c.armBottom);
    ReadField(j, "armLeft", c.armLeft);
    ReadField(j, "armRight", c.armRight);
    ReadField(j, "rainbow", c.rainbow);
    ReadField(j, "rainbowSpeed", c.rainbowSpeed);
    ReadField(j, "hideWhileZoom", c.hideWhileZoom);
    ReadField(j, "hideWhileRmb", c.hideWhileRmb);
    ReadField(j, "imagePath", c.imagePath);
    ReadField(j, "imageScale", c.imageScale);
    ReadField(j, "imageTint", c.imageTint);
    c.rotation = std::clamp(c.rotation, -180.0f, 180.0f);
    c.dotSize = std::clamp(c.dotSize, 0.0f, 20.0f);
    c.rainbowSpeed = std::clamp(c.rainbowSpeed, 0.02f, 3.0f);
    c.imageScale = std::clamp(c.imageScale, 0.05f, 8.0f);
    for (float& ch : c.outlineColor) ch = std::clamp(ch, 0.0f, 1.0f);
    c.anchor = std::clamp(c.anchor, 0, 1);
    c.style = std::clamp(c.style, 0, static_cast<int>(CrosshairStyle::Count) - 1);
    c.size = std::clamp(c.size, 1.0f, 200.0f);
    c.thickness = std::clamp(c.thickness, 0.5f, 20.0f);
    c.gap = std::clamp(c.gap, 0.0f, 100.0f);
    c.outlineThickness = std::clamp(c.outlineThickness, 0.5f, 6.0f);
    for (float& ch : c.color) ch = std::clamp(ch, 0.0f, 1.0f);
}

void Read(const json& j, ZoomSettings& z) {
    if (!j.is_object()) return;
    ReadField(j, "enabled", z.enabled);
    ReadField(j, "factor", z.factor);
    ReadField(j, "radius", z.radius);
    ReadField(j, "holdKey", z.holdKey);
    ReadField(j, "border", z.border);
    z.factor = std::clamp(z.factor, 1.1f, 10.0f);
    z.radius = std::clamp(z.radius, 40.0f, 1000.0f);
    if (z.holdKey <= 0 || z.holdKey > 0xFE) z.holdKey = VK_XBUTTON2;
    for (float& ch : z.border) ch = std::clamp(ch, 0.0f, 1.0f);
}

void Read(const json& j, Profile& p) {
    if (!j.is_object()) return;
    ReadField(j, "name", p.name);
    if (p.name.size() > 64) p.name.resize(64);
    if (auto it = j.find("color"); it != j.end()) Read(*it, p.color);
    if (auto it = j.find("sharpen"); it != j.end()) Read(*it, p.sharpen);
    if (auto it = j.find("vignette"); it != j.end()) Read(*it, p.vignette);
    if (auto it = j.find("crosshair"); it != j.end()) Read(*it, p.crosshair);
    if (auto it = j.find("zoom"); it != j.end()) Read(*it, p.zoom);
    ReadField(j, "autoExe", p.autoExe);
    if (p.autoExe.size() > 260) p.autoExe.resize(260);
}

std::filesystem::path ExeDirectory() {
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD len = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (len == 0) return std::filesystem::current_path();
        if (len < buffer.size()) {
            buffer.resize(len);
            break;
        }
        buffer.resize(buffer.size() * 2);
    }
    return std::filesystem::path(buffer).parent_path();
}

}  // namespace

AppConfig::AppConfig() {
    for (int i = 0; i < kProfileCount; ++i) profiles[static_cast<size_t>(i)] = DefaultProfile(i);
    auto set = [&](BindId id, int vk, int mods) { binds[static_cast<size_t>(id)] = KeyBind{vk, mods}; };
    set(BindId::Menu, VK_INSERT, 0);
    set(BindId::Panic, VK_END, 0);
    for (int i = 0; i < kProfileCount; ++i) set(static_cast<BindId>(static_cast<int>(BindId::Profile1) + i), VK_F1 + i, MOD_CONTROL);
}

std::string KeyBindName(const KeyBind& b) {
    if (b.vk == 0) return "None";
    std::string s;
    if (b.mods & MOD_CONTROL) s += "Ctrl+";
    if (b.mods & MOD_ALT) s += "Alt+";
    if (b.mods & MOD_SHIFT) s += "Shift+";
    switch (b.vk) {
        case VK_LBUTTON: return s + "Mouse 1";
        case VK_RBUTTON: return s + "Mouse 2";
        case VK_MBUTTON: return s + "Mouse 3";
        case VK_XBUTTON1: return s + "Mouse 4";
        case VK_XBUTTON2: return s + "Mouse 5";
        default: break;
    }
    if (b.vk >= VK_F1 && b.vk <= VK_F24) return s + "F" + std::to_string(b.vk - VK_F1 + 1);
    UINT sc = MapVirtualKeyW(static_cast<UINT>(b.vk), MAPVK_VK_TO_VSC);
    switch (b.vk) {  // extended keys need the flag or they read as numpad
        case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
        case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_RCONTROL: case VK_RMENU: case VK_DIVIDE:
            sc |= 0x100; break;
        default: break;
    }
    char name[64] = {};
    if (sc && GetKeyNameTextA(static_cast<LONG>(sc << 16), name, sizeof(name)) > 0) return s + name;
    return s + "Key " + std::to_string(b.vk);
}

Profile AppConfig::DefaultProfile(int slot) {
    Profile p;
    const auto presets = GetColorPresets();
    switch (slot) {
        case 1:
            p.name = "Tactical";
            p.color = presets[static_cast<size_t>(PresetId::TacticalClarity)].params;
            p.sharpen.enabled = false;
            p.crosshair.enabled = false;
            break;
        case 2:
            p.name = "Night";
            p.color = presets[static_cast<size_t>(PresetId::NightVisionGreen)].params;
            break;
        case 3:
            p.name = "Daylight";
            p.color = presets[static_cast<size_t>(PresetId::DaylightVibrance)].params;
            break;
        default:
            p.name = "Default";
            break;
    }
    return p;
}

std::filesystem::path DefaultConfigPath() { return ExeDirectory() / L"config.json"; }

bool LoadConfig(const std::filesystem::path& path, AppConfig& cfg, std::string& error) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        error.clear();
        return false;  // first run, defaults stay
    }

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "Cannot open config.json";
        return false;
    }

    json root = json::parse(in, nullptr, /*allow_exceptions*/ false, /*ignore_comments*/ true);
    if (root.is_discarded() || !root.is_object()) {
        error = "config.json is corrupted, using defaults";
        return false;
    }

    int version = 1;
    ReadField(root, "version", version);
    ReadField(root, "masterEnabled", cfg.masterEnabled);
    ReadField(root, "activeProfile", cfg.activeProfile);
    cfg.activeProfile = std::clamp(cfg.activeProfile, 0, kProfileCount - 1);
    ReadField(root, "wallpaperPath", cfg.wallpaperPath);
    ReadField(root, "wallpaperDim", cfg.wallpaperDim);
    ReadField(root, "panelOpacity", cfg.wallpaperBlurPanels);
    cfg.wallpaperDim = std::clamp(cfg.wallpaperDim, 0.0f, 1.0f);
    cfg.wallpaperBlurPanels = std::clamp(cfg.wallpaperBlurPanels, 0.0f, 1.0f);
    ReadField(root, "wallpaperBlur", cfg.wallpaperBlur);
    ReadField(root, "accent", cfg.accent);
    ReadField(root, "saturationEngine", cfg.saturationEngine);
    cfg.saturationEngine = std::clamp(cfg.saturationEngine, 0, 1);
    ReadField(root, "vibranceMode", cfg.vibranceMode);
    cfg.vibranceMode = std::clamp(cfg.vibranceMode, 0, 1);
    ReadField(root, "startWithWindows", cfg.startWithWindows);
    ReadField(root, "showToasts", cfg.showToasts);
    ReadField(root, "autoProfile", cfg.autoProfile);
    ReadField(root, "pauseWhenNotFocused", cfg.pauseWhenNotFocused);
    if (auto it = root.find("binds"); it != root.end() && it->is_object()) {
        for (int i = 0; i < kBindCount; ++i) {
            if (auto b = it->find(kBindKeys[i]); b != it->end() && b->is_object()) {
                KeyBind& k = cfg.binds[static_cast<size_t>(i)];
                ReadField(*b, "vk", k.vk);
                ReadField(*b, "mods", k.mods);
                if (k.vk < 0 || k.vk > 0xFE) k.vk = 0;
                k.mods &= MOD_ALT | MOD_CONTROL | MOD_SHIFT;
            }
        }
    }
    cfg.wallpaperBlur = std::clamp(cfg.wallpaperBlur, 0.0f, 1.0f);
    for (float& ch : cfg.accent) ch = std::clamp(ch, 0.0f, 1.0f);

    if (auto it = root.find("profiles"); it != root.end() && it->is_array()) {
        const size_t n = std::min(it->size(), static_cast<size_t>(kProfileCount));
        for (size_t i = 0; i < n; ++i) Read((*it)[i], cfg.profiles[i]);
    }
    if (version < 2) {
        // v1 presets used shader vibrance, which needs full-screen capture and costs a lot of FPS.
        // Fold it into (free) driver saturation and switch off the default Night vignette.
        for (Profile& p : cfg.profiles) {
            if (p.color.vibrance > 0.0f) {
                p.color.saturation = std::clamp(p.color.saturation * (1.0f + p.color.vibrance * 0.5f), 0.0f, 4.0f);
                p.color.vibrance = 0.0f;
            }
            if (p.name == "Night") p.vignette.enabled = false;
        }
    }
    error.clear();
    return true;
}

bool SaveConfig(const std::filesystem::path& path, const AppConfig& cfg, std::string& error) {
    json root;
    root["version"] = kConfigVersion;
    root["masterEnabled"] = cfg.masterEnabled;
    root["activeProfile"] = cfg.activeProfile;
    root["wallpaperPath"] = cfg.wallpaperPath;
    root["wallpaperDim"] = cfg.wallpaperDim;
    root["panelOpacity"] = cfg.wallpaperBlurPanels;
    root["wallpaperBlur"] = cfg.wallpaperBlur;
    root["accent"] = cfg.accent;
    root["saturationEngine"] = cfg.saturationEngine;
    root["vibranceMode"] = cfg.vibranceMode;
    root["startWithWindows"] = cfg.startWithWindows;
    root["showToasts"] = cfg.showToasts;
    root["autoProfile"] = cfg.autoProfile;
    root["pauseWhenNotFocused"] = cfg.pauseWhenNotFocused;
    {
        json b = json::object();
        for (int i = 0; i < kBindCount; ++i)
            b[kBindKeys[i]] = json{{"vk", cfg.binds[static_cast<size_t>(i)].vk}, {"mods", cfg.binds[static_cast<size_t>(i)].mods}};
        root["binds"] = std::move(b);
    }
    json profiles = json::array();
    for (const Profile& p : cfg.profiles) profiles.push_back(Write(p));
    root["profiles"] = std::move(profiles);

    std::filesystem::path tmp = path;
    tmp += L".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = "Cannot write config";
            return false;
        }
        out << root.dump(2);
        if (!out.good()) {
            error = "Failed to write config";
            return false;
        }
    }
    // write to .tmp first so a crash never leaves a broken file
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = "Cannot replace config.json (error " + std::to_string(GetLastError()) + ")";
        std::error_code ec;
        std::filesystem::remove(tmp, ec);
        return false;
    }
    error.clear();
    return true;
}

}  // namespace pv
