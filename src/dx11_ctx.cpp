#include "dx11_ctx.h"
#include "build_config.h"
#include "log.h"
#include "project.h"
#include "resources.h"
#include <d3d11sdklayers.h>
#include <d3dcompiler.h>
#include <dxgi1_5.h>
#include <math.h>
#include <stdlib.h>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

// This module wraps the shared Direct3D 11 objects used by the whole tool:
// swap chain, scene targets, depth buffers, samplers, and shared CBs.

DX11Ctx g_dx = {};

static bool dx_check_present_allow_tearing() {
    IDXGIFactory5* factory5 = nullptr;
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory5), (void**)&factory5);
    if (FAILED(hr) || !factory5)
        return false;

    BOOL allow_tearing = FALSE;
    hr = factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,
                                       &allow_tearing, sizeof(allow_tearing));
    factory5->Release();
    return SUCCEEDED(hr) && allow_tearing == TRUE;
}

static const char* s_shadow_vs_src = R"HLSL(
cbuffer SceneCB : register(b0)
{
    float4x4 WorldToView;
    float4x4 ViewToWorld;
    float4x4 ViewToClip;
    float4x4 ClipToView;
    float4x4 PrevWorldToView;
    float4x4 PrevViewToWorld;
    float4x4 PrevViewToClip;
    float4x4 PrevClipToView;
    float4 TimeVec;
    float4 CameraParams;
    float4 LightDir;
    float4 LightColor;
    float4 LightPos;
    float4 LightParams;
    float4 ShadowCascadeSplits;
    float4 ShadowParams;
    float4x4 ShadowWorldToClip;
    float4x4 PrevShadowWorldToClip;
    float4 ShadowCascadeRects[4];
    float4x4 ShadowCascadeWorldToClip[4];
};
cbuffer ObjectCB : register(b1)
{
    float4x4 LocalToWorld;
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
    float4 wpos = mul(LocalToWorld, float4(v.pos, 1.0));
    o.pos = mul(ShadowWorldToClip, wpos);
    return o;
}
)HLSL";

#ifndef LAZYTOOL_PLAYER_ONLY
struct EditorGridCBData {
    float color[4];
    float fade[4];
    float camera[4];
};

struct EditorGridVertex {
    float pos[3];
    float alpha;
};

static ID3D11VertexShader* s_editor_grid_vs = nullptr;
static ID3D11PixelShader*  s_editor_grid_ps = nullptr;
static ID3D11InputLayout*  s_editor_grid_il = nullptr;
static ID3D11Buffer*       s_editor_grid_cb = nullptr;
static ID3D11Buffer*       s_editor_grid_vb = nullptr;
static const int           EDITOR_GRID_MAX_VERTS = 16384;

struct EditorGridCache {
    bool valid;
    float eye[3];
    float fade_start;
    float fade_end;
    float level_alpha[3];
    bool distance_fade;
    int vertex_count;
};

static EditorGridCache s_editor_grid_cache = {};

static Vec3 scene_cb_camera_position_ws() {
    return v3(g_dx.scene_cb_data.view_to_world[12],
              g_dx.scene_cb_data.view_to_world[13],
              g_dx.scene_cb_data.view_to_world[14]);
}
#endif

static SceneCBData   s_uploaded_scene_cb = {};
static ObjectCBData  s_uploaded_object_cb = {};
static ID3D11Buffer* s_uploaded_scene_cb_buffer = nullptr;
static ID3D11Buffer* s_uploaded_object_cb_buffer = nullptr;
static bool          s_uploaded_scene_cb_valid = false;
static bool          s_uploaded_object_cb_valid = false;

#ifndef LAZYTOOL_PLAYER_ONLY
static const char* s_editor_grid_vs_src = R"HLSL(
cbuffer SceneCB : register(b0)
{
    float4x4 WorldToView;
    float4x4 ViewToWorld;
    float4x4 ViewToClip;
    float4x4 ClipToView;
    float4x4 PrevWorldToView;
    float4x4 PrevViewToWorld;
    float4x4 PrevViewToClip;
    float4x4 PrevClipToView;
    float4 TimeVec;
    float4 CameraParams;
    float4 LightDir;
    float4 LightColor;
    float4 LightPos;
    float4 LightParams;
    float4 ShadowCascadeSplits;
    float4 ShadowParams;
    float4x4 ShadowWorldToClip;
    float4x4 PrevShadowWorldToClip;
    float4 ShadowCascadeRects[4];
    float4x4 ShadowCascadeWorldToClip[4];
};

struct VSIn {
    float3 pos   : POSITION;
    float  alpha : TEXCOORD0;
};

struct VSOut {
    float4 pos   : SV_POSITION;
    float  alpha : TEXCOORD0;
    float3 world : TEXCOORD1;
};

VSOut VSMain(VSIn v) {
    VSOut o;
    o.pos = mul(ViewToClip, mul(WorldToView, float4(v.pos, 1.0)));
    o.alpha = v.alpha;
    o.world = v.pos;
    return o;
}
)HLSL";
#endif

#ifndef LAZYTOOL_PLAYER_ONLY
static const char* s_editor_grid_ps_src = R"HLSL(
cbuffer EditorGridCB : register(b1)
{
    float4 GridColor;
    float4 GridFade;
    float4 GridCamera;
};

struct PSIn {
    float4 pos   : SV_POSITION;
    float  alpha : TEXCOORD0;
    float3 world : TEXCOORD1;
};

float4 PSMain(PSIn i) : SV_Target
{
    float alpha = saturate(GridColor.a * i.alpha);
    if (GridFade.z > 0.5)
    {
        float dist_to_camera = distance(i.world, GridCamera.xyz);
        float fade = 1.0 - saturate((dist_to_camera - GridFade.x) / max(GridFade.y - GridFade.x, 1e-4));
        alpha *= fade;
    }
    if (alpha <= 1e-4)
        return 0.0.xxxx;
    return float4(GridColor.rgb, alpha);
}
)HLSL";
#endif

static void safe_release_scene_rt() {
    Resource* scene = res_get(g_builtin_scene_color);
    Resource* depth = res_get(g_builtin_scene_depth);
    if (scene && scene->owns_gpu_backing) res_release_gpu(scene);
    if (depth && depth->owns_gpu_backing) res_release_gpu(depth);
}

static void safe_release_shadow_map() {
    if (g_dx.shadow_preview_srv) { g_dx.shadow_preview_srv->Release(); g_dx.shadow_preview_srv = nullptr; }
    for (int i = 0; i < MAX_SHADOW_CASCADES; i++) {
        if (g_dx.shadow_slice_dsv[i]) {
            g_dx.shadow_slice_dsv[i]->Release();
            g_dx.shadow_slice_dsv[i] = nullptr;
        }
    }
    Resource* shadow = res_get(g_builtin_shadow_map);
    if (shadow && shadow->owns_gpu_backing) res_release_gpu(shadow);
    g_dx.shadow_layers = 0;
}

static void safe_release_info_queue() {
    if (g_dx.info_queue) { g_dx.info_queue->Release(); g_dx.info_queue = nullptr; }
}

static void safe_release_editor_grid() {
#ifndef LAZYTOOL_PLAYER_ONLY
    s_editor_grid_cache = {};
    if (s_editor_grid_vb) { s_editor_grid_vb->Release(); s_editor_grid_vb = nullptr; }
    if (s_editor_grid_cb) { s_editor_grid_cb->Release(); s_editor_grid_cb = nullptr; }
    if (s_editor_grid_il) { s_editor_grid_il->Release(); s_editor_grid_il = nullptr; }
    if (s_editor_grid_ps) { s_editor_grid_ps->Release(); s_editor_grid_ps = nullptr; }
    if (s_editor_grid_vs) { s_editor_grid_vs->Release(); s_editor_grid_vs = nullptr; }
#endif
}


static void dx11_wide_to_utf8(const wchar_t* src, char* dst, int dst_size) {
    if (!dst || dst_size <= 0)
        return;
    dst[0] = 0;
    if (!src)
        return;
    int n = WideCharToMultiByte(CP_UTF8, 0, src, -1, dst, dst_size, nullptr, nullptr);
    if (n <= 0)
        snprintf(dst, (size_t)dst_size, "unknown");
    dst[dst_size - 1] = 0;
}

static IDXGIAdapter1* choose_high_performance_adapter() {
    IDXGIFactory1* factory = nullptr;
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory);
    if (FAILED(hr) || !factory)
        return nullptr;

    IDXGIAdapter1* best_adapter = nullptr;
    DXGI_ADAPTER_DESC1 best_desc = {};
    SIZE_T best_vram = 0;

    for (UINT i = 0;; i++) {
        IDXGIAdapter1* adapter = nullptr;
        if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND)
            break;
        if (!adapter)
            continue;

        DXGI_ADAPTER_DESC1 desc = {};
        adapter->GetDesc1(&desc);
        bool software = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
        if (!software && desc.DedicatedVideoMemory >= best_vram) {
            if (best_adapter)
                best_adapter->Release();
            best_adapter = adapter;
            best_desc = desc;
            best_vram = desc.DedicatedVideoMemory;
        } else {
            adapter->Release();
        }
    }

    if (best_adapter) {
        char name[128] = {};
        dx11_wide_to_utf8(best_desc.Description, name, (int)sizeof(name));
        log_info("DX11 preferred adapter: %s (%llu MB dedicated VRAM)",
            name, (unsigned long long)(best_desc.DedicatedVideoMemory / (1024ull * 1024ull)));
    }

    factory->Release();
    return best_adapter;
}

static void store_active_adapter_info() {
    snprintf(g_dx.adapter_name, sizeof(g_dx.adapter_name), "unknown");
    g_dx.adapter_vendor_id = 0;
    g_dx.adapter_device_id = 0;
    g_dx.adapter_dedicated_vram_mb = 0;

    if (!g_dx.dev)
        return;

    IDXGIDevice* dxgi_device = nullptr;
    HRESULT hr = g_dx.dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgi_device);
    if (FAILED(hr) || !dxgi_device)
        return;

    IDXGIAdapter* adapter = nullptr;
    hr = dxgi_device->GetAdapter(&adapter);
    dxgi_device->Release();
    if (FAILED(hr) || !adapter)
        return;

    DXGI_ADAPTER_DESC desc = {};
    adapter->GetDesc(&desc);
    adapter->Release();

    dx11_wide_to_utf8(desc.Description, g_dx.adapter_name, (int)sizeof(g_dx.adapter_name));
    g_dx.adapter_vendor_id = desc.VendorId;
    g_dx.adapter_device_id = desc.DeviceId;
    g_dx.adapter_dedicated_vram_mb = (unsigned long long)(desc.DedicatedVideoMemory / (1024ull * 1024ull));
    g_dx.adapter_force_high_performance = true;

    log_info("DX11 active adapter: %s vendor=0x%04X device=0x%04X dedicated=%llu MB",
        g_dx.adapter_name,
        g_dx.adapter_vendor_id,
        g_dx.adapter_device_id,
        g_dx.adapter_dedicated_vram_mb);
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

#ifndef LAZYTOOL_PLAYER_ONLY
static bool create_editor_grid_shader() {
    safe_release_editor_grid();

    ID3DBlob* vs_blob = nullptr;
    ID3DBlob* ps_blob = nullptr;
    ID3DBlob* err = nullptr;
    HRESULT hr = D3DCompile(s_editor_grid_vs_src, strlen(s_editor_grid_vs_src), "editor_grid_vs",
        nullptr, nullptr, "VSMain", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vs_blob, &err);
    if (FAILED(hr) || !vs_blob) {
        const char* msg = err ? (const char*)err->GetBufferPointer() : "unknown error";
        log_error("Editor grid VS compile failed: %s", msg);
        if (err) err->Release();
        return false;
    }
    if (err) { err->Release(); err = nullptr; }

    hr = D3DCompile(s_editor_grid_ps_src, strlen(s_editor_grid_ps_src), "editor_grid_ps",
        nullptr, nullptr, "PSMain", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &ps_blob, &err);
    if (FAILED(hr) || !ps_blob) {
        const char* msg = err ? (const char*)err->GetBufferPointer() : "unknown error";
        log_error("Editor grid PS compile failed: %s", msg);
        if (err) err->Release();
        vs_blob->Release();
        return false;
    }
    if (err) { err->Release(); err = nullptr; }

    hr = g_dx.dev->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, &s_editor_grid_vs);
    if (SUCCEEDED(hr))
        hr = g_dx.dev->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), nullptr, &s_editor_grid_ps);
    if (SUCCEEDED(hr)) {
        D3D11_INPUT_ELEMENT_DESC grid_ied[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32_FLOAT,       0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        };
        hr = g_dx.dev->CreateInputLayout(grid_ied, 2,
            vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), &s_editor_grid_il);
    }
    vs_blob->Release();
    ps_blob->Release();
    if (FAILED(hr) || !s_editor_grid_vs || !s_editor_grid_ps || !s_editor_grid_il) {
        log_error("Editor grid shader create failed: 0x%08X", hr);
        safe_release_editor_grid();
        return false;
    }

    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth = (UINT)((sizeof(EditorGridCBData) + 15) & ~15);
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = g_dx.dev->CreateBuffer(&cbd, nullptr, &s_editor_grid_cb);
    if (FAILED(hr) || !s_editor_grid_cb) {
        log_error("Editor grid cbuffer create failed: 0x%08X", hr);
        safe_release_editor_grid();
        return false;
    }

    D3D11_BUFFER_DESC vbd = {};
    vbd.ByteWidth = sizeof(EditorGridVertex) * EDITOR_GRID_MAX_VERTS;
    vbd.Usage = D3D11_USAGE_DYNAMIC;
    vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    vbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = g_dx.dev->CreateBuffer(&vbd, nullptr, &s_editor_grid_vb);
    if (FAILED(hr) || !s_editor_grid_vb) {
        log_error("Editor grid vertex buffer create failed: 0x%08X", hr);
        safe_release_editor_grid();
        return false;
    }

    return true;
}
#endif

bool dx_init(HWND hwnd, int w, int h) {
    g_dx.hwnd   = hwnd;
    g_dx.width  = w;
    g_dx.height = h;

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
    scd.SwapEffect                         = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    g_dx.present_allow_tearing             = dx_check_present_allow_tearing();
    if (g_dx.present_allow_tearing)
        scd.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    UINT flags = 0;
#if LAZYTOOL_ENABLE_D3D11_VALIDATION
    if (g_dx.d3d11_validation)
        flags |= D3D11_CREATE_DEVICE_DEBUG;
    g_dx.d3d11_validation_supported = true;
#else
    g_dx.d3d11_validation = false;
    g_dx.d3d11_validation_supported = false;
#endif

    g_dx.d3d11_validation_active = false;
    safe_release_info_queue();

    IDXGIAdapter1* preferred_adapter = choose_high_performance_adapter();
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        preferred_adapter,
        preferred_adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        flags, &fl, 1, D3D11_SDK_VERSION,
        &scd, &g_dx.sc, &g_dx.dev, nullptr, &g_dx.ctx);
#if LAZYTOOL_ENABLE_D3D11_VALIDATION
    if (FAILED(hr) && g_dx.d3d11_validation) {
        g_dx.d3d11_validation_supported = false;
        log_warn("D3D11 validation requested, but the debug layer is unavailable. Retrying without it.");
        hr = D3D11CreateDeviceAndSwapChain(
            preferred_adapter,
            preferred_adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            0, &fl, 1, D3D11_SDK_VERSION,
            &scd, &g_dx.sc, &g_dx.dev, nullptr, &g_dx.ctx);
    }
#endif
    if (FAILED(hr) && preferred_adapter) {
        log_warn("Preferred adapter failed with 0x%08X. Retrying default hardware adapter.", hr);
        preferred_adapter->Release();
        preferred_adapter = nullptr;
#if LAZYTOOL_ENABLE_D3D11_VALIDATION
        UINT retry_flags = (g_dx.d3d11_validation && g_dx.d3d11_validation_supported) ? flags : 0;
#else
        UINT retry_flags = 0;
#endif
        hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            retry_flags,
            &fl, 1, D3D11_SDK_VERSION,
            &scd, &g_dx.sc, &g_dx.dev, nullptr, &g_dx.ctx);
    }
    if (preferred_adapter)
        preferred_adapter->Release();
    if (FAILED(hr)) { log_error("D3D11CreateDeviceAndSwapChain failed: 0x%08X", hr); return false; }
    g_dx.d3d11_validation_active = g_dx.d3d11_validation && g_dx.d3d11_validation_supported;
    store_active_adapter_info();
    if (g_dx.present_allow_tearing)
        log_info("DXGI present tearing enabled for VSync-off presentation.");

#if LAZYTOOL_ENABLE_D3D11_VALIDATION
    if (g_dx.d3d11_validation_active) {
        HRESULT info_hr = g_dx.dev->QueryInterface(__uuidof(ID3D11InfoQueue), (void**)&g_dx.info_queue);
        if (FAILED(info_hr) || !g_dx.info_queue)
            log_warn("D3D11 validation is active, but InfoQueue is unavailable (0x%08X).", info_hr);
        else
            log_info("D3D11 validation active.");
    }
#endif

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
#ifndef LAZYTOOL_PLAYER_ONLY
    rd.FillMode = D3D11_FILL_WIREFRAME;
    rd.CullMode = D3D11_CULL_BACK;
    g_dx.dev->CreateRasterizerState(&rd, &g_dx.rs_wire_solid);
    rd.CullMode = D3D11_CULL_NONE;
    g_dx.dev->CreateRasterizerState(&rd, &g_dx.rs_wire_cull_none);
#endif

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
    Resource default_dl = {};
    project_apply_default_light(&default_dl);
    dx_create_shadow_map(default_dl.shadow_width, default_dl.shadow_height);
    create_builtin_shadow_shader();
#ifndef LAZYTOOL_PLAYER_ONLY
    if (!create_editor_grid_shader())
        log_warn("Editor grid shader unavailable. Grid overlay disabled.");
#endif

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

    Resource* scene = res_get(g_builtin_scene_color);
    Resource* depth = res_get(g_builtin_scene_depth);
    if (!scene || !depth || !g_dx.dev) {
        log_error("Cannot create scene targets before built-in resources and D3D are initialized.");
        return;
    }

    scene->width = w;
    scene->height = h;
    scene->depth = 1;
    scene->tex_fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
    scene->has_rtv = true;
    scene->has_srv = true;
    scene->has_uav = true;
    scene->has_dsv = false;
    scene->scene_scale_divisor = 1;
    scene->owns_gpu_backing = true;
    scene->runtime_managed = true;

    depth->width = w;
    depth->height = h;
    depth->depth = 1;
    depth->tex_fmt = DXGI_FORMAT_R24G8_TYPELESS;
    depth->has_rtv = false;
    depth->has_srv = true;
    depth->has_uav = false;
    depth->has_dsv = true;
    depth->scene_scale_divisor = 1;
    depth->owns_gpu_backing = true;
    depth->runtime_managed = true;

    D3D11_TEXTURE2D_DESC td = {};
    td.Width     = w; td.Height = h;
    td.MipLevels = 1; td.ArraySize = 1;
    td.Format    = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage     = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    g_dx.dev->CreateTexture2D(&td, nullptr, &scene->tex);
    if (scene->tex) {
        g_dx.dev->CreateRenderTargetView(scene->tex, nullptr, &scene->rtv);
        g_dx.dev->CreateShaderResourceView(scene->tex, nullptr, &scene->srv);
        g_dx.dev->CreateUnorderedAccessView(scene->tex, nullptr, &scene->uav);
    }

    D3D11_TEXTURE2D_DESC dd = td;
    dd.Format    = DXGI_FORMAT_R24G8_TYPELESS;
    dd.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    g_dx.dev->CreateTexture2D(&dd, nullptr, &depth->tex);

    D3D11_DEPTH_STENCIL_VIEW_DESC dsvd = {};
    dsvd.Format        = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dsvd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    if (depth->tex)
        g_dx.dev->CreateDepthStencilView(depth->tex, &dsvd, &depth->dsv);

    D3D11_SHADER_RESOURCE_VIEW_DESC srvd = {};
    srvd.Format                    = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    srvd.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvd.Texture2D.MipLevels       = 1;
    if (depth->tex)
        g_dx.dev->CreateShaderResourceView(depth->tex, &srvd, &depth->srv);
    g_dx.scene_width  = w;
    g_dx.scene_height = h;
    res_sync_size_resource(g_builtin_scene_color);
    res_sync_size_resource(g_builtin_scene_depth);
    dx_invalidate_scene_history();
}

void dx_destroy_scene_rt() { safe_release_scene_rt(); }

void dx_create_shadow_map(int w, int h, int layers) {
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (layers < 1) layers = 1;
    if (layers > MAX_SHADOW_CASCADES) layers = MAX_SHADOW_CASCADES;
    safe_release_shadow_map();
    Resource* shadow = res_get(g_builtin_shadow_map);
    if (!shadow || !g_dx.dev) {
        log_error("Cannot create shadow map before its built-in resource and D3D are initialized.");
        return;
    }
    shadow->width = w;
    shadow->height = h;
    shadow->depth = layers;
    shadow->tex_fmt = DXGI_FORMAT_R24G8_TYPELESS;
    shadow->has_rtv = false;
    shadow->has_srv = true;
    shadow->has_uav = false;
    shadow->has_dsv = true;
    shadow->owns_gpu_backing = true;
    shadow->runtime_managed = true;

    D3D11_TEXTURE2D_DESC td = {};
    td.Width     = (UINT)w;
    td.Height    = (UINT)h;
    td.MipLevels = 1;
    td.ArraySize = (UINT)layers;
    td.Format    = DXGI_FORMAT_R24G8_TYPELESS;
    td.SampleDesc.Count = 1;
    td.Usage     = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    HRESULT hr = g_dx.dev->CreateTexture2D(&td, nullptr, &shadow->tex);
    if (FAILED(hr) || !shadow->tex) {
        log_error("Shadow map texture create failed: 0x%08X", hr);
        return;
    }

    D3D11_DEPTH_STENCIL_VIEW_DESC dsvd = {};
    dsvd.Format        = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dsvd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
    dsvd.Texture2DArray.FirstArraySlice = 0;
    dsvd.Texture2DArray.ArraySize = (UINT)layers;
    dsvd.Texture2DArray.MipSlice = 0;
    hr = g_dx.dev->CreateDepthStencilView(shadow->tex, &dsvd, &shadow->dsv);
    if (FAILED(hr)) log_error("Shadow map DSV create failed: 0x%08X", hr);

    for (int i = 0; i < layers; i++) {
        D3D11_DEPTH_STENCIL_VIEW_DESC slice_dsvd = {};
        slice_dsvd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        slice_dsvd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
        slice_dsvd.Texture2DArray.MipSlice = 0;
        slice_dsvd.Texture2DArray.FirstArraySlice = (UINT)i;
        slice_dsvd.Texture2DArray.ArraySize = 1;
        hr = g_dx.dev->CreateDepthStencilView(shadow->tex, &slice_dsvd, &g_dx.shadow_slice_dsv[i]);
        if (FAILED(hr)) log_error("Shadow map slice DSV create failed: 0x%08X", hr);
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srvd = {};
    srvd.Format              = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    srvd.ViewDimension       = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    srvd.Texture2DArray.MostDetailedMip = 0;
    srvd.Texture2DArray.MipLevels = 1;
    srvd.Texture2DArray.FirstArraySlice = 0;
    srvd.Texture2DArray.ArraySize = (UINT)layers;
    hr = g_dx.dev->CreateShaderResourceView(shadow->tex, &srvd, &shadow->srv);
    if (FAILED(hr)) log_error("Shadow map SRV create failed: 0x%08X", hr);

    if (layers == 1) {
        D3D11_SHADER_RESOURCE_VIEW_DESC preview_srvd = {};
        preview_srvd.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        preview_srvd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        preview_srvd.Texture2D.MipLevels = 1;
        hr = g_dx.dev->CreateShaderResourceView(shadow->tex, &preview_srvd, &g_dx.shadow_preview_srv);
        if (FAILED(hr)) g_dx.shadow_preview_srv = nullptr;
    }

    g_dx.shadow_width = w;
    g_dx.shadow_height = h;
    g_dx.shadow_layers = layers;
    res_sync_size_resource(g_builtin_shadow_map);
}

void dx_resize(int w, int h) {
    if (w == g_dx.width && h == g_dx.height) return;
    g_dx.width = w; g_dx.height = h;
    g_dx.ctx->OMSetRenderTargets(0, nullptr, nullptr);
    if (g_dx.back_rtv) { g_dx.back_rtv->Release(); g_dx.back_rtv = nullptr; }
    UINT resize_flags = g_dx.present_allow_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u;
    g_dx.sc->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, resize_flags);
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
    s_uploaded_scene_cb_valid = false;
    s_uploaded_object_cb_valid = false;
    s_uploaded_scene_cb_buffer = nullptr;
    s_uploaded_object_cb_buffer = nullptr;
}

void dx_update_scene_cb(const SceneCBData& d) {
    g_dx.scene_cb_data = d;
    g_dx.scene_cb_history_valid = true;
    if (!g_dx.scene_cb)
        return;

    bool same_buffer = s_uploaded_scene_cb_buffer == g_dx.scene_cb;
    bool same_bytes = s_uploaded_scene_cb_valid && same_buffer &&
                      memcmp(&s_uploaded_scene_cb, &d, sizeof(d)) == 0;
    if (same_bytes)
        return;

    D3D11_MAPPED_SUBRESOURCE ms = {};
    g_dx.ctx->Map(g_dx.scene_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms);
    memcpy(ms.pData, &d, sizeof(d));
    g_dx.ctx->Unmap(g_dx.scene_cb, 0);

    s_uploaded_scene_cb = d;
    s_uploaded_scene_cb_buffer = g_dx.scene_cb;
    s_uploaded_scene_cb_valid = true;
}

void dx_update_object_cb(const ObjectCBData& d) {
    g_dx.object_cb_data = d;
    if (!g_dx.object_cb) return;

    bool same_buffer = s_uploaded_object_cb_buffer == g_dx.object_cb;
    bool same_bytes = s_uploaded_object_cb_valid && same_buffer &&
                      memcmp(&s_uploaded_object_cb, &d, sizeof(d)) == 0;
    if (same_bytes)
        return;

    D3D11_MAPPED_SUBRESOURCE ms = {};
    g_dx.ctx->Map(g_dx.object_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms);
    memcpy(ms.pData, &d, sizeof(d));
    g_dx.ctx->Unmap(g_dx.object_cb, 0);

    s_uploaded_object_cb = d;
    s_uploaded_object_cb_buffer = g_dx.object_cb;
    s_uploaded_object_cb_valid = true;
}

// Bind the off-screen scene surface and shared render state so command
// execution can render a complete frame for the editor viewport.
void dx_begin_scene() {
    Resource* scene = res_get(g_builtin_scene_color);
    Resource* depth = res_get(g_builtin_scene_depth);
    if (!scene || !depth || !scene->rtv || !depth->dsv)
        return;
    float clear[4] = { 0.05f, 0.05f, 0.08f, 1.0f };
    ID3D11ShaderResourceView* null_srvs[8] = {};
    g_dx.ctx->PSSetShaderResources(0, 8, null_srvs);
    g_dx.ctx->CSSetShaderResources(0, 8, null_srvs);
    g_dx.ctx->OMSetRenderTargets(1, &scene->rtv, depth->dsv);
    g_dx.ctx->ClearRenderTargetView(scene->rtv, clear);
    g_dx.ctx->ClearDepthStencilView(depth->dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
    D3D11_VIEWPORT vp = { 0, 0, (float)g_dx.scene_width, (float)g_dx.scene_height, 0, 1 };
    g_dx.ctx->RSSetViewports(1, &vp);
    g_dx.ctx->RSSetState(g_dx.rs_solid);
    g_dx.ctx->OMSetDepthStencilState(g_dx.dss_default, 0);
    float bf[4] = {};
    g_dx.ctx->OMSetBlendState(g_dx.bs_opaque, bf, 0xFFFFFFFF);
    g_dx.ctx->VSSetConstantBuffers(0, 1, &g_dx.scene_cb);
    g_dx.ctx->PSSetConstantBuffers(0, 1, &g_dx.scene_cb);
    g_dx.ctx->CSSetConstantBuffers(0, 1, &g_dx.scene_cb);
    g_dx.ctx->VSSetConstantBuffers(1, 1, &g_dx.object_cb);
    g_dx.ctx->VSSetConstantBuffers(2, 1, &g_dx.object_cb);
    g_dx.ctx->PSSetConstantBuffers(1, 1, &g_dx.object_cb);
    g_dx.ctx->PSSetConstantBuffers(2, 1, &g_dx.object_cb);
    g_dx.ctx->PSSetSamplers(0, 1, &g_dx.smp_linear);
    g_dx.ctx->PSSetSamplers(1, 1, &g_dx.smp_shadow);
    g_dx.ctx->CSSetSamplers(0, 1, &g_dx.smp_linear);
    g_dx.ctx->CSSetSamplers(1, 1, &g_dx.smp_shadow);
}

void dx_end_scene() {
    ID3D11RenderTargetView* null_rtv = nullptr;
    g_dx.ctx->OMSetRenderTargets(1, &null_rtv, nullptr);
    ID3D11ShaderResourceView* null_srvs[8] = {};
    g_dx.ctx->PSSetShaderResources(0, 8, null_srvs);
}

#ifndef LAZYTOOL_PLAYER_ONLY
static void editor_grid_push_vertex(EditorGridVertex* verts, int* count,
                                    float x, float z, float alpha) {
    if (!verts || !count || *count >= EDITOR_GRID_MAX_VERTS)
        return;
    EditorGridVertex& v = verts[(*count)++];
    v.pos[0] = x;
    v.pos[1] = 0.0f;
    v.pos[2] = z;
    v.alpha = alpha;
}

static void editor_grid_push_line(EditorGridVertex* verts, int* count,
                                  float x0, float z0, float x1, float z1,
                                  float alpha) {
    if (!verts || !count || *count + 2 > EDITOR_GRID_MAX_VERTS || alpha <= 0.001f)
        return;
    editor_grid_push_vertex(verts, count, x0, z0, alpha);
    editor_grid_push_vertex(verts, count, x1, z1, alpha);
}

static void editor_grid_push_level(EditorGridVertex* verts, int* count,
                                   float cell, float alpha, float center_x, float center_z,
                                   float radius) {
    if (cell <= 0.00001f || alpha <= 0.001f)
        return;

    int half_lines = (int)ceilf(radius / cell) + 2;
    const int max_half_lines = 768;
    if (half_lines > max_half_lines)
        return;
    if (half_lines < 2)
        half_lines = 2;

    float base_x = floorf(center_x / cell) * cell;
    float base_z = floorf(center_z / cell) * cell;
    float span = cell * (float)half_lines;
    float min_x = base_x - span;
    float max_x = base_x + span;
    float min_z = base_z - span;
    float max_z = base_z + span;

    for (int i = -half_lines; i <= half_lines; i++) {
        float x = base_x + (float)i * cell;
        float z = base_z + (float)i * cell;
        float axis_boost_x = fabsf(x) < cell * 0.001f ? 1.65f : 1.0f;
        float axis_boost_z = fabsf(z) < cell * 0.001f ? 1.65f : 1.0f;
        editor_grid_push_line(verts, count, x, min_z, x, max_z, alpha * axis_boost_x);
        editor_grid_push_line(verts, count, min_x, z, max_x, z, alpha * axis_boost_z);
    }
}

static int editor_grid_build_vertices(EditorGridVertex* verts) {
    if (!verts)
        return 0;

    Vec3 eye = scene_cb_camera_position_ws();
    float eye_x = eye.x;
    float eye_y = eye.y;
    float eye_z = eye.z;
    float scaled_focus = fmaxf(fabsf(eye_y) * 0.2f, 0.0001f);
    float radius = g_dx.scene_grid_distance_fade ? g_dx.scene_grid_fade_end : 360.0f;
    if (radius < 16.0f)
        radius = 16.0f;
    const float grid_ratio = 3.0f;
    float log_focus = logf(scaled_focus) / logf(grid_ratio);
    float level = floorf(log_focus);
    float transition = log_focus - level;
    float cell0 = powf(grid_ratio, level);
    float cell1 = cell0 * grid_ratio;
    float cell2 = cell1 * grid_ratio;

    float alpha0 = (1.0f - transition) * g_dx.scene_grid_level_alpha[0];
    float alpha1 = (1.0f - transition * 0.64f) * g_dx.scene_grid_level_alpha[1];
    float alpha2 = transition * g_dx.scene_grid_level_alpha[2];

    int count = 0;
    editor_grid_push_level(verts, &count, cell0, alpha0, eye_x, eye_z, radius);
    editor_grid_push_level(verts, &count, cell1, alpha1, eye_x, eye_z, radius);
    editor_grid_push_level(verts, &count, cell2, alpha2, eye_x, eye_z, radius);
    return count;
}

static bool editor_grid_cache_matches() {
    if (!s_editor_grid_cache.valid)
        return false;

    const float eps = 0.0001f;
    Vec3 eye = scene_cb_camera_position_ws();
    if (fabsf(s_editor_grid_cache.eye[0] - eye.x) > eps) return false;
    if (fabsf(s_editor_grid_cache.eye[1] - eye.y) > eps) return false;
    if (fabsf(s_editor_grid_cache.eye[2] - eye.z) > eps) return false;
    if (fabsf(s_editor_grid_cache.fade_start - g_dx.scene_grid_fade_start) > eps) return false;
    if (fabsf(s_editor_grid_cache.fade_end - g_dx.scene_grid_fade_end) > eps) return false;
    if (fabsf(s_editor_grid_cache.level_alpha[0] - g_dx.scene_grid_level_alpha[0]) > eps) return false;
    if (fabsf(s_editor_grid_cache.level_alpha[1] - g_dx.scene_grid_level_alpha[1]) > eps) return false;
    if (fabsf(s_editor_grid_cache.level_alpha[2] - g_dx.scene_grid_level_alpha[2]) > eps) return false;
    if (s_editor_grid_cache.distance_fade != g_dx.scene_grid_distance_fade) return false;
    return true;
}

static void editor_grid_store_cache(int vertex_count) {
    s_editor_grid_cache.valid = true;
    Vec3 eye = scene_cb_camera_position_ws();
    s_editor_grid_cache.eye[0] = eye.x;
    s_editor_grid_cache.eye[1] = eye.y;
    s_editor_grid_cache.eye[2] = eye.z;
    s_editor_grid_cache.fade_start = g_dx.scene_grid_fade_start;
    s_editor_grid_cache.fade_end = g_dx.scene_grid_fade_end;
    s_editor_grid_cache.level_alpha[0] = g_dx.scene_grid_level_alpha[0];
    s_editor_grid_cache.level_alpha[1] = g_dx.scene_grid_level_alpha[1];
    s_editor_grid_cache.level_alpha[2] = g_dx.scene_grid_level_alpha[2];
    s_editor_grid_cache.distance_fade = g_dx.scene_grid_distance_fade;
    s_editor_grid_cache.vertex_count = vertex_count;
}
#endif

void dx_render_scene_grid_overlay() {
#ifdef LAZYTOOL_PLAYER_ONLY
    return;
#else
    Resource* scene = res_get(g_builtin_scene_color);
    if (!g_dx.scene_grid_enabled || !g_dx.ctx || !scene || !scene->rtv ||
        !g_dx.scene_cb || !s_editor_grid_vs || !s_editor_grid_ps || !s_editor_grid_il ||
        !s_editor_grid_cb || !s_editor_grid_vb ||
        g_dx.scene_width <= 0 || g_dx.scene_height <= 0)
        return;

    int vertex_count = s_editor_grid_cache.vertex_count;
    if (!editor_grid_cache_matches()) {
        D3D11_MAPPED_SUBRESOURCE vms = {};
        HRESULT hr = g_dx.ctx->Map(s_editor_grid_vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &vms);
        if (FAILED(hr) || !vms.pData)
            return;
        vertex_count = editor_grid_build_vertices((EditorGridVertex*)vms.pData);
        g_dx.ctx->Unmap(s_editor_grid_vb, 0);
        editor_grid_store_cache(vertex_count);
    }
    if (vertex_count <= 0)
        return;

    D3D11_MAPPED_SUBRESOURCE ms = {};
    HRESULT hr = g_dx.ctx->Map(s_editor_grid_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms);
    if (FAILED(hr) || !ms.pData)
        return;

    EditorGridCBData* cbd = (EditorGridCBData*)ms.pData;
    memcpy(cbd->color, g_dx.scene_grid_color, sizeof(cbd->color));
    cbd->fade[0] = g_dx.scene_grid_fade_start;
    cbd->fade[1] = g_dx.scene_grid_fade_end;
    cbd->fade[2] = g_dx.scene_grid_distance_fade ? 1.0f : 0.0f;
    cbd->fade[3] = 0.0f;
    Vec3 eye = scene_cb_camera_position_ws();
    cbd->camera[0] = eye.x;
    cbd->camera[1] = eye.y;
    cbd->camera[2] = eye.z;
    cbd->camera[3] = 1.0f;
    g_dx.ctx->Unmap(s_editor_grid_cb, 0);

    D3D11_VIEWPORT vp = { 0, 0, (float)g_dx.scene_width, (float)g_dx.scene_height, 0, 1 };
    float blend_factor[4] = {};
    ID3D11RenderTargetView* scene_rtv = scene->rtv;
    ID3D11Buffer* ps_cbs[] = { g_dx.scene_cb, s_editor_grid_cb };
    UINT stride = sizeof(EditorGridVertex);
    UINT offset = 0;

    g_dx.ctx->OMSetRenderTargets(1, &scene_rtv, nullptr);
    g_dx.ctx->RSSetViewports(1, &vp);
    g_dx.ctx->RSSetState(g_dx.rs_cull_none);
    g_dx.ctx->OMSetDepthStencilState(g_dx.dss_depth_off, 0);
    g_dx.ctx->OMSetBlendState(g_dx.bs_alpha, blend_factor, 0xFFFFFFFF);

    g_dx.ctx->IASetInputLayout(s_editor_grid_il);
    g_dx.ctx->IASetVertexBuffers(0, 1, &s_editor_grid_vb, &stride, &offset);
    g_dx.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
    g_dx.ctx->VSSetShader(s_editor_grid_vs, nullptr, 0);
    g_dx.ctx->PSSetShader(s_editor_grid_ps, nullptr, 0);
    g_dx.ctx->GSSetShader(nullptr, nullptr, 0);
    g_dx.ctx->HSSetShader(nullptr, nullptr, 0);
    g_dx.ctx->DSSetShader(nullptr, nullptr, 0);
    g_dx.ctx->VSSetConstantBuffers(0, 1, &g_dx.scene_cb);
    g_dx.ctx->PSSetConstantBuffers(0, 2, ps_cbs);
    g_dx.ctx->Draw(vertex_count, 0);
    ID3D11Buffer* null_vb = nullptr;
    UINT null_stride = 0, null_offset = 0;
    g_dx.ctx->IASetVertexBuffers(0, 1, &null_vb, &null_stride, &null_offset);
    g_dx.ctx->VSSetShader(nullptr, nullptr, 0);
    g_dx.ctx->PSSetShader(nullptr, nullptr, 0);
#endif
}

void dx_begin_ui() {
    g_dx.ctx->OMSetRenderTargets(1, &g_dx.back_rtv, nullptr);
    float clear[4] = { 0.1f, 0.1f, 0.1f, 1.0f };
    g_dx.ctx->ClearRenderTargetView(g_dx.back_rtv, clear);
}

void dx_present_scene_to_backbuffer() {
    Resource* scene = res_get(g_builtin_scene_color);
    if (!g_dx.sc || !g_dx.ctx || !scene || !scene->tex)
        return;

    ID3D11RenderTargetView* null_rtv = nullptr;
    g_dx.ctx->OMSetRenderTargets(1, &null_rtv, nullptr);
    ID3D11ShaderResourceView* null_srv = nullptr;
    ID3D11UnorderedAccessView* null_uav = nullptr;
    for (int i = 0; i < 8; i++) {
        g_dx.ctx->VSSetShaderResources(i, 1, &null_srv);
        g_dx.ctx->PSSetShaderResources(i, 1, &null_srv);
        g_dx.ctx->CSSetShaderResources(i, 1, &null_srv);
        g_dx.ctx->CSSetUnorderedAccessViews(i, 1, &null_uav, nullptr);
    }

    ID3D11Texture2D* bb = nullptr;
    if (SUCCEEDED(g_dx.sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb)) && bb) {
        g_dx.ctx->CopyResource(bb, scene->tex);
        bb->Release();
    }
}

#if LAZYTOOL_ENABLE_D3D11_VALIDATION
void dx_debug_clear_messages() {
    if (g_dx.info_queue)
        g_dx.info_queue->ClearStoredMessages();
}

void dx_debug_log_messages() {
    if (!g_dx.d3d11_validation_active || !g_dx.info_queue)
        return;

    UINT64 message_count = g_dx.info_queue->GetNumStoredMessagesAllowedByRetrievalFilter();
    if (message_count == 0)
        return;

    const UINT64 max_messages_to_log = 32;
    UINT64 emitted = 0;
    UINT64 suppressed = 0;
    for (UINT64 i = 0; i < message_count; i++) {
        SIZE_T msg_len = 0;
        if (FAILED(g_dx.info_queue->GetMessage(i, nullptr, &msg_len)) || msg_len == 0)
            continue;

        D3D11_MESSAGE* msg = (D3D11_MESSAGE*)malloc(msg_len);
        if (!msg) {
            suppressed++;
            continue;
        }

        if (FAILED(g_dx.info_queue->GetMessage(i, msg, &msg_len))) {
            free(msg);
            continue;
        }

        bool should_log = msg->Severity == D3D11_MESSAGE_SEVERITY_WARNING ||
                          msg->Severity == D3D11_MESSAGE_SEVERITY_ERROR ||
                          msg->Severity == D3D11_MESSAGE_SEVERITY_CORRUPTION;
        if (!should_log) {
            free(msg);
            continue;
        }

        if (emitted >= max_messages_to_log) {
            suppressed++;
            free(msg);
            continue;
        }

        const char* severity = "message";
        if (msg->Severity == D3D11_MESSAGE_SEVERITY_WARNING) severity = "warning";
        if (msg->Severity == D3D11_MESSAGE_SEVERITY_ERROR) severity = "error";
        if (msg->Severity == D3D11_MESSAGE_SEVERITY_CORRUPTION) severity = "corruption";

        if (msg->Severity == D3D11_MESSAGE_SEVERITY_WARNING)
            log_warn("D3D11 [%s] %s", severity, msg->pDescription ? msg->pDescription : "(no description)");
        else
            log_error("D3D11 [%s] %s", severity, msg->pDescription ? msg->pDescription : "(no description)");
        emitted++;
        free(msg);
    }

    if (suppressed > 0)
        log_warn("D3D11: %llu additional validation message(s) suppressed this frame.", (unsigned long long)suppressed);

    g_dx.info_queue->ClearStoredMessages();
}

#else
void dx_debug_clear_messages() {}
void dx_debug_log_messages() {}
#endif

void dx_shutdown() {
    safe_release_scene_rt();
    safe_release_shadow_map();
    safe_release_editor_grid();
    safe_release_info_queue();
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
    if (g_dx.rs_wire_cull_none) g_dx.rs_wire_cull_none->Release();
    if (g_dx.rs_wire_solid) g_dx.rs_wire_solid->Release();
    if (g_dx.rs_cull_none) g_dx.rs_cull_none->Release();
    if (g_dx.rs_solid)    g_dx.rs_solid->Release();
    if (g_dx.back_rtv)    g_dx.back_rtv->Release();
    if (g_dx.sc)          g_dx.sc->Release();
    if (g_dx.ctx)         g_dx.ctx->Release();
    if (g_dx.dev)         g_dx.dev->Release();
}
