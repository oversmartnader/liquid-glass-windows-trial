#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <DirectXMath.h>
#include <wrl/client.h>
#include <string>
#include <cmath>
#include <chrono>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxgi.lib")

// Added for newer Windows capture exclusion
#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

using Microsoft::WRL::ComPtr;
using namespace DirectX;

static const int WINDOW_WIDTH  = 420;
static const int WINDOW_HEIGHT = 320;
static float     g_time        = 0.0f;
static HWND      g_hwnd        = nullptr;

struct Vertex {
    XMFLOAT3 pos;
    XMFLOAT2 uv;
};

struct CBPerFrame {
    float time;
    float width;
    float height;
    float pad;
};

// Global COM Objects
ComPtr<ID3D11Device>             g_device;
ComPtr<ID3D11DeviceContext>      g_ctx;
ComPtr<IDXGISwapChain>           g_swapChain;
ComPtr<ID3D11RenderTargetView>   g_rtv;
ComPtr<ID3D11Buffer>             g_vb;
ComPtr<ID3D11Buffer>             g_ib;
ComPtr<ID3D11Buffer>             g_cb;
ComPtr<ID3D11VertexShader>       g_vs;
ComPtr<ID3D11PixelShader>        g_ps;
ComPtr<ID3D11InputLayout>        g_il;
ComPtr<ID3D11Texture2D>          g_captureTex;
ComPtr<ID3D11ShaderResourceView> g_captureSRV;
ComPtr<ID3D11SamplerState>       g_sampler;
ComPtr<ID3D11BlendState>         g_blendState;
ComPtr<ID3D11RasterizerState>    g_rsState;

// --- SHADERS ---
static const char* g_vsSource = R"(
struct VSIn {
    float3 pos : POSITION;
    float2 uv  : TEXCOORD0;
};
struct VSOut {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};
VSOut main(VSIn i) {
    VSOut o;
    o.pos = float4(i.pos, 1.0f);
    o.uv  = i.uv;
    return o;
}
)";

static const char* g_psSource = R"(
Texture2D    gTex    : register(t0);
SamplerState gSampler: register(s0);

cbuffer CBPerFrame : register(b0) {
    float gTime;
    float gWidth;
    float gHeight;
    float gPad;
};

float sdRoundBox(float2 p, float2 b, float r) {
    float2 q = abs(p) - b + r;
    return length(max(q, 0.0f)) + min(max(q.x, q.y), 0.0f) - r;
}

float3 LiquidGlass(float2 uv, float2 resolution, float time) {
    float2 p = uv * resolution;
    float2 center = resolution * 0.5f;
    float2 rel = p - center;
    float2 boxSize = float2(resolution.x * 0.42f, resolution.y * 0.42f);
    float radius = 38.0f;
    float dist = sdRoundBox(rel, boxSize, radius);

    float edge = 1.0f - smoothstep(-1.5f, 1.5f, dist);
    float innerMask = 1.0f - smoothstep(-2.0f, 0.0f, dist);

    float2 norm = normalize(rel + float2(0.001f, 0.001f));
    float len = length(rel);
    float2 boxN = rel / (boxSize + radius);

    float wave1 = sin(uv.x * 7.0f + time * 1.1f) * 0.012f;
    float wave2 = cos(uv.y * 6.0f + time * 0.9f) * 0.010f;
    float wave3 = sin((uv.x + uv.y) * 5.0f + time * 1.3f) * 0.008f;

    float rimFactor = smoothstep(0.55f, 1.0f, length(boxN));
    float2 refract = float2(
        wave1 + wave3 + boxN.x * rimFactor * 0.018f,
        wave2 + wave3 + boxN.y * rimFactor * 0.018f
    );

    float2 distUV = uv + refract * innerMask;
    distUV = clamp(distUV, 0.001f, 0.999f);
    float3 bgColor = gTex.Sample(gSampler, distUV).rgb;

    float blurStrength = 0.004f;
    float3 blurred = float3(0,0,0);
    int samples = 9;
    float2 px = float2(1.0f/gWidth, 1.0f/gHeight);
    
    for(int x=-1;x<=1;x++){
        for(int y=-1;y<=1;y++){
            float2 off = float2(x,y)*px*blurStrength*120.0f;
            blurred += gTex.Sample(gSampler, clamp(distUV+off,0.001f,0.999f)).rgb;
        }
    }
    blurred /= float(samples);

    float3 glassColor = lerp(bgColor, blurred, 0.72f);
    glassColor = lerp(glassColor, float3(0.96f,0.97f,1.0f), 0.13f);

    float3 lightDir = normalize(float3(-0.6f, -0.8f, 1.0f));
    float3 normal3D = normalize(float3(boxN.x, boxN.y, 0.5f));
    float spec = pow(max(dot(normal3D, lightDir),0.0f), 22.0f);
    float rimLight = pow(1.0f - max(dot(normalize(float3(rel,0.3f)), float3(0,0,1)),0.0f), 3.5f);

    float highlight1 = exp(-length(rel - float2(-boxSize.x*0.28f, -boxSize.y*0.32f)) / 38.0f);
    float highlight2 = exp(-length(rel - float2( boxSize.x*0.18f,  boxSize.y*0.25f)) / 55.0f);
    float shimmer = sin(time*2.2f + uv.x*11.0f)*0.5f+0.5f;
    float shimmer2= cos(time*1.7f + uv.y*9.0f )*0.5f+0.5f;

    glassColor += spec * 0.55f * float3(1.0f,1.0f,1.0f);
    glassColor += rimLight * 0.09f * float3(0.8f,0.9f,1.0f);
    glassColor += highlight1 * 0.18f * float3(1.0f,1.0f,1.0f) * (0.7f+0.3f*shimmer);
    glassColor += highlight2 * 0.10f * float3(0.9f,0.95f,1.0f)* (0.7f+0.3f*shimmer2);

    float borderGlow = exp(-abs(dist)*0.12f) * (dist < 0.0f ? 0.0f : 1.0f);
    glassColor += borderGlow * float3(0.85f,0.92f,1.0f) * 0.22f;

    float alpha = innerMask;
    return glassColor * alpha + bgColor * (1.0f - alpha);
}

float4 main(float4 svpos : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
    float3 col = LiquidGlass(uv, float2(gWidth, gHeight), gTime);
    return float4(col, 1.0f);
}
)";

// --- LOGIC ---
bool CaptureDesktopToTexture() {
    HDC screenDC = GetDC(nullptr);
    HDC memDC    = CreateCompatibleDC(screenDC);

    RECT wr; GetWindowRect(g_hwnd, &wr);
    int wx = wr.left, wy = wr.top;

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = WINDOW_WIDTH;
    bmi.bmiHeader.biHeight      = -WINDOW_HEIGHT; // Top-down
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP hbm = CreateDIBSection(memDC, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HBITMAP oldHbm = (HBITMAP)SelectObject(memDC, hbm);

    // Capture (Window itself is excluded via WDA_EXCLUDEFROMCAPTURE in WinMain)
    BitBlt(memDC, 0, 0, WINDOW_WIDTH, WINDOW_HEIGHT, screenDC, wx, wy, SRCCOPY);

    // Zero-copy Map/Unmap to update the GPU Texture
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(g_ctx->Map(g_captureTex.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        BYTE* dest = (BYTE*)mapped.pData;
        BYTE* src = (BYTE*)bits;
        for (int y = 0; y < WINDOW_HEIGHT; y++) {
            memcpy(dest + (y * mapped.RowPitch), src + (y * WINDOW_WIDTH * 4), WINDOW_WIDTH * 4);
        }
        g_ctx->Unmap(g_captureTex.Get(), 0);
    }

    SelectObject(memDC, oldHbm);
    DeleteObject(hbm);
    DeleteDC(memDC);
    ReleaseDC(nullptr, screenDC);
    
    return true;
}

bool InitD3D(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC scd = {};
    scd.BufferCount       = 2;
    scd.BufferDesc.Width  = WINDOW_WIDTH;
    scd.BufferDesc.Height = WINDOW_HEIGHT;
    scd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.BufferDesc.RefreshRate.Numerator   = 60;
    scd.BufferDesc.RefreshRate.Denominator = 1;
    scd.BufferUsage  = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = hwnd;
    scd.SampleDesc.Count = 1;
    scd.Windowed    = TRUE;
    scd.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scd.Flags       = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION,
        &scd, &g_swapChain, &g_device, &fl, &g_ctx
    );
    if (FAILED(hr)) return false;

    ComPtr<ID3D11Texture2D> bb;
    g_swapChain->GetBuffer(0, IID_PPV_ARGS(&bb));
    g_device->CreateRenderTargetView(bb.Get(), nullptr, &g_rtv);

    ComPtr<ID3DBlob> vsBlob, psBlob, errBlob;
    hr = D3DCompile(g_vsSource, strlen(g_vsSource), nullptr, nullptr, nullptr,
        "main", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vsBlob, &errBlob);
    if (FAILED(hr)) return false;

    hr = D3DCompile(g_psSource, strlen(g_psSource), nullptr, nullptr, nullptr,
        "main", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &psBlob, &errBlob);
    if (FAILED(hr)) return false;

    g_device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &g_vs);
    g_device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &g_ps);

    D3D11_INPUT_ELEMENT_DESC ied[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    g_device->CreateInputLayout(ied, 2, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &g_il);

    Vertex verts[] = {
        {{-1.0f,  1.0f, 0.0f}, {0.0f, 0.0f}},
        {{ 1.0f,  1.0f, 0.0f}, {1.0f, 0.0f}},
        {{ 1.0f, -1.0f, 0.0f}, {1.0f, 1.0f}},
        {{-1.0f, -1.0f, 0.0f}, {0.0f, 1.0f}},
    };
    UINT indices[] = {0,1,2, 0,2,3};

    D3D11_BUFFER_DESC bd = {};
    bd.Usage     = D3D11_USAGE_DEFAULT;
    bd.ByteWidth = sizeof(verts);
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA vsd = {verts};
    g_device->CreateBuffer(&bd, &vsd, &g_vb);

    bd.ByteWidth = sizeof(indices);
    bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    D3D11_SUBRESOURCE_DATA isd = {indices};
    g_device->CreateBuffer(&bd, &isd, &g_ib);

    bd.ByteWidth = sizeof(CBPerFrame);
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.Usage     = D3D11_USAGE_DYNAMIC;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    g_device->CreateBuffer(&bd, nullptr, &g_cb);

    D3D11_SAMPLER_DESC smpd = {};
    smpd.Filter   = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    smpd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    smpd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    smpd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    g_device->CreateSamplerState(&smpd, &g_sampler);

    D3D11_BLEND_DESC bld = {};
    bld.RenderTarget[0].BlendEnable           = TRUE;
    bld.RenderTarget[0].SrcBlend              = D3D11_BLEND_SRC_ALPHA;
    bld.RenderTarget[0].DestBlend             = D3D11_BLEND_INV_SRC_ALPHA;
    bld.RenderTarget[0].BlendOp               = D3D11_BLEND_OP_ADD;
    bld.RenderTarget[0].SrcBlendAlpha         = D3D11_BLEND_ONE;
    bld.RenderTarget[0].DestBlendAlpha        = D3D11_BLEND_ZERO;
    bld.RenderTarget[0].BlendOpAlpha          = D3D11_BLEND_OP_ADD;
    bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    g_device->CreateBlendState(&bld, &g_blendState);

    D3D11_RASTERIZER_DESC rsd = {};
    rsd.FillMode = D3D11_FILL_SOLID;
    rsd.CullMode = D3D11_CULL_NONE;
    g_device->CreateRasterizerState(&rsd, &g_rsState);

    // --- NEW: Create Dynamic Texture ONCE ---
    D3D11_TEXTURE2D_DESC td = {};
    td.Width            = WINDOW_WIDTH;
    td.Height           = WINDOW_HEIGHT;
    td.MipLevels        = 1;
    td.ArraySize        = 1;
    // B8G8R8X8 format tells DX to ignore the alpha channel entirely. No CPU conversion needed!
    td.Format           = DXGI_FORMAT_B8G8R8X8_UNORM; 
    td.SampleDesc.Count = 1;
    td.Usage            = D3D11_USAGE_DYNAMIC;
    td.BindFlags        = D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags   = D3D11_CPU_ACCESS_WRITE;

    hr = g_device->CreateTexture2D(&td, nullptr, &g_captureTex);
    if (FAILED(hr)) return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC srvd = {};
    srvd.Format              = DXGI_FORMAT_B8G8R8X8_UNORM;
    srvd.ViewDimension       = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvd.Texture2D.MipLevels = 1;
    hr = g_device->CreateShaderResourceView(g_captureTex.Get(), &srvd, &g_captureSRV);
    if (FAILED(hr)) return false;

    return true;
}

void Render() {
    static auto startTime = std::chrono::high_resolution_clock::now();
    auto now = std::chrono::high_resolution_clock::now();
    g_time = std::chrono::duration<float>(now - startTime).count();

    CaptureDesktopToTexture();

    float clearColor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    g_ctx->ClearRenderTargetView(g_rtv.Get(), clearColor);
    g_ctx->OMSetRenderTargets(1, g_rtv.GetAddressOf(), nullptr);

    D3D11_VIEWPORT vp = {0.0f, 0.0f, (float)WINDOW_WIDTH, (float)WINDOW_HEIGHT, 0.0f, 1.0f};
    g_ctx->RSSetViewports(1, &vp);
    g_ctx->RSSetState(g_rsState.Get());

    D3D11_MAPPED_SUBRESOURCE mapped;
    g_ctx->Map(g_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    CBPerFrame* cb = (CBPerFrame*)mapped.pData;
    cb->time   = g_time;
    cb->width  = (float)WINDOW_WIDTH;
    cb->height = (float)WINDOW_HEIGHT;
    cb->pad    = 0.0f;
    g_ctx->Unmap(g_cb.Get(), 0);

    UINT stride = sizeof(Vertex), offset = 0;
    g_ctx->IASetVertexBuffers(0, 1, g_vb.GetAddressOf(), &stride, &offset);
    g_ctx->IASetIndexBuffer(g_ib.Get(), DXGI_FORMAT_R32_UINT, 0);
    g_ctx->IASetInputLayout(g_il.Get());
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_ctx->VSSetShader(g_vs.Get(), nullptr, 0);
    g_ctx->PSSetShader(g_ps.Get(), nullptr, 0);
    g_ctx->PSSetConstantBuffers(0, 1, g_cb.GetAddressOf());
    g_ctx->PSSetShaderResources(0, 1, g_captureSRV.GetAddressOf());
    g_ctx->PSSetSamplers(0, 1, g_sampler.GetAddressOf());
    
    float blendFactor[4] = {0,0,0,0};
    g_ctx->OMSetBlendState(g_blendState.Get(), blendFactor, 0xffffffff);

    g_ctx->DrawIndexed(6, 0, 0);
    g_swapChain->Present(1, 0);
}

void SetWindowTransparent(HWND hwnd) {
    SetWindowLong(hwnd, GWL_EXSTYLE,
        GetWindowLong(hwnd, GWL_EXSTYLE) | WS_EX_LAYERED | WS_EX_TOPMOST);

    DWM_BLURBEHIND bb = {};
    bb.dwFlags  = DWM_BB_ENABLE;
    bb.fEnable  = TRUE;
    DwmEnableBlurBehindWindow(hwnd, &bb);

    MARGINS margins = {-1, -1, -1, -1};
    DwmExtendFrameIntoClientArea(hwnd, &margins);

    BOOL enable = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &enable, sizeof(enable));

    DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUNDSMALL;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
}

static bool   g_dragging = false;
static POINT  g_dragStart = {};
static RECT   g_winStart  = {};

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_LBUTTONDOWN: {
        g_dragging = true;
        SetCapture(hwnd);
        g_dragStart = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        GetWindowRect(hwnd, &g_winStart);
        ClientToScreen(hwnd, &g_dragStart);
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (g_dragging) {
            POINT cur; GetCursorPos(&cur);
            int dx = cur.x - g_dragStart.x;
            int dy = cur.y - g_dragStart.y;
            SetWindowPos(hwnd, nullptr,
                g_winStart.left + dx, g_winStart.top + dy,
                0, 0, SWP_NOSIZE | SWP_NOZORDER);
        }
        return 0;
    }
    case WM_LBUTTONUP:
        g_dragging = false;
        ReleaseCapture();
        return 0;
    case WM_RBUTTONDOWN:
        PostQuitMessage(0);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    // Initialize COM for safe DirectX interactions
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    WNDCLASSEX wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = L"LiquidGlass";
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassEx(&wc);

    // Added WS_EX_NOREDIRECTIONBITMAP to support modern flip-model rendering
    g_hwnd = CreateWindowEx(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP,
        L"LiquidGlass", L"Liquid Glass",
        WS_POPUP,
        100, 100, WINDOW_WIDTH, WINDOW_HEIGHT,
        nullptr, nullptr, hInst, nullptr
    );

    // Prevents Hall-of-Mirrors feedback loop during BitBlt
    SetWindowDisplayAffinity(g_hwnd, WDA_EXCLUDEFROMCAPTURE);

    SetWindowTransparent(g_hwnd);
    
    if (!InitD3D(g_hwnd)) {
        CoUninitialize();
        return -1;
    }

    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);

    MSG msg = {};
    while (true) {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) goto done;
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        Render();
    }
done:
    CoUninitialize();
    return (int)msg.wParam;
}
