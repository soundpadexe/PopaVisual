// PopaVisual: hardware color (gamma LUT + GPU driver saturation, DWM matrix as fallback)
// and a transparent overlay on top of games.

#include "ColorMatrix.h"
#include "Config.h"
#include "DriverColor.h"
#include "DxOverlay.h"
#include "UIManager.h"

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

namespace {

using Clock = std::chrono::steady_clock;

constexpr wchar_t kOverlayClass[] = L"PopaVisual.Overlay";
constexpr wchar_t kHostClass[] = L"PopaVisual.Host";
constexpr UINT WM_PV_TRAY = WM_APP + 1;
constexpr UINT kTrayId = 1;

enum HotkeyId : int {
    kHotkeyMenu = 1,
    kHotkeyPanic = 2,
    kHotkeyProfileBase = 10,  // 10..13
};

enum TrayCommand : UINT {
    kCmdProfileBase = 100,  // 100..103
    kCmdToggle = 200,
    kCmdShowMenu,
    kCmdPanic,
    kCmdExit,
};

// on crash, put the display back the way it was
pv::MagnificationColor* g_mag = nullptr;
pv::GammaRamp* g_gamma = nullptr;
pv::DriverSaturation* g_sat = nullptr;

LONG WINAPI CrashFilter(EXCEPTION_POINTERS*) {
    if (g_gamma) g_gamma->Restore();
    if (g_mag) g_mag->Reset();
    if (g_sat) g_sat->Restore();
    return EXCEPTION_CONTINUE_SEARCH;
}

struct HandleCloser {
    void operator()(HANDLE h) const noexcept {
        if (h) CloseHandle(h);
    }
};
using UniqueHandle = std::unique_ptr<std::remove_pointer_t<HANDLE>, HandleCloser>;

struct MenuDeleter {
    void operator()(HMENU m) const noexcept {
        if (m) DestroyMenu(m);
    }
};
using UniqueMenu = std::unique_ptr<std::remove_pointer_t<HMENU>, MenuDeleter>;

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
    return out;
}

class App {
public:
    int Run(HINSTANCE instance);

private:
    static LRESULT CALLBACK StaticWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    bool CreateWindows(HINSTANCE instance);
    bool InitGraphics();
    void ShutdownGraphics() noexcept;
    void RecreateGraphics();
    void RepositionOverlay();

    void RegisterHotkeys();
    void UnregisterHotkeys() noexcept;
    void AddTrayIcon();
    void RemoveTrayIcon() noexcept;
    void UpdateTrayTip();
    void ShowTrayMenu();
    void HandleCommand(UINT cmd);

    void SetMenuOpen(bool open);
    void SwitchProfile(int index);
    void ToggleMaster();
    void Panic();
    void RequestExit();

    void ApplyColorPipeline(bool force = false);
    void MarkConfigDirty();
    void SaveConfigNow();
    void ShowToast(std::string text);

    void Tick();
    void RenderFrame(UINT syncInterval);
    void UpdateLayout();
    void RestoreDisplay() noexcept;
    // vibrance only goes through the capture shader in "exact" mode
    float ShaderVibrance() const noexcept {
        return (m_cfg.masterEnabled && m_cfg.vibranceMode == 1) ? m_cfg.Active().color.vibrance : 0.0f;
    }

    [[nodiscard]] RECT PrimaryMonitorRect() const;

    // The overlay window only covers what it has to draw. A full-screen layered window
    // over a game forces DWM to compose it every frame; a hidden or tiny one does not.
    enum class Layout { Hidden, Box, Full };

    HINSTANCE m_instance = nullptr;
    HWND m_overlay = nullptr;
    HWND m_host = nullptr;
    HWND m_prevForeground = nullptr;
    UINT m_taskbarCreatedMsg = 0;
    float m_dpiScale = 1.0f;

    pv::AppConfig m_cfg;
    std::filesystem::path m_configPath;
    std::string m_configError;
    bool m_configDirty = false;
    Clock::time_point m_lastChange{};

    std::unique_ptr<pv::MagnificationColor> m_mag;
    std::unique_ptr<pv::GammaRamp> m_gamma;
    std::unique_ptr<pv::DriverSaturation> m_sat;
    pv::ColorParams m_appliedParams{};
    int m_appliedEngine = -1;
    bool m_hasApplied = false;
    bool m_magOk = true;
    bool m_gammaOk = true;
    std::string m_satEngineName = "none";

    Layout m_layout = Layout::Full;
    RECT m_layoutRect{};
    POINT m_anchor{};       // crosshair center in screen coordinates
    POINT m_lastGameCenter{};
    bool m_hasGameCenter = false;

    pv::DxOverlay m_dx;
    pv::UIManager m_ui;
    bool m_graphicsOk = false;
    bool m_excludedFromCapture = false;
    bool m_hotkeysOk = true;

    bool m_running = true;
    bool m_menuOpen = false;
    bool m_zoomHeld = false;
    bool m_contentDirty = true;
    float m_frameMs = 0.0f;
    Clock::time_point m_lastFrame{};
    Clock::time_point m_lastTopmost{};

    std::string m_toast;
    Clock::time_point m_toastUntil{};
};

int App::Run(HINSTANCE instance) {
    m_instance = instance;
    m_configPath = pv::DefaultConfigPath();
    pv::LoadConfig(m_configPath, m_cfg, m_configError);

    m_mag = std::make_unique<pv::MagnificationColor>();
    m_gamma = std::make_unique<pv::GammaRamp>();
    m_sat = std::make_unique<pv::DriverSaturation>();
    g_mag = m_mag.get();
    g_gamma = m_gamma.get();
    g_sat = m_sat.get();
    SetUnhandledExceptionFilter(CrashFilter);

    if (!CreateWindows(instance)) {
        MessageBoxW(nullptr, L"Failed to create the overlay window.", L"PopaVisual", MB_ICONERROR);
        return 1;
    }
    if (!InitGraphics()) {
        const std::wstring msg = L"Direct3D 11 / DirectComposition initialisation failed:\n\n" + m_dx.LastError() +
                                 L"\n\nColor filters (Magnification API) will still work.";
        MessageBoxW(nullptr, msg.c_str(), L"PopaVisual", MB_ICONWARNING);
    }

    RegisterHotkeys();
    AddTrayIcon();
    ApplyColorPipeline(true);
    ShowToast("PopaVisual running - Insert for menu");

    MSG msg{};
    while (m_running) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                m_running = false;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!m_running) break;
        Tick();
    }

    if (m_configDirty) SaveConfigNow();
    UnregisterHotkeys();
    RemoveTrayIcon();
    RestoreDisplay();
    ShutdownGraphics();
    if (m_overlay) DestroyWindow(m_overlay);
    if (m_host) DestroyWindow(m_host);
    g_mag = nullptr;
    g_gamma = nullptr;
    g_sat = nullptr;
    m_sat.reset();
    m_gamma.reset();
    m_mag.reset();
    return 0;
}

void App::RestoreDisplay() noexcept {
    if (m_gamma) m_gamma->Restore();
    if (m_mag) m_mag->Reset();
    if (m_sat) m_sat->Restore();
    m_hasApplied = false;
}

RECT App::PrimaryMonitorRect() const {
    const HMONITOR mon = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(mon, &mi)) return mi.rcMonitor;
    return RECT{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
}

bool App::CreateWindows(HINSTANCE instance) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = StaticWndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.lpszClassName = kOverlayClass;
    if (!RegisterClassExW(&wc)) return false;

    wc.lpszClassName = kHostClass;
    if (!RegisterClassExW(&wc)) return false;

    // hidden window for the tray icon and hotkeys
    m_host = CreateWindowExW(WS_EX_TOOLWINDOW, kHostClass, L"PopaVisual", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
                             instance, this);
    if (!m_host) return false;

    const RECT r = PrimaryMonitorRect();
    constexpr DWORD exStyle = WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_TOOLWINDOW |
                              WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP;
    m_overlay = CreateWindowExW(exStyle, kOverlayClass, L"PopaVisual Overlay", WS_POPUP, r.left, r.top,
                                r.right - r.left, r.bottom - r.top, nullptr, nullptr, instance, this);
    if (!m_overlay) return false;

    // alpha comes from DirectComposition; layered is what makes WS_EX_TRANSPARENT work
    SetLayeredWindowAttributes(m_overlay, 0, 255, LWA_ALPHA);
    // hide from capture, otherwise sharpen/zoom would see their own output
    m_excludedFromCapture = SetWindowDisplayAffinity(m_overlay, WDA_EXCLUDEFROMCAPTURE) != FALSE;

    m_dpiScale = static_cast<float>(GetDpiForWindow(m_overlay)) / 96.0f;
    m_taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");
    return true;
}

bool App::InitGraphics() {
    RECT rc{};
    GetClientRect(m_overlay, &rc);
    const HRESULT hr = m_dx.Initialize(m_overlay, static_cast<UINT>(rc.right - rc.left), static_cast<UINT>(rc.bottom - rc.top));
    if (FAILED(hr)) {
        m_graphicsOk = false;
        return false;
    }
    if (!m_ui.Initialize(m_overlay, m_dx.Device(), m_dx.Context(), m_dpiScale)) {
        m_graphicsOk = false;
        return false;
    }
    m_graphicsOk = true;
    m_contentDirty = true;
    return true;
}

void App::ShutdownGraphics() noexcept {
    m_ui.Shutdown();
    m_dx.Shutdown();
    m_graphicsOk = false;
}

void App::RecreateGraphics() {
    ShutdownGraphics();
    InitGraphics();
}

void App::RepositionOverlay() {
    const float newScale = static_cast<float>(GetDpiForWindow(m_overlay)) / 96.0f;
    if (newScale != m_dpiScale) {
        m_dpiScale = newScale;
        RecreateGraphics();
    } else if (m_graphicsOk) {
        m_dx.ResetCapture();  // output mode changed
    }
    m_layoutRect = RECT{};  // force a new layout
    UpdateLayout();
    m_contentDirty = true;
}

void App::UpdateLayout() {
    const RECT mon = PrimaryMonitorRect();
    const int monW = mon.right - mon.left;
    const int monH = mon.bottom - mon.top;
    const pv::Profile& p = m_cfg.Active();
    const bool on = m_cfg.masterEnabled;

    // crosshair center: monitor center, or the center of the game's client area
    // (a windowed game or a title bar shifts the game's own crosshair down)
    POINT anchor{mon.left + monW / 2, mon.top + monH / 2};
    if (p.crosshair.anchor == 1) {
        const HWND fg = GetForegroundWindow();
        RECT cr{};
        if (fg && fg != m_overlay && fg != m_host && !IsIconic(fg) && GetClientRect(fg, &cr) && cr.right > 64 &&
            cr.bottom > 64) {
            POINT tl{0, 0};
            ClientToScreen(fg, &tl);
            m_lastGameCenter = POINT{tl.x + cr.right / 2, tl.y + cr.bottom / 2};
            m_hasGameCenter = true;
        }
        if (m_hasGameCenter && PtInRect(&mon, m_lastGameCenter)) anchor = m_lastGameCenter;
    }
    if (anchor.x != m_anchor.x || anchor.y != m_anchor.y) {
        m_anchor = anchor;
        m_contentDirty = true;
    }

    Layout layout = Layout::Hidden;
    RECT r{};
    const bool full = m_menuOpen || m_ui.MenuVisible() ||
                      (on && (p.sharpen.enabled || ShaderVibrance() != 0.0f || p.vignette.enabled || m_zoomHeld));
    if (full) {
        layout = Layout::Full;
        r = mon;
    } else {
        const bool crosshair = on && p.crosshair.enabled;
        const bool toast = !m_toast.empty();
        if (crosshair) {
            const auto& x = p.crosshair;
            const float reach = std::max(x.size + x.gap, x.size) + x.thickness + x.outlineThickness * 2.0f + 6.0f;
            const int ext = static_cast<int>(std::ceil(reach));
            const int cx = m_anchor.x + static_cast<int>(std::lround(x.offsetX));
            const int cy = m_anchor.y + static_cast<int>(std::lround(x.offsetY));
            r = RECT{cx - ext, cy - ext, cx + ext, cy + ext};
        }
        if (toast) {
            const int halfW = static_cast<int>(420 * m_dpiScale);
            const RECT t{mon.left + monW / 2 - halfW, mon.top, mon.left + monW / 2 + halfW,
                         mon.top + static_cast<int>(110 * m_dpiScale)};
            if (crosshair)
                UnionRect(&r, &r, &t);
            else
                r = t;
        }
        if (crosshair || toast) {
            layout = Layout::Box;
            IntersectRect(&r, &r, &mon);
        }
    }

    m_ui.SetScreenOrigin(static_cast<float>(r.left - mon.left), static_cast<float>(r.top - mon.top),
                         static_cast<float>(monW), static_cast<float>(monH), static_cast<float>(anchor.x - mon.left),
                         static_cast<float>(anchor.y - mon.top));

    if (layout == m_layout && (layout == Layout::Hidden || EqualRect(&r, &m_layoutRect))) return;
    m_layout = layout;
    m_layoutRect = r;

    if (layout == Layout::Hidden) {
        ShowWindow(m_overlay, SW_HIDE);
        return;
    }
    const int w = std::max(1, static_cast<int>(r.right - r.left));
    const int h = std::max(1, static_cast<int>(r.bottom - r.top));
    SetWindowPos(m_overlay, HWND_TOPMOST, r.left, r.top, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    if (m_graphicsOk && FAILED(m_dx.Resize(static_cast<UINT>(w), static_cast<UINT>(h)))) RecreateGraphics();
    m_contentDirty = true;
}

void App::RegisterHotkeys() {
    m_hotkeysOk = true;
    m_hotkeysOk &= RegisterHotKey(m_host, kHotkeyMenu, MOD_NOREPEAT, VK_INSERT) != FALSE;
    m_hotkeysOk &= RegisterHotKey(m_host, kHotkeyPanic, MOD_NOREPEAT, VK_END) != FALSE;
    for (int i = 0; i < pv::kProfileCount; ++i)
        m_hotkeysOk &= RegisterHotKey(m_host, kHotkeyProfileBase + i, MOD_CONTROL | MOD_NOREPEAT, VK_F1 + i) != FALSE;
}

void App::UnregisterHotkeys() noexcept {
    UnregisterHotKey(m_host, kHotkeyMenu);
    UnregisterHotKey(m_host, kHotkeyPanic);
    for (int i = 0; i < pv::kProfileCount; ++i) UnregisterHotKey(m_host, kHotkeyProfileBase + i);
}

void App::AddTrayIcon() {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = m_host;
    nid.uID = kTrayId;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_PV_TRAY;
    nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    Shell_NotifyIconW(NIM_ADD, &nid);
    UpdateTrayTip();
}

void App::RemoveTrayIcon() noexcept {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = m_host;
    nid.uID = kTrayId;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

void App::UpdateTrayTip() {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = m_host;
    nid.uID = kTrayId;
    nid.uFlags = NIF_TIP;
    const std::wstring tip = L"PopaVisual - " + Widen(m_cfg.Active().name) + (m_cfg.masterEnabled ? L"" : L" (off)");
    wcsncpy_s(nid.szTip, tip.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void App::ShowTrayMenu() {
    UniqueMenu menu(CreatePopupMenu());
    if (!menu) return;
    for (int i = 0; i < pv::kProfileCount; ++i) {
        const std::wstring label = L"Ctrl+F" + std::to_wstring(i + 1) + L"   " + Widen(m_cfg.profiles[static_cast<size_t>(i)].name);
        AppendMenuW(menu.get(), MF_STRING | (m_cfg.activeProfile == i ? MF_CHECKED : 0), kCmdProfileBase + i, label.c_str());
    }
    AppendMenuW(menu.get(), MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu.get(), MF_STRING | (m_cfg.masterEnabled ? MF_CHECKED : 0), kCmdToggle, L"Enabled");
    AppendMenuW(menu.get(), MF_STRING, kCmdShowMenu, m_menuOpen ? L"Hide menu\tInsert" : L"Show menu\tInsert");
    AppendMenuW(menu.get(), MF_STRING, kCmdPanic, L"Panic reset\tEnd");
    AppendMenuW(menu.get(), MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu.get(), MF_STRING, kCmdExit, L"Exit");

    POINT pt{};
    GetCursorPos(&pt);
    SetForegroundWindow(m_host);  // otherwise the menu does not close on outside click
    const UINT cmd = static_cast<UINT>(TrackPopupMenu(menu.get(), TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
                                                      pt.x, pt.y, 0, m_host, nullptr));
    PostMessageW(m_host, WM_NULL, 0, 0);
    if (cmd) HandleCommand(cmd);
}

void App::HandleCommand(UINT cmd) {
    if (cmd >= kCmdProfileBase && cmd < kCmdProfileBase + pv::kProfileCount) {
        SwitchProfile(static_cast<int>(cmd - kCmdProfileBase));
        return;
    }
    switch (cmd) {
        case kCmdToggle: ToggleMaster(); break;
        case kCmdShowMenu: SetMenuOpen(!m_menuOpen); break;
        case kCmdPanic: Panic(); break;
        case kCmdExit: RequestExit(); break;
        default: break;
    }
}

void App::SetMenuOpen(bool open) {
    if (open == m_menuOpen || !m_graphicsOk) return;
    m_menuOpen = open;

    LONG_PTR ex = GetWindowLongPtrW(m_overlay, GWL_EXSTYLE);
    if (open) {
        m_prevForeground = GetForegroundWindow();
        UpdateLayout();  // go full screen before taking focus
        ex &= ~static_cast<LONG_PTR>(WS_EX_TRANSPARENT | WS_EX_NOACTIVATE);
        SetWindowLongPtrW(m_overlay, GWL_EXSTYLE, ex);
        SetWindowPos(m_overlay, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED);
        SetForegroundWindow(m_overlay);
        SetFocus(m_overlay);
    } else {
        ex |= static_cast<LONG_PTR>(WS_EX_TRANSPARENT | WS_EX_NOACTIVATE);
        SetWindowLongPtrW(m_overlay, GWL_EXSTYLE, ex);
        SetWindowPos(m_overlay, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        if (m_prevForeground && IsWindow(m_prevForeground) && m_prevForeground != m_overlay)
            SetForegroundWindow(m_prevForeground);
        m_prevForeground = nullptr;
        if (m_configDirty) SaveConfigNow();
    }
    m_contentDirty = true;
}

void App::SwitchProfile(int index) {
    if (index < 0 || index >= pv::kProfileCount) return;
    m_cfg.activeProfile = index;
    ApplyColorPipeline();
    MarkConfigDirty();
    UpdateTrayTip();
    ShowToast("Ctrl+F" + std::to_string(index + 1) + ": " + m_cfg.Active().name);
}

void App::ToggleMaster() {
    m_cfg.masterEnabled = !m_cfg.masterEnabled;
    ApplyColorPipeline();
    MarkConfigDirty();
    UpdateTrayTip();
    ShowToast(m_cfg.masterEnabled ? "PopaVisual on" : "PopaVisual off");
}

void App::Panic() {
    pv::Profile& p = m_cfg.Active();
    p.color = pv::ColorParams{};
    p.sharpen.enabled = false;
    p.vignette.enabled = false;
    p.zoom.enabled = false;

    RestoreDisplay();
    ApplyColorPipeline(true);
    MarkConfigDirty();
    ShowToast("Filters reset");
}

void App::RequestExit() {
    SetMenuOpen(false);
    m_running = false;
    PostQuitMessage(0);
}

// Routing, cheapest first:
//   per-channel (exposure, RGB, contrast, brightness, invert, gamma, shadows) -> gamma LUT
//   saturation -> GPU driver; DWM matrix only if no driver support or "exact" is chosen
//   if Windows rejects the full LUT, the linear part moves to the DWM matrix instead.
// The DWM effect is the only part that costs game FPS, so it is used as little as possible.
void App::ApplyColorPipeline(bool force) {
    pv::ColorParams params = m_cfg.masterEnabled ? m_cfg.Active().color : pv::ColorParams{};
    // engine id also encodes the vibrance mode so a mode switch re-applies
    const int engine = m_cfg.saturationEngine + m_cfg.vibranceMode * 2;
    if (!force && m_hasApplied && params == m_appliedParams && engine == m_appliedEngine) return;
    m_appliedParams = params;
    m_appliedEngine = engine;
    m_hasApplied = true;

    // Fast vibrance: hand it to the display hardware together with saturation
    // (NVIDIA Digital Vibrance / AMD saturation). No capture, no overlay, 0 FPS.
    if (m_cfg.vibranceMode == 0) {
        params.saturation = std::clamp(params.saturation * (1.0f + 0.6f * params.vibrance), 0.0f, 4.0f);
        params.vibrance = 0.0f;
    }

    pv::ColorParams matrixPart;  // what the DWM matrix still has to do
    if (m_gamma->Apply(params, true)) {
        m_gammaOk = true;
        matrixPart.saturation = params.saturation;
    } else {
        m_gammaOk = m_gamma->Apply(params, false);
        matrixPart = params;
        matrixPart.gamma = 1.0f;
        matrixPart.shadowBoost = 0.0f;
        matrixPart.vibrance = 0.0f;
    }

    const bool sat = std::fabs(params.saturation - 1.0f) > 1e-4f;
    const bool driver = engine == 0 && m_sat->Available() && (params.saturation >= 1.0f || m_sat->SupportsDesaturate());
    if (driver) {
        m_sat->Set(params.saturation);
        matrixPart.saturation = 1.0f;
        m_satEngineName = m_sat->Name();
    } else {
        m_sat->Restore();
        m_satEngineName = sat ? "DWM matrix" : (m_sat->Available() ? m_sat->Name() : "DWM matrix");
    }

    m_magOk = m_mag->Apply(pv::BuildColorMatrix(matrixPart));
    m_contentDirty = true;
}

void App::MarkConfigDirty() {
    m_configDirty = true;
    m_lastChange = Clock::now();
    m_contentDirty = true;
}

void App::SaveConfigNow() {
    if (pv::SaveConfig(m_configPath, m_cfg, m_configError)) m_configDirty = false;
    else m_lastChange = Clock::now();
}

void App::ShowToast(std::string text) {
    m_toast = std::move(text);
    m_toastUntil = Clock::now() + std::chrono::milliseconds(1800);
    m_contentDirty = true;
}

void App::Tick() {
    const auto now = Clock::now();
    const pv::Profile& p = m_cfg.Active();

    const bool zoomHeld = m_cfg.masterEnabled && p.zoom.enabled && !m_menuOpen &&
                          (GetAsyncKeyState(p.zoom.holdKey) & 0x8000) != 0;
    if (zoomHeld != m_zoomHeld) {
        m_zoomHeld = zoomHeld;
        m_contentDirty = true;
    }

    // an open duplication session alone makes DWM do extra work, so it only exists while needed
    if (m_graphicsOk) m_dx.SetCaptureEnabled(m_cfg.masterEnabled && (p.sharpen.enabled || ShaderVibrance() != 0.0f || m_zoomHeld));

    if (!m_toast.empty() && now >= m_toastUntil + std::chrono::milliseconds(50)) {
        m_toast.clear();
        m_contentDirty = true;
    }
    const bool toastActive = !m_toast.empty();

    UpdateLayout();

    const bool animating = m_menuOpen || m_ui.MenuVisible() || toastActive;
    const bool captureLive = m_cfg.masterEnabled && (p.sharpen.enabled || ShaderVibrance() != 0.0f || m_zoomHeld);
    const DWORD idleTimeout = (m_cfg.masterEnabled && p.zoom.enabled) ? 8 : 100;

    if (!m_graphicsOk || m_layout == Layout::Hidden) {
        m_frameMs = 0.0f;
        m_contentDirty = false;
        MsgWaitForMultipleObjectsEx(0, nullptr, idleTimeout, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    } else if (animating) {
        // menu / toast: paced by vblank
        if (captureLive) m_dx.PollCapture(0);
        m_contentDirty = false;
        RenderFrame(1);
    } else if (captureLive) {
        // shaders: redraw only when the desktop really produced a new frame
        const bool fresh = m_dx.PollCapture(8);
        if (fresh || m_contentDirty) {
            m_contentDirty = false;
            RenderFrame(0);
        }
    } else if (m_contentDirty) {
        // static content (crosshair): DComp keeps the last frame, draw once
        m_contentDirty = false;
        RenderFrame(0);
    } else {
        m_frameMs = 0.0f;
        MsgWaitForMultipleObjectsEx(0, nullptr, idleTimeout, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }

    // some games steal topmost
    if (m_layout != Layout::Hidden && now - m_lastTopmost > std::chrono::seconds(2)) {
        m_lastTopmost = now;
        SetWindowPos(m_overlay, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    }

    if (m_configDirty && now - m_lastChange > std::chrono::seconds(1)) SaveConfigNow();
}

void App::RenderFrame(UINT syncInterval) {
    const auto frameStart = Clock::now();

    m_dx.BeginFrame();
    if (m_cfg.masterEnabled) {
        const pv::Profile& p = m_cfg.Active();
        pv::EffectParams ep;
        ep.sharpen = p.sharpen.enabled;
        ep.vibrance = ShaderVibrance();
        ep.sharpenStrength = p.sharpen.strength;
        ep.sharpenClamp = p.sharpen.clamp;
        ep.vignette = p.vignette.enabled;
        ep.vignetteStrength = p.vignette.strength;
        ep.vignetteRadius = p.vignette.radius;
        ep.vignetteSoftness = p.vignette.softness;
        ep.zoom = m_zoomHeld;
        ep.zoomFactor = p.zoom.factor;
        ep.zoomRadius = p.zoom.radius;
        ep.zoomBorder = p.zoom.border;
        m_dx.RenderEffects(ep);
    }

    m_ui.BeginFrame(m_menuOpen);
    if (m_cfg.masterEnabled && m_cfg.Active().crosshair.enabled) m_ui.DrawCrosshair(m_cfg.Active().crosshair);

    if (!m_toast.empty()) {
        const float remaining = std::chrono::duration<float>(m_toastUntil - Clock::now()).count();
        m_ui.DrawToast(m_toast, std::clamp(remaining / 0.35f, 0.0f, 1.0f));
    }

    pv::UiResult ui;
    if (m_menuOpen || m_ui.MenuVisible()) {
        pv::UiStatus status;
        status.magnificationOk = m_magOk;
        status.gammaOk = m_gammaOk;
        status.captureActive = m_dx.CaptureActive();
        status.captureExcluded = m_excludedFromCapture;
        status.hotkeysOk = m_hotkeysOk;
        status.frameMs = m_frameMs;
        status.overlayError = Narrow(m_dx.LastError());
        status.configError = m_configError;
        status.configPath = Narrow(m_configPath.wstring());
        status.saturationEngine = m_satEngineName;
        status.dwmEffectActive = m_mag->Active();
        {
            const pv::Profile& ap = m_cfg.Active();
            std::string cost;
            auto add = [&](bool on, const char* what) {
                if (!on) return;
                if (!cost.empty()) cost += ", ";
                cost += what;
            };
            const bool on = m_cfg.masterEnabled;
            add(m_mag->Active(), "DWM color effect");
            add(on && ShaderVibrance() != 0.0f, "Vibrance");
            add(on && ap.sharpen.enabled, "LumaSharpen");
            add(on && ap.vignette.enabled, "Vignette");
            status.fpsCost = cost;
        }
        status.driverSaturationAvailable = m_sat->Available();

        const int profileBefore = m_cfg.activeProfile;
        ui = m_ui.DrawMenu(m_cfg, status, m_menuOpen);
        if (ui.changed) {
            ApplyColorPipeline();
            MarkConfigDirty();
            if (profileBefore != m_cfg.activeProfile) UpdateTrayTip();
        }
    }
    m_ui.EndFrame();

    const HRESULT hr = m_dx.Present(syncInterval);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        const bool wasOpen = m_menuOpen;
        SetMenuOpen(false);
        RecreateGraphics();
        if (wasOpen) SetMenuOpen(true);
        return;
    }

    const float ms = std::chrono::duration<float, std::milli>(Clock::now() - frameStart).count();
    m_frameMs = m_frameMs <= 0.0f ? ms : m_frameMs * 0.9f + ms * 0.1f;
    m_lastFrame = frameStart;

    if (ui.panic) Panic();
    if (ui.closeMenu) SetMenuOpen(false);
    if (ui.exitApp) RequestExit();
}

LRESULT CALLBACK App::StaticWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    App* app = nullptr;
    if (msg == WM_NCCREATE) {
        app = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    } else {
        app = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    return app ? app->WndProc(hwnd, msg, wParam, lParam) : DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT App::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (hwnd == m_overlay) {
        if (m_menuOpen && m_ui.Initialized() && pv::UIManager::HandleMessage(hwnd, msg, wParam, lParam)) return TRUE;
        switch (msg) {
            case WM_MOUSEACTIVATE:
                return m_menuOpen ? MA_ACTIVATE : MA_NOACTIVATE;
            case WM_SETCURSOR:
                if (m_menuOpen) {
                    SetCursor(nullptr);
                    return TRUE;
                }
                break;
            case WM_DISPLAYCHANGE:
            case WM_DPICHANGED:
                RepositionOverlay();
                return 0;
            case WM_CLOSE:
                RequestExit();
                return 0;
            default:
                break;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    if (hwnd == m_host || m_host == nullptr) {
        if (msg == m_taskbarCreatedMsg && m_taskbarCreatedMsg != 0) {
            AddTrayIcon();  // explorer restarted
            return 0;
        }
        switch (msg) {
            case WM_HOTKEY: {
                const int id = static_cast<int>(wParam);
                if (id == kHotkeyMenu)
                    SetMenuOpen(!m_menuOpen);
                else if (id == kHotkeyPanic)
                    Panic();
                else if (id >= kHotkeyProfileBase && id < kHotkeyProfileBase + pv::kProfileCount)
                    SwitchProfile(id - kHotkeyProfileBase);
                return 0;
            }
            case WM_PV_TRAY:
                switch (LOWORD(lParam)) {
                    case WM_RBUTTONUP:
                    case WM_CONTEXTMENU:
                        ShowTrayMenu();
                        break;
                    case WM_LBUTTONDBLCLK:
                        SetMenuOpen(!m_menuOpen);
                        break;
                    default:
                        break;
                }
                return 0;
            case WM_DISPLAYCHANGE:
                if (m_overlay) RepositionOverlay();
                return 0;
            case WM_QUERYENDSESSION:
                return TRUE;
            case WM_ENDSESSION:
                if (wParam) {
                    if (m_configDirty) SaveConfigNow();
                    RestoreDisplay();
                }
                return 0;
            case WM_CLOSE:
                RequestExit();
                return 0;
            default:
                break;
        }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace

int WINAPI wWinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE, _In_ LPWSTR, _In_ int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    // COM for the WIC image loader and the file dialog
    const HRESULT comHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    UniqueHandle mutex(CreateMutexW(nullptr, TRUE, L"Local\\PopaVisual.SingleInstance"));
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr, L"PopaVisual is already running (see tray).", L"PopaVisual", MB_ICONINFORMATION);
        return 0;
    }

    int code = 0;
    {
        App app;
        code = app.Run(instance);
    }
    if (SUCCEEDED(comHr)) CoUninitialize();
    return code;
}
