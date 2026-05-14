#include "dx11_ctx.h"
#include "log.h"
#include "project.h"
#include <d3d11sdklayers.h>
#include <d3dcompiler.h>
#include <stdlib.h>
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
    float4 CamDir;
    float4 ShadowCascadeSplits;
    float4 ShadowParams;
    float4 ShadowCascadeRects[4];
    float4x4 ShadowCascadeViewProj[4];
};
cbuffer ObjectCB : register(b1)
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

struct EditorGridCBData {
    float color[4];
    float viewport[4];
};

static ID3D11VertexShader* s_editor_grid_vs = nullptr;
static ID3D11PixelShader*  s_editor_grid_ps = nullptr;
static ID3D11Buffer*       s_editor_grid_cb = nullptr;

static SceneCBData   s_uploaded_scene_cb = {};
static ObjectCBData  s_uploaded_object_cb = {};
static ID3D11Buffer* s_uploaded_scene_cb_buffer = nullptr;
static ID3D11Buffer* s_uploaded_object_cb_buffer = nullptr;
static bool          s_uploaded_scene_cb_valid = false;
static bool          s_uploaded_object_cb_valid = false;

static const char* s_editor_grid_vs_src = R"HLSL(
struct VSOut {
    float4 pos : SV_POSITION;
};

VSOut VSMain(uint vid : SV_VertexID) {
    float2 pos;
    if (vid == 0) pos = float2(-1.0, -1.0);
    else if (vid == 1) pos = float2(-1.0, 3.0);
    else pos = float2(3.0, -1.0);

    VSOut o;
    o.pos = float4(pos, 0.0, 1.0);
    return o;
}
)HLSL";

static const char* s_editor_grid_ps_src = R"HLSL(
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
    float4 CamDir;
    float4 ShadowCascadeSplits;
    float4 ShadowParams;
    float4 ShadowCascadeRects[4];
    float4x4 ShadowCascadeViewProj[4];
};

cbuffer EditorGridCB : register(b1)
{
    float4 GridColor;
    float4 Viewport;
};

Texture2D<float> DepthTex : register(t0);

float2 pixel_to_uv(float2 pixel)
{
    return (pixel + 0.5) / Viewport.xy;
}

float3 reconstruct_world_far(float2 uv)
{
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float4 world = mul(InvViewProj, float4(ndc, 1.0, 1.0));
    return world.xyz / max(abs(world.w), 1e-5);
}

float grid_line_mask(float2 world_xz, float cell_size)
{
    float2 coord = world_xz / max(cell_size, 1e-5);
    float2 grid = abs(frac(coord - 0.5) - 0.5) / max(fwidth(coord), 1e-5);
    return 1.0 - saturate(min(grid.x, grid.y));
}

float4 PSMain(float4 sv_pos : SV_POSITION) : SV_Target
{
    int2 pixel = int2(sv_pos.xy);
    if (pixel.x < 0 || pixel.y < 0 || pixel.x >= (int)Viewport.x || pixel.y >= (int)Viewport.y)
        return 0.0.xxxx;

    float2 uv = pixel_to_uv(pixel);
    float3 eye = CamPos.xyz;
    float3 world_far = reconstruct_world_far(uv);
    float3 ray_dir = normalize(world_far - eye);

    if (abs(ray_dir.y) < 1e-4)
        return 0.0.xxxx;

    float hit_t = -eye.y / ray_dir.y;
    if (hit_t <= 0.0)
        return 0.0.xxxx;

    float3 world_pos = eye + ray_dir * hit_t;
    float4 clip = mul(ViewProj, float4(world_pos, 1.0));
    if (clip.w <= 1e-5)
        return 0.0.xxxx;

    float grid_depth = clip.z / clip.w;
    if (grid_depth < 0.0 || grid_depth > 1.0)
        return 0.0.xxxx;

    float scene_depth = DepthTex.Load(int3(pixel, 0));
    if (scene_depth < 0.99999 && scene_depth + 2e-4 < grid_depth)
        return 0.0.xxxx;

    float focus_dist = abs(eye.y) / max(abs(CamDir.y), 0.15);
    float scaled_focus = max(focus_dist * 0.2, 1e-4);
    float level = floor(log10(scaled_focus));
    float transition = saturate(log10(scaled_focus) - level);
    float cell0 = pow(10.0, level);
    float cell1 = cell0 * 10.0;
    float cell2 = cell1 * 10.0;

    float pair_a = saturate(
        grid_line_mask(world_pos.xz, cell0) * (GridColor.a * 0.32) +
        grid_line_mask(world_pos.xz, cell1) * GridColor.a);
    float pair_b = saturate(
        grid_line_mask(world_pos.xz, cell1) * (GridColor.a * 0.32) +
        grid_line_mask(world_pos.xz, cell2) * GridColor.a);
    float alpha = lerp(pair_a, pair_b, transition);
    alpha *= pow(saturate(abs(ray_dir.y)), 0.35);
    if (alpha <= 1e-4)
        return 0.0.xxxx;

    return float4(GridColor.rgb, saturate(alpha));
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

static void safe_release_info_queue() {
    if (g_dx.info_queue) { g_dx.info_queue->Release(); g_dx.info_queue = nullptr; }
}

static void safe_release_editor_grid() {
    if (s_editor_grid_cb) { s_editor_grid_cb->Release(); s_editor_grid_cb = nullptr; }
    if (s_editor_grid_ps) { s_editor_grid_ps->Release(); s_editor_grid_ps = nullptr; }
    if (s_editor_grid_vs) { s_editor_grid_vs->Release(); s_editor_grid_vs = nullptr; }
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
    vs_blob->Release();
    ps_blob->Release();
    if (FAILED(hr) || !s_editor_grid_vs || !s_editor_grid_ps) {
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

    return true;
}

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
    scd.SwapEffect                         = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    UINT flags = 0;
    if (g_dx.d3d11_validation)
        flags |= D3D11_CREATE_DEVICE_DEBUG;

    g_dx.d3d11_validation_active = false;
    g_dx.d3d11_validation_supported = true;
    safe_release_info_queue();

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        flags, &fl, 1, D3D11_SDK_VERSION,
        &scd, &g_dx.sc, &g_dx.dev, nullptr, &g_dx.ctx);
    if (FAILED(hr) && g_dx.d3d11_validation) {
        g_dx.d3d11_validation_supported = false;
        log_warn("D3D11 validation requested, but the debug layer is unavailable. Retrying without it.");
        hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            0, &fl, 1, D3D11_SDK_VERSION,
            &scd, &g_dx.sc, &g_dx.dev, nullptr, &g_dx.ctx);
    }
    if (FAILED(hr)) { log_error("D3D11CreateDeviceAndSwapChain failed: 0x%08X", hr); return false; }
    g_dx.d3d11_validation_active = g_dx.d3d11_validation && g_dx.d3d11_validation_supported;

    if (g_dx.d3d11_validation_active) {
        HRESULT info_hr = g_dx.dev->QueryInterface(__uuidof(ID3D11InfoQueue), (void**)&g_dx.info_queue);
        if (FAILED(info_hr) || !g_dx.info_queue)
            log_warn("D3D11 validation is active, but InfoQueue is unavailable (0x%08X).", info_hr);
        else
            log_info("D3D11 validation active.");
    }

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
    rd.FillMode = D3D11_FILL_WIREFRAME;
    rd.CullMode = D3D11_CULL_BACK;
    g_dx.dev->CreateRasterizerState(&rd, &g_dx.rs_wire_solid);
    rd.CullMode = D3D11_CULL_NONE;
    g_dx.dev->CreateRasterizerState(&rd, &g_dx.rs_wire_cull_none);

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
    project_apply_default_dirlight(&default_dl);
    dx_create_shadow_map(default_dl.shadow_width, default_dl.shadow_height);
    create_builtin_shadow_shader();
    if (!create_editor_grid_shader())
        log_warn("Editor grid shader unavailable. Grid overlay disabled.");

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
    float clear[4] = { 0.05f, 0.05f, 0.08f, 1.0f };
    ID3D11ShaderResourceView* null_srvs[8] = {};
    g_dx.ctx->PSSetShaderResources(0, 8, null_srvs);
    g_dx.ctx->CSSetShaderResources(0, 8, null_srvs);
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

void dx_render_scene_grid_overlay() {
    if (!g_dx.scene_grid_enabled || !g_dx.ctx || !g_dx.scene_rtv || !g_dx.depth_srv ||
        !g_dx.scene_cb || !s_editor_grid_vs || !s_editor_grid_ps || !s_editor_grid_cb ||
        g_dx.scene_width <= 0 || g_dx.scene_height <= 0)
        return;

    D3D11_MAPPED_SUBRESOURCE ms = {};
    HRESULT hr = g_dx.ctx->Map(s_editor_grid_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms);
    if (FAILED(hr) || !ms.pData)
        return;

    EditorGridCBData* cbd = (EditorGridCBData*)ms.pData;
    memcpy(cbd->color, g_dx.scene_grid_color, sizeof(cbd->color));
    cbd->viewport[0] = (float)g_dx.scene_width;
    cbd->viewport[1] = (float)g_dx.scene_height;
    cbd->viewport[2] = g_dx.scene_width > 0 ? (1.0f / (float)g_dx.scene_width) : 0.0f;
    cbd->viewport[3] = g_dx.scene_height > 0 ? (1.0f / (float)g_dx.scene_height) : 0.0f;
    g_dx.ctx->Unmap(s_editor_grid_cb, 0);

    D3D11_VIEWPORT vp = { 0, 0, (float)g_dx.scene_width, (float)g_dx.scene_height, 0, 1 };
    float blend_factor[4] = {};
    ID3D11RenderTargetView* scene_rtv = g_dx.scene_rtv;
    ID3D11ShaderResourceView* depth_srv = g_dx.depth_srv;
    ID3D11ShaderResourceView* null_srv = nullptr;
    ID3D11Buffer* ps_cbs[] = { g_dx.scene_cb, s_editor_grid_cb };

    g_dx.ctx->OMSetRenderTargets(1, &scene_rtv, nullptr);
    g_dx.ctx->RSSetViewports(1, &vp);
    g_dx.ctx->RSSetState(g_dx.rs_cull_none);
    g_dx.ctx->OMSetDepthStencilState(g_dx.dss_depth_off, 0);
    g_dx.ctx->OMSetBlendState(g_dx.bs_alpha, blend_factor, 0xFFFFFFFF);

    g_dx.ctx->IASetInputLayout(nullptr);
    g_dx.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_dx.ctx->VSSetShader(s_editor_grid_vs, nullptr, 0);
    g_dx.ctx->PSSetShader(s_editor_grid_ps, nullptr, 0);
    g_dx.ctx->GSSetShader(nullptr, nullptr, 0);
    g_dx.ctx->HSSetShader(nullptr, nullptr, 0);
    g_dx.ctx->DSSetShader(nullptr, nullptr, 0);
    g_dx.ctx->PSSetConstantBuffers(0, 2, ps_cbs);
    g_dx.ctx->PSSetShaderResources(0, 1, &depth_srv);
    g_dx.ctx->Draw(3, 0);
    g_dx.ctx->PSSetShaderResources(0, 1, &null_srv);
    g_dx.ctx->VSSetShader(nullptr, nullptr, 0);
    g_dx.ctx->PSSetShader(nullptr, nullptr, 0);
}

void dx_begin_ui() {
    g_dx.ctx->OMSetRenderTargets(1, &g_dx.back_rtv, nullptr);
    float clear[4] = { 0.1f, 0.1f, 0.1f, 1.0f };
    g_dx.ctx->ClearRenderTargetView(g_dx.back_rtv, clear);
}

void dx_present_scene_to_backbuffer() {
    if (!g_dx.sc || !g_dx.ctx || !g_dx.scene_tex)
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
        g_dx.ctx->CopyResource(bb, g_dx.scene_tex);
        bb->Release();
    }
}

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
