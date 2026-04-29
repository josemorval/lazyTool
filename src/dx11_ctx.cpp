#include "dx11_ctx.h"
#include "log.h"
#include <d3dcompiler.h>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

// This module wraps the shared Direct3D 11 objects used by the whole tool:
// swap chain, scene targets, depth buffers, samplers, and shared CBs.

DX11Ctx g_dx = {};

static const char* s_shadow_vs_src = R"HLSL(
cbuffer SceneCB : register(b0)
{
    float4x4 ViewProj;
    float4 TimeVec;
    float4 LightDir;
    float4 LightColor;
    float4 CamPos;
    float4x4 ShadowViewProj;
    float4x4 InvViewProj;
    float4x4 PrevViewProj;
    float4x4 PrevInvViewProj;
    float4x4 PrevShadowViewProj;
};
cbuffer ObjectCB : register(b2)
{
    float4x4 World;
};

struct VSIn {
    float3 pos : POSITION;
    float3 nor : NORMAL;
    float2 uv  : TEXCOORD0;
};

struct VSOut {
    float4 pos : SV_POSITION;
};

VSOut VSMain(VSIn v) {
    VSOut o;
    float4 wpos = mul(World, float4(v.pos, 1.0));
    o.pos = mul(ShadowViewProj, wpos);
    return o;
}
)HLSL";

static void safe_release_scene_rt() {
    if (g_dx.scene_rtv) { g_dx.scene_rtv->Release(); g_dx.scene_rtv = nullptr; }
    if (g_dx.scene_srv) { g_dx.scene_srv->Release(); g_dx.scene_srv = nullptr; }
    if (g_dx.scene_uav) { g_dx.scene_uav->Release(); g_dx.scene_uav = nullptr; }
    if (g_dx.scene_tex) { g_dx.scene_tex->Release(); g_dx.scene_tex = nullptr; }
    if (g_dx.depth_dsv) { g_dx.depth_dsv->Release(); g_dx.depth_dsv = nullptr; }
    if (g_dx.depth_srv) { g_dx.depth_srv->Release(); g_dx.depth_srv = nullptr; }
    if (g_dx.depth_tex) { g_dx.depth_tex->Release(); g_dx.depth_tex = nullptr; }
}

static void safe_release_shadow_map() {
    if (g_dx.shadow_srv) { g_dx.shadow_srv->Release(); g_dx.shadow_srv = nullptr; }
    if (g_dx.shadow_dsv) { g_dx.shadow_dsv->Release(); g_dx.shadow_dsv = nullptr; }
    if (g_dx.shadow_tex) { g_dx.shadow_tex->Release(); g_dx.shadow_tex = nullptr; }
}

static void create_builtin_shadow_shader() {
    ID3DBlob* blob = nullptr;
    ID3DBlob* err = nullptr;
    HRESULT hr = D3DCompile(s_shadow_vs_src, strlen(s_shadow_vs_src), "builtin_shadow_vs",
        nullptr, nullptr, "VSMain", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &err);
    if (FAILED(hr) || !blob) {
        const char* msg = err ? (const char*)err->GetBufferPointer() : "unknown error";
        log_error("Builtin shadow VS compile failed: %s", msg);
        if (err) err->Release();
        return;
    }
    if (err) err->Release();

    g_dx.dev->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &g_dx.shadow_vs);

    D3D11_INPUT_ELEMENT_DESC ied[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    g_dx.dev->CreateInputLayout(ied, 3, blob->GetBufferPointer(), blob->GetBufferSize(), &g_dx.shadow_il);
    blob->Release();
}

bool dx_init(HWND hwnd, int w, int h) {
    g_dx.hwnd   = hwnd;
    g_dx.width  = w;
    g_dx.height = h;
    g_dx.vsync  = false;

    DXGI_SWAP_CHAIN_DESC scd = {};
    scd.BufferCount                        = 2;
    scd.BufferDesc.Width                   = w;
    scd.BufferDesc.Height                  = h;
    scd.BufferDesc.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferDesc.RefreshRate.Numerator   = 60;
    scd.BufferDesc.RefreshRate.Denominator = 1;
    scd.BufferUsage                        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow                       = hwnd;
    scd.SampleDesc.Count                   = 1;
    scd.Windowed                           = TRUE;
    scd.SwapEffect                         = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    UINT flags = 0;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        flags, &fl, 1, D3D11_SDK_VERSION,
        &scd, &g_dx.sc, &g_dx.dev, nullptr, &g_dx.ctx);
    if (FAILED(hr)) { log_error("D3D11CreateDeviceAndSwapChain failed: 0x%08X", hr); return false; }

    IDXGIDevice1* dxgi_dev1 = nullptr;
    hr = g_dx.dev->QueryInterface(__uuidof(IDXGIDevice1), (void**)&dxgi_dev1);
    if (SUCCEEDED(hr) && dxgi_dev1) {
        dxgi_dev1->SetMaximumFrameLatency(1);
        dxgi_dev1->Release();
    }

    ID3D11Texture2D* bb = nullptr;
    g_dx.sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb);
    g_dx.dev->CreateRenderTargetView(bb, nullptr, &g_dx.back_rtv);
    bb->Release();

    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode        = D3D11_FILL_SOLID;
    rd.CullMode        = D3D11_CULL_BACK;
    rd.FrontCounterClockwise = FALSE;
    rd.DepthClipEnable = TRUE;
    g_dx.dev->CreateRasterizerState(&rd, &g_dx.rs_solid);
    rd.CullMode = D3D11_CULL_NONE;
    g_dx.dev->CreateRasterizerState(&rd, &g_dx.rs_cull_none);

    D3D11_DEPTH_STENCIL_DESC dsd = {};
    dsd.DepthEnable    = TRUE;
    dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dsd.DepthFunc      = D3D11_COMPARISON_LESS;
    g_dx.dev->CreateDepthStencilState(&dsd, &g_dx.dss_default);
    dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    g_dx.dev->CreateDepthStencilState(&dsd, &g_dx.dss_depth_read);
    dsd.DepthEnable    = FALSE;
    dsd.DepthFunc      = D3D11_COMPARISON_ALWAYS;
    g_dx.dev->CreateDepthStencilState(&dsd, &g_dx.dss_depth_off);

    D3D11_BLEND_DESC bd = {};
    bd.RenderTarget[0].BlendEnable           = FALSE;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    g_dx.dev->CreateBlendState(&bd, &g_dx.bs_opaque);
    bd.RenderTarget[0].BlendEnable           = TRUE;
    bd.RenderTarget[0].SrcBlend              = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend             = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp               = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha         = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha        = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOpAlpha          = D3D11_BLEND_OP_ADD;
    g_dx.dev->CreateBlendState(&bd, &g_dx.bs_alpha);

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter   = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sd.MaxLOD   = D3D11_FLOAT32_MAX;
    g_dx.dev->CreateSamplerState(&sd, &g_dx.smp_linear);

    D3D11_SAMPLER_DESC shd = {};
    shd.Filter         = D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    shd.AddressU       = D3D11_TEXTURE_ADDRESS_BORDER;
    shd.AddressV       = D3D11_TEXTURE_ADDRESS_BORDER;
    shd.AddressW       = D3D11_TEXTURE_ADDRESS_BORDER;
    shd.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
    shd.BorderColor[0] = shd.BorderColor[1] = shd.BorderColor[2] = shd.BorderColor[3] = 1.0f;
    shd.MaxLOD         = D3D11_FLOAT32_MAX;
    g_dx.dev->CreateSamplerState(&shd, &g_dx.smp_shadow);

    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth      = (sizeof(SceneCBData) + 15) & ~15;
    cbd.Usage          = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    g_dx.dev->CreateBuffer(&cbd, nullptr, &g_dx.scene_cb);

    D3D11_BUFFER_DESC obd = {};
    obd.ByteWidth      = (sizeof(ObjectCBData) + 15) & ~15;
    obd.Usage          = D3D11_USAGE_DYNAMIC;
    obd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    obd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    g_dx.dev->CreateBuffer(&obd, nullptr, &g_dx.object_cb);

    dx_create_scene_rt(w, h);
    dx_create_shadow_map(1024, 1024);
    create_builtin_shadow_shader();

    log_info("DX11 init OK (%dx%d)", w, h);
    return true;
}

void dx_create_scene_rt(int w, int h) {
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (g_dx.ctx) {
        ID3D11RenderTargetView* null_rtv = nullptr;
        ID3D11DepthStencilView* null_dsv = nullptr;
        g_dx.ctx->OMSetRenderTargets(1, &null_rtv, null_dsv);
        ID3D11ShaderResourceView* null_srv = nullptr;
        ID3D11UnorderedAccessView* null_uav = nullptr;
        for (int i = 0; i < 8; i++) {
            g_dx.ctx->VSSetShaderResources(i, 1, &null_srv);
            g_dx.ctx->PSSetShaderResources(i, 1, &null_srv);
            g_dx.ctx->CSSetShaderResources(i, 1, &null_srv);
            g_dx.ctx->CSSetUnorderedAccessViews(i, 1, &null_uav, nullptr);
        }
    }
    safe_release_scene_rt();

    D3D11_TEXTURE2D_DESC td = {};
    td.Width     = w; td.Height = h;
    td.MipLevels = 1; td.ArraySize = 1;
    td.Format    = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage     = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    g_dx.dev->CreateTexture2D(&td, nullptr, &g_dx.scene_tex);
    g_dx.dev->CreateRenderTargetView(g_dx.scene_tex, nullptr, &g_dx.scene_rtv);
    g_dx.dev->CreateShaderResourceView(g_dx.scene_tex, nullptr, &g_dx.scene_srv);
    g_dx.dev->CreateUnorderedAccessView(g_dx.scene_tex, nullptr, &g_dx.scene_uav);

    D3D11_TEXTURE2D_DESC dd = td;
    dd.Format    = DXGI_FORMAT_R24G8_TYPELESS;
    dd.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    g_dx.dev->CreateTexture2D(&dd, nullptr, &g_dx.depth_tex);

    D3D11_DEPTH_STENCIL_VIEW_DESC dsvd = {};
    dsvd.Format        = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dsvd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    g_dx.dev->CreateDepthStencilView(g_dx.depth_tex, &dsvd, &g_dx.depth_dsv);

    D3D11_SHADER_RESOURCE_VIEW_DESC srvd = {};
    srvd.Format                    = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    srvd.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvd.Texture2D.MipLevels       = 1;
    g_dx.dev->CreateShaderResourceView(g_dx.depth_tex, &srvd, &g_dx.depth_srv);
    g_dx.scene_width  = w;
    g_dx.scene_height = h;
    dx_invalidate_scene_history();
}

void dx_destroy_scene_rt() { safe_release_scene_rt(); }

void dx_create_shadow_map(int w, int h) {
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    safe_release_shadow_map();

    D3D11_TEXTURE2D_DESC td = {};
    td.Width     = (UINT)w;
    td.Height    = (UINT)h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format    = DXGI_FORMAT_R24G8_TYPELESS;
    td.SampleDesc.Count = 1;
    td.Usage     = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    HRESULT hr = g_dx.dev->CreateTexture2D(&td, nullptr, &g_dx.shadow_tex);
    if (FAILED(hr) || !g_dx.shadow_tex) {
        log_error("Shadow map texture create failed: 0x%08X", hr);
        return;
    }

    D3D11_DEPTH_STENCIL_VIEW_DESC dsvd = {};
    dsvd.Format        = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dsvd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    hr = g_dx.dev->CreateDepthStencilView(g_dx.shadow_tex, &dsvd, &g_dx.shadow_dsv);
    if (FAILED(hr)) log_error("Shadow map DSV create failed: 0x%08X", hr);

    D3D11_SHADER_RESOURCE_VIEW_DESC srvd = {};
    srvd.Format              = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    srvd.ViewDimension       = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvd.Texture2D.MipLevels = 1;
    hr = g_dx.dev->CreateShaderResourceView(g_dx.shadow_tex, &srvd, &g_dx.shadow_srv);
    if (FAILED(hr)) log_error("Shadow map SRV create failed: 0x%08X", hr);

    g_dx.shadow_width = w;
    g_dx.shadow_height = h;
}

void dx_resize(int w, int h) {
    if (w == g_dx.width && h == g_dx.height) return;
    g_dx.width = w; g_dx.height = h;
    g_dx.ctx->OMSetRenderTargets(0, nullptr, nullptr);
    if (g_dx.back_rtv) { g_dx.back_rtv->Release(); g_dx.back_rtv = nullptr; }
    g_dx.sc->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0);
    ID3D11Texture2D* bb = nullptr;
    g_dx.sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb);
    g_dx.dev->CreateRenderTargetView(bb, nullptr, &g_dx.back_rtv);
    bb->Release();
    dx_create_scene_rt(w, h);
    log_info("Resize %dx%d", w, h);
}

void dx_invalidate_scene_history() {
    memset(&g_dx.scene_cb_data, 0, sizeof(g_dx.scene_cb_data));
    g_dx.scene_cb_history_valid = false;
}

void dx_update_scene_cb(const SceneCBData& d) {
    g_dx.scene_cb_data = d;
    g_dx.scene_cb_history_valid = true;
    D3D11_MAPPED_SUBRESOURCE ms = {};
    g_dx.ctx->Map(g_dx.scene_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms);
    memcpy(ms.pData, &d, sizeof(d));
    g_dx.ctx->Unmap(g_dx.scene_cb, 0);
}

void dx_update_object_cb(const ObjectCBData& d) {
    g_dx.object_cb_data = d;
    if (!g_dx.object_cb) return;
    D3D11_MAPPED_SUBRESOURCE ms = {};
    g_dx.ctx->Map(g_dx.object_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms);
    memcpy(ms.pData, &d, sizeof(d));
    g_dx.ctx->Unmap(g_dx.object_cb, 0);
}

// Bind the off-screen scene surface and shared render state so command
// execution can render a complete frame for the editor viewport.
void dx_begin_scene() {
    float clear[4] = { 0.05f, 0.05f, 0.08f, 1.0f };
    ID3D11ShaderResourceView* null_srv = nullptr;
    for (int i = 0; i < 8; i++) {
        g_dx.ctx->PSSetShaderResources(i, 1, &null_srv);
        g_dx.ctx->CSSetShaderResources(i, 1, &null_srv);
    }
    g_dx.ctx->OMSetRenderTargets(1, &g_dx.scene_rtv, g_dx.depth_dsv);
    g_dx.ctx->ClearRenderTargetView(g_dx.scene_rtv, clear);
    g_dx.ctx->ClearDepthStencilView(g_dx.depth_dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
    D3D11_VIEWPORT vp = { 0, 0, (float)g_dx.scene_width, (float)g_dx.scene_height, 0, 1 };
    g_dx.ctx->RSSetViewports(1, &vp);
    g_dx.ctx->RSSetState(g_dx.rs_solid);
    g_dx.ctx->OMSetDepthStencilState(g_dx.dss_default, 0);
    float bf[4] = {};
    g_dx.ctx->OMSetBlendState(g_dx.bs_opaque, bf, 0xFFFFFFFF);
    g_dx.ctx->VSSetConstantBuffers(0, 1, &g_dx.scene_cb);
    g_dx.ctx->PSSetConstantBuffers(0, 1, &g_dx.scene_cb);
    g_dx.ctx->CSSetConstantBuffers(0, 1, &g_dx.scene_cb);
    g_dx.ctx->VSSetConstantBuffers(2, 1, &g_dx.object_cb);
    g_dx.ctx->PSSetConstantBuffers(2, 1, &g_dx.object_cb);
    g_dx.ctx->PSSetSamplers(0, 1, &g_dx.smp_linear);
    g_dx.ctx->PSSetSamplers(1, 1, &g_dx.smp_shadow);
}

void dx_end_scene() {
    ID3D11RenderTargetView* null_rtv = nullptr;
    g_dx.ctx->OMSetRenderTargets(1, &null_rtv, nullptr);
    ID3D11ShaderResourceView* null_srv = nullptr;
    for (int i = 0; i < 8; i++) g_dx.ctx->PSSetShaderResources(i, 1, &null_srv);
}

void dx_begin_ui() {
    g_dx.ctx->OMSetRenderTargets(1, &g_dx.back_rtv, nullptr);
    float clear[4] = { 0.1f, 0.1f, 0.1f, 1.0f };
    g_dx.ctx->ClearRenderTargetView(g_dx.back_rtv, clear);
}

void dx_shutdown() {
    safe_release_scene_rt();
    safe_release_shadow_map();
    if (g_dx.shadow_il)   g_dx.shadow_il->Release();
    if (g_dx.shadow_vs)   g_dx.shadow_vs->Release();
    if (g_dx.object_cb)   g_dx.object_cb->Release();
    if (g_dx.scene_cb)    g_dx.scene_cb->Release();
    if (g_dx.smp_shadow)  g_dx.smp_shadow->Release();
    if (g_dx.smp_linear)  g_dx.smp_linear->Release();
    if (g_dx.bs_alpha)    g_dx.bs_alpha->Release();
    if (g_dx.bs_opaque)   g_dx.bs_opaque->Release();
    if (g_dx.dss_depth_off)  g_dx.dss_depth_off->Release();
    if (g_dx.dss_depth_read) g_dx.dss_depth_read->Release();
    if (g_dx.dss_default) g_dx.dss_default->Release();
    if (g_dx.rs_cull_none) g_dx.rs_cull_none->Release();
    if (g_dx.rs_solid)    g_dx.rs_solid->Release();
    if (g_dx.back_rtv)    g_dx.back_rtv->Release();
    if (g_dx.sc)          g_dx.sc->Release();
    if (g_dx.ctx)         g_dx.ctx->Release();
    if (g_dx.dev)         g_dx.dev->Release();
}
