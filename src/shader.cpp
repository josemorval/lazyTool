#include "shader.h"
#include "dx11_ctx.h"
#include "log.h"
#include <d3d11shader.h>
#include <stdio.h>
#include <stdlib.h>

// shader.cpp compiles HLSL, builds fallback shaders for error cases, and
// reflects constant-buffer layouts so the editor can expose shader params.

static const char* s_fallback_vs = R"HLSL(
cbuffer SceneCB : register(b0) {
    float4x4 ViewProj;
    float4 TimeVec; float4 LightDir; float4 LightColor; float4 CamPos;
    float4x4 ShadowViewProj;
    float4x4 InvViewProj;
    float4x4 PrevViewProj;
    float4x4 PrevInvViewProj;
    float4x4 PrevShadowViewProj;
};
cbuffer ObjectCB : register(b2) { float4x4 World; };
struct VSIn  { float3 pos : POSITION; float3 nor : NORMAL; float2 uv : TEXCOORD0; };
struct VSOut { float4 pos : SV_POSITION; float3 nor : NORMAL; float2 uv : TEXCOORD0; float3 wpos : TEXCOORD1; };
VSOut VSMain(VSIn v) {
    VSOut o;
    float4 wpos = mul(World, float4(v.pos, 1.0));
    o.wpos = wpos.xyz;
    o.pos  = mul(ViewProj, wpos);
    o.nor  = normalize(mul(World, float4(v.nor, 0.0)).xyz); o.uv = v.uv;
    return o;
}
)HLSL";

static const char* s_fallback_ps = R"HLSL(
cbuffer SceneCB : register(b0) {
    float4x4 ViewProj;
    float4 TimeVec; float4 LightDir; float4 LightColor; float4 CamPos;
    float4x4 ShadowViewProj;
    float4x4 InvViewProj;
    float4x4 PrevViewProj;
    float4x4 PrevInvViewProj;
    float4x4 PrevShadowViewProj;
};
cbuffer ObjectCB : register(b2) { float4x4 World; };
struct PSIn { float4 pos : SV_POSITION; float3 nor : NORMAL; float2 uv : TEXCOORD0; float3 wpos : TEXCOORD1; };
float4 PSMain(PSIn i) : SV_Target {
    float3 n   = normalize(i.nor);
    float3 ld  = normalize(-LightDir.xyz);
    float  ndl = saturate(dot(n, ld));
    float3 col = LightColor.xyz * LightDir.w * ndl + float3(0.03,0.03,0.05);
    return float4(col, 1.0);
}
)HLSL";

static const char* s_fallback_cs = R"HLSL(
RWTexture2D<float4> Output : register(u0);
[numthreads(8,8,1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    Output[id.xy] = float4(0.2, 0.4, 0.8, 1.0);
}
)HLSL";

static bool compile_blob(const char* src, size_t src_len, const char* src_name,
                          const char* entry, const char* profile,
                          ID3DBlob** out_blob, char* err_buf, int err_buf_sz)
{
    ID3DBlob* err_blob = nullptr;
    HRESULT hr = D3DCompile(
        src, src_len, src_name, nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
        entry, profile, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
        out_blob, &err_blob);
    if (FAILED(hr)) {
        if (err_blob && err_buf) {
            strncpy(err_buf, (char*)err_blob->GetBufferPointer(), err_buf_sz - 1);
            err_buf[err_buf_sz - 1] = '\0';
        }
        if (err_blob) err_blob->Release();
        return false;
    }
    if (err_blob) err_blob->Release();
    return true;
}

static bool read_file(const char* path, char** out, size_t* out_sz) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    *out_sz = (size_t)ftell(f);
    rewind(f);
    *out = (char*)malloc(*out_sz + 1);
    fread(*out, 1, *out_sz, f);
    (*out)[*out_sz] = '\0';
    fclose(f);
    return true;
}

static ResType reflected_type_to_res_type(const D3D11_SHADER_TYPE_DESC& td) {
    if (td.Rows != 1 || td.Columns < 1 || td.Columns > 4)
        return RES_NONE;
    if (td.Class != D3D_SVC_SCALAR && td.Class != D3D_SVC_VECTOR)
        return RES_NONE;

    if (td.Type == D3D_SVT_FLOAT) {
        switch (td.Columns) {
        case 1: return RES_FLOAT;
        case 2: return RES_FLOAT2;
        case 3: return RES_FLOAT3;
        case 4: return RES_FLOAT4;
        }
    }

    if (td.Type == D3D_SVT_INT || td.Type == D3D_SVT_UINT || td.Type == D3D_SVT_BOOL) {
        switch (td.Columns) {
        case 1: return RES_INT;
        case 2: return RES_INT2;
        case 3: return RES_INT3;
        default: return RES_NONE;
        }
    }

    return RES_NONE;
}

static ShaderCBVar* reflected_find_var(ShaderCBLayout* cb, const char* name) {
    for (int i = 0; i < cb->var_count; i++)
        if (strcmp(cb->vars[i].name, name) == 0)
            return &cb->vars[i];
    return nullptr;
}

static uint32_t s_shader_cb_layout_version_counter = 0;

uint32_t shader_cb_next_layout_version() {
    // Guard against wrap-around to 0 which means "not synced".
    uint32_t v = ++s_shader_cb_layout_version_counter;
    if (v == 0)
        v = ++s_shader_cb_layout_version_counter;
    return v;
}

static void reflect_command_cbuffer(Resource* r, ID3DBlob* blob) {
    if (!r || !blob) return;

    ID3D11ShaderReflection* refl = nullptr;
    HRESULT hr = D3DReflect(blob->GetBufferPointer(), blob->GetBufferSize(),
                            __uuidof(ID3D11ShaderReflection), (void**)&refl);
    if (FAILED(hr) || !refl)
        return;

    D3D11_SHADER_DESC sd = {};
    refl->GetDesc(&sd);
    for (UINT bi = 0; bi < sd.BoundResources; bi++) {
        D3D11_SHADER_INPUT_BIND_DESC bind = {};
        if (FAILED(refl->GetResourceBindingDesc(bi, &bind)))
            continue;
        if (bind.Type != D3D_SIT_CBUFFER || bind.BindPoint != 1)
            continue;

        ID3D11ShaderReflectionConstantBuffer* src_cb = refl->GetConstantBufferByName(bind.Name);
        if (!src_cb)
            continue;

        D3D11_SHADER_BUFFER_DESC cbd = {};
        if (FAILED(src_cb->GetDesc(&cbd)))
            continue;

        ShaderCBLayout* dst = &r->shader_cb;
        if (!dst->active) {
            dst->active = true;
            strncpy(dst->name, bind.Name ? bind.Name : "UserCB", MAX_NAME - 1);
            dst->name[MAX_NAME - 1] = '\0';
            dst->bind_slot = bind.BindPoint;
            dst->size = cbd.Size;
        } else if (cbd.Size > dst->size) {
            dst->size = cbd.Size;
        }

        for (UINT vi = 0; vi < cbd.Variables; vi++) {
            ID3D11ShaderReflectionVariable* var = src_cb->GetVariableByIndex(vi);
            if (!var) continue;

            D3D11_SHADER_VARIABLE_DESC vd = {};
            if (FAILED(var->GetDesc(&vd)) || !vd.Name)
                continue;

            ID3D11ShaderReflectionType* type = var->GetType();
            if (!type) continue;

            D3D11_SHADER_TYPE_DESC td = {};
            if (FAILED(type->GetDesc(&td)))
                continue;

            ResType rt = reflected_type_to_res_type(td);
            if (rt == RES_NONE)
                continue;

            ShaderCBVar* out = reflected_find_var(dst, vd.Name);
            if (!out) {
                if (dst->var_count >= MAX_SHADER_CB_VARS)
                    continue;
                out = &dst->vars[dst->var_count++];
                memset(out, 0, sizeof(*out));
                strncpy(out->name, vd.Name, MAX_NAME - 1);
                out->name[MAX_NAME - 1] = '\0';
            }
            out->type = rt;
            out->offset = vd.StartOffset;
            out->size = vd.Size;
        }
    }

    // Mark the layout as freshly built so commands re-sync exactly once.
    if (r->shader_cb.active)
        r->shader_cb.layout_version = shader_cb_next_layout_version();

    refl->Release();
}

// Compile the standard VS/PS pair and reflect user-editable parameters from
// the shader's constant-buffer layout.
bool shader_compile_vs_ps(Resource* r, const char* path,
                          const char* vs_entry, const char* ps_entry)
{
    shader_release(r);
    strncpy(r->path, path ? path : "", MAX_PATH_LEN - 1);
    r->path[MAX_PATH_LEN - 1] = '\0';
    r->compiled_ok    = false;
    r->using_fallback = false;
    r->compile_err[0] = '\0';

    const char* vs_src   = s_fallback_vs;
    size_t      vs_len   = strlen(vs_src);
    const char* ps_src   = s_fallback_ps;
    size_t      ps_len   = strlen(ps_src);
    const char* src_name = "fallback";
    char*  file_src = nullptr;
    size_t file_sz  = 0;
    bool   used_file = false;

    if (path && path[0]) {
        if (read_file(path, &file_src, &file_sz)) {
            vs_src = ps_src = file_src;
            vs_len = ps_len = file_sz;
            src_name = path;
            used_file = true;
        } else {
            log_warn("Shader: '%s' not found, using fallback", path);
            r->using_fallback = true;
            snprintf(r->compile_err, sizeof(r->compile_err), "File not found: %s. Using fallback.", path);
        }
    } else {
        r->using_fallback = true;
    }

    ID3DBlob* vs_blob = nullptr;
    ID3DBlob* ps_blob = nullptr;
    char err[512] = {};

    bool ok_vs = compile_blob(vs_src, vs_len, src_name,
                               vs_entry ? vs_entry : "VSMain", "vs_5_0",
                               &vs_blob, err, sizeof(err));
    if (!ok_vs) {
        log_error("VS compile error [%s]: %s", src_name, err);
        r->using_fallback = true;
        snprintf(r->compile_err, sizeof(r->compile_err), "VS compile failed. Using fallback.\n%s", err);
        if (used_file && file_src) { free(file_src); file_src = nullptr; used_file = false; }
        compile_blob(s_fallback_vs, strlen(s_fallback_vs), "fallback_vs", "VSMain", "vs_5_0", &vs_blob, err, sizeof(err));
        compile_blob(s_fallback_ps, strlen(s_fallback_ps), "fallback_ps", "PSMain", "ps_5_0", &ps_blob, err, sizeof(err));
    } else {
        bool ok_ps = compile_blob(ps_src, ps_len, src_name,
                                   ps_entry ? ps_entry : "PSMain", "ps_5_0",
                                   &ps_blob, err, sizeof(err));
        if (!ok_ps) {
            log_error("PS compile error [%s]: %s", src_name, err);
            r->using_fallback = true;
            snprintf(r->compile_err, sizeof(r->compile_err), "PS compile failed. Using fallback.\n%s", err);
            vs_blob->Release(); vs_blob = nullptr;
            compile_blob(s_fallback_vs, strlen(s_fallback_vs), "fallback_vs", "VSMain", "vs_5_0", &vs_blob, err, sizeof(err));
            compile_blob(s_fallback_ps, strlen(s_fallback_ps), "fallback_ps", "PSMain", "ps_5_0", &ps_blob, err, sizeof(err));
        }
    }

    if (used_file && file_src) free(file_src);

    if (!vs_blob || !ps_blob) {
        if (vs_blob) vs_blob->Release();
        if (ps_blob) ps_blob->Release();
        log_error("Shader: total compile failure for '%s'", path ? path : "");
        return false;
    }

    g_dx.dev->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, &r->vs);
    g_dx.dev->CreatePixelShader (ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), nullptr, &r->ps);
    reflect_command_cbuffer(r, vs_blob);
    reflect_command_cbuffer(r, ps_blob);

    D3D11_INPUT_ELEMENT_DESC ied[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    g_dx.dev->CreateInputLayout(ied, 3, vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), &r->il);

    vs_blob->Release();
    ps_blob->Release();

    r->compiled_ok = !r->using_fallback || !(path && path[0]);
    if (r->compiled_ok)
        log_info("Shader compiled OK: %s", path && path[0] ? path : "fallback");
    else
        log_warn("Shader using fallback: %s", path && path[0] ? path : "fallback");
    return true;
}

// Compute shaders use the same pipeline, but with a single entry point.
bool shader_compile_cs(Resource* r, const char* path, const char* cs_entry) {
    shader_release(r);
    strncpy(r->path, path ? path : "", MAX_PATH_LEN - 1);
    r->path[MAX_PATH_LEN - 1] = '\0';
    r->compiled_ok    = false;
    r->using_fallback = false;
    r->compile_err[0] = '\0';

    const char* src  = s_fallback_cs;
    size_t      sz   = strlen(src);
    const char* src_name = "fallback_cs";
    char*  file_src  = nullptr;
    size_t file_sz   = 0;
    bool   used_file = false;

    if (path && path[0]) {
        if (read_file(path, &file_src, &file_sz)) {
            src = file_src; sz = file_sz;
            src_name = path;
            used_file = true;
        } else {
            log_warn("CS: '%s' not found, using fallback", path);
            r->using_fallback = true;
            snprintf(r->compile_err, sizeof(r->compile_err), "File not found: %s. Using fallback.", path);
        }
    } else {
        r->using_fallback = true;
    }

    ID3DBlob* blob = nullptr;
    char err[512] = {};
    if (!compile_blob(src, sz, src_name,
                       cs_entry ? cs_entry : "CSMain", "cs_5_0",
                       &blob, err, sizeof(err))) {
        log_error("CS compile error [%s]: %s", src_name, err);
        r->using_fallback = true;
        snprintf(r->compile_err, sizeof(r->compile_err), "CS compile failed. Using fallback.\n%s", err);
        if (used_file && file_src) { free(file_src); used_file = false; file_src = nullptr; }
        compile_blob(s_fallback_cs, strlen(s_fallback_cs), "fallback_cs", "CSMain", "cs_5_0", &blob, err, sizeof(err));
    }

    if (used_file && file_src) free(file_src);
    if (!blob) return false;

    g_dx.dev->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &r->cs);
    reflect_command_cbuffer(r, blob);
    blob->Release();
    r->compiled_ok = !r->using_fallback || !(path && path[0]);
    if (r->compiled_ok)
        log_info("CS compiled OK: %s", path && path[0] ? path : "fallback");
    else
        log_warn("CS using fallback: %s", path && path[0] ? path : "fallback");
    return true;
}

void shader_release(Resource* r) {
    if (r->vs) { r->vs->Release(); r->vs = nullptr; }
    if (r->ps) { r->ps->Release(); r->ps = nullptr; }
    if (r->cs) { r->cs->Release(); r->cs = nullptr; }
    if (r->il) { r->il->Release(); r->il = nullptr; }
    memset(&r->shader_cb, 0, sizeof(r->shader_cb));
    r->compiled_ok = false;
    r->using_fallback = false;
}
