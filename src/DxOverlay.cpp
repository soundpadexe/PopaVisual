#include "DxOverlay.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>

namespace pv {

namespace {

struct alignas(16) EffectConstants {
    float screenSize[2];
    float invScreenSize[2];
    float sharpStrength;
    float sharpClamp;
    float zoomFactor;
    float zoomRadius;
    float vignetteStrength;
    float vignetteRadius;
    float vignetteSoftness;
    float vibrance;
    float zoomBorder[4];
};
static_assert(sizeof(EffectConstants) == 64, "cbuffer layout must match Shaders.hlsl");

std::wstring FormatHr(HRESULT hr, const wchar_t* what) {
    wchar_t buf[256];
    swprintf_s(buf, L"%s failed (HRESULT 0x%08X)", what, static_cast<unsigned>(hr));
    return buf;
}

std::wstring Widen(const char* text, size_t len) {
    if (!text || len == 0) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, text, static_cast<int>(len), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, static_cast<int>(len), out.data(), n);
    return out;
}

std::filesystem::path ExeDir() {
    wchar_t buf[MAX_PATH]{};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return std::filesystem::path(buf).parent_path();
}

}  // namespace

#define PV_CHECK(expr, what)                         \
    do {                                             \
        const HRESULT hr_ = (expr);                  \
        if (FAILED(hr_)) {                           \
            m_lastError = FormatHr(hr_, what);       \
            return hr_;                              \
        }                                            \
    } while (0)

DxOverlay::~DxOverlay() { Shutdown(); }

HRESULT DxOverlay::Initialize(HWND hwnd, UINT width, UINT height) {
    Shutdown();
    m_hwnd = hwnd;
    m_width = std::max(width, 1u);
    m_height = std::max(height, 1u);
    m_lastError.clear();

    if (HRESULT hr = CreateDevice(); FAILED(hr)) return hr;
    if (HRESULT hr = CreateSwapChain(); FAILED(hr)) return hr;
    if (HRESULT hr = CreateRenderTarget(); FAILED(hr)) return hr;
    if (HRESULT hr = CreateComposition(); FAILED(hr)) return hr;
    if (HRESULT hr = CreateShaders(); FAILED(hr)) return hr;
    if (HRESULT hr = CreateStates(); FAILED(hr)) return hr;
    return S_OK;
}

void DxOverlay::Shutdown() noexcept {
    StopDuplication();
    m_captureSrv.Reset();
    m_captureTex.Reset();
    if (m_context) {
        m_context->ClearState();
        m_context->Flush();
    }
    m_depthDisabled.Reset();
    m_rasterizer.Reset();
    m_blendPremultiplied.Reset();
    m_sampler.Reset();
    m_constants.Reset();
    m_psZoom.Reset();
    m_psVignette.Reset();
    m_psPost.Reset();
    m_vsFullscreen.Reset();
    m_dcompVisual.Reset();
    m_dcompTarget.Reset();
    m_dcompDevice.Reset();
    m_rtv.Reset();
    m_swapChain.Reset();
    m_output.Reset();
    m_context.Reset();
    m_device.Reset();
    m_factory.Reset();
}

// Create the device on the adapter of the primary monitor, otherwise DuplicateOutput
// fails on hybrid-GPU laptops.
HRESULT DxOverlay::CreateDevice() {
    PV_CHECK(CreateDXGIFactory2(0, IID_PPV_ARGS(&m_factory)), L"CreateDXGIFactory2");

    const HMONITOR primary = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    ComPtr<IDXGIAdapter1> chosenAdapter;
    ComPtr<IDXGIOutput> chosenOutput;

    ComPtr<IDXGIAdapter1> adapter;
    for (UINT a = 0; !chosenOutput && m_factory->EnumAdapters1(a, adapter.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++a) {
        ComPtr<IDXGIOutput> output;
        for (UINT o = 0; adapter->EnumOutputs(o, output.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++o) {
            DXGI_OUTPUT_DESC desc{};
            if (SUCCEEDED(output->GetDesc(&desc)) && desc.Monitor == primary) {
                chosenAdapter = adapter;
                chosenOutput = output;
                break;
            }
        }
    }

    constexpr UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                        D3D_FEATURE_LEVEL_10_0};
    const D3D_DRIVER_TYPE driverType = chosenAdapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE;

    HRESULT hr = D3D11CreateDevice(chosenAdapter.Get(), driverType, nullptr, flags, levels,
                                   static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, &m_device,
                                   &m_featureLevel, &m_context);
    if (hr == E_INVALIDARG) {  // runtime without 11.1
        hr = D3D11CreateDevice(chosenAdapter.Get(), driverType, nullptr, flags, levels + 1,
                               static_cast<UINT>(std::size(levels) - 1), D3D11_SDK_VERSION, &m_device,
                               &m_featureLevel, &m_context);
    }
    PV_CHECK(hr, L"D3D11CreateDevice");

    if (!chosenOutput) {
        ComPtr<IDXGIDevice> dxgiDevice;
        PV_CHECK(m_device.As(&dxgiDevice), L"Query IDXGIDevice");
        ComPtr<IDXGIAdapter> fallbackAdapter;
        PV_CHECK(dxgiDevice->GetAdapter(&fallbackAdapter), L"IDXGIDevice::GetAdapter");
        PV_CHECK(fallbackAdapter->EnumOutputs(0, &chosenOutput), L"EnumOutputs");
    }
    PV_CHECK(chosenOutput.As(&m_output), L"Query IDXGIOutput1");

    ComPtr<IDXGIDevice1> dxgiDevice1;
    if (SUCCEEDED(m_device.As(&dxgiDevice1))) dxgiDevice1->SetMaximumFrameLatency(1);
    return S_OK;
}

HRESULT DxOverlay::CreateSwapChain() {
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = m_width;
    desc.Height = m_height;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;

    HRESULT hr = m_factory->CreateSwapChainForComposition(m_device.Get(), &desc, nullptr, &m_swapChain);
    if (FAILED(hr)) {  // older drivers only take FLIP_SEQUENTIAL
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        hr = m_factory->CreateSwapChainForComposition(m_device.Get(), &desc, nullptr, &m_swapChain);
    }
    PV_CHECK(hr, L"CreateSwapChainForComposition");
    return S_OK;
}

HRESULT DxOverlay::CreateRenderTarget() {
    ComPtr<ID3D11Texture2D> backBuffer;
    PV_CHECK(m_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer)), L"GetBuffer");
    PV_CHECK(m_device->CreateRenderTargetView(backBuffer.Get(), nullptr, &m_rtv), L"CreateRenderTargetView");
    return S_OK;
}

HRESULT DxOverlay::CreateComposition() {
    ComPtr<IDXGIDevice> dxgiDevice;
    PV_CHECK(m_device.As(&dxgiDevice), L"Query IDXGIDevice");
    PV_CHECK(DCompositionCreateDevice(dxgiDevice.Get(), IID_PPV_ARGS(&m_dcompDevice)), L"DCompositionCreateDevice");
    PV_CHECK(m_dcompDevice->CreateTargetForHwnd(m_hwnd, TRUE, &m_dcompTarget), L"CreateTargetForHwnd");
    PV_CHECK(m_dcompDevice->CreateVisual(&m_dcompVisual), L"CreateVisual");
    PV_CHECK(m_dcompVisual->SetContent(m_swapChain.Get()), L"Visual::SetContent");
    PV_CHECK(m_dcompTarget->SetRoot(m_dcompVisual.Get()), L"Target::SetRoot");
    PV_CHECK(m_dcompDevice->Commit(), L"DComp Commit");
    return S_OK;
}

HRESULT DxOverlay::CreateShaders() {
    const std::filesystem::path path = ExeDir() / L"Shaders.hlsl";
    const bool sm5 = m_featureLevel >= D3D_FEATURE_LEVEL_11_0;
    const char* vsTarget = sm5 ? "vs_5_0" : "vs_4_0";
    const char* psTarget = sm5 ? "ps_5_0" : "ps_4_0";

    auto compile = [&](const char* entry, const char* target, ComPtr<ID3DBlob>& out) -> HRESULT {
        ComPtr<ID3DBlob> errors;
        const UINT compileFlags = D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_ENABLE_STRICTNESS;
        const HRESULT hr = D3DCompileFromFile(path.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, entry,
                                              target, compileFlags, 0, &out, &errors);
        if (FAILED(hr)) {
            m_lastError = FormatHr(hr, L"Shader compile") + L" [" + Widen(entry, std::strlen(entry)) + L"] " +
                          path.wstring();
            if (errors)
                m_lastError += L"\n" + Widen(static_cast<const char*>(errors->GetBufferPointer()),
                                             errors->GetBufferSize());
        }
        return hr;
    };

    ComPtr<ID3DBlob> blob;
    if (HRESULT hr = compile("VSFullscreen", vsTarget, blob); FAILED(hr)) return hr;
    PV_CHECK(m_device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &m_vsFullscreen),
             L"CreateVertexShader");

    struct PsEntry {
        const char* entry;
        ComPtr<ID3D11PixelShader>* target;
    };
    const PsEntry entries[] = {{"PSPost", &m_psPost}, {"PSVignette", &m_psVignette}, {"PSZoom", &m_psZoom}};
    for (const PsEntry& e : entries) {
        blob.Reset();
        if (HRESULT hr = compile(e.entry, psTarget, blob); FAILED(hr)) return hr;
        PV_CHECK(m_device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr,
                                             e.target->ReleaseAndGetAddressOf()),
                 L"CreatePixelShader");
    }
    return S_OK;
}

HRESULT DxOverlay::CreateStates() {
    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = sizeof(EffectConstants);
    cb.Usage = D3D11_USAGE_DYNAMIC;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    PV_CHECK(m_device->CreateBuffer(&cb, nullptr, &m_constants), L"CreateBuffer(cbuffer)");

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    PV_CHECK(m_device->CreateSamplerState(&sd, &m_sampler), L"CreateSamplerState");

    D3D11_BLEND_DESC bd{};
    auto& rt = bd.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = D3D11_BLEND_ONE;
    rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOp = D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D11_BLEND_ONE;
    rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    PV_CHECK(m_device->CreateBlendState(&bd, &m_blendPremultiplied), L"CreateBlendState");

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    PV_CHECK(m_device->CreateRasterizerState(&rd, &m_rasterizer), L"CreateRasterizerState");

    D3D11_DEPTH_STENCIL_DESC dsd{};
    dsd.DepthEnable = FALSE;
    dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dsd.DepthFunc = D3D11_COMPARISON_ALWAYS;
    PV_CHECK(m_device->CreateDepthStencilState(&dsd, &m_depthDisabled), L"CreateDepthStencilState");
    return S_OK;
}

HRESULT DxOverlay::Resize(UINT width, UINT height) {
    if (!m_swapChain) return E_UNEXPECTED;
    width = std::max(width, 1u);
    height = std::max(height, 1u);
    if (width == m_width && height == m_height) return S_OK;

    // duplication belongs to the output, not to our swap chain: keep it running
    m_context->OMSetRenderTargets(0, nullptr, nullptr);
    m_rtv.Reset();
    m_context->Flush();
    m_width = width;
    m_height = height;
    PV_CHECK(m_swapChain->ResizeBuffers(0, m_width, m_height, DXGI_FORMAT_UNKNOWN, 0), L"ResizeBuffers");
    return CreateRenderTarget();
}

void DxOverlay::SetCaptureEnabled(bool enabled) {
    if (enabled == m_captureWanted) {
        if (enabled && !m_duplication) TryStartDuplication();
        return;
    }
    m_captureWanted = enabled;
    if (enabled) {
        m_nextDuplicationAttempt = 0;
        TryStartDuplication();
    } else {

        StopDuplication();
        m_captureSrv.Reset();
        m_captureTex.Reset();
    }
}

bool DxOverlay::TryStartDuplication() {
    if (m_duplication) return true;
    if (!m_output || !m_device || !m_captureWanted) return false;
    const ULONGLONG now = GetTickCount64();
    if (now < m_nextDuplicationAttempt) return false;

    const HRESULT hr = m_output->DuplicateOutput(m_device.Get(), &m_duplication);
    if (FAILED(hr)) {
        // E_ACCESSDENIED on UAC / lock screen, retry in a second
        m_duplication.Reset();
        m_nextDuplicationAttempt = now + 1000;
        m_lastError = FormatHr(hr, L"DuplicateOutput");
        return false;
    }
    return true;
}

void DxOverlay::StopDuplication() noexcept { m_duplication.Reset(); }

HRESULT DxOverlay::EnsureCaptureTexture(const D3D11_TEXTURE2D_DESC& src) {
    if (m_captureTex) {
        D3D11_TEXTURE2D_DESC cur{};
        m_captureTex->GetDesc(&cur);
        if (cur.Width == src.Width && cur.Height == src.Height && cur.Format == src.Format) return S_OK;
    }
    m_captureSrv.Reset();
    m_captureTex.Reset();

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = src.Width;
    desc.Height = src.Height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = src.Format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    PV_CHECK(m_device->CreateTexture2D(&desc, nullptr, &m_captureTex), L"CreateTexture2D(capture)");
    PV_CHECK(m_device->CreateShaderResourceView(m_captureTex.Get(), nullptr, &m_captureSrv), L"CreateSRV(capture)");
    return S_OK;
}

void DxOverlay::ResetCapture() noexcept {
    StopDuplication();
    m_captureSrv.Reset();
    m_captureTex.Reset();
    m_nextDuplicationAttempt = 0;
}

bool DxOverlay::PollCapture(UINT timeoutMs) {
    if (!m_captureWanted) return false;
    if (!m_duplication && !TryStartDuplication()) {
        if (timeoutMs) Sleep(std::min<UINT>(timeoutMs, 10));
        return false;
    }

    DXGI_OUTDUPL_FRAME_INFO info{};
    ComPtr<IDXGIResource> resource;
    // waits for the desktop to actually change, so an unchanged frame is never redrawn
    const HRESULT hr = m_duplication->AcquireNextFrame(timeoutMs, &info, &resource);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) return false;
    if (FAILED(hr)) {
        // ACCESS_LOST: mode change, fullscreen switch, desktop switch
        StopDuplication();
        m_nextDuplicationAttempt = GetTickCount64() + 250;
        return false;
    }

    bool fresh = false;
    // LastPresentTime == 0: only the mouse pointer moved, the image itself is the same
    if (info.LastPresentTime.QuadPart != 0 || !m_captureSrv) {
        ComPtr<ID3D11Texture2D> frame;
        if (SUCCEEDED(resource.As(&frame))) {
            D3D11_TEXTURE2D_DESC desc{};
            frame->GetDesc(&desc);
            if (SUCCEEDED(EnsureCaptureTexture(desc))) {
                m_context->CopyResource(m_captureTex.Get(), frame.Get());
                fresh = true;
            }
        }
    }
    m_duplication->ReleaseFrame();
    return fresh;
}

void DxOverlay::BeginFrame() {
    constexpr float clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    ID3D11RenderTargetView* rtv = m_rtv.Get();
    m_context->OMSetRenderTargets(1, &rtv, nullptr);
    m_context->ClearRenderTargetView(m_rtv.Get(), clear);

    D3D11_VIEWPORT vp{};
    vp.Width = static_cast<float>(m_width);
    vp.Height = static_cast<float>(m_height);
    vp.MaxDepth = 1.0f;
    m_context->RSSetViewports(1, &vp);
}

void DxOverlay::DrawFullscreen(ID3D11PixelShader* ps, ID3D11ShaderResourceView* srv) {
    m_context->PSSetShader(ps, nullptr, 0);
    m_context->PSSetShaderResources(0, 1, &srv);
    m_context->Draw(3, 0);
}

void DxOverlay::RenderEffects(const EffectParams& p) {
    const bool post = p.sharpen || p.vibrance != 0.0f;
    const bool needCapture = post || p.zoom;
    const bool haveCapture = needCapture && m_captureWanted && m_captureSrv != nullptr;
    if (!p.vignette && !haveCapture) return;

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(m_context->Map(m_constants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;
    auto* cb = static_cast<EffectConstants*>(mapped.pData);
    cb->screenSize[0] = static_cast<float>(m_width);
    cb->screenSize[1] = static_cast<float>(m_height);
    cb->invScreenSize[0] = 1.0f / static_cast<float>(m_width);
    cb->invScreenSize[1] = 1.0f / static_cast<float>(m_height);
    cb->sharpStrength = p.sharpen ? p.sharpenStrength : 0.0f;
    cb->sharpClamp = p.sharpenClamp;
    cb->zoomFactor = std::max(p.zoomFactor, 1.0f);
    cb->zoomRadius = p.zoomRadius;
    cb->vignetteStrength = p.vignetteStrength;
    cb->vignetteRadius = p.vignetteRadius;
    cb->vignetteSoftness = std::max(p.vignetteSoftness, 0.01f);
    cb->vibrance = p.vibrance;
    std::copy(p.zoomBorder.begin(), p.zoomBorder.end(), cb->zoomBorder);
    m_context->Unmap(m_constants.Get(), 0);

    ID3D11RenderTargetView* rtv = m_rtv.Get();
    ID3D11Buffer* cbs = m_constants.Get();
    ID3D11SamplerState* sampler = m_sampler.Get();
    constexpr float blendFactor[4] = {0, 0, 0, 0};

    m_context->OMSetRenderTargets(1, &rtv, nullptr);
    m_context->OMSetBlendState(m_blendPremultiplied.Get(), blendFactor, 0xFFFFFFFF);
    m_context->OMSetDepthStencilState(m_depthDisabled.Get(), 0);
    m_context->RSSetState(m_rasterizer.Get());
    m_context->IASetInputLayout(nullptr);
    m_context->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_context->VSSetShader(m_vsFullscreen.Get(), nullptr, 0);
    m_context->GSSetShader(nullptr, nullptr, 0);
    m_context->PSSetConstantBuffers(0, 1, &cbs);
    m_context->PSSetSamplers(0, 1, &sampler);

    if (post && haveCapture) DrawFullscreen(m_psPost.Get(), m_captureSrv.Get());
    if (p.vignette) DrawFullscreen(m_psVignette.Get(), nullptr);
    if (p.zoom && haveCapture) DrawFullscreen(m_psZoom.Get(), m_captureSrv.Get());

    ID3D11ShaderResourceView* nullSrv = nullptr;
    m_context->PSSetShaderResources(0, 1, &nullSrv);
}

HRESULT DxOverlay::Present(UINT syncInterval) {
    if (!m_swapChain) return E_UNEXPECTED;
    const HRESULT hr = m_swapChain->Present(syncInterval, 0);
    if (FAILED(hr)) m_lastError = FormatHr(hr, L"Present");
    return hr;
}

#undef PV_CHECK

}  // namespace pv
