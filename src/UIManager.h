#pragma once

#include "Config.h"

#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <vector>

struct ImFont;
struct ImDrawList;
struct ImVec2;

namespace pv {

struct UiStatus {
    bool magnificationOk = false;
    bool gammaOk = true;
    bool captureActive = false;
    bool captureExcluded = false;
    bool hotkeysOk = true;
    float frameMs = 0.0f;
    std::string overlayError;
    std::string configError;
    std::string configPath;
    std::string saturationEngine;
    bool dwmEffectActive = false;
    std::string fpsCost;  // features that currently cost game FPS, empty = none
    bool driverSaturationAvailable = false;
};

struct UiResult {
    bool changed = false;
    bool panic = false;
    bool closeMenu = false;
    bool exitApp = false;
};

class UIManager {
public:
    UIManager() = default;
    ~UIManager();
    UIManager(const UIManager&) = delete;
    UIManager& operator=(const UIManager&) = delete;

    bool Initialize(HWND hwnd, ID3D11Device* device, ID3D11DeviceContext* context, float dpiScale);
    void Shutdown() noexcept;
    bool Initialized() const noexcept { return m_initialized; }

    void BeginFrame(bool menuOpen);
    // Where our window sits on the monitor; the overlay window shrinks to what it draws.
    // Where our window sits on the monitor, and where the crosshair center is (monitor-relative).
    void SetScreenOrigin(float x, float y, float screenW, float screenH, float centerX, float centerY) noexcept {
        m_originX = x;
        m_originY = y;
        m_screenW = screenW;
        m_screenH = screenH;
        m_centerX = centerX;
        m_centerY = centerY;
    }
    void DrawCrosshair(const CrosshairSettings& s, float time);
    // half-size of the box the crosshair needs, px
    float CrosshairReach(const CrosshairSettings& s);
    bool CapturingBind() const noexcept { return m_capture != nullptr; }
    void SetForegroundExe(const std::string& exe) { m_fgExe = exe; }
    void DrawToast(const std::string& text, float alpha);
    // Call while open, and keep calling after close until MenuVisible() is false (fade-out).
    UiResult DrawMenu(AppConfig& cfg, const UiStatus& status, bool open);
    bool MenuVisible() const noexcept { return m_menuT > 0.0f; }
    void EndFrame();

    static bool HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

private:
    enum class Tab : int { Color = 0, Effects, Crosshair, Zoom, Profiles, Advanced, Settings, Count };

    void LoadFonts();
    void ApplyStyle();
    void ApplyAccent(const std::array<float, 3>& accent);

    void DrawWindowBackground(const AppConfig& cfg);
    void DrawHeader(AppConfig& cfg);
    void DrawSidebar();
    void ColorTab(AppConfig& cfg, const UiStatus& st);
    void EffectsTab(AppConfig& cfg, const UiStatus& st);
    void CrosshairTab(AppConfig& cfg);
    void ZoomTab(AppConfig& cfg);
    void ProfilesTab(AppConfig& cfg);
    void SettingsTab(AppConfig& cfg, const UiStatus& st);
    void AdvancedTab(AppConfig& cfg);
    bool BindButton(const char* id, KeyBind& b, bool allowMods, float width);
    void UpdateBindCapture();
    void SyncCrosshairImage(const std::string& path);

    // widgets
    bool Toggle(const char* label, bool* v);
    bool Slider(const char* label, float* v, float min, float max, float def, const char* fmt = "%.2f");
    void LabelRow(const char* label);
    void CostNote(const char* text);
    bool NavItem(int index, const char* label, bool selected);
    bool PresetTile(int index, bool active, float width);
    void BeginCard(const char* id, const char* title);
    void EndCard();
    void ToneCurve(const ColorParams& p, float w, float h);
    bool AccentButton(const char* label, const ImVec2& size, bool danger = false);

    // frosted glass: blurred wallpaper under a rect
    void Glass(ImDrawList* dl, const ImVec2& p0, const ImVec2& p1, float rounding, int flags, float tint);
    void SyncWallpaper(const AppConfig& cfg);
    void RebuildBlur(float strength);
    bool BrowseForImage(std::string& outPath);

    float Anim(unsigned id, float target, float speed = 14.0f);

    static void DrawIcon(ImDrawList* dl, int icon, const ImVec2& c, float size, unsigned col);
    void DrawCrosshairShape(ImDrawList* dl, float cx, float cy, const CrosshairSettings& s, float scale, float time);
    bool LoadImageTexture(const std::string& path, UINT maxSide, Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& out,
                          float& w, float& h, std::string& err);

    bool m_initialized = false;
    HWND m_hwnd = nullptr;
    float m_scale = 1.0f;
    UiResult* m_res = nullptr;

    Tab m_tab = Tab::Color;
    float m_menuT = 0.0f;     // open/close fade
    float m_tabT = 1.0f;      // content transition
    float m_navY = -1.0f;     // animated sidebar indicator
    float m_profileX = -1.0f; // animated profile selector
    std::array<float, 3> m_accent{-1, -1, -1};

    // main window rect and wallpaper UVs (cover fit), used to map glass panels
    float m_winX = 0, m_winY = 0, m_winW = 1, m_winH = 1;
    float m_uv0x = 0, m_uv0y = 0, m_uv1x = 1, m_uv1y = 1;
    float m_panelTint = 0.55f;
    float m_originX = 0, m_originY = 0, m_screenW = 1920, m_screenH = 1080;
    float m_centerX = 960, m_centerY = 540;

    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_wallpaper;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_wallBlur;
    std::vector<uint8_t> m_blurSource;  // downscaled RGBA used to rebuild the blur
    int m_blurW = 0, m_blurH = 0;
    float m_blurBuilt = -1.0f;
    float m_wallW = 0.0f, m_wallH = 0.0f;
    std::string m_wallLoaded;
    std::string m_wallError;

    // custom crosshair image
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_xhTex;
    float m_xhW = 0.0f, m_xhH = 0.0f;
    std::string m_xhLoaded;
    std::string m_xhError;

    // key bind capture
    KeyBind* m_capture = nullptr;
    bool m_captureMods = true;
    std::array<bool, 256> m_captureDown{};
    int m_captureFrames = 0;
    std::string m_fgExe;

    ImFont* m_fontRegular = nullptr;
    ImFont* m_fontBold = nullptr;
    ImFont* m_fontTitle = nullptr;
    ImFont* m_fontSmall = nullptr;
};

}  // namespace pv
