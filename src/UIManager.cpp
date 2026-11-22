#define IMGUI_DEFINE_MATH_OPERATORS
#include "UIManager.h"

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <misc/cpp/imgui_stdlib.h>

#include <objbase.h>
#include <shobjidl.h>
#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace pv {

using Microsoft::WRL::ComPtr;

namespace {

constexpr float kPi = 3.14159265f;

// graphite palette; the accent is user-configurable
ImVec4 gAccent{0.91f, 0.34f, 0.23f, 1.0f};
constexpr ImVec4 kBg{0.071f, 0.075f, 0.086f, 1.0f};
constexpr ImVec4 kPanel{0.102f, 0.106f, 0.122f, 1.0f};
constexpr ImVec4 kRaised{0.140f, 0.145f, 0.165f, 1.0f};
constexpr ImVec4 kLine{1.0f, 1.0f, 1.0f, 0.07f};
constexpr ImVec4 kText{0.90f, 0.90f, 0.92f, 1.0f};
constexpr ImVec4 kDim{0.54f, 0.55f, 0.59f, 1.0f};
constexpr ImVec4 kWarn{0.96f, 0.72f, 0.28f, 1.0f};
constexpr ImVec4 kDanger{0.92f, 0.30f, 0.32f, 1.0f};

ImVec4 WithA(const ImVec4& c, float a) { return ImVec4(c.x, c.y, c.z, c.w * a); }
// GetColorU32 applies the global style alpha, so everything fades with the menu
ImU32 Col(const ImVec4& c, float a = 1.0f) { return ImGui::GetColorU32(WithA(c, a)); }
ImVec4 Mix(const ImVec4& a, const ImVec4& b, float t) {
    return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t);
}
float EaseOut(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    const float u = 1.0f - t;
    return 1.0f - u * u * u;
}

struct HoldKey {
    int vk;
    const char* name;
};
constexpr HoldKey kHoldKeys[] = {
    {VK_XBUTTON1, "Mouse 4"},     {VK_XBUTTON2, "Mouse 5"},  {VK_RBUTTON, "Right Mouse"},
    {VK_MBUTTON, "Middle Mouse"}, {VK_LSHIFT, "Left Shift"}, {VK_LMENU, "Left Alt"},
    {VK_LCONTROL, "Left Ctrl"},   {VK_CAPITAL, "Caps Lock"}, {'Z', "Z"},
    {'X', "X"},                   {'C', "C"},                {'V', "V"},
};

const char* HoldKeyName(int vk) {
    for (const auto& k : kHoldKeys)
        if (k.vk == vk) return k.name;
    return "Custom";
}

constexpr const char* kStyles[] = {"Dot", "Cross", "Circle", "Cross + Dot", "T-Shape", "Circle + Dot"};
static_assert(std::size(kStyles) == static_cast<size_t>(CrosshairStyle::Count));

constexpr const char* kTabNames[] = {"Color", "Shaders", "Crosshair", "Zoom", "Profiles", "Settings"};

// CPU copy of the pipeline for previews: vibrance -> DWM matrix -> gamma ramp
std::array<float, 3> Preview(const Mat5& m, const ColorParams& p, float r, float g, float b) {
    if (p.vibrance != 0.0f) {
        const float y = r * kLumaR + g * kLumaG + b * kLumaB;
        const float sat = std::max({r, g, b}) - std::min({r, g, b});
        const float k = 1.0f + p.vibrance * (1.0f - sat);
        r = std::clamp(y + (r - y) * k, 0.0f, 1.0f);
        g = std::clamp(y + (g - y) * k, 0.0f, 1.0f);
        b = std::clamp(y + (b - y) * k, 0.0f, 1.0f);
    }
    auto c = TransformColor(m, r, g, b);
    for (float& ch : c) ch = GammaRamp::Curve(ch, p.gamma, p.shadowBoost);
    return c;
}

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::string Narrow(const wchar_t* w) {
    if (!w || !*w) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n > 0 ? n - 1 : 0), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), n, nullptr, nullptr);
    return out;
}

struct CoTaskMemDeleter {
    void operator()(wchar_t* p) const noexcept { CoTaskMemFree(p); }
};

// one pass of a sliding-window box blur along lines of an RGBA image
void BoxBlurPass(const uint8_t* src, uint8_t* dst, int lines, int len, int lineStride, int pixStride, int r) {
    const int n = 2 * r + 1;
    for (int l = 0; l < lines; ++l) {
        const uint8_t* in = src + static_cast<ptrdiff_t>(l) * lineStride;
        uint8_t* out = dst + static_cast<ptrdiff_t>(l) * lineStride;
        int acc[4] = {0, 0, 0, 0};
        for (int i = -r; i <= r; ++i) {
            const int x = std::clamp(i, 0, len - 1);
            for (int c = 0; c < 4; ++c) acc[c] += in[x * pixStride + c];
        }
        for (int x = 0; x < len; ++x) {
            for (int c = 0; c < 4; ++c) out[x * pixStride + c] = static_cast<uint8_t>(acc[c] / n);
            const int add = std::min(x + r + 1, len - 1);
            const int sub = std::max(x - r, 0);
            for (int c = 0; c < 4; ++c) acc[c] += in[add * pixStride + c] - in[sub * pixStride + c];
        }
    }
}

ComPtr<ID3D11ShaderResourceView> MakeTexture(ID3D11Device* dev, const uint8_t* pixels, UINT w, UINT h) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = w;
    desc.Height = h;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init{pixels, w * 4, 0};
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11ShaderResourceView> srv;
    if (FAILED(dev->CreateTexture2D(&desc, &init, &tex))) return nullptr;
    if (FAILED(dev->CreateShaderResourceView(tex.Get(), nullptr, &srv))) return nullptr;
    return srv;
}

bool ReadScaled(IWICImagingFactory* wic, IWICBitmapSource* src, UINT w, UINT h, std::vector<uint8_t>& out) {
    ComPtr<IWICBitmapSource> s = src;
    UINT sw = 0, sh = 0;
    src->GetSize(&sw, &sh);
    if (sw != w || sh != h) {
        ComPtr<IWICBitmapScaler> scaler;
        if (FAILED(wic->CreateBitmapScaler(&scaler)) ||
            FAILED(scaler->Initialize(src, w, h, WICBitmapInterpolationModeFant)))
            return false;
        s = scaler;
    }
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(wic->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(s.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                WICBitmapPaletteTypeCustom)))
        return false;
    out.resize(static_cast<size_t>(w) * h * 4);
    return SUCCEEDED(conv->CopyPixels(nullptr, w * 4, static_cast<UINT>(out.size()), out.data()));
}

}  // namespace

UIManager::~UIManager() { Shutdown(); }

bool UIManager::Initialize(HWND hwnd, ID3D11Device* device, ID3D11DeviceContext* context, float dpiScale) {
    Shutdown();
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    m_hwnd = hwnd;
    m_device = device;
    m_scale = std::clamp(dpiScale, 0.75f, 4.0f);
    LoadFonts();
    ApplyStyle();

    if (!ImGui_ImplWin32_Init(hwnd)) {
        ImGui::DestroyContext();
        return false;
    }
    if (!ImGui_ImplDX11_Init(device, context)) {
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        return false;
    }
    m_initialized = true;
    return true;
}

void UIManager::Shutdown() noexcept {
    m_wallpaper.Reset();
    m_wallBlur.Reset();
    m_blurSource.clear();
    m_wallLoaded.clear();
    m_blurBuilt = -1.0f;
    m_device.Reset();
    m_accent = {-1, -1, -1};
    if (!m_initialized) return;
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    m_initialized = false;
    m_fontRegular = m_fontBold = m_fontTitle = m_fontSmall = nullptr;
}

bool UIManager::HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (!ImGui::GetCurrentContext()) return false;
    return ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam) != 0;
}

void UIManager::LoadFonts() {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();

    char winDir[MAX_PATH]{};
    GetWindowsDirectoryA(winDir, MAX_PATH);
    const auto dir = std::filesystem::path(winDir) / "Fonts";

    auto load = [&](const char* file, float size) -> ImFont* {
        std::error_code ec;
        const auto path = dir / file;
        if (!std::filesystem::exists(path, ec)) return nullptr;
        ImFontConfig cfg;
        cfg.OversampleH = 3;
        cfg.OversampleV = 2;
        return io.Fonts->AddFontFromFileTTF(path.string().c_str(), size * m_scale, &cfg);
    };

    m_fontRegular = load("segoeui.ttf", 15.5f);
    if (!m_fontRegular) {
        ImFontConfig cfg;
        cfg.SizePixels = 13.0f * m_scale;
        m_fontRegular = io.Fonts->AddFontDefault(&cfg);
    }
    m_fontBold = load("seguisb.ttf", 15.5f);
    if (!m_fontBold) m_fontBold = m_fontRegular;
    m_fontTitle = load("seguisb.ttf", 20.0f);
    if (!m_fontTitle) m_fontTitle = m_fontBold;
    m_fontSmall = load("segoeui.ttf", 12.5f);
    if (!m_fontSmall) m_fontSmall = m_fontRegular;
    io.FontDefault = m_fontRegular;
}

void UIManager::ApplyStyle() {
    ImGuiStyle& s = ImGui::GetStyle();
    s = ImGuiStyle();
    s.WindowRounding = 10.0f;
    s.ChildRounding = 8.0f;
    s.FrameRounding = 6.0f;
    s.PopupRounding = 8.0f;
    s.ScrollbarRounding = 6.0f;
    s.GrabRounding = 6.0f;
    s.WindowBorderSize = 1.0f;
    s.ChildBorderSize = 0.0f;
    s.FrameBorderSize = 0.0f;
    s.PopupBorderSize = 1.0f;
    s.WindowPadding = ImVec2(16, 14);
    s.FramePadding = ImVec2(10, 6);
    s.ItemSpacing = ImVec2(10, 9);
    s.ItemInnerSpacing = ImVec2(8, 6);
    s.GrabMinSize = 10.0f;
    s.ScrollbarSize = 8.0f;
    s.ScaleAllSizes(m_scale);

    ImVec4* c = s.Colors;
    c[ImGuiCol_Text] = kText;
    c[ImGuiCol_TextDisabled] = kDim;
    c[ImGuiCol_WindowBg] = kBg;
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = ImVec4(0.09f, 0.093f, 0.105f, 0.98f);
    c[ImGuiCol_Border] = kLine;
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = ImVec4(1, 1, 1, 0.055f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(1, 1, 1, 0.085f);
    c[ImGuiCol_FrameBgActive] = ImVec4(1, 1, 1, 0.11f);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(1, 1, 1, 0.10f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(1, 1, 1, 0.18f);
    c[ImGuiCol_Button] = ImVec4(1, 1, 1, 0.06f);
    c[ImGuiCol_ButtonHovered] = ImVec4(1, 1, 1, 0.10f);
    c[ImGuiCol_Separator] = kLine;
    c[ImGuiCol_ResizeGrip] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableBorderLight] = kLine;
    c[ImGuiCol_TableBorderStrong] = kLine;
}

void UIManager::ApplyAccent(const std::array<float, 3>& a) {
    if (a == m_accent) return;
    m_accent = a;
    gAccent = ImVec4(a[0], a[1], a[2], 1.0f);
    ImVec4* c = ImGui::GetStyle().Colors;
    c[ImGuiCol_CheckMark] = gAccent;
    c[ImGuiCol_SliderGrab] = gAccent;
    c[ImGuiCol_SliderGrabActive] = Mix(gAccent, ImVec4(1, 1, 1, 1), 0.25f);
    c[ImGuiCol_Button] = WithA(gAccent, 0.16f);
    c[ImGuiCol_ButtonHovered] = WithA(gAccent, 0.28f);
    c[ImGuiCol_ButtonActive] = WithA(gAccent, 0.40f);
    c[ImGuiCol_FrameBgActive] = WithA(gAccent, 0.18f);
    c[ImGuiCol_Border] = ImVec4(1, 1, 1, 0.07f);
    c[ImGuiCol_ResizeGripHovered] = WithA(gAccent, 0.4f);
    c[ImGuiCol_ResizeGripActive] = gAccent;
    c[ImGuiCol_PlotLines] = gAccent;
    c[ImGuiCol_PlotHistogram] = gAccent;
    c[ImGuiCol_Header] = WithA(gAccent, 0.20f);
    c[ImGuiCol_HeaderHovered] = WithA(gAccent, 0.28f);
    c[ImGuiCol_HeaderActive] = WithA(gAccent, 0.36f);
    c[ImGuiCol_ScrollbarGrabActive] = gAccent;
    c[ImGuiCol_SeparatorActive] = gAccent;
    c[ImGuiCol_TextSelectedBg] = WithA(gAccent, 0.30f);
    c[ImGuiCol_NavHighlight] = gAccent;
}

void UIManager::BeginFrame(bool menuOpen) {
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGuiIO& io = ImGui::GetIO();
    io.MouseDrawCursor = menuOpen;  // games usually hide the OS cursor
    if (menuOpen)
        io.ConfigFlags &= ~ImGuiConfigFlags_NoMouse;
    else
        io.ConfigFlags |= ImGuiConfigFlags_NoMouse;
    ImGui::NewFrame();
}

void UIManager::EndFrame() {
    ImGui::Render();
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}

float UIManager::Anim(unsigned id, float target, float speed) {
    ImGuiStorage* st = ImGui::GetStateStorage();
    float v = st->GetFloat(id, target);
    v += (target - v) * std::min(1.0f, ImGui::GetIO().DeltaTime * speed);
    if (std::fabs(target - v) < 0.001f) v = target;
    st->SetFloat(id, v);
    return v;
}

// ---- wallpaper + frosted glass ----

void UIManager::SyncWallpaper(const AppConfig& cfg) {
    if (cfg.wallpaperPath != m_wallLoaded) {
        m_wallLoaded = cfg.wallpaperPath;
        m_wallpaper.Reset();
        m_wallBlur.Reset();
        m_blurSource.clear();
        m_blurBuilt = -1.0f;
        m_wallError.clear();
        if (!m_wallLoaded.empty() && m_device) {
            auto fail = [&](const char* what) {
                m_wallError = what;
                m_wallpaper.Reset();
                m_blurSource.clear();
            };
            ComPtr<IWICImagingFactory> wic;
            ComPtr<IWICBitmapDecoder> decoder;
            ComPtr<IWICBitmapFrameDecode> frame;
            UINT w = 0, h = 0;
            if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) {
                fail("WIC unavailable");
            } else if (FAILED(wic->CreateDecoderFromFilename(Widen(m_wallLoaded).c_str(), nullptr, GENERIC_READ,
                                                             WICDecodeMetadataCacheOnDemand, &decoder)) ||
                       FAILED(decoder->GetFrame(0, &frame)) || FAILED(frame->GetSize(&w, &h)) || !w || !h) {
                fail("Can't read this image");
            } else {
                // full image, capped at 2560 on the long side
                const float k = std::min(1.0f, 2560.0f / static_cast<float>(std::max(w, h)));
                const UINT fw = std::max(1u, static_cast<UINT>(w * k));
                const UINT fh = std::max(1u, static_cast<UINT>(h * k));
                std::vector<uint8_t> pixels;
                if (!ReadScaled(wic.Get(), frame.Get(), fw, fh, pixels)) {
                    fail("Unsupported image format");
                } else {
                    m_wallpaper = MakeTexture(m_device.Get(), pixels.data(), fw, fh);
                    m_wallW = static_cast<float>(fw);
                    m_wallH = static_cast<float>(fh);
                    // small copy for the blur, cheap to re-blur while the slider moves
                    const float bk = 480.0f / static_cast<float>(std::max(fw, fh));
                    m_blurW = std::max(1, static_cast<int>(fw * bk));
                    m_blurH = std::max(1, static_cast<int>(fh * bk));
                    if (!ReadScaled(wic.Get(), frame.Get(), static_cast<UINT>(m_blurW), static_cast<UINT>(m_blurH), m_blurSource))
                        m_blurSource.clear();
                    if (!m_wallpaper) fail("Can't create texture");
                }
            }
        }
    }
    if (!m_blurSource.empty() && std::fabs(cfg.wallpaperBlur - m_blurBuilt) > 0.001f) RebuildBlur(cfg.wallpaperBlur);
}

void UIManager::RebuildBlur(float strength) {
    m_blurBuilt = strength;
    const int r = static_cast<int>(std::round(strength * 22.0f));
    if (r <= 0) {
        m_wallBlur = MakeTexture(m_device.Get(), m_blurSource.data(), static_cast<UINT>(m_blurW), static_cast<UINT>(m_blurH));
        return;
    }
    std::vector<uint8_t> a = m_blurSource;
    std::vector<uint8_t> b(a.size());
    const int w = m_blurW, h = m_blurH;
    for (int pass = 0; pass < 3; ++pass) {  // 3 box passes ~ gaussian
        BoxBlurPass(a.data(), b.data(), h, w, w * 4, 4, r);
        BoxBlurPass(b.data(), a.data(), w, h, 4, w * 4, r);
    }
    m_wallBlur = MakeTexture(m_device.Get(), a.data(), static_cast<UINT>(w), static_cast<UINT>(h));
}

void UIManager::Glass(ImDrawList* dl, const ImVec2& p0, const ImVec2& p1, float rounding, int flags, float tint) {
    if (m_wallBlur) {
        auto uv = [&](const ImVec2& p) {
            const float u = (p.x - m_winX) / m_winW;
            const float v = (p.y - m_winY) / m_winH;
            return ImVec2(m_uv0x + u * (m_uv1x - m_uv0x), m_uv0y + v * (m_uv1y - m_uv0y));
        };
        dl->AddImageRounded((ImTextureID)(uintptr_t)m_wallBlur.Get(), p0, p1, uv(p0), uv(p1), Col(ImVec4(1, 1, 1, 1)),
                            rounding, flags);
        dl->AddRectFilled(p0, p1, Col(kPanel, tint), rounding, flags);
    } else {
        dl->AddRectFilled(p0, p1, Col(kPanel), rounding, flags);
    }
    // thin top highlight gives the glass edge
    dl->AddLine(ImVec2(p0.x + rounding, p0.y + 0.5f), ImVec2(p1.x - rounding, p0.y + 0.5f), Col(ImVec4(1, 1, 1, 0.07f)));
}

void UIManager::DrawWindowBackground(const AppConfig& cfg) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetWindowPos();
    const ImVec2 sz = ImGui::GetWindowSize();
    const ImVec2 p1 = p0 + sz;
    m_winX = p0.x;
    m_winY = p0.y;
    m_winW = std::max(1.0f, sz.x);
    m_winH = std::max(1.0f, sz.y);
    if (!m_wallpaper) return;

    const float winAspect = m_winW / m_winH;
    const float imgAspect = m_wallW / std::max(1.0f, m_wallH);
    m_uv0x = m_uv0y = 0.0f;
    m_uv1x = m_uv1y = 1.0f;
    if (imgAspect > winAspect) {
        const float u = winAspect / imgAspect;
        m_uv0x = (1.0f - u) * 0.5f;
        m_uv1x = m_uv0x + u;
    } else {
        const float v = imgAspect / winAspect;
        m_uv0y = (1.0f - v) * 0.5f;
        m_uv1y = m_uv0y + v;
    }
    const float rounding = ImGui::GetStyle().WindowRounding;
    dl->AddImageRounded((ImTextureID)(uintptr_t)m_wallpaper.Get(), p0, p1, ImVec2(m_uv0x, m_uv0y), ImVec2(m_uv1x, m_uv1y),
                        Col(ImVec4(1, 1, 1, 1)), rounding);
    dl->AddRectFilled(p0, p1, Col(ImVec4(0.03f, 0.03f, 0.035f, cfg.wallpaperDim)), rounding);
}

bool UIManager::BrowseForImage(std::string& outPath) {
    ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return false;
    const COMDLG_FILTERSPEC filters[] = {
        {L"PNG / JPEG", L"*.png;*.jpg;*.jpeg"},
        {L"Other images", L"*.bmp;*.gif;*.webp;*.tif;*.tiff"},
    };
    dlg->SetFileTypes(static_cast<UINT>(std::size(filters)), filters);
    dlg->SetTitle(L"Background image");
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
    if (FAILED(dlg->Show(m_hwnd))) return false;
    ComPtr<IShellItem> item;
    if (FAILED(dlg->GetResult(&item))) return false;
    wchar_t* raw = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw))) return false;
    std::unique_ptr<wchar_t, CoTaskMemDeleter> path(raw);
    outPath = Narrow(path.get());
    return !outPath.empty();
}

// ---- icons ----

void UIManager::DrawIcon(ImDrawList* dl, int icon, const ImVec2& c, float size, unsigned col) {
    const float r = size * 0.5f;
    const float t = std::max(1.4f, size * 0.09f);
    switch (icon) {
        case 0: {  // color: three overlapping rings
            const float rr = r * 0.42f;
            dl->AddCircle(c + ImVec2(0, -r * 0.26f), rr, col, 24, t);
            dl->AddCircle(c + ImVec2(-r * 0.30f, r * 0.24f), rr, col, 24, t);
            dl->AddCircle(c + ImVec2(r * 0.30f, r * 0.24f), rr, col, 24, t);
            break;
        }
        case 1: {  // shaders: sparkle
            auto star = [&](ImVec2 o, float outer, float inner, bool fill) {
                for (int i = 0; i < 8; ++i) {
                    const float a = i * kPi / 4.0f - kPi / 2.0f;
                    const float rad = (i % 2 == 0) ? outer : inner;
                    dl->PathLineTo(o + ImVec2(std::cos(a) * rad, std::sin(a) * rad));
                }
                if (fill)
                    dl->PathFillConvex(col);
                else
                    dl->PathStroke(col, ImDrawFlags_Closed, t);
            };
            star(c + ImVec2(-r * 0.12f, r * 0.10f), r * 0.78f, r * 0.20f, false);
            star(c + ImVec2(r * 0.58f, -r * 0.58f), r * 0.30f, r * 0.09f, true);
            break;
        }
        case 2: {  // crosshair
            dl->AddCircle(c, r * 0.58f, col, 28, t);
            dl->AddLine(c + ImVec2(0, -r), c + ImVec2(0, -r * 0.30f), col, t);
            dl->AddLine(c + ImVec2(0, r * 0.30f), c + ImVec2(0, r), col, t);
            dl->AddLine(c + ImVec2(-r, 0), c + ImVec2(-r * 0.30f, 0), col, t);
            dl->AddLine(c + ImVec2(r * 0.30f, 0), c + ImVec2(r, 0), col, t);
            dl->AddCircleFilled(c, t * 0.8f, col);
            break;
        }
        case 3: {  // zoom: magnifier
            const ImVec2 lens = c + ImVec2(-r * 0.16f, -r * 0.16f);
            dl->AddCircle(lens, r * 0.55f, col, 28, t);
            dl->AddLine(c + ImVec2(r * 0.25f, r * 0.25f), c + ImVec2(r * 0.85f, r * 0.85f), col, t * 1.6f);
            dl->AddLine(lens + ImVec2(-r * 0.24f, 0), lens + ImVec2(r * 0.24f, 0), col, t);
            dl->AddLine(lens + ImVec2(0, -r * 0.24f), lens + ImVec2(0, r * 0.24f), col, t);
            break;
        }
        case 4: {  // profiles: stacked layers
            auto diamond = [&](float dy) {
                dl->PathLineTo(c + ImVec2(0, -r * 0.45f + dy));
                dl->PathLineTo(c + ImVec2(r * 0.85f, dy));
                dl->PathLineTo(c + ImVec2(0, r * 0.45f + dy));
                dl->PathLineTo(c + ImVec2(-r * 0.85f, dy));
                dl->PathStroke(col, ImDrawFlags_Closed, t);
            };
            diamond(r * 0.32f);
            diamond(-r * 0.22f);
            break;
        }
        case 5: {  // settings: gear
            for (int i = 0; i < 8; ++i) {
                const float a = i * kPi / 4.0f;
                const ImVec2 d(std::cos(a), std::sin(a));
                dl->AddLine(c + d * (r * 0.52f), c + d * (r * 0.92f), col, t * 1.9f);
            }
            dl->AddCircle(c, r * 0.58f, col, 28, t);
            dl->AddCircle(c, r * 0.22f, col, 16, t);
            break;
        }
        default: break;
    }
}

// ---- crosshair / toast ----

void UIManager::DrawCrosshairShape(ImDrawList* dl, float cx, float cy, const CrosshairSettings& s, float scale) {
    const ImU32 col = ImGui::ColorConvertFloat4ToU32(ImVec4(s.color[0], s.color[1], s.color[2], s.color[3]));
    const ImU32 outline = ImGui::ColorConvertFloat4ToU32(ImVec4(0, 0, 0, s.color[3] * 0.85f));
    const float t = std::max(1.0f, s.thickness * scale);
    const float len = s.size * scale;
    const float gap = s.gap * scale;
    const float o = s.outline ? s.outlineThickness * scale : 0.0f;

    // odd thickness -> center on a half pixel so lines stay crisp
    const float half = (static_cast<int>(std::round(t)) % 2) ? 0.5f : 0.0f;
    const float x = std::floor(cx) + half;
    const float y = std::floor(cy) + half;
    const float ht = t * 0.5f;

    auto rect = [&](float x0, float y0, float x1, float y1) {
        if (o > 0) dl->AddRectFilled(ImVec2(x0 - o, y0 - o), ImVec2(x1 + o, y1 + o), outline);
        dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), col);
    };
    auto dot = [&] { rect(x - ht, y - ht, x + ht, y + ht); };
    auto cross = [&](bool top) {
        if (top) rect(x - ht, y - gap - len, x + ht, y - gap);
        rect(x - ht, y + gap, x + ht, y + gap + len);
        rect(x - gap - len, y - ht, x - gap, y + ht);
        rect(x + gap, y - ht, x + gap + len, y + ht);
    };
    auto circle = [&] {
        const float r = std::max(len, 2.0f);
        if (o > 0) dl->AddCircle(ImVec2(x, y), r, outline, 0, t + o * 2);
        dl->AddCircle(ImVec2(x, y), r, col, 0, t);
    };

    switch (static_cast<CrosshairStyle>(s.style)) {
        case CrosshairStyle::Dot: dot(); break;
        case CrosshairStyle::Cross: cross(true); break;
        case CrosshairStyle::Circle: circle(); break;
        case CrosshairStyle::CrossDot: cross(true); dot(); break;
        case CrosshairStyle::TShape: cross(false); break;
        case CrosshairStyle::CircleDot: circle(); dot(); break;
        default: break;
    }
}

void UIManager::DrawCrosshair(const CrosshairSettings& s) {
    // integer screen center, so the crosshair lands on the same pixels in any window layout
    const float cx = m_centerX + std::round(s.offsetX) - m_originX;
    const float cy = m_centerY + std::round(s.offsetY) - m_originY;
    DrawCrosshairShape(ImGui::GetBackgroundDrawList(), cx, cy, s, 1.0f);
}

void UIManager::DrawToast(const std::string& text, float alpha) {
    if (text.empty() || alpha <= 0.0f) return;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImGuiIO& io = ImGui::GetIO();
    ImFont* font = m_fontBold;
    const float fs = font->FontSize;
    const ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, text.c_str());
    const ImVec2 pad(16 * m_scale, 9 * m_scale);
    const float slide = (1.0f - EaseOut(alpha)) * -12.0f * m_scale;
    (void)io;
    const ImVec2 p0(std::floor((m_screenW - ts.x) * 0.5f - pad.x) - m_originX, 36 * m_scale + slide - m_originY);
    const ImVec2 p1(p0.x + ts.x + pad.x * 2, p0.y + ts.y + pad.y * 2);
    const auto u = [&](ImVec4 c) { return ImGui::ColorConvertFloat4ToU32(WithA(c, alpha)); };
    dl->AddRectFilled(p0, p1, u(ImVec4(0.07f, 0.075f, 0.085f, 0.94f)), 8 * m_scale);
    dl->AddRectFilled(ImVec2(p0.x, p0.y + 7 * m_scale), ImVec2(p0.x + 3 * m_scale, p1.y - 7 * m_scale), u(gAccent), 2);
    dl->AddText(font, fs, p0 + pad, u(kText), text.c_str());
}

// ---- widgets ----

bool UIManager::Toggle(const char* label, bool* v) {
    ImGui::PushID(label);
    const float fh = ImGui::GetFrameHeight();
    const float h = fh * 0.72f;
    const float w = h * 1.8f;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const char* hash = std::strstr(label, "##");
    const char* end = hash ? hash : label + std::strlen(label);
    const float labelW = end != label ? ImGui::CalcTextSize(label, end).x + 10 * m_scale : 0.0f;
    ImGui::InvisibleButton("##t", ImVec2(w + labelW, fh));
    const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    if (clicked) {
        *v = !*v;
        if (m_res) m_res->changed = true;
    }
    const ImGuiID id = ImGui::GetItemID();
    const float on = Anim(id, *v ? 1.0f : 0.0f, 16.0f);
    const float hov = Anim(id + 1, ImGui::IsItemHovered() ? 1.0f : 0.0f, 12.0f);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 a(p.x, p.y + (fh - h) * 0.5f);
    const ImVec2 b(a.x + w, a.y + h);
    const ImVec4 off(1, 1, 1, 0.10f + 0.05f * hov);
    dl->AddRectFilled(a, b, Col(Mix(off, gAccent, on)), h * 0.5f);
    const float r = h * 0.5f - 3.0f * m_scale;
    const ImVec2 knob(a.x + h * 0.5f + (w - h) * on, a.y + h * 0.5f);
    dl->AddCircleFilled(knob, r + hov * m_scale, Col(ImVec4(0.97f, 0.97f, 0.98f, 1)));
    if (end != label)
        dl->AddText(ImVec2(b.x + 10 * m_scale, p.y + (fh - ImGui::GetFontSize()) * 0.5f), Col(kText), label, end);
    ImGui::PopID();
    return clicked;
}

void UIManager::CostNote(const char* text) {
    ImGui::PushFont(m_fontSmall);
    ImGui::TextColored(kWarn, "%s", text);
    ImGui::PopFont();
}

void UIManager::LabelRow(const char* label) {
    const char* hash = std::strstr(label, "##");
    const char* end = hash ? hash : label + std::strlen(label);
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::TextUnformatted(label, end);
    ImGui::PopStyleColor();
    ImGui::SameLine(130.0f * m_scale);
    ImGui::SetNextItemWidth(-1);
}

bool UIManager::Slider(const char* label, float* v, float min, float max, float def, const char* fmt) {
    ImGui::PushID(label);
    LabelRow(label);
    bool changed = ImGui::SliderFloat("##v", v, min, max, fmt, ImGuiSliderFlags_AlwaysClamp);
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        *v = def;
        changed = true;
    }
    if (changed && m_res) m_res->changed = true;
    ImGui::PopID();
    return changed;
}

bool UIManager::AccentButton(const char* label, const ImVec2& size, bool danger) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(label, size);
    const float hov = Anim(ImGui::GetItemID(), ImGui::IsItemHovered() ? 1.0f : 0.0f, 12.0f);
    const ImVec4 base = danger ? kDanger : gAccent;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, p + size, Col(base, 0.14f + 0.16f * hov + (ImGui::IsItemActive() ? 0.15f : 0.0f)), 6 * m_scale);
    dl->AddRect(p, p + size, Col(base, 0.35f + 0.4f * hov), 6 * m_scale);
    const char* hash = std::strstr(label, "##");
    const char* end = hash ? hash : label + std::strlen(label);
    const ImVec2 ts = ImGui::CalcTextSize(label, end);
    dl->AddText(p + (size - ts) * 0.5f, Col(kText), label, end);
    return pressed;
}

bool UIManager::NavItem(int index, const char* label, bool selected) {
    const ImVec2 size(ImGui::GetContentRegionAvail().x, 38.0f * m_scale);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(label, size);
    const float hov = Anim(ImGui::GetItemID(), ImGui::IsItemHovered() ? 1.0f : 0.0f, 12.0f);
    const float sel = Anim(ImGui::GetItemID() + 1, selected ? 1.0f : 0.0f, 14.0f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (hov > 0.0f && !selected) dl->AddRectFilled(p, p + size, Col(ImVec4(1, 1, 1, 0.04f * hov)), 7 * m_scale);

    const ImVec4 col = Mix(Mix(kDim, kText, hov * 0.6f), kText, sel);
    const ImVec4 icol = Mix(Mix(kDim, kText, hov * 0.6f), gAccent, sel);
    const float iconSize = 17.0f * m_scale;
    const float shift = sel * 3.0f * m_scale;
    DrawIcon(dl, index, ImVec2(p.x + 22 * m_scale + shift, p.y + size.y * 0.5f), iconSize, Col(icol));
    dl->AddText(ImVec2(p.x + 44 * m_scale + shift, p.y + (size.y - ImGui::GetFontSize()) * 0.5f), Col(col), label);
    return pressed;
}

void UIManager::BeginCard(const char* id, const char* title) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16 * m_scale, 14 * m_scale));
    ImGui::BeginChild(id, ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetWindowPos();
    const ImVec2 p1 = p0 + ImGui::GetWindowSize();
    const float rounding = ImGui::GetStyle().ChildRounding;
    Glass(dl, p0, p1, rounding, ImDrawFlags_None, m_panelTint);
    dl->AddRect(p0, p1, Col(kLine), rounding);
    if (title && *title) {
        ImGui::PushFont(m_fontBold);
        ImGui::TextUnformatted(title);
        ImGui::PopFont();
        ImGui::Spacing();
    }
}

void UIManager::EndCard() {
    ImGui::EndChild();
    ImGui::Dummy(ImVec2(0, 2 * m_scale));
}

bool UIManager::PresetTile(int index, bool active, float width) {
    const ColorPreset& preset = GetColorPresets()[static_cast<size_t>(index)];
    ImGui::PushID(index);
    const ImVec2 size(width, 64.0f * m_scale);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton("##preset", size);
    const float hov = Anim(ImGui::GetItemID(), ImGui::IsItemHovered() ? 1.0f : 0.0f, 12.0f);
    const float act = Anim(ImGui::GetItemID() + 1, active ? 1.0f : 0.0f, 12.0f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float rounding = 7.0f * m_scale;
    const float lift = hov * 2.0f * m_scale;
    const ImVec2 a(p.x, p.y - lift), b(p.x + size.x, p.y + size.y - lift);

    dl->AddRectFilled(a, b, Col(ImVec4(1, 1, 1, 0.035f + 0.03f * hov)), rounding);
    dl->AddRect(a, b, Col(Mix(kLine, gAccent, act)), rounding, 0, 1.0f + act * 0.5f * m_scale);

    const ImVec2 pad(10 * m_scale, 9 * m_scale);
    dl->AddText(m_fontBold, m_fontBold->FontSize, a + pad, Col(Mix(kText, gAccent, act)), preset.name);

    const Mat5 m = BuildColorMatrix(preset.params);
    constexpr int kSwatches = 14;
    const float y0 = b.y - pad.y - 12 * m_scale;
    const float y1 = b.y - pad.y;
    const float stripW = size.x - pad.x * 2;
    for (int k = 0; k < kSwatches; ++k) {
        float r, g, bl;
        if (k < kSwatches - 3)
            ImGui::ColorConvertHSVtoRGB(static_cast<float>(k) / (kSwatches - 3), 0.6f, 0.8f, r, g, bl);
        else
            r = g = bl = 0.12f + 0.3f * static_cast<float>(k - (kSwatches - 3));
        const auto c = Preview(m, preset.params, r, g, bl);
        const float x0 = a.x + pad.x + stripW * k / kSwatches;
        const float x1 = a.x + pad.x + stripW * (k + 1) / kSwatches;
        const ImDrawFlags corners = k == 0 ? ImDrawFlags_RoundCornersLeft
                                           : (k == kSwatches - 1 ? ImDrawFlags_RoundCornersRight : ImDrawFlags_RoundCornersNone);
        dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), Col(ImVec4(c[0], c[1], c[2], 1)), 3.0f * m_scale, corners);
    }
    ImGui::PopID();
    return pressed;
}

void UIManager::ToneCurve(const ColorParams& params, float w, float h) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 size(w, h);
    ImGui::Dummy(size);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, p + size, Col(ImVec4(0, 0, 0, 0.30f)), 6.0f * m_scale);
    for (int i = 1; i < 4; ++i) {
        const float fx = p.x + size.x * i / 4.0f;
        const float fy = p.y + size.y * i / 4.0f;
        dl->AddLine(ImVec2(fx, p.y), ImVec2(fx, p.y + size.y), Col(ImVec4(1, 1, 1, 0.05f)));
        dl->AddLine(ImVec2(p.x, fy), ImVec2(p.x + size.x, fy), Col(ImVec4(1, 1, 1, 0.05f)));
    }
    dl->AddLine(ImVec2(p.x, p.y + size.y), ImVec2(p.x + size.x, p.y), Col(ImVec4(1, 1, 1, 0.12f)));

    const Mat5 m = BuildColorMatrix(params);
    constexpr int kPoints = 64;
    const ImVec4 cols[3] = {{1.0f, 0.38f, 0.38f, 0.9f}, {0.40f, 0.90f, 0.55f, 0.9f}, {0.40f, 0.60f, 1.0f, 0.9f}};
    for (size_t ch = 0; ch < 3; ++ch) {
        ImVec2 pts[kPoints];
        for (int i = 0; i < kPoints; ++i) {
            const float x = static_cast<float>(i) / (kPoints - 1);
            const auto c = Preview(m, params, x, x, x);
            pts[i] = ImVec2(p.x + x * size.x, p.y + (1.0f - c[ch]) * size.y);
        }
        dl->AddPolyline(pts, kPoints, Col(cols[ch]), ImDrawFlags_None, 1.8f * m_scale);
    }
}

// ---- menu ----

UiResult UIManager::DrawMenu(AppConfig& cfg, const UiStatus& st, bool open) {
    UiResult res;
    m_res = &res;

    const float dt = ImGui::GetIO().DeltaTime;
    m_menuT = std::clamp(m_menuT + (open ? dt * 5.0f : -dt * 7.0f), 0.0f, 1.0f);
    m_tabT = std::min(1.0f, m_tabT + dt * 5.5f);
    if (m_menuT <= 0.0f) {
        m_res = nullptr;
        return res;
    }
    const float e = EaseOut(m_menuT);

    ApplyAccent(cfg.accent);
    SyncWallpaper(cfg);
    m_panelTint = cfg.wallpaperBlurPanels;

    // dim the game behind the menu
    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), disp, IM_COL32(0, 0, 0, static_cast<int>(90 * e)));

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Once, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(1060 * m_scale, 720 * m_scale), ImGuiCond_Once);
    ImGui::SetNextWindowSizeConstraints(ImVec2(860 * m_scale, 560 * m_scale), ImVec2(FLT_MAX, FLT_MAX));

    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                                       ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, e);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    if (m_wallpaper) ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
    const bool visible = ImGui::Begin("PopaVisual##main", nullptr, flags);
    if (m_wallpaper) ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    if (visible) {
        DrawWindowBackground(cfg);
        DrawHeader(cfg);

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10 * m_scale, 12 * m_scale));
        ImGui::BeginChild("##sidebar", ImVec2(196 * m_scale, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
        DrawSidebar();
        ImGui::EndChild();
        ImGui::SameLine(0, 0);

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16 * m_scale, 14 * m_scale));
        ImGui::BeginChild("##content", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
        ImGui::PopStyleVar(2);

        // tab transition: fade + slide up
        const float te = EaseOut(m_tabT);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, e * te);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (1.0f - te) * 16.0f * m_scale);
        switch (m_tab) {
            case Tab::Color: ColorTab(cfg, st); break;
            case Tab::Effects: EffectsTab(cfg, st); break;
            case Tab::Crosshair: CrosshairTab(cfg); break;
            case Tab::Zoom: ZoomTab(cfg); break;
            case Tab::Profiles: ProfilesTab(cfg); break;
            case Tab::Settings: SettingsTab(cfg, st); break;
            default: break;
        }
        ImGui::PopStyleVar();
        ImGui::EndChild();
    }
    ImGui::End();
    ImGui::PopStyleVar();

    if (open && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) res.closeMenu = true;
    m_res = nullptr;
    return res;
}

void UIManager::DrawHeader(AppConfig& cfg) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = 58.0f * m_scale;
    const float w = ImGui::GetWindowWidth();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float rounding = ImGui::GetStyle().WindowRounding;
    Glass(dl, p, ImVec2(p.x + w, p.y + h), rounding, ImDrawFlags_RoundCornersTop, std::min(1.0f, m_panelTint + 0.15f));
    dl->AddLine(ImVec2(p.x, p.y + h - 0.5f), ImVec2(p.x + w, p.y + h - 0.5f), Col(kLine));

    const float midY = p.y + h * 0.5f;
    DrawIcon(dl, 2, ImVec2(p.x + 30 * m_scale, midY), 20 * m_scale, Col(gAccent));
    const float tfs = m_fontTitle->FontSize;
    dl->AddText(m_fontTitle, tfs, ImVec2(p.x + 50 * m_scale, midY - tfs * 0.55f), Col(kText), "PopaVisual");

    // profile selector (Ctrl+F1..F4), with a sliding highlight
    const float chipW = 104 * m_scale;
    const float chipH = 30 * m_scale;
    const float groupW = chipW * kProfileCount;
    const ImVec2 g0(p.x + (w - groupW) * 0.5f, midY - chipH * 0.5f);
    dl->AddRectFilled(g0, ImVec2(g0.x + groupW, g0.y + chipH), Col(ImVec4(0, 0, 0, 0.28f)), chipH * 0.5f);
    const float targetX = g0.x + chipW * cfg.activeProfile;
    if (m_profileX < 0.0f) m_profileX = targetX;
    m_profileX += (targetX - m_profileX) * std::min(1.0f, ImGui::GetIO().DeltaTime * 14.0f);
    dl->AddRectFilled(ImVec2(m_profileX + 2, g0.y + 2), ImVec2(m_profileX + chipW - 2, g0.y + chipH - 2), Col(gAccent, 0.9f),
                      chipH * 0.5f);
    for (int i = 0; i < kProfileCount; ++i) {
        ImGui::SetCursorScreenPos(ImVec2(g0.x + chipW * i, g0.y));
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##prof", ImVec2(chipW, chipH)) && cfg.activeProfile != i) {
            cfg.activeProfile = i;
            m_res->changed = true;
        }
        const float hov = Anim(ImGui::GetItemID(), ImGui::IsItemHovered() ? 1.0f : 0.0f);
        ImGui::PopID();
        std::string name = cfg.profiles[static_cast<size_t>(i)].name;
        if (name.size() > 11) name = name.substr(0, 10) + ".";
        const ImVec2 ts = ImGui::CalcTextSize(name.c_str());
        const bool active = cfg.activeProfile == i;
        dl->AddText(ImVec2(g0.x + chipW * i + (chipW - ts.x) * 0.5f, g0.y + (chipH - ts.y) * 0.5f),
                    Col(active ? ImVec4(1, 1, 1, 1) : Mix(kDim, kText, hov)), name.c_str());
    }

    // master switch
    const float tw = ImGui::GetFrameHeight() * 0.72f * 1.8f + ImGui::CalcTextSize("Enabled").x + 10 * m_scale;
    ImGui::SetCursorScreenPos(ImVec2(p.x + w - tw - 22 * m_scale, midY - ImGui::GetFrameHeight() * 0.5f));
    Toggle("Enabled", &cfg.masterEnabled);

    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h));
}

void UIManager::DrawSidebar() {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 wp = ImGui::GetWindowPos();
    const ImVec2 ws = ImGui::GetWindowSize();
    Glass(dl, wp, wp + ws, ImGui::GetStyle().WindowRounding, ImDrawFlags_RoundCornersBottomLeft, m_panelTint);
    dl->AddLine(ImVec2(wp.x + ws.x - 0.5f, wp.y), ImVec2(wp.x + ws.x - 0.5f, wp.y + ws.y), Col(kLine));

    // sliding selection indicator
    const float itemH = 38.0f * m_scale;
    const float stride = itemH + ImGui::GetStyle().ItemSpacing.y;
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float targetY = start.y + stride * static_cast<int>(m_tab);
    if (m_navY < 0.0f) m_navY = targetY;
    m_navY += (targetY - m_navY) * std::min(1.0f, ImGui::GetIO().DeltaTime * 16.0f);
    const float iw = ImGui::GetContentRegionAvail().x;
    dl->AddRectFilled(ImVec2(start.x, m_navY), ImVec2(start.x + iw, m_navY + itemH), Col(gAccent, 0.14f), 7 * m_scale);
    dl->AddRectFilled(ImVec2(start.x, m_navY + 9 * m_scale), ImVec2(start.x + 3 * m_scale, m_navY + itemH - 9 * m_scale),
                      Col(gAccent), 2.0f);

    for (int i = 0; i < static_cast<int>(Tab::Count); ++i) {
        if (NavItem(i, kTabNames[i], static_cast<int>(m_tab) == i) && static_cast<int>(m_tab) != i) {
            m_tab = static_cast<Tab>(i);
            m_tabT = 0.0f;
        }
    }
}

void UIManager::ColorTab(AppConfig& cfg, const UiStatus& st) {
    ColorParams& c = cfg.Active().color;
    const ColorParams d{};

    BeginCard("##presets", "Presets");
    {
        const auto presets = GetColorPresets();
        const int perRow = static_cast<int>(presets.size());
        const float sp = ImGui::GetStyle().ItemSpacing.x;
        const float tileW = (ImGui::GetContentRegionAvail().x - sp * (perRow - 1)) / perRow;
        ImGui::Dummy(ImVec2(0, 2 * m_scale));
        for (int i = 0; i < perRow; ++i) {
            if (i) ImGui::SameLine();
            if (PresetTile(i, presets[static_cast<size_t>(i)].params == c, tileW)) {
                c = presets[static_cast<size_t>(i)].params;
                m_res->changed = true;
            }
        }
    }
    EndCard();

    BeginCard("##color", "Color");
    Slider("Saturation", &c.saturation, 0.0f, 3.0f, d.saturation, "%.2fx");
    Slider("Vibrance", &c.vibrance, -1.0f, 1.0f, d.vibrance, "%+.2f");
    if (c.vibrance != 0.0f && cfg.vibranceMode == 1) CostNote("Exact vibrance captures the screen and costs FPS (Settings > Performance)");
    EndCard();

    BeginCard("##tone", "Tone");
    if (ImGui::BeginTable("##toneTbl", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("a", ImGuiTableColumnFlags_WidthStretch, 1.8f);
        ImGui::TableSetupColumn("b", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableNextColumn();
        Slider("Exposure", &c.exposure, -2.0f, 2.0f, d.exposure, "%+.2f EV");
        Slider("Contrast", &c.contrast, 0.5f, 2.0f, d.contrast, "%.2fx");
        Slider("Brightness", &c.brightness, -0.3f, 0.3f, d.brightness, "%+.3f");
        Slider("Gamma", &c.gamma, 0.5f, 2.5f, d.gamma);
        Slider("Shadows", &c.shadowBoost, 0.0f, 1.0f, d.shadowBoost);
        if (!st.gammaOk) ImGui::TextColored(kWarn, "Gamma rejected by Windows");
        ImGui::TableNextColumn();
        const float cw = ImGui::GetContentRegionAvail().x;
        ToneCurve(c, cw, std::min(cw, 168.0f * m_scale));
        ImGui::EndTable();
    }
    EndCard();

    BeginCard("##balance", "RGB");
    Slider("Red", &c.gainR, 0.0f, 2.0f, 1.0f, "%.2fx");
    Slider("Green", &c.gainG, 0.0f, 2.0f, 1.0f, "%.2fx");
    Slider("Blue", &c.gainB, 0.0f, 2.0f, 1.0f, "%.2fx");
    Toggle("Invert", &c.invert);
    EndCard();

    if (AccentButton("Reset color", ImVec2(ImGui::GetContentRegionAvail().x, 32 * m_scale))) {
        c = ColorParams{};
        m_res->changed = true;
    }
}

void UIManager::EffectsTab(AppConfig& cfg, const UiStatus& st) {
    Profile& p = cfg.Active();

    BeginCard("##sharpen", "LumaSharpen");
    Toggle("Enabled##sharp", &p.sharpen.enabled);
    if (p.sharpen.enabled) CostNote("Captures the screen, costs FPS");
    ImGui::BeginDisabled(!p.sharpen.enabled);
    Slider("Strength", &p.sharpen.strength, 0.0f, 1.5f, 0.65f);
    Slider("Edge limit", &p.sharpen.clamp, 0.01f, 0.25f, 0.06f, "%.3f");
    ImGui::EndDisabled();
    EndCard();

    BeginCard("##vignette", "Vignette");
    Toggle("Enabled##vig", &p.vignette.enabled);
    if (p.vignette.enabled) CostNote("Full-screen overlay, costs some FPS");
    ImGui::BeginDisabled(!p.vignette.enabled);
    Slider("Strength##v", &p.vignette.strength, 0.0f, 1.0f, 0.55f);
    Slider("Radius", &p.vignette.radius, 0.0f, 1.0f, 0.55f);
    Slider("Softness", &p.vignette.softness, 0.02f, 1.0f, 0.45f);
    ImGui::EndDisabled();
    EndCard();

    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::Text("Capture: %s", st.captureActive ? "running" : "off");
    if (st.frameMs > 0.0f) {
        ImGui::SameLine(0, 20 * m_scale);
        ImGui::Text("%.2f ms", st.frameMs);
    }
    ImGui::PopStyleColor();
}

void UIManager::CrosshairTab(AppConfig& cfg) {
    CrosshairSettings& x = cfg.Active().crosshair;
    const CrosshairSettings d{};

    if (!ImGui::BeginTable("##xhTbl", 2, ImGuiTableFlags_SizingStretchProp)) return;
    ImGui::TableSetupColumn("a", ImGuiTableColumnFlags_WidthStretch, 1.35f);
    ImGui::TableSetupColumn("b", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableNextColumn();

    BeginCard("##xh", "Crosshair");
    Toggle("Show##xh", &x.enabled);
    ImGui::Spacing();
    LabelRow("Style");
    if (ImGui::BeginCombo("##style", kStyles[x.style])) {
        for (int i = 0; i < static_cast<int>(CrosshairStyle::Count); ++i) {
            if (ImGui::Selectable(kStyles[i], x.style == i)) {
                x.style = i;
                m_res->changed = true;
            }
        }
        ImGui::EndCombo();
    }
    Slider("Size", &x.size, 1.0f, 60.0f, d.size, "%.0f px");
    Slider("Thickness", &x.thickness, 1.0f, 10.0f, d.thickness, "%.0f px");
    Slider("Gap", &x.gap, 0.0f, 30.0f, d.gap, "%.0f px");
    Toggle("Outline", &x.outline);
    ImGui::BeginDisabled(!x.outline);
    Slider("Outline width", &x.outlineThickness, 0.5f, 4.0f, d.outlineThickness, "%.1f px");
    ImGui::EndDisabled();
    Slider("Offset X", &x.offsetX, -200.0f, 200.0f, 0.0f, "%.0f px");
    Slider("Offset Y", &x.offsetY, -200.0f, 200.0f, 0.0f, "%.0f px");
    LabelRow("Center on");
    {
        const char* anchors[] = {"Screen", "Game window"};
        if (ImGui::Combo("##anchor", &x.anchor, anchors, 2)) m_res->changed = true;
    }
    EndCard();

    BeginCard("##xhcolor", "Color");
    {
        // full custom picker: SV square + hue bar + alpha, with RGB and HEX inputs
        ImGui::SetNextItemWidth(std::min(ImGui::GetContentRegionAvail().x, 300.0f * m_scale));
        if (ImGui::ColorPicker4("##xhpick", x.color.data(),
                                ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_PickerHueBar |
                                    ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_DisplayRGB |
                                    ImGuiColorEditFlags_DisplayHex))
            m_res->changed = true;
        ImGui::Spacing();
        constexpr std::array<float, 4> quick[] = {{0.10f, 1.0f, 0.55f, 1}, {1, 0.15f, 0.25f, 1}, {0.2f, 0.85f, 1, 1},
                                                  {1, 0.9f, 0.1f, 1},     {1, 0.2f, 1, 1},       {1, 1, 1, 1}};
        for (size_t i = 0; i < std::size(quick); ++i) {
            if (i) ImGui::SameLine();
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::ColorButton("##q", ImVec4(quick[i][0], quick[i][1], quick[i][2], 1), ImGuiColorEditFlags_NoTooltip,
                                   ImVec2(26 * m_scale, 26 * m_scale))) {
                x.color = quick[i];
                m_res->changed = true;
            }
            ImGui::PopID();
        }
    }
    EndCard();

    ImGui::TableNextColumn();
    BeginCard("##xhprev", "Preview");
    {
        const float w = ImGui::GetContentRegionAvail().x;
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const ImVec2 size(w, w);
        ImGui::Dummy(size);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float rr = 6 * m_scale;
        dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y * 0.55f), Col(ImVec4(0.47f, 0.58f, 0.72f, 1)), rr,
                          ImDrawFlags_RoundCornersTop);
        dl->AddRectFilled(ImVec2(p.x, p.y + size.y * 0.55f), p + size, Col(ImVec4(0.22f, 0.21f, 0.17f, 1)), rr,
                          ImDrawFlags_RoundCornersBottom);
        dl->PushClipRect(p, p + size, true);
        DrawCrosshairShape(dl, p.x + size.x * 0.5f, p.y + size.y * 0.5f, x, 2.0f);
        dl->PopClipRect();
    }
    EndCard();
    ImGui::EndTable();
}

void UIManager::ZoomTab(AppConfig& cfg) {
    ZoomSettings& z = cfg.Active().zoom;
    const ZoomSettings d{};

    BeginCard("##zoom", "Sniper Zoom");
    Toggle("Enabled##zoom", &z.enabled);
    ImGui::BeginDisabled(!z.enabled);
    LabelRow("Hold key");
    if (ImGui::BeginCombo("##holdkey", HoldKeyName(z.holdKey))) {
        for (const auto& k : kHoldKeys) {
            if (ImGui::Selectable(k.name, z.holdKey == k.vk)) {
                z.holdKey = k.vk;
                m_res->changed = true;
            }
        }
        ImGui::EndCombo();
    }
    Slider("Magnification", &z.factor, 1.5f, 8.0f, d.factor, "%.1fx");
    Slider("Lens radius", &z.radius, 60.0f, 600.0f, d.radius, "%.0f px");
    LabelRow("Border");
    if (ImGui::ColorEdit4("##border", z.border.data(), ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_DisplayHex))
        m_res->changed = true;
    ImGui::EndDisabled();
    EndCard();
}

void UIManager::ProfilesTab(AppConfig& cfg) {
    BeginCard("##profiles", "Profiles");
    for (int i = 0; i < kProfileCount; ++i) {
        Profile& pr = cfg.profiles[static_cast<size_t>(i)];
        const bool active = cfg.activeProfile == i;
        ImGui::PushID(i);
        ImGui::AlignTextToFramePadding();
        ImGui::PushFont(m_fontBold);
        ImGui::TextColored(active ? gAccent : kDim, "Ctrl+F%d", i + 1);
        ImGui::PopFont();
        ImGui::SameLine(84 * m_scale);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 330 * m_scale);
        if (ImGui::InputText("##name", &pr.name)) {
            if (pr.name.size() > 64) pr.name.resize(64);
            m_res->changed = true;
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(active);
        if (AccentButton(active ? "Active" : "Use", ImVec2(90 * m_scale, ImGui::GetFrameHeight()))) {
            cfg.activeProfile = i;
            m_res->changed = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Copy active", ImVec2(120 * m_scale, 0))) {
            const std::string name = pr.name;
            pr = cfg.Active();
            pr.name = name;
            m_res->changed = true;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Reset", ImVec2(-1, 0))) {
            pr = AppConfig::DefaultProfile(i);
            m_res->changed = true;
        }
        ImGui::PopID();
    }
    EndCard();
}

void UIManager::SettingsTab(AppConfig& cfg, const UiStatus& st) {
    BeginCard("##bg", "Background");
    {
        const float thumbW = 192 * m_scale;
        const float thumbH = 108 * m_scale;
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(thumbW, thumbH));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (m_wallpaper) {
            dl->AddImageRounded((ImTextureID)(uintptr_t)m_wallpaper.Get(), p, ImVec2(p.x + thumbW, p.y + thumbH),
                                ImVec2(0, 0), ImVec2(1, 1), Col(ImVec4(1, 1, 1, 1)), 6 * m_scale);
        } else {
            dl->AddRectFilled(p, ImVec2(p.x + thumbW, p.y + thumbH), Col(ImVec4(1, 1, 1, 0.04f)), 6 * m_scale);
            DrawIcon(dl, 0, ImVec2(p.x + thumbW * 0.5f, p.y + thumbH * 0.5f), 26 * m_scale, Col(kDim));
        }
        dl->AddRect(p, ImVec2(p.x + thumbW, p.y + thumbH), Col(kLine), 6 * m_scale);

        ImGui::SameLine(0, 16 * m_scale);
        ImGui::BeginGroup();
        if (AccentButton("Choose PNG / JPG", ImVec2(170 * m_scale, 32 * m_scale))) {
            std::string path;
            if (BrowseForImage(path)) {
                cfg.wallpaperPath = path;
                m_res->changed = true;
            }
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(cfg.wallpaperPath.empty());
        if (ImGui::Button("Remove", ImVec2(90 * m_scale, 32 * m_scale))) {
            cfg.wallpaperPath.clear();
            m_res->changed = true;
        }
        ImGui::EndDisabled();
        if (!cfg.wallpaperPath.empty()) {
            ImGui::PushFont(m_fontSmall);
            ImGui::TextColored(kDim, "%s", std::filesystem::path(Widen(cfg.wallpaperPath)).filename().string().c_str());
            ImGui::PopFont();
        }
        if (!m_wallError.empty()) ImGui::TextColored(kWarn, "%s", m_wallError.c_str());
        ImGui::EndGroup();

        ImGui::Spacing();
        ImGui::BeginDisabled(!m_wallpaper);
        Slider("Blur", &cfg.wallpaperBlur, 0.0f, 1.0f, 0.6f);
        Slider("Glass tint", &cfg.wallpaperBlurPanels, 0.0f, 1.0f, 0.55f);
        Slider("Dim", &cfg.wallpaperDim, 0.0f, 0.95f, 0.55f);
        ImGui::EndDisabled();
    }
    EndCard();

    BeginCard("##look", "Accent color");
    {
        LabelRow("Color");
        if (ImGui::ColorEdit3("##accent", cfg.accent.data(), ImGuiColorEditFlags_DisplayHex | ImGuiColorEditFlags_PickerHueWheel))
            m_res->changed = true;
        ImGui::Dummy(ImVec2(130.0f * m_scale - ImGui::GetStyle().ItemSpacing.x, 0));
        ImGui::SameLine();
        constexpr std::array<float, 3> swatches[] = {{0.91f, 0.34f, 0.23f}, {0.95f, 0.65f, 0.15f}, {0.35f, 0.78f, 0.40f},
                                                     {0.25f, 0.62f, 0.98f}, {0.62f, 0.45f, 0.98f}, {0.93f, 0.33f, 0.60f}};
        for (size_t i = 0; i < std::size(swatches); ++i) {
            if (i) ImGui::SameLine();
            ImGui::PushID(static_cast<int>(i) + 100);
            if (ImGui::ColorButton("##a", ImVec4(swatches[i][0], swatches[i][1], swatches[i][2], 1),
                                   ImGuiColorEditFlags_NoTooltip, ImVec2(26 * m_scale, 26 * m_scale))) {
                cfg.accent = swatches[i];
                m_res->changed = true;
            }
            ImGui::PopID();
        }
    }
    EndCard();

    BeginCard("##perf", "Performance");
    {
        LabelRow("Saturation via");
        const char* engines[] = {"GPU driver (zero cost)", "DWM matrix (exact Rec.709)"};
        if (ImGui::Combo("##sateng", &cfg.saturationEngine, engines, 2)) m_res->changed = true;
        LabelRow("Vibrance via");
        const char* vib[] = {"GPU driver (zero cost)", "Shader (exact, costs FPS)"};
        if (ImGui::Combo("##vibmode", &cfg.vibranceMode, vib, 2)) m_res->changed = true;
        if (!st.driverSaturationAvailable && cfg.saturationEngine == 0)
            ImGui::TextColored(kWarn, "No supported GPU driver found, using DWM");
    }
    EndCard();

    BeginCard("##sys", "System");
    {
        auto line = [&](const char* name, bool ok, const char* good, const char* bad) {
            ImGui::TextColored(kDim, "%s", name);
            ImGui::SameLine(170 * m_scale);
            ImGui::TextColored(ok ? kText : kWarn, "%s", ok ? good : bad);
        };
        line("Hotkeys", st.hotkeysOk, "Insert  /  Ctrl+F1-F4  /  End", "some are taken by another app");
        line("Magnification", st.magnificationOk, "ok", "unavailable");
        line("Gamma ramp", st.gammaOk, "ok", "rejected");
        line("Capture exclusion", st.captureExcluded, "ok", "unsupported");
        line("Saturation", true, st.saturationEngine.c_str(), "");
        line("FPS impact", st.fpsCost.empty(), "none", st.fpsCost.c_str());
        ImGui::TextColored(kDim, "Config");
        ImGui::SameLine(170 * m_scale);
        ImGui::TextUnformatted(st.configPath.c_str());
        if (!st.configError.empty()) ImGui::TextColored(kWarn, "%s", st.configError.c_str());
        if (!st.overlayError.empty()) ImGui::TextColored(kWarn, "%s", st.overlayError.c_str());
    }
    EndCard();

    const float bw = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    if (AccentButton("Reset filters", ImVec2(bw, 34 * m_scale))) m_res->panic = true;
    ImGui::SameLine();
    if (AccentButton("Exit", ImVec2(bw, 34 * m_scale), true)) m_res->exitApp = true;
}

}  // namespace pv
