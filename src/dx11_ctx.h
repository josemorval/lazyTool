#pragma once
#include "types.h"
struct ID3D11InfoQueue;

// Shared Direct3D 11 device state and helper entry points used by the rest of
// the tool. Most modules touch the GPU through this narrow surface.

struct DX11Ctx {
    HWND                     hwnd;
    ID3D11Device*            dev;
    ID3D11DeviceContext*     ctx;
    IDXGISwapChain*          sc;
    ID3D11RenderTargetView*  back_rtv;
    int                      width, height;
    bool                     vsync;
    bool                     d3d11_validation;
    bool                     d3d11_validation_active;
    bool                     d3d11_validation_supported;
    bool                     shader_validation_warnings;
    ID3D11InfoQueue*         info_queue;

    ID3D11Texture2D*            scene_tex;
    ID3D11RenderTargetView*     scene_rtv;
    ID3D11ShaderResourceView*   scene_srv;
    ID3D11UnorderedAccessView*  scene_uav;

    ID3D11Texture2D*            depth_tex;
    ID3D11DepthStencilView*     depth_dsv;
    ID3D11ShaderResourceView*   depth_srv;

    ID3D11Texture2D*            shadow_tex;
    ID3D11DepthStencilView*     shadow_dsv;
    ID3D11ShaderResourceView*   shadow_srv;
    int                         shadow_width, shadow_height;
    ID3D11VertexShader*         shadow_vs;
    ID3D11InputLayout*          shadow_il;

    int                          scene_width, scene_height;
    ID3D11Buffer*               scene_cb;
    ID3D11Buffer*               object_cb;
    SceneCBData                 scene_cb_data;
    ObjectCBData                object_cb_data;
    bool                        scene_cb_history_valid;

    ID3D11RasterizerState*   rs_solid;
    ID3D11RasterizerState*   rs_cull_none;
    ID3D11RasterizerState*   rs_wire_solid;
    ID3D11RasterizerState*   rs_wire_cull_none;
    bool                     scene_wireframe;
    bool                     scene_grid_enabled;
    bool                     scene_orientation_gizmo_enabled;
    float                    scene_grid_color[4];
    ID3D11DepthStencilState* dss_default;
    ID3D11DepthStencilState* dss_depth_read;
    ID3D11DepthStencilState* dss_depth_off;
    ID3D11BlendState*        bs_opaque;
    ID3D11BlendState*        bs_alpha;
    ID3D11SamplerState*      smp_linear;
    ID3D11SamplerState*      smp_shadow;
};

extern DX11Ctx g_dx;

bool dx_init(HWND hwnd, int w, int h);
void dx_resize(int w, int h);
void dx_create_scene_rt(int w, int h);
void dx_destroy_scene_rt();
void dx_create_shadow_map(int w, int h);
void dx_invalidate_scene_history();
void dx_update_scene_cb(const SceneCBData& d);
void dx_update_object_cb(const ObjectCBData& d);
void dx_begin_scene();
void dx_end_scene();
void dx_render_scene_grid_overlay();
void dx_begin_ui();
void dx_present_scene_to_backbuffer();
void dx_debug_log_messages();
void dx_debug_clear_messages();
void dx_shutdown();
