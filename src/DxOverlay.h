#pragma once

#include <windows.h>

#include <d3d11.h>
#include <dcomp.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <array>
#include <string>

namespace pv {

using Microsoft::WRL::ComPtr;

struct EffectParams {
    bool sharpen = false;
    float vibrance = 0.0f;
    float sharpenStrength = 0.65f;
    float sharpenClamp = 0.06f;

    bool vignette = false;
    float vignetteStrength = 0.5f;
    float vignetteRadius = 0.55f;
    float vignetteSoftness = 0.45f;

    bool zoom = false;
    float zoomFactor = 2.5f;
    float zoomRadius = 200.0f;
    std::array<float, 4> zoomBorder{0.13f, 0.82f, 0.72f, 0.9f};
};

// Transparent D3D11 + DirectComposition overlay.
// Desktop Duplication runs only while an effect needs the screen.
class DxOverlay {
public:
    DxOverlay() = default;
    ~DxOverlay();
    DxOverlay(const DxOverlay&) = delete;
    DxOverlay& operator=(const DxOverlay&) = delete;

    HRESULT Initialize(HWND hwnd, UINT width, UINT height);
    void Shutdown() noexcept;
    HRESULT Resize(UINT width, UINT height);

    void SetCaptureEnabled(bool enabled);
    // Copies a new desktop frame if there is one, waiting up to timeoutMs. True = image changed.
    bool PollCapture(UINT timeoutMs);
    void ResetCapture() noexcept;  // display mode changed
    [[nodiscard]] bool CaptureActive() const noexcept { return m_duplication != nullptr; }

    void BeginFrame();
    void RenderEffects(const EffectParams& params);
    // 1 = wait for vblank (menu animations), 0 = hand the frame to DWM right away (lowest latency)
    HRESULT Present(UINT syncInterval);

    [[nodiscard]] ID3D11Device* Device() const noexcept { return m_device.Get(); }
    [[nodiscard]] ID3D11DeviceContext* Context() const noexcept { return m_context.Get(); }
    [[nodiscard]] const std::wstring& LastError() const noexcept { return m_lastError; }
    [[nodiscard]] UINT Width() const noexcept { return m_width; }
    [[nodiscard]] UINT Height() const noexcept { return m_height; }

private:
    HRESULT CreateDevice();
    HRESULT CreateSwapChain();
    HRESULT CreateRenderTarget();
    HRESULT CreateComposition();
    HRESULT CreateShaders();
    HRESULT CreateStates();

    bool TryStartDuplication();
    void StopDuplication() noexcept;
    HRESULT EnsureCaptureTexture(const D3D11_TEXTURE2D_DESC& srcDesc);
    void DrawFullscreen(ID3D11PixelShader* ps, ID3D11ShaderResourceView* srv);

    HWND m_hwnd = nullptr;
    UINT m_width = 1;
    UINT m_height = 1;
    D3D_FEATURE_LEVEL m_featureLevel = D3D_FEATURE_LEVEL_11_0;
    std::wstring m_lastError;

    ComPtr<IDXGIFactory2> m_factory;
    ComPtr<ID3D11Device> m_device;
    ComPtr<ID3D11DeviceContext> m_context;
    ComPtr<IDXGIOutput1> m_output;
    ComPtr<IDXGISwapChain1> m_swapChain;
    ComPtr<ID3D11RenderTargetView> m_rtv;

    ComPtr<IDCompositionDevice> m_dcompDevice;
    ComPtr<IDCompositionTarget> m_dcompTarget;
    ComPtr<IDCompositionVisual> m_dcompVisual;

    ComPtr<ID3D11VertexShader> m_vsFullscreen;
    ComPtr<ID3D11PixelShader> m_psPost;
    ComPtr<ID3D11PixelShader> m_psVignette;
    ComPtr<ID3D11PixelShader> m_psZoom;
    ComPtr<ID3D11Buffer> m_constants;
    ComPtr<ID3D11SamplerState> m_sampler;
    ComPtr<ID3D11BlendState> m_blendPremultiplied;
    ComPtr<ID3D11RasterizerState> m_rasterizer;
    ComPtr<ID3D11DepthStencilState> m_depthDisabled;

    bool m_captureWanted = false;
    ULONGLONG m_nextDuplicationAttempt = 0;
    ComPtr<IDXGIOutputDuplication> m_duplication;
    ComPtr<ID3D11Texture2D> m_captureTex;
    ComPtr<ID3D11ShaderResourceView> m_captureSrv;
};

}  // namespace pv
