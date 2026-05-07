#include "shader.h"
#include "dx11_ctx.h"
#include "log.h"
#include "embedded_pack.h"
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
    float4 CamDir;
    float4 ShadowCascadeSplits;
    float4 ShadowParams;
    float4 ShadowCascadeRects[4];
    float4x4 ShadowCascadeViewProj[4];
};
cbuffer ObjectCB : register(b1) { float4x4 World; };
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
    float4 CamDir;
    float4 ShadowCascadeSplits;
    float4 ShadowParams;
    float4 ShadowCascadeRects[4];
    float4x4 ShadowCascadeViewProj[4];
};
cbuffer ObjectCB : register(b1) { float4x4 World; };
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

static bool shader_path_is_absolute(const char* path) {
    if (!path || !path[0]) return false;
    if (path[0] == '/' || path[0] == '\\') return true;
    return path[1] == ':' && (path[2] == '\\' || path[2] == '/');
}

static void shader_path_dirname(const char* path, char* out, int out_sz) {
    out[0] = '\0';
    if (!path) return;
    const char* slash1 = strrchr(path, '/');
    const char* slash2 = strrchr(path, '\\');
    const char* slash = slash1 > slash2 ? slash1 : slash2;
    if (!slash) return;
    int len = (int)(slash - path);
    if (len >= out_sz) len = out_sz - 1;
    memcpy(out, path, len);
    out[len] = '\0';
}

static void shader_path_join(const char* dir, const char* file, char* out, int out_sz) {
    if (shader_path_is_absolute(file) || !dir || !dir[0]) {
        strncpy(out, file ? file : "", out_sz - 1);
        out[out_sz - 1] = '\0';
        return;
    }
    snprintf(out, out_sz, "%s/%s", dir, file ? file : "");
}

class LtShaderInclude : public ID3DInclude {
public:
    explicit LtShaderInclude(const char* root) {
        strncpy(root_dir, root ? root : "", MAX_PATH_LEN - 1);
        root_dir[MAX_PATH_LEN - 1] = '\0';
    }

    HRESULT STDMETHODCALLTYPE Open(D3D_INCLUDE_TYPE, LPCSTR file_name,
                                   LPCVOID, LPCVOID* out_data, UINT* out_bytes) override {
        if (!file_name || !out_data || !out_bytes)
            return E_INVALIDARG;

        void* data = nullptr;
        size_t size = 0;
        char full[MAX_PATH_LEN] = {};
        shader_path_join(root_dir, file_name, full, MAX_PATH_LEN);
        if (!lt_read_file(full, &data, &size) && !lt_read_file(file_name, &data, &size))
            return E_FAIL;

        *out_data = data;
        *out_bytes = (UINT)size;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Close(LPCVOID data) override {
        lt_free_file((void*)data);
        return S_OK;
    }

private:
    char root_dir[MAX_PATH_LEN] = {};
};

static bool compile_blob(const char* src, size_t src_len, const char* src_name,
                          const char* include_root,
                          const char* entry, const char* profile,
                          ID3DBlob** out_blob, char* err_buf, int err_buf_sz)
{
    ID3DBlob* err_blob = nullptr;
    LtShaderInclude include_handler(include_root);
    HRESULT hr = D3DCompile(
        src, src_len, src_name, nullptr, &include_handler,
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
    void* data = nullptr;
    size_t size = 0;
    if (!lt_read_file(path, &data, &size))
        return false;
    *out = (char*)data;
    *out_sz = size;
    return true;
}

#ifndef LAZYTOOL_NO_SHADER_REFLECTION
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

static uint32_t reflected_input_bind_to_kind(D3D_SHADER_INPUT_TYPE type) {
    switch (type) {
    case D3D_SIT_CBUFFER:
        return SHADER_BIND_CBUFFER;
    case D3D_SIT_TEXTURE:
    case D3D_SIT_TBUFFER:
    case D3D_SIT_STRUCTURED:
    case D3D_SIT_BYTEADDRESS:
        return SHADER_BIND_SRV;
    case D3D_SIT_UAV_RWTYPED:
    case D3D_SIT_UAV_RWSTRUCTURED:
    case D3D_SIT_UAV_RWBYTEADDRESS:
    case D3D_SIT_UAV_APPEND_STRUCTURED:
    case D3D_SIT_UAV_CONSUME_STRUCTURED:
    case D3D_SIT_UAV_RWSTRUCTURED_WITH_COUNTER:
        return SHADER_BIND_UAV;
    case D3D_SIT_SAMPLER:
        return SHADER_BIND_SAMPLER;
    default:
        return SHADER_BIND_NONE;
    }
}

static void reflected_merge_shader_binding(Resource* r, const D3D11_SHADER_INPUT_BIND_DESC& bind,
                                           uint32_t kind, uint32_t stage_mask) {
    if (!r || kind == SHADER_BIND_NONE || !bind.Name)
        return;

    for (int i = 0; i < r->shader_binding_count; i++) {
        ShaderBinding& existing = r->shader_bindings[i];
        if (existing.kind != kind)
            continue;
        if (existing.bind_slot != bind.BindPoint || existing.bind_count != bind.BindCount)
            continue;
        if (strcmp(existing.name, bind.Name) != 0)
            continue;
        existing.stage_mask |= stage_mask;
        return;
    }

    if (r->shader_binding_count >= MAX_SHADER_RESOURCE_BINDINGS)
        return;

    ShaderBinding& out = r->shader_bindings[r->shader_binding_count++];
    memset(&out, 0, sizeof(out));
    strncpy(out.name, bind.Name, MAX_NAME - 1);
    out.name[MAX_NAME - 1] = '\0';
    out.kind = kind;
    out.bind_slot = bind.BindPoint;
    out.bind_count = bind.BindCount;
    out.stage_mask = stage_mask;
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
        if (bind.Type != D3D_SIT_CBUFFER || !bind.Name)
            continue;

        ID3D11ShaderReflectionConstantBuffer* src_cb = refl->GetConstantBufferByName(bind.Name);
        if (!src_cb)
            continue;

        D3D11_SHADER_BUFFER_DESC cbd = {};
        if (FAILED(src_cb->GetDesc(&cbd)))
            continue;

        if (strcmp(bind.Name, "ObjectCB") == 0) {
            r->object_cb_active = true;
            r->object_cb_bind_slot = bind.BindPoint;
            continue;
        }

        if (strcmp(bind.Name, "UserCB") != 0)
            continue;

        ShaderCBLayout* dst = &r->shader_cb;
        if (!dst->active) {
            dst->active = true;
            strncpy(dst->name, bind.Name, MAX_NAME - 1);
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

static void reflect_shader_bindings(Resource* r, ID3DBlob* blob, uint32_t stage_mask) {
    if (!r || !blob || stage_mask == 0)
        return;

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

        uint32_t kind = reflected_input_bind_to_kind(bind.Type);
        if (kind == SHADER_BIND_NONE || kind == SHADER_BIND_SAMPLER || kind == SHADER_BIND_CBUFFER)
            continue;
        reflected_merge_shader_binding(r, bind, kind, stage_mask);
    }

    refl->Release();
}
#else
uint32_t shader_cb_next_layout_version() {
    return 1;
}

static void reflect_command_cbuffer(Resource* r, ID3DBlob*) {
    if (!r)
        return;
    // Ultra player convention: ObjectCB is fixed at b1 and UserCB is disabled.
    r->object_cb_active = true;
    r->object_cb_bind_slot = 1;
}

static void reflect_shader_bindings(Resource*, ID3DBlob*, uint32_t) {
}
#endif

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
    char   include_root[MAX_PATH_LEN] = {};

    if (path && path[0]) {
        if (read_file(path, &file_src, &file_sz)) {
            vs_src = ps_src = file_src;
            vs_len = ps_len = file_sz;
            src_name = path;
            used_file = true;
            shader_path_dirname(path, include_root, MAX_PATH_LEN);
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

    bool ok_vs = compile_blob(vs_src, vs_len, src_name, include_root,
                               vs_entry ? vs_entry : "VSMain", "vs_5_0",
                               &vs_blob, err, sizeof(err));
    if (!ok_vs) {
        log_error("VS compile error [%s]: %s", src_name, err);
        r->using_fallback = true;
        snprintf(r->compile_err, sizeof(r->compile_err), "VS compile failed. Using fallback.\n%s", err);
        if (used_file && file_src) { lt_free_file(file_src); file_src = nullptr; used_file = false; }
        compile_blob(s_fallback_vs, strlen(s_fallback_vs), "fallback_vs", "", "VSMain", "vs_5_0", &vs_blob, err, sizeof(err));
        compile_blob(s_fallback_ps, strlen(s_fallback_ps), "fallback_ps", "", "PSMain", "ps_5_0", &ps_blob, err, sizeof(err));
    } else {
        bool ok_ps = compile_blob(ps_src, ps_len, src_name, include_root,
                                   ps_entry ? ps_entry : "PSMain", "ps_5_0",
                                   &ps_blob, err, sizeof(err));
        if (!ok_ps) {
            log_error("PS compile error [%s]: %s", src_name, err);
            r->using_fallback = true;
            snprintf(r->compile_err, sizeof(r->compile_err), "PS compile failed. Using fallback.\n%s", err);
            vs_blob->Release(); vs_blob = nullptr;
            compile_blob(s_fallback_vs, strlen(s_fallback_vs), "fallback_vs", "", "VSMain", "vs_5_0", &vs_blob, err, sizeof(err));
            compile_blob(s_fallback_ps, strlen(s_fallback_ps), "fallback_ps", "", "PSMain", "ps_5_0", &ps_blob, err, sizeof(err));
        }
    }

    if (used_file && file_src) lt_free_file(file_src);

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
    reflect_shader_bindings(r, vs_blob, SHADER_STAGE_VERTEX);
    reflect_shader_bindings(r, ps_blob, SHADER_STAGE_PIXEL);

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
    char   include_root[MAX_PATH_LEN] = {};

    if (path && path[0]) {
        if (read_file(path, &file_src, &file_sz)) {
            src = file_src; sz = file_sz;
            src_name = path;
            used_file = true;
            shader_path_dirname(path, include_root, MAX_PATH_LEN);
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
    if (!compile_blob(src, sz, src_name, include_root,
                       cs_entry ? cs_entry : "CSMain", "cs_5_0",
                       &blob, err, sizeof(err))) {
        log_error("CS compile error [%s]: %s", src_name, err);
        r->using_fallback = true;
        snprintf(r->compile_err, sizeof(r->compile_err), "CS compile failed. Using fallback.\n%s", err);
        if (used_file && file_src) { lt_free_file(file_src); used_file = false; file_src = nullptr; }
        compile_blob(s_fallback_cs, strlen(s_fallback_cs), "fallback_cs", "", "CSMain", "cs_5_0", &blob, err, sizeof(err));
    }

    if (used_file && file_src) lt_free_file(file_src);
    if (!blob) return false;

    g_dx.dev->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &r->cs);
    reflect_command_cbuffer(r, blob);
    reflect_shader_bindings(r, blob, SHADER_STAGE_COMPUTE);
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
    r->object_cb_active = false;
    r->object_cb_bind_slot = 0;
    memset(r->shader_bindings, 0, sizeof(r->shader_bindings));
    r->shader_binding_count = 0;
    r->compiled_ok = false;
    r->using_fallback = false;
}
