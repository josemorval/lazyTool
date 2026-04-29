#include "resources.h"
#include "user_cb.h"
#include "shader.h"
#include "dx11_ctx.h"
#include "log.h"
#include "stb_image.h"
#include "cgltf.h"
#include <stddef.h>
#include <stdlib.h>

// This module owns all editor/runtime resources: textures, meshes, shaders,
// render targets, and the built-in handles that expose engine state to users.

Resource  g_resources[MAX_RESOURCES] = {};
int       g_resource_count = 0;

ResHandle g_builtin_time        = INVALID_HANDLE;
ResHandle g_builtin_scene_color = INVALID_HANDLE;
ResHandle g_builtin_scene_depth = INVALID_HANDLE;
ResHandle g_builtin_shadow_map  = INVALID_HANDLE;
ResHandle g_builtin_dirlight    = INVALID_HANDLE;

struct Vertex { float pos[3]; float nor[3]; float uv[2]; };

struct GltfPrimitiveRange {
    cgltf_primitive* prim;
    int              start_index;
    int              index_count;
};

struct VertexArray {
    Vertex* items;
    int     count;
    int     capacity;
};

struct IndexArray {
    uint32_t* items;
    int       count;
    int       capacity;
};

struct PrimitiveRangeArray {
    GltfPrimitiveRange* items;
    int                 count;
    int                 capacity;
};

struct GltfMeshBuildContext {
    const char*          resource_name;
    cgltf_data*          data;
    int                  imported_material_count;
    MeshPart*            imported_parts;
    int*                 imported_part_count;
    bool*                part_overflow;
    VertexArray*         verts;
    IndexArray*          indices;
    PrimitiveRangeArray* primitive_cache;
};

static bool res_push_mesh_part(MeshPart* parts, int* part_count, const char* part_name,
                               int start_index, int index_count, int material_index,
                               const float local_transform[16]);

static bool res_grow_pod_array(void** items, int* capacity, int needed, size_t item_size) {
    if (!items || !capacity || needed <= 0)
        return false;
    if (needed <= *capacity)
        return true;

    int new_capacity = *capacity > 0 ? *capacity : 16;
    while (new_capacity < needed) {
        if (new_capacity > 0x3fffffff) {
            new_capacity = needed;
            break;
        }
        new_capacity *= 2;
    }

    void* grown = realloc(*items, item_size * (size_t)new_capacity);
    if (!grown)
        return false;

    *items = grown;
    *capacity = new_capacity;
    return true;
}

static bool res_vertex_array_reserve(VertexArray* arr, int needed) {
    if (!arr)
        return false;
    return res_grow_pod_array((void**)&arr->items, &arr->capacity, needed, sizeof(Vertex));
}

static bool res_index_array_reserve(IndexArray* arr, int needed) {
    if (!arr)
        return false;
    return res_grow_pod_array((void**)&arr->items, &arr->capacity, needed, sizeof(uint32_t));
}

static bool res_primitive_range_array_reserve(PrimitiveRangeArray* arr, int needed) {
    if (!arr)
        return false;
    return res_grow_pod_array((void**)&arr->items, &arr->capacity, needed, sizeof(GltfPrimitiveRange));
}

static void res_vertex_array_free(VertexArray* arr) {
    if (!arr)
        return;
    free(arr->items);
    arr->items = nullptr;
    arr->count = 0;
    arr->capacity = 0;
}

static void res_index_array_free(IndexArray* arr) {
    if (!arr)
        return;
    free(arr->items);
    arr->items = nullptr;
    arr->count = 0;
    arr->capacity = 0;
}

static void res_primitive_range_array_free(PrimitiveRangeArray* arr) {
    if (!arr)
        return;
    free(arr->items);
    arr->items = nullptr;
    arr->count = 0;
    arr->capacity = 0;
}

static void res_mesh_identity(float out[16]) {
    memset(out, 0, sizeof(float) * 16);
    out[0] = 1.0f;
    out[5] = 1.0f;
    out[10] = 1.0f;
    out[15] = 1.0f;
}

static void res_init_mesh_material(MeshMaterial* mat) {
    if (!mat) return;
    memset(mat, 0, sizeof(*mat));
    for (int i = 0; i < MAX_MESH_MATERIAL_TEXTURES; i++)
        mat->textures[i] = INVALID_HANDLE;
}

static void res_init_mesh_part(MeshPart* part) {
    if (!part) return;
    memset(part, 0, sizeof(*part));
    part->material_index = -1;
    part->enabled = true;
    res_mesh_identity(part->local_transform);
}

static void res_reset_mesh_asset(Resource* r) {
    if (!r) return;

    r->mesh_part_count = 0;
    r->mesh_material_count = 0;
    for (int i = 0; i < MAX_MESH_PARTS; i++)
        res_init_mesh_part(&r->mesh_parts[i]);
    for (int i = 0; i < MAX_MESH_MATERIALS; i++)
        res_init_mesh_material(&r->mesh_materials[i]);
}

static void res_set_default_mesh_layout(Resource* r) {
    if (!r || r->vert_count <= 0 || MAX_MESH_PARTS <= 0)
        return;

    res_reset_mesh_asset(r);

    MeshPart* part = &r->mesh_parts[0];
    res_init_mesh_part(part);
    strncpy(part->name, r->name, MAX_NAME - 1);
    part->name[MAX_NAME - 1] = '\0';
    part->start_index = 0;
    part->index_count = r->idx_count > 0 ? r->idx_count : r->vert_count;
    r->mesh_part_count = 1;
}

static Vec3 res_v3_from(const float p[3]) {
    return {p[0], p[1], p[2]};
}

static void res_set_vertex(Vertex* v, Vec3 p, Vec3 n, float u, float vv) {
    v->pos[0] = p.x; v->pos[1] = p.y; v->pos[2] = p.z;
    v->nor[0] = n.x; v->nor[1] = n.y; v->nor[2] = n.z;
    v->uv[0]  = u;   v->uv[1]  = vv;
}

static void res_add_quad_face(Vertex* verts, uint32_t* idx, int* vert_cursor, int* index_cursor,
                              Vec3 a, Vec3 b, Vec3 c, Vec3 d, Vec3 n)
{
    int base = *vert_cursor;
    res_set_vertex(&verts[(*vert_cursor)++], a, n, 0.f, 1.f);
    res_set_vertex(&verts[(*vert_cursor)++], b, n, 1.f, 1.f);
    res_set_vertex(&verts[(*vert_cursor)++], c, n, 1.f, 0.f);
    res_set_vertex(&verts[(*vert_cursor)++], d, n, 0.f, 0.f);
    idx[(*index_cursor)++] = base + 0;
    idx[(*index_cursor)++] = base + 1;
    idx[(*index_cursor)++] = base + 2;
    idx[(*index_cursor)++] = base + 0;
    idx[(*index_cursor)++] = base + 2;
    idx[(*index_cursor)++] = base + 3;
}

static void res_add_oriented_tri(const Vertex* verts, uint32_t* idx, int* idx_count,
                                 uint32_t a, uint32_t b, uint32_t c)
{
    Vec3 pa = res_v3_from(verts[a].pos);
    Vec3 pb = res_v3_from(verts[b].pos);
    Vec3 pc = res_v3_from(verts[c].pos);
    Vec3 n  = v3_cross(v3_sub(pb, pa), v3_sub(pc, pa));
    Vec3 center = v3_scale(v3_add(v3_add(pa, pb), pc), 1.0f / 3.0f);
    if (v3_dot(n, center) < 0.0f) {
        uint32_t tmp = b; b = c; c = tmp;
    }
    idx[(*idx_count)++] = a;
    idx[(*idx_count)++] = b;
    idx[(*idx_count)++] = c;
}

static bool res_upload_mesh(Resource* r, const Vertex* verts, int vert_count,
                            const uint32_t* indices, int idx_count)
{
    if (!r || !verts || vert_count <= 0) return false;

    res_release_gpu(r);
    r->type = RES_MESH;
    r->vert_count = vert_count;
    r->idx_count = idx_count;
    r->vert_stride = sizeof(Vertex);
    res_set_default_mesh_layout(r);

    D3D11_BUFFER_DESC vbd = {};
    vbd.ByteWidth = (UINT)(sizeof(Vertex) * vert_count);
    vbd.Usage     = D3D11_USAGE_IMMUTABLE;
    vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA vsd = { verts, 0, 0 };
    HRESULT hr = g_dx.dev->CreateBuffer(&vbd, &vsd, &r->vb);
    if (FAILED(hr) || !r->vb) {
        log_error("Mesh vertex buffer create failed: %s", r->name);
        return false;
    }

    if (indices && idx_count > 0) {
        D3D11_BUFFER_DESC ibd = {};
        ibd.ByteWidth = (UINT)(sizeof(uint32_t) * idx_count);
        ibd.Usage     = D3D11_USAGE_IMMUTABLE;
        ibd.BindFlags = D3D11_BIND_INDEX_BUFFER;
        D3D11_SUBRESOURCE_DATA isd = { indices, 0, 0 };
        hr = g_dx.dev->CreateBuffer(&ibd, &isd, &r->ib);
        if (FAILED(hr) || !r->ib) {
            log_error("Mesh index buffer create failed: %s", r->name);
            return false;
        }
    }

    return true;
}

static bool res_make_cube_mesh(Resource* r) {
    Vertex verts[24] = {};
    uint32_t idx[36] = {};
    int v = 0, ii = 0;

    res_add_quad_face(verts, idx, &v, &ii, {-1,-1, 1}, { 1,-1, 1}, { 1, 1, 1}, {-1, 1, 1}, { 0, 0, 1});
    res_add_quad_face(verts, idx, &v, &ii, { 1,-1,-1}, {-1,-1,-1}, {-1, 1,-1}, { 1, 1,-1}, { 0, 0,-1});
    res_add_quad_face(verts, idx, &v, &ii, { 1,-1, 1}, { 1,-1,-1}, { 1, 1,-1}, { 1, 1, 1}, { 1, 0, 0});
    res_add_quad_face(verts, idx, &v, &ii, {-1,-1,-1}, {-1,-1, 1}, {-1, 1, 1}, {-1, 1,-1}, {-1, 0, 0});
    res_add_quad_face(verts, idx, &v, &ii, {-1, 1, 1}, { 1, 1, 1}, { 1, 1,-1}, {-1, 1,-1}, { 0, 1, 0});
    res_add_quad_face(verts, idx, &v, &ii, {-1,-1,-1}, { 1,-1,-1}, { 1,-1, 1}, {-1,-1, 1}, { 0,-1, 0});

    return res_upload_mesh(r, verts, 24, idx, 36);
}

static bool res_make_tetrahedron_mesh(Resource* r) {
    Vec3 p[4] = {
        { 1,  1,  1},
        {-1, -1,  1},
        {-1,  1, -1},
        { 1, -1, -1},
    };
    int faces[4][3] = {
        {0, 1, 2}, {0, 3, 1}, {0, 2, 3}, {1, 3, 2},
    };

    Vertex verts[12] = {};
    uint32_t idx[12] = {};
    int v = 0;
    for (int f = 0; f < 4; f++) {
        Vec3 a = p[faces[f][0]];
        Vec3 b = p[faces[f][1]];
        Vec3 c = p[faces[f][2]];
        Vec3 n = v3_norm(v3_cross(v3_sub(b, a), v3_sub(c, a)));
        Vec3 center = v3_scale(v3_add(v3_add(a, b), c), 1.0f / 3.0f);
        if (v3_dot(n, center) < 0.0f) {
            Vec3 tmp = b; b = c; c = tmp;
            n = v3_scale(n, -1.0f);
        }
        res_set_vertex(&verts[v + 0], a, n, 0.5f, 0.f);
        res_set_vertex(&verts[v + 1], b, n, 0.f, 1.f);
        res_set_vertex(&verts[v + 2], c, n, 1.f, 1.f);
        idx[v + 0] = v + 0; idx[v + 1] = v + 1; idx[v + 2] = v + 2;
        v += 3;
    }

    return res_upload_mesh(r, verts, 12, idx, 12);
}

static bool res_make_sphere_mesh(Resource* r) {
    const int rings = 16;
    const int segs  = 32;
    const int vert_count = (rings + 1) * (segs + 1);
    const int max_idx_count = rings * segs * 6;
    Vertex* verts = (Vertex*)malloc(sizeof(Vertex) * vert_count);
    uint32_t* idx = (uint32_t*)malloc(sizeof(uint32_t) * max_idx_count);
    if (!verts || !idx) {
        if (verts) free(verts);
        if (idx) free(idx);
        return false;
    }

    int v = 0;
    const float pi = 3.14159265358979323846f;
    for (int r_i = 0; r_i <= rings; r_i++) {
        float tv = (float)r_i / (float)rings;
        float theta = tv * pi;
        float y = cosf(theta);
        float sr = sinf(theta);
        for (int s = 0; s <= segs; s++) {
            float u = (float)s / (float)segs;
            float phi = u * pi * 2.0f;
            Vec3 n = {sr * cosf(phi), y, sr * sinf(phi)};
            res_set_vertex(&verts[v++], n, n, u, tv);
        }
    }

    int ii = 0;
    for (int r_i = 0; r_i < rings; r_i++) {
        for (int s = 0; s < segs; s++) {
            uint32_t i0 = (uint32_t)(r_i * (segs + 1) + s);
            uint32_t i1 = (uint32_t)((r_i + 1) * (segs + 1) + s);
            uint32_t i2 = i0 + 1;
            uint32_t i3 = i1 + 1;

            if (r_i == 0) {
                res_add_oriented_tri(verts, idx, &ii, i0, i1, i3);
            } else if (r_i == rings - 1) {
                res_add_oriented_tri(verts, idx, &ii, i0, i1, i2);
            } else {
                res_add_oriented_tri(verts, idx, &ii, i0, i1, i2);
                res_add_oriented_tri(verts, idx, &ii, i2, i1, i3);
            }
        }
    }

    bool ok = res_upload_mesh(r, verts, vert_count, idx, ii);
    free(verts);
    free(idx);
    return ok;
}

static bool res_make_fullscreen_triangle_mesh(Resource* r) {
    Vertex verts[3] = {};
    res_set_vertex(&verts[0], v3(-1.0f, -1.0f, 0.0f), v3(0.0f, 0.0f, 1.0f), 0.0f, 1.0f);
    res_set_vertex(&verts[1], v3(-1.0f,  3.0f, 0.0f), v3(0.0f, 0.0f, 1.0f), 0.0f,-1.0f);
    res_set_vertex(&verts[2], v3( 3.0f, -1.0f, 0.0f), v3(0.0f, 0.0f, 1.0f), 2.0f, 1.0f);
    return res_upload_mesh(r, verts, 3, nullptr, 0);
}

const char* res_type_str(ResType t) {
    switch (t) {
    case RES_INT:                 return "int";
    case RES_INT2:                return "int2";
    case RES_INT3:                return "int3";
    case RES_FLOAT:               return "float";
    case RES_FLOAT2:              return "float2";
    case RES_FLOAT3:              return "float3";
    case RES_FLOAT4:              return "float4";
    case RES_TEXTURE2D:           return "Texture2D";
    case RES_RENDER_TEXTURE2D:    return "RenderTexture2D";
    case RES_RENDER_TEXTURE3D:    return "RenderTexture3D";
    case RES_STRUCTURED_BUFFER:   return "StructuredBuffer";
    case RES_MESH:                return "Mesh";
    case RES_SHADER:              return "Shader";
    case RES_BUILTIN_TIME:        return "[time]";
    case RES_BUILTIN_SCENE_COLOR: return "[scene_color]";
    case RES_BUILTIN_SCENE_DEPTH: return "[scene_depth]";
    case RES_BUILTIN_SHADOW_MAP:  return "[shadow_map]";
    case RES_BUILTIN_DIRLIGHT:    return "[dirlight]";
    default:                      return "?";
    }
}

static int res_format_bytes_per_pixel(DXGI_FORMAT fmt) {
    switch (fmt) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R32_UINT:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
        return 4;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return 8;
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
        return 16;
    default:
        return 4;
    }
}

uint64_t res_estimate_gpu_bytes(const Resource& r) {
    if (!r.active)
        return 0;

    switch (r.type) {
    case RES_TEXTURE2D:
        return (uint64_t)(r.width > 0 ? r.width : 0) *
               (uint64_t)(r.height > 0 ? r.height : 0) *
               (uint64_t)res_format_bytes_per_pixel(r.tex_fmt);
    case RES_RENDER_TEXTURE2D:
    case RES_BUILTIN_SCENE_COLOR:
    case RES_BUILTIN_SCENE_DEPTH:
    case RES_BUILTIN_SHADOW_MAP:
        return (uint64_t)(r.width > 0 ? r.width : 0) *
               (uint64_t)(r.height > 0 ? r.height : 0) *
               (uint64_t)res_format_bytes_per_pixel(r.tex_fmt);
    case RES_RENDER_TEXTURE3D:
        return (uint64_t)(r.width > 0 ? r.width : 0) *
               (uint64_t)(r.height > 0 ? r.height : 0) *
               (uint64_t)(r.depth > 0 ? r.depth : 0) *
               (uint64_t)res_format_bytes_per_pixel(r.tex_fmt);
    case RES_STRUCTURED_BUFFER:
        return (uint64_t)(r.elem_size > 0 ? r.elem_size : 0) *
               (uint64_t)(r.elem_count > 0 ? r.elem_count : 0);
    case RES_MESH:
        return (uint64_t)(r.vert_stride > 0 ? r.vert_stride : 0) *
               (uint64_t)(r.vert_count > 0 ? r.vert_count : 0) +
               (uint64_t)(r.idx_count > 0 ? r.idx_count : 0) * sizeof(uint32_t);
    default:
        return 0;
    }
}

uint64_t res_estimate_gpu_total(bool include_builtin) {
    uint64_t total = 0;
    for (int i = 0; i < MAX_RESOURCES; i++) {
        Resource& r = g_resources[i];
        if (!r.active || (!include_builtin && r.is_builtin))
            continue;
        total += res_estimate_gpu_bytes(r);
    }
    return total;
}

void res_init() {
    memset(g_resources, 0, sizeof(g_resources));
    g_resource_count = 0;

    g_builtin_time = res_alloc("time", RES_BUILTIN_TIME);
    g_resources[g_builtin_time - 1].is_builtin = true;

    g_builtin_scene_color = res_alloc("scene_color", RES_BUILTIN_SCENE_COLOR);
    g_resources[g_builtin_scene_color - 1].is_builtin = true;

    g_builtin_scene_depth = res_alloc("scene_depth", RES_BUILTIN_SCENE_DEPTH);
    g_resources[g_builtin_scene_depth - 1].is_builtin = true;

    g_builtin_shadow_map = res_alloc("shadow_map", RES_BUILTIN_SHADOW_MAP);
    g_resources[g_builtin_shadow_map - 1].is_builtin = true;

    g_builtin_dirlight = res_alloc("directional_light", RES_BUILTIN_DIRLIGHT);
    Resource* dl       = res_get(g_builtin_dirlight);
    dl->is_builtin       = true;
    dl->light_dir[0]     = -0.577f;
    dl->light_dir[1]     = -0.577f;
    dl->light_dir[2]     = -0.577f;
    dl->light_pos[0]     = -0.8f;
    dl->light_pos[1]     = 1.2f;
    dl->light_pos[2]     = -0.8f;
    dl->light_target[0]  = 0.0f;
    dl->light_target[1]  = 0.0f;
    dl->light_target[2]  = 0.0f;
    dl->light_color[0]   = 1.0f;
    dl->light_color[1]   = 0.95f;
    dl->light_color[2]   = 0.9f;
    dl->light_intensity  = 1.0f;
    dl->shadow_extent[0] = 2.2f;
    dl->shadow_extent[1] = 2.2f;
    dl->shadow_near      = 0.01f;
    dl->shadow_far       = 4.0f;
    dl->shadow_width     = 1024;
    dl->shadow_height    = 1024;
    dl->shadow_cascade_count = 1;
    dl->shadow_distance = 12.0f;
    dl->shadow_split_lambda = 0.65f;
    for (int i = 0; i < MAX_SHADOW_CASCADES; i++) {
        float split_t = (float)(i + 1) / (float)MAX_SHADOW_CASCADES;
        dl->shadow_cascade_split[i] = dl->shadow_distance * split_t;
        dl->shadow_cascade_extent[i][0] = dl->shadow_extent[0];
        dl->shadow_cascade_extent[i][1] = dl->shadow_extent[1];
        dl->shadow_cascade_near[i] = dl->shadow_near;
        dl->shadow_cascade_far[i] = dl->shadow_far;
    }

    res_sync_size_resource(g_builtin_scene_color);
    res_sync_size_resource(g_builtin_scene_depth);
    res_sync_size_resource(g_builtin_shadow_map);

    log_info("Resource system init, 5 built-ins registered");
}

void res_shutdown() {
    for (int i = 0; i < MAX_RESOURCES; i++)
        if (g_resources[i].active && !g_resources[i].is_builtin) res_release_gpu(&g_resources[i]);
}

ResHandle res_alloc(const char* name, ResType type) {
    for (int i = 0; i < MAX_RESOURCES; i++) {
        if (!g_resources[i].active) {
            memset(&g_resources[i], 0, sizeof(Resource));
            strncpy(g_resources[i].name, name, MAX_NAME - 1);
            g_resources[i].type   = type;
            g_resources[i].active = true;
            g_resources[i].generated_from = INVALID_HANDLE;
            g_resources[i].size_handle = INVALID_HANDLE;
            g_resources[i].mesh_primitive_type = -1;
            res_reset_mesh_asset(&g_resources[i]);
            g_resource_count++;
            return (ResHandle)(i + 1);
        }
    }
    log_error("res_alloc: out of resource slots");
    return INVALID_HANDLE;
}

void res_free(ResHandle h) {
    Resource* r = res_get(h);
    if (!r) return;
    if (r->is_generated && r->generated_from != INVALID_HANDLE) {
        Resource* owner = res_get(r->generated_from);
        if (owner && owner->size_handle == h)
            owner->size_handle = INVALID_HANDLE;
    }
    if (r->size_handle != INVALID_HANDLE) {
        ResHandle size_h = r->size_handle;
        r->size_handle = INVALID_HANDLE;
        res_free(size_h);
    }
    char name[MAX_NAME];
    strncpy(name, r->name, MAX_NAME - 1);
    name[MAX_NAME - 1] = '\0';
    user_cb_detach_resource(h);
    res_release_gpu(r);
    r->active = false;
    g_resource_count--;
    log_info("Resource freed: %s", name);
}

static bool res_has_size_variable(const Resource& r) {
    switch (r.type) {
    case RES_TEXTURE2D:
    case RES_RENDER_TEXTURE2D:
    case RES_RENDER_TEXTURE3D:
    case RES_STRUCTURED_BUFFER:
    case RES_BUILTIN_SCENE_COLOR:
    case RES_BUILTIN_SCENE_DEPTH:
    case RES_BUILTIN_SHADOW_MAP:
        return true;
    default:
        return false;
    }
}

static ResType res_size_type_for(const Resource& r) {
    if (r.type == RES_RENDER_TEXTURE3D)
        return RES_INT3;
    if (r.type == RES_STRUCTURED_BUFFER)
        return RES_INT;
    return RES_INT2;
}

static void res_size_name_for(const Resource& r, char* out, int out_sz) {
    if (r.type == RES_STRUCTURED_BUFFER)
        snprintf(out, out_sz, "%s.count", r.name);
    else
        snprintf(out, out_sz, "%s.size", r.name);
    out[out_sz - 1] = '\0';
}

// Every size-bearing resource exposes a generated dimensions/count variable so
// compute dispatches and shader params can stay data-driven instead of
// hardcoding scene-dependent numbers in the project file.
void res_sync_size_resource(ResHandle h) {
    Resource* r = res_get(h);
    if (!r || r->is_generated)
        return;

    if (!res_has_size_variable(*r)) {
        if (r->size_handle != INVALID_HANDLE) {
            ResHandle size_h = r->size_handle;
            r->size_handle = INVALID_HANDLE;
            res_free(size_h);
        }
        return;
    }

    char size_name[MAX_NAME] = {};
    res_size_name_for(*r, size_name, MAX_NAME);
    ResType size_type = res_size_type_for(*r);

    Resource* size_res = res_get(r->size_handle);
    if (!size_res || size_res->type != size_type) {
        if (r->size_handle != INVALID_HANDLE) {
            ResHandle old_h = r->size_handle;
            r->size_handle = INVALID_HANDLE;
            res_free(old_h);
        }
        r->size_handle = res_alloc(size_name, size_type);
        size_res = res_get(r->size_handle);
        if (!size_res)
            return;
        size_res->is_generated = true;
        size_res->generated_from = h;
    }

    strncpy(size_res->name, size_name, MAX_NAME - 1);
    size_res->name[MAX_NAME - 1] = '\0';
    size_res->type = size_type;
    if (r->type == RES_STRUCTURED_BUFFER) {
        size_res->ival[0] = r->elem_count > 0 ? r->elem_count : 1;
        size_res->ival[1] = 1;
        size_res->ival[2] = 1;
    } else {
        size_res->ival[0] = r->width > 0 ? r->width : 1;
        size_res->ival[1] = r->height > 0 ? r->height : 1;
        size_res->ival[2] = r->depth > 0 ? r->depth : 1;
    }
}

Resource* res_get(ResHandle h) {
    if (h == INVALID_HANDLE || h > MAX_RESOURCES) return nullptr;
    Resource* r = &g_resources[h - 1];
    return r->active ? r : nullptr;
}

ResHandle res_find_by_name(const char* name) {
    for (int i = 0; i < MAX_RESOURCES; i++)
        if (g_resources[i].active && strcmp(g_resources[i].name, name) == 0)
            return (ResHandle)(i + 1);
    return INVALID_HANDLE;
}

void res_release_gpu(Resource* r) {
    if (r->srv) { r->srv->Release(); r->srv = nullptr; }
    if (r->rtv) { r->rtv->Release(); r->rtv = nullptr; }
    if (r->uav) { r->uav->Release(); r->uav = nullptr; }
    if (r->dsv) { r->dsv->Release(); r->dsv = nullptr; }
    if (r->tex) { r->tex->Release(); r->tex = nullptr; }
    if (r->tex3d) { r->tex3d->Release(); r->tex3d = nullptr; }
    if (r->buf) { r->buf->Release(); r->buf = nullptr; }
    if (r->vb)  { r->vb->Release();  r->vb  = nullptr; }
    if (r->ib)  { r->ib->Release();  r->ib  = nullptr; }
    shader_release(r);
}

static bool res_is_depth_format(DXGI_FORMAT fmt) {
    return fmt == DXGI_FORMAT_R24G8_TYPELESS ||
           fmt == DXGI_FORMAT_D24_UNORM_S8_UINT ||
           fmt == DXGI_FORMAT_R32_TYPELESS ||
           fmt == DXGI_FORMAT_D32_FLOAT;
}

static DXGI_FORMAT res_tex_storage_format(DXGI_FORMAT fmt) {
    switch (fmt) {
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R24G8_TYPELESS: return DXGI_FORMAT_R24G8_TYPELESS;
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_TYPELESS:   return DXGI_FORMAT_R32_TYPELESS;
    default:                         return fmt;
    }
}

static DXGI_FORMAT res_tex_srv_format(DXGI_FORMAT fmt) {
    switch (fmt) {
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R24G8_TYPELESS: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_TYPELESS:   return DXGI_FORMAT_R32_FLOAT;
    default:                         return fmt;
    }
}

static DXGI_FORMAT res_tex_dsv_format(DXGI_FORMAT fmt) {
    switch (fmt) {
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_D32_FLOAT;
    default:                       return DXGI_FORMAT_D24_UNORM_S8_UINT;
    }
}

static bool res_is_uint_format(DXGI_FORMAT fmt) {
    return fmt == DXGI_FORMAT_R32_UINT;
}

static int res_resolve_scene_scaled_dim(int scene_dim, int fallback_dim, int divisor) {
    int dim = scene_dim > 0 ? scene_dim : fallback_dim;
    if (dim < 1) dim = 1;
    if (divisor > 1)
        dim = (dim + divisor - 1) / divisor;
    return dim > 0 ? dim : 1;
}

static void res_resolve_render_texture_size(int in_w, int in_h, int scene_scale_divisor,
                                            int* out_w, int* out_h) {
    int w = in_w;
    int h = in_h;
    if (scene_scale_divisor > 0) {
        w = res_resolve_scene_scaled_dim(g_dx.scene_width, in_w, scene_scale_divisor);
        h = res_resolve_scene_scaled_dim(g_dx.scene_height, in_h, scene_scale_divisor);
    }
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
}

static void res_clear_new_texture(Resource* r) {
    if (!r || !g_dx.ctx) return;

    if (r->uav) {
        if (res_is_uint_format(r->tex_fmt)) {
            UINT zero[4] = {};
            g_dx.ctx->ClearUnorderedAccessViewUint(r->uav, zero);
        } else {
            float zero[4] = {};
            g_dx.ctx->ClearUnorderedAccessViewFloat(r->uav, zero);
        }
    } else if (r->rtv) {
        float zero[4] = {};
        g_dx.ctx->ClearRenderTargetView(r->rtv, zero);
    }
}

bool res_recreate_render_texture(ResHandle h, int w, int hgt, DXGI_FORMAT fmt,
                                 bool want_rtv, bool want_srv, bool want_uav, bool want_dsv,
                                 int scene_scale_divisor)
{
    Resource* r = res_get(h);
    if (!r || r->type != RES_RENDER_TEXTURE2D) return false;

    if (scene_scale_divisor < 0)
        scene_scale_divisor = r->scene_scale_divisor;

    int actual_w = 1;
    int actual_h = 1;
    res_resolve_render_texture_size(w, hgt, scene_scale_divisor, &actual_w, &actual_h);

    bool is_depth = res_is_depth_format(fmt);
    if (is_depth) {
        want_rtv = false;
        want_uav = false;
        want_dsv = true;
    } else {
        want_dsv = false;
    }

    res_release_gpu(r);

    r->width   = actual_w;
    r->height  = actual_h;
    r->depth   = 1;
    r->tex_fmt = fmt;
    r->has_rtv = want_rtv;
    r->has_srv = want_srv;
    r->has_uav = want_uav;
    r->has_dsv = want_dsv;
    r->scene_scale_divisor = scene_scale_divisor;

    D3D11_TEXTURE2D_DESC td = {};
    td.Width     = (UINT)actual_w;
    td.Height    = (UINT)actual_h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format    = res_tex_storage_format(fmt);
    td.SampleDesc.Count = 1;
    td.Usage     = D3D11_USAGE_DEFAULT;
    if (want_rtv) td.BindFlags |= D3D11_BIND_RENDER_TARGET;
    if (want_srv) td.BindFlags |= D3D11_BIND_SHADER_RESOURCE;
    if (want_uav) td.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
    if (want_dsv) td.BindFlags |= D3D11_BIND_DEPTH_STENCIL;

    HRESULT hr = g_dx.dev->CreateTexture2D(&td, nullptr, &r->tex);
    if (FAILED(hr) || !r->tex) {
        log_error("RenderTexture2D create failed: %s (fmt %d, %dx%d)", r->name, (int)fmt, actual_w, actual_h);
        return false;
    }

    if (want_srv) {
        D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.ViewDimension       = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        sd.Format              = res_tex_srv_format(fmt);
        hr = g_dx.dev->CreateShaderResourceView(r->tex, &sd, &r->srv);
        if (FAILED(hr)) log_error("RenderTexture2D SRV create failed: %s", r->name);
    }
    if (want_rtv) {
        hr = g_dx.dev->CreateRenderTargetView(r->tex, nullptr, &r->rtv);
        if (FAILED(hr)) log_error("RenderTexture2D RTV create failed: %s", r->name);
    }
    if (want_uav) {
        hr = g_dx.dev->CreateUnorderedAccessView(r->tex, nullptr, &r->uav);
        if (FAILED(hr)) log_error("RenderTexture2D UAV create failed: %s", r->name);
    }
    if (want_dsv) {
        D3D11_DEPTH_STENCIL_VIEW_DESC dd = {};
        dd.Format        = res_tex_dsv_format(fmt);
        dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        hr = g_dx.dev->CreateDepthStencilView(r->tex, &dd, &r->dsv);
        if (FAILED(hr)) log_error("RenderTexture2D DSV create failed: %s", r->name);
    }

    res_clear_new_texture(r);
    res_sync_size_resource(h);
    return true;
}

ResHandle res_create_render_texture(const char* name, int w, int h, DXGI_FORMAT fmt,
                                     bool want_rtv, bool want_srv, bool want_uav, bool want_dsv,
                                     int scene_scale_divisor)
{
    ResHandle handle = res_alloc(name, RES_RENDER_TEXTURE2D);
    if (handle == INVALID_HANDLE) return INVALID_HANDLE;
    if (!res_recreate_render_texture(handle, w, h, fmt, want_rtv, want_srv, want_uav, want_dsv,
                                     scene_scale_divisor)) {
        res_free(handle);
        return INVALID_HANDLE;
    }

    Resource* r = res_get(handle);
    int actual_w = r ? r->width : w;
    int actual_h = r ? r->height : h;
    log_info("RenderTexture2D created: %s (%dx%d)%s", name, actual_w, actual_h,
             scene_scale_divisor > 0 ? " scene-scaled" : "");
    return handle;
}

bool res_recreate_render_texture3d(ResHandle h, int w, int hgt, int d, DXGI_FORMAT fmt,
                                   bool want_rtv, bool want_srv, bool want_uav)
{
    Resource* r = res_get(h);
    if (!r || r->type != RES_RENDER_TEXTURE3D) return false;

    if (w < 1) w = 1;
    if (hgt < 1) hgt = 1;
    if (d < 1) d = 1;

    if (res_is_depth_format(fmt)) {
        log_error("RenderTexture3D does not support depth formats: %s", r->name);
        return false;
    }

    res_release_gpu(r);

    r->width   = w;
    r->height  = hgt;
    r->depth   = d;
    r->tex_fmt = fmt;
    r->has_rtv = want_rtv;
    r->has_srv = want_srv;
    r->has_uav = want_uav;
    r->has_dsv = false;

    D3D11_TEXTURE3D_DESC td = {};
    td.Width     = (UINT)w;
    td.Height    = (UINT)hgt;
    td.Depth     = (UINT)d;
    td.MipLevels = 1;
    td.Format    = fmt;
    td.Usage     = D3D11_USAGE_DEFAULT;
    if (want_rtv) td.BindFlags |= D3D11_BIND_RENDER_TARGET;
    if (want_srv) td.BindFlags |= D3D11_BIND_SHADER_RESOURCE;
    if (want_uav) td.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;

    HRESULT hr = g_dx.dev->CreateTexture3D(&td, nullptr, &r->tex3d);
    if (FAILED(hr) || !r->tex3d) {
        log_error("RenderTexture3D create failed: %s (fmt %d, %dx%dx%d)", r->name, (int)fmt, w, hgt, d);
        return false;
    }

    if (want_srv) {
        D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE3D;
        sd.Format                    = fmt;
        sd.Texture3D.MostDetailedMip = 0;
        sd.Texture3D.MipLevels       = 1;
        hr = g_dx.dev->CreateShaderResourceView(r->tex3d, &sd, &r->srv);
        if (FAILED(hr)) log_error("RenderTexture3D SRV create failed: %s", r->name);
    }
    if (want_rtv) {
        D3D11_RENDER_TARGET_VIEW_DESC rd = {};
        rd.ViewDimension         = D3D11_RTV_DIMENSION_TEXTURE3D;
        rd.Format                = fmt;
        rd.Texture3D.MipSlice    = 0;
        rd.Texture3D.FirstWSlice = 0;
        rd.Texture3D.WSize       = (UINT)d;
        hr = g_dx.dev->CreateRenderTargetView(r->tex3d, &rd, &r->rtv);
        if (FAILED(hr)) log_error("RenderTexture3D RTV create failed: %s", r->name);
    }
    if (want_uav) {
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
        ud.ViewDimension         = D3D11_UAV_DIMENSION_TEXTURE3D;
        ud.Format                = fmt;
        ud.Texture3D.MipSlice    = 0;
        ud.Texture3D.FirstWSlice = 0;
        ud.Texture3D.WSize       = (UINT)d;
        hr = g_dx.dev->CreateUnorderedAccessView(r->tex3d, &ud, &r->uav);
        if (FAILED(hr)) log_error("RenderTexture3D UAV create failed: %s", r->name);
    }

    res_clear_new_texture(r);
    res_sync_size_resource(h);
    return true;
}

ResHandle res_create_render_texture3d(const char* name, int w, int h, int d, DXGI_FORMAT fmt,
                                       bool want_rtv, bool want_srv, bool want_uav)
{
    ResHandle handle = res_alloc(name, RES_RENDER_TEXTURE3D);
    if (handle == INVALID_HANDLE) return INVALID_HANDLE;
    if (!res_recreate_render_texture3d(handle, w, h, d, fmt, want_rtv, want_srv, want_uav)) {
        res_free(handle);
        return INVALID_HANDLE;
    }

    log_info("RenderTexture3D created: %s (%dx%dx%d)", name, w, h, d);
    return handle;
}

bool res_recreate_structured_buffer(ResHandle h, int elem_size, int elem_count,
                                    bool want_srv, bool want_uav)
{
    Resource* r = res_get(h);
    if (!r || r->type != RES_STRUCTURED_BUFFER) return false;

    if (elem_size < 1) elem_size = 1;
    if (elem_count < 1) elem_count = 1;

    res_release_gpu(r);

    r->elem_size  = elem_size;
    r->elem_count = elem_count;
    r->has_srv    = want_srv;
    r->has_uav    = want_uav;

    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth           = (UINT)(elem_size * elem_count);
    bd.Usage               = D3D11_USAGE_DEFAULT;
    bd.MiscFlags           = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    bd.StructureByteStride = (UINT)elem_size;
    if (want_srv) bd.BindFlags |= D3D11_BIND_SHADER_RESOURCE;
    if (want_uav) bd.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;

    void* zero_data = calloc(1, bd.ByteWidth);
    D3D11_SUBRESOURCE_DATA init = {};
    init.pSysMem = zero_data;

    HRESULT hr = g_dx.dev->CreateBuffer(&bd, zero_data ? &init : nullptr, &r->buf);
    if (zero_data) free(zero_data);
    if (FAILED(hr) || !r->buf) {
        log_error("StructuredBuffer create failed: %s (%d x %d bytes)", r->name, elem_count, elem_size);
        return false;
    }

    if (want_srv) {
        D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format             = DXGI_FORMAT_UNKNOWN;
        sd.ViewDimension      = D3D11_SRV_DIMENSION_BUFFER;
        sd.Buffer.NumElements = (UINT)elem_count;
        hr = g_dx.dev->CreateShaderResourceView(r->buf, &sd, &r->srv);
        if (FAILED(hr)) log_error("StructuredBuffer SRV create failed: %s", r->name);
    }
    if (want_uav) {
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
        ud.Format             = DXGI_FORMAT_UNKNOWN;
        ud.ViewDimension      = D3D11_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = (UINT)elem_count;
        hr = g_dx.dev->CreateUnorderedAccessView(r->buf, &ud, &r->uav);
        if (FAILED(hr)) log_error("StructuredBuffer UAV create failed: %s", r->name);
    }

    res_sync_size_resource(h);
    return true;
}

void res_reset_transient_gpu_resources() {
    if (g_dx.ctx) {
        ID3D11RenderTargetView* null_rtvs[8] = {};
        ID3D11ShaderResourceView* null_srvs[16] = {};
        ID3D11UnorderedAccessView* null_uavs[8] = {};
        g_dx.ctx->OMSetRenderTargets(8, null_rtvs, nullptr);
        g_dx.ctx->VSSetShaderResources(0, 16, null_srvs);
        g_dx.ctx->PSSetShaderResources(0, 16, null_srvs);
        g_dx.ctx->CSSetShaderResources(0, 16, null_srvs);
        g_dx.ctx->CSSetUnorderedAccessViews(0, 8, null_uavs, nullptr);
    }

    int reset_count = 0;
    for (int i = 0; i < MAX_RESOURCES; i++) {
        Resource& r = g_resources[i];
        if (!r.active || r.is_builtin)
            continue;

        ResHandle h = (ResHandle)(i + 1);
        switch (r.type) {
        case RES_RENDER_TEXTURE2D: {
            int w = r.width;
            int hgt = r.height;
            DXGI_FORMAT fmt = r.tex_fmt;
            bool rtv = r.has_rtv;
            bool srv = r.has_srv;
            bool uav = r.has_uav;
            bool dsv = r.has_dsv;
            if (res_recreate_render_texture(h, w, hgt, fmt, rtv, srv, uav, dsv, r.scene_scale_divisor))
                reset_count++;
            break;
        }
        case RES_RENDER_TEXTURE3D: {
            int w = r.width;
            int hgt = r.height;
            int d = r.depth;
            DXGI_FORMAT fmt = r.tex_fmt;
            bool rtv = r.has_rtv;
            bool srv = r.has_srv;
            bool uav = r.has_uav;
            if (res_recreate_render_texture3d(h, w, hgt, d, fmt, rtv, srv, uav))
                reset_count++;
            break;
        }
        case RES_STRUCTURED_BUFFER: {
            int elem_size = r.elem_size;
            int elem_count = r.elem_count;
            bool srv = r.has_srv;
            bool uav = r.has_uav;
            if (res_recreate_structured_buffer(h, elem_size, elem_count, srv, uav))
                reset_count++;
            break;
        }
        default:
            break;
        }
    }

    log_info("Restart reset %d transient GPU resources.", reset_count);
}

void res_sync_scene_dependent_render_textures() {
    int sync_count = 0;
    for (int i = 0; i < MAX_RESOURCES; i++) {
        Resource& r = g_resources[i];
        if (!r.active || r.is_builtin || r.type != RES_RENDER_TEXTURE2D || r.scene_scale_divisor <= 0)
            continue;

        int want_w = 1;
        int want_h = 1;
        res_resolve_render_texture_size(r.width, r.height, r.scene_scale_divisor, &want_w, &want_h);
        if (want_w == r.width && want_h == r.height)
            continue;

        ResHandle h = (ResHandle)(i + 1);
        if (res_recreate_render_texture(h, r.width, r.height, r.tex_fmt, r.has_rtv, r.has_srv, r.has_uav,
                                        r.has_dsv, r.scene_scale_divisor))
            sync_count++;
    }

    if (sync_count > 0)
        log_info("Resized %d scene-scaled render textures.", sync_count);
}

ResHandle res_create_structured_buffer(const char* name, int elem_size, int elem_count,
                                        bool want_srv, bool want_uav)
{
    ResHandle handle = res_alloc(name, RES_STRUCTURED_BUFFER);
    if (handle == INVALID_HANDLE) return INVALID_HANDLE;
    if (!res_recreate_structured_buffer(handle, elem_size, elem_count, want_srv, want_uav)) {
        res_free(handle);
        return INVALID_HANDLE;
    }

    log_info("StructuredBuffer created: %s (%d x %d bytes)", name, elem_count, elem_size);
    return handle;
}

ResHandle res_create_shader(const char* name, const char* path,
                             const char* vs_entry, const char* ps_entry)
{
    ResHandle handle = res_alloc(name, RES_SHADER);
    if (handle == INVALID_HANDLE) return INVALID_HANDLE;
    Resource* r = res_get(handle);
    shader_compile_vs_ps(r, path,
                          vs_entry ? vs_entry : "VSMain",
                          ps_entry ? ps_entry : "PSMain");
    return handle;
}

ResHandle res_create_compute_shader(const char* name, const char* path, const char* cs_entry) {
    ResHandle handle = res_alloc(name, RES_SHADER);
    if (handle == INVALID_HANDLE) return INVALID_HANDLE;
    Resource* r = res_get(handle);
    shader_compile_cs(r, path, cs_entry ? cs_entry : "CSMain");
    return handle;
}

static bool res_upload_texture_2d(Resource* r, const char* source_label,
                                  const void* pixels, int w, int h,
                                  DXGI_FORMAT fmt, UINT row_pitch)
{
    if (!r || !pixels || w <= 0 || h <= 0) return false;

    res_release_gpu(r);

    r->width   = w;
    r->height  = h;
    r->depth   = 1;
    r->tex_fmt = fmt;
    r->has_srv = true;
    r->has_rtv = false;
    r->has_uav = false;
    r->has_dsv = false;
    if (source_label) {
        strncpy(r->path, source_label, MAX_PATH_LEN - 1);
        r->path[MAX_PATH_LEN - 1] = '\0';
    }

    D3D11_TEXTURE2D_DESC td = {};
    td.Width     = (UINT)w;
    td.Height    = (UINT)h;
    td.MipLevels = 0;
    td.ArraySize = 1;
    td.Format    = fmt;
    td.SampleDesc.Count = 1;
    td.Usage     = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;

    HRESULT hr = g_dx.dev->CreateTexture2D(&td, nullptr, &r->tex);
    if (FAILED(hr) || !r->tex) {
        log_error("Texture create failed: %s", r->name);
        return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srvd = {};
    srvd.Format = fmt;
    srvd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvd.Texture2D.MostDetailedMip = 0;
    srvd.Texture2D.MipLevels = (UINT)-1;
    hr = g_dx.dev->CreateShaderResourceView(r->tex, &srvd, &r->srv);
    if (FAILED(hr) || !r->srv) {
        log_error("Texture SRV create failed: %s", r->name);
        return false;
    }

    g_dx.ctx->UpdateSubresource(r->tex, 0, nullptr, pixels, row_pitch, 0);
    g_dx.ctx->GenerateMips(r->srv);

    ResHandle handle = (ResHandle)((r - g_resources) + 1);
    res_sync_size_resource(handle);
    return true;
}

static bool res_upload_texture_rgba8(Resource* r, const char* source_label,
                                     const unsigned char* pixels, int w, int h)
{
    return res_upload_texture_2d(r, source_label, pixels, w, h,
                                 DXGI_FORMAT_R8G8B8A8_UNORM, (UINT)(w * 4));
}

static bool res_upload_texture_rgba32f(Resource* r, const char* source_label,
                                       const float* pixels, int w, int h)
{
    return res_upload_texture_2d(r, source_label, pixels, w, h,
                                 DXGI_FORMAT_R32G32B32A32_FLOAT, (UINT)(w * sizeof(float) * 4));
}

static bool res_load_texture_file_into(Resource* r, const char* path) {
    if (stbi_is_hdr(path)) {
        int w = 0, h = 0, ch = 0;
        float* data = stbi_loadf(path, &w, &h, &ch, 4);
        if (!data) {
            log_error("HDR texture load failed: %s", path);
            return false;
        }

        bool ok = res_upload_texture_rgba32f(r, path, data, w, h);
        stbi_image_free(data);
        if (ok)
            log_info("HDR texture loaded: %s (%dx%d)", r->name, w, h);
        return ok;
    }

    int w = 0, h = 0, ch = 0;
    unsigned char* data = stbi_load(path, &w, &h, &ch, 4);
    if (!data) {
        log_error("Texture load failed: %s", path);
        return false;
    }

    bool ok = res_upload_texture_rgba8(r, path, data, w, h);
    stbi_image_free(data);
    if (ok)
        log_info("Texture loaded: %s (%dx%d)", r->name, w, h);
    return ok;
}

bool res_reload_texture(Resource* r, const char* path) {
    if (!r || r->type != RES_TEXTURE2D || !path || !path[0])
        return false;
    return res_load_texture_file_into(r, path);
}

static ResHandle res_load_texture_from_memory(const char* name, const char* label,
                                              const void* bytes, int byte_count)
{
    ResHandle handle = res_alloc(name, RES_TEXTURE2D);
    if (handle == INVALID_HANDLE) return INVALID_HANDLE;
    Resource* r = res_get(handle);

    if (stbi_is_hdr_from_memory((const stbi_uc*)bytes, byte_count)) {
        int w = 0, h = 0, ch = 0;
        float* data = stbi_loadf_from_memory((const stbi_uc*)bytes, byte_count, &w, &h, &ch, 4);
        if (!data) {
            log_error("HDR texture load failed: %s", label ? label : name);
            res_free(handle);
            return INVALID_HANDLE;
        }

        bool ok = res_upload_texture_rgba32f(r, label, data, w, h);
        stbi_image_free(data);
        if (!ok) {
            res_free(handle);
            return INVALID_HANDLE;
        }

        log_info("HDR texture loaded: %s (%dx%d)", name, w, h);
        return handle;
    }

    int w = 0, h = 0, ch = 0;
    unsigned char* data = stbi_load_from_memory((const stbi_uc*)bytes, byte_count, &w, &h, &ch, 4);
    if (!data) {
        log_error("Texture load failed: %s", label ? label : name);
        res_free(handle);
        return INVALID_HANDLE;
    }

    bool ok = res_upload_texture_rgba8(r, label, data, w, h);
    stbi_image_free(data);
    if (!ok) {
        res_free(handle);
        return INVALID_HANDLE;
    }

    log_info("Texture loaded: %s (%dx%d)", name, w, h);
    return handle;
}

static bool res_path_is_absolute(const char* path) {
    if (!path || !path[0]) return false;
    if ((path[0] == '/' || path[0] == '\\')) return true;
    return path[1] == ':' && ((path[2] == '\\') || (path[2] == '/'));
}

static void res_path_dirname(const char* path, char* out, int out_sz) {
    out[0] = '\0';
    const char* slash1 = strrchr(path, '/');
    const char* slash2 = strrchr(path, '\\');
    const char* slash = slash1 > slash2 ? slash1 : slash2;
    if (!slash) return;

    int len = (int)(slash - path);
    if (len >= out_sz) len = out_sz - 1;
    memcpy(out, path, len);
    out[len] = '\0';
}

static void res_path_join(const char* dir, const char* file, char* out, int out_sz) {
    if (res_path_is_absolute(file) || !dir || !dir[0]) {
        strncpy(out, file, out_sz - 1);
        out[out_sz - 1] = '\0';
        return;
    }

    snprintf(out, out_sz, "%s\\%s", dir, file);
}

ResHandle res_load_texture(const char* name, const char* path) {
    ResHandle handle = res_alloc(name, RES_TEXTURE2D);
    if (handle == INVALID_HANDLE) return INVALID_HANDLE;
    Resource* r = res_get(handle);
    if (!res_load_texture_file_into(r, path)) {
        res_free(handle);
        return INVALID_HANDLE;
    }
    return handle;
}

static cgltf_image* res_gltf_texture_image(cgltf_texture* tex) {
    if (!tex) return nullptr;
    cgltf_image* img = tex->image ? tex->image : tex->basisu_image;
    if (!img && tex->webp_image) img = tex->webp_image;
    return img;
}

static ResHandle res_load_gltf_texture_ref(const char* mesh_name, int texture_index,
                                           const char* dir, cgltf_texture* tex)
{
    cgltf_image* img = res_gltf_texture_image(tex);
    if (!img) return INVALID_HANDLE;

    char base[MAX_NAME] = {};
    char uname[MAX_NAME] = {};
    snprintf(base, sizeof(base), "%s.texture%d", mesh_name, texture_index);
    res_make_unique_name(base, uname, MAX_NAME);

    if (img->uri && img->uri[0]) {
        if (strncmp(img->uri, "data:", 5) == 0) {
            log_warn("glTF texture skipped (data URI not supported yet): %s", uname);
            return INVALID_HANDLE;
        }

        char uri[MAX_PATH_LEN] = {};
        strncpy(uri, img->uri, MAX_PATH_LEN - 1);
        cgltf_decode_uri(uri);

        char full[MAX_PATH_LEN] = {};
        res_path_join(dir, uri, full, MAX_PATH_LEN);
        return res_load_texture(uname, full);
    }

    if (img->buffer_view && img->buffer_view->buffer && img->buffer_view->buffer->data) {
        const char* label = img->name ? img->name : uname;
        void* bytes = (char*)img->buffer_view->buffer->data + img->buffer_view->offset;
        return res_load_texture_from_memory(uname, label, bytes, (int)img->buffer_view->size);
    }

    return INVALID_HANDLE;
}

static ResHandle res_load_gltf_texture_cached(const char* mesh_name, const char* dir,
                                              cgltf_data* data, cgltf_texture* tex,
                                              ResHandle* cache, int cache_count)
{
    if (!tex)
        return INVALID_HANDLE;

    int texture_index = -1;
    if (data && data->textures && data->textures_count > 0) {
        ptrdiff_t ti = tex - data->textures;
        if (ti >= 0 && ti < (ptrdiff_t)data->textures_count)
            texture_index = (int)ti;
    }

    if (cache && texture_index >= 0 && texture_index < cache_count &&
        cache[texture_index] != INVALID_HANDLE)
        return cache[texture_index];

    ResHandle loaded = res_load_gltf_texture_ref(mesh_name,
        texture_index >= 0 ? texture_index : 0, dir, tex);
    if (cache && texture_index >= 0 && texture_index < cache_count)
        cache[texture_index] = loaded;
    return loaded;
}

static void res_load_gltf_material_texture_slot(MeshMaterial* dst, int slot,
                                                const char* mesh_name, const char* dir,
                                                cgltf_data* data, cgltf_texture* tex,
                                                ResHandle* cache, int cache_count)
{
    if (!dst || slot < 0 || slot >= MAX_MESH_MATERIAL_TEXTURES || !tex)
        return;
    dst->textures[slot] = res_load_gltf_texture_cached(mesh_name, dir, data, tex, cache, cache_count);
}

static int res_load_gltf_materials(const char* mesh_name, const char* gltf_path,
                                   cgltf_data* data, MeshMaterial* out_materials)
{
    if (!out_materials)
        return 0;

    for (int i = 0; i < MAX_MESH_MATERIALS; i++)
        res_init_mesh_material(&out_materials[i]);

    if (!data || data->materials_count == 0)
        return 0;

    if (data->materials_count > MAX_MESH_MATERIALS) {
        log_warn("glTF materials truncated for %s: %u -> %d",
            mesh_name, (unsigned)data->materials_count, MAX_MESH_MATERIALS);
    }

    char dir[MAX_PATH_LEN] = {};
    res_path_dirname(gltf_path, dir, MAX_PATH_LEN);

    int texture_cache_count = (int)data->textures_count;
    ResHandle* texture_cache = nullptr;
    if (texture_cache_count > 0) {
        texture_cache = (ResHandle*)malloc(sizeof(ResHandle) * (size_t)texture_cache_count);
        if (texture_cache) {
            for (int i = 0; i < texture_cache_count; i++)
                texture_cache[i] = INVALID_HANDLE;
        } else {
            texture_cache_count = 0;
        }
    }

    int material_count = (int)data->materials_count;
    if (material_count > MAX_MESH_MATERIALS)
        material_count = MAX_MESH_MATERIALS;

    for (int mi = 0; mi < material_count; mi++) {
        cgltf_material* src = &data->materials[mi];
        MeshMaterial* dst = &out_materials[mi];
        res_init_mesh_material(dst);

        const char* src_name = src->name && src->name[0] ? src->name : nullptr;
        if (src_name) {
            strncpy(dst->name, src_name, MAX_NAME - 1);
            dst->name[MAX_NAME - 1] = '\0';
        } else {
            snprintf(dst->name, sizeof(dst->name), "material_%d", mi);
        }

        dst->double_sided = src->double_sided != 0;
        dst->alpha_blend = src->alpha_mode == cgltf_alpha_mode_blend;

        if (src->has_pbr_metallic_roughness) {
            res_load_gltf_material_texture_slot(dst, 0, mesh_name, dir, data,
                src->pbr_metallic_roughness.base_color_texture.texture, texture_cache, texture_cache_count);
            res_load_gltf_material_texture_slot(dst, 1, mesh_name, dir, data,
                src->pbr_metallic_roughness.metallic_roughness_texture.texture, texture_cache, texture_cache_count);
        } else if (src->has_pbr_specular_glossiness) {
            res_load_gltf_material_texture_slot(dst, 0, mesh_name, dir, data,
                src->pbr_specular_glossiness.diffuse_texture.texture, texture_cache, texture_cache_count);
            res_load_gltf_material_texture_slot(dst, 1, mesh_name, dir, data,
                src->pbr_specular_glossiness.specular_glossiness_texture.texture, texture_cache, texture_cache_count);
        }
        res_load_gltf_material_texture_slot(dst, 2, mesh_name, dir, data,
            src->normal_texture.texture, texture_cache, texture_cache_count);
        res_load_gltf_material_texture_slot(dst, 3, mesh_name, dir, data,
            src->emissive_texture.texture, texture_cache, texture_cache_count);
        res_load_gltf_material_texture_slot(dst, 4, mesh_name, dir, data,
            src->occlusion_texture.texture, texture_cache, texture_cache_count);
    }

    int loaded_textures = 0;
    for (int i = 0; i < texture_cache_count; i++)
        if (texture_cache[i] != INVALID_HANDLE)
            loaded_textures++;
    if (loaded_textures > 0)
        log_info("glTF textures loaded for %s: %d", mesh_name, loaded_textures);

    free(texture_cache);
    return material_count;
}

// Flatten one glTF primitive into the engine's simple contiguous mesh arrays.
// The importer keeps vertices and indices in append-only buffers so multiple
// parts can share one uploaded mesh resource.
static bool res_extract_gltf_primitive(VertexArray* verts,
                                       IndexArray* indices,
                                       cgltf_primitive* prim,
                                       int* out_start_index,
                                       int* out_index_count,
                                       char* err,
                                       int err_sz)
{
    if (!prim || !out_start_index || !out_index_count) {
        if (err && err_sz > 0)
            snprintf(err, err_sz, "invalid primitive");
        return false;
    }

    if (prim->type != cgltf_primitive_type_triangles) {
        if (err && err_sz > 0)
            snprintf(err, err_sz, "primitive topology %d not supported", (int)prim->type);
        return false;
    }

    int vert_count = 0;
    cgltf_accessor* pos_acc = nullptr;
    cgltf_accessor* nor_acc = nullptr;
    cgltf_accessor* uv_acc = nullptr;

    for (cgltf_size ai = 0; ai < prim->attributes_count; ai++) {
        cgltf_attribute* attr = &prim->attributes[ai];
        cgltf_accessor* acc = attr->data;
        if (attr->type == cgltf_attribute_type_position) {
            pos_acc = acc;
            vert_count = (int)acc->count;
        } else if (attr->type == cgltf_attribute_type_normal) {
            nor_acc = acc;
        } else if (attr->type == cgltf_attribute_type_texcoord && attr->index == 0) {
            uv_acc = acc;
        }
    }

    if (!pos_acc || vert_count == 0) {
        if (err && err_sz > 0)
            snprintf(err, err_sz, "no POSITION data");
        return false;
    }

    uint32_t base_vertex = (uint32_t)verts->count;
    int old_vert_count = verts->count;
    if (!res_vertex_array_reserve(verts, verts->count + vert_count)) {
        if (err && err_sz > 0)
            snprintf(err, err_sz, "out of memory growing vertex array");
        return false;
    }

    for (int i = 0; i < vert_count; i++) {
        float p[3] = {};
        float n[3] = {0.f, 1.f, 0.f};
        float uv[2] = {};
        cgltf_accessor_read_float(pos_acc, (cgltf_size)i, p, 3);
        if (nor_acc) cgltf_accessor_read_float(nor_acc, (cgltf_size)i, n, 3);
        if (uv_acc) cgltf_accessor_read_float(uv_acc, (cgltf_size)i, uv, 2);

        Vertex* v = &verts->items[old_vert_count + i];
        v->pos[0] = p[0];  v->pos[1] = p[1];  v->pos[2] = p[2];
        v->nor[0] = n[0];  v->nor[1] = n[1];  v->nor[2] = n[2];
        v->uv[0] = uv[0];  v->uv[1] = uv[1];
    }
    verts->count += vert_count;

    int start_index = indices->count;
    int append_count = prim->indices ? (int)prim->indices->count : vert_count;
    if (!res_index_array_reserve(indices, indices->count + append_count)) {
        if (err && err_sz > 0)
            snprintf(err, err_sz, "out of memory growing index array");
        return false;
    }
    if (prim->indices) {
        cgltf_accessor* ia = prim->indices;
        for (cgltf_size ii = 0; ii < ia->count; ii++)
            indices->items[indices->count++] = base_vertex + (uint32_t)cgltf_accessor_read_index(ia, ii);
    } else {
        for (int i = 0; i < vert_count; i++)
            indices->items[indices->count++] = base_vertex + (uint32_t)i;
    }

    *out_start_index = start_index;
    *out_index_count = indices->count - start_index;
    return true;
}

static bool res_find_cached_gltf_primitive(const PrimitiveRangeArray* cache, cgltf_primitive* prim,
                                           int* out_start_index, int* out_index_count)
{
    if (!cache || !prim || !out_start_index || !out_index_count)
        return false;

    for (int i = 0; i < cache->count; i++) {
        const GltfPrimitiveRange* entry = &cache->items[i];
        if (entry->prim != prim)
            continue;
        *out_start_index = entry->start_index;
        *out_index_count = entry->index_count;
        return true;
    }
    return false;
}

static bool res_push_cached_gltf_primitive(PrimitiveRangeArray* cache, cgltf_primitive* prim,
                                           int start_index, int index_count)
{
    GltfPrimitiveRange* entry = nullptr;
    if (!cache || !prim)
        return false;
    if (!res_primitive_range_array_reserve(cache, cache->count + 1))
        return false;
    entry = &cache->items[cache->count++];
    entry->prim = prim;
    entry->start_index = start_index;
    entry->index_count = index_count;
    return true;
}

static int res_gltf_resolve_material_index(const GltfMeshBuildContext* ctx, cgltf_material* mat) {
    ptrdiff_t material_index = 0;
    if (!ctx || !mat || !ctx->data || !ctx->data->materials || ctx->imported_material_count <= 0)
        return -1;
    material_index = mat - ctx->data->materials;
    if (material_index < 0 || material_index >= ctx->imported_material_count)
        return -1;
    return (int)material_index;
}

static void res_gltf_append_part(GltfMeshBuildContext* ctx, cgltf_primitive* prim,
                                 const char* part_name, const float local_transform[16])
{
    int start_index = 0;
    int index_count = 0;
    char err[256] = {};
    if (!ctx || !prim || !ctx->part_overflow || *ctx->part_overflow)
        return;

    if (!res_find_cached_gltf_primitive(ctx->primitive_cache, prim, &start_index, &index_count)) {
        if (!res_extract_gltf_primitive(ctx->verts, ctx->indices, prim, &start_index, &index_count, err, sizeof(err))) {
            log_warn("glTF primitive skipped in %s: %s",
                ctx->resource_name, err[0] ? err : "unsupported primitive");
            return;
        }
        if (!res_push_cached_gltf_primitive(ctx->primitive_cache, prim, start_index, index_count)) {
            log_warn("glTF primitive cache disabled for %s: out of memory", ctx->resource_name);
        }
    }

    if (!res_push_mesh_part(ctx->imported_parts, ctx->imported_part_count, part_name,
            start_index, index_count, res_gltf_resolve_material_index(ctx, prim->material), local_transform)) {
        if (!*ctx->part_overflow) {
            log_warn("glTF parts truncated for %s: max %d", ctx->resource_name, MAX_MESH_PARTS);
            *ctx->part_overflow = true;
        }
    }
}

static void res_gltf_append_node_mesh(GltfMeshBuildContext* ctx, cgltf_node* node) {
    const char* node_name = nullptr;
    const char* mesh_name = nullptr;
    float world[16] = {};
    if (!ctx || !node || !node->mesh)
        return;

    cgltf_node_transform_world(node, world);
    node_name = node->name && node->name[0] ? node->name : nullptr;
    mesh_name = node->mesh->name && node->mesh->name[0] ? node->mesh->name : nullptr;

    for (cgltf_size pi = 0; pi < node->mesh->primitives_count; pi++) {
        char part_name[MAX_NAME] = {};
        if (node_name) {
            snprintf(part_name, sizeof(part_name), "%s.%u", node_name, (unsigned)pi);
        } else if (mesh_name) {
            snprintf(part_name, sizeof(part_name), "%s.%u", mesh_name, (unsigned)pi);
        } else {
            snprintf(part_name, sizeof(part_name), "part_%d", *ctx->imported_part_count);
        }
        res_gltf_append_part(ctx, &node->mesh->primitives[pi], part_name, world);
    }
}

static void res_gltf_visit_node(GltfMeshBuildContext* ctx, cgltf_node* node) {
    if (!ctx || !node)
        return;
    res_gltf_append_node_mesh(ctx, node);
    for (cgltf_size ci = 0; ci < node->children_count; ci++)
        res_gltf_visit_node(ctx, node->children[ci]);
}

static bool res_push_mesh_part(MeshPart* parts, int* part_count, const char* part_name,
                               int start_index, int index_count, int material_index,
                               const float local_transform[16])
{
    if (!parts || !part_count || *part_count >= MAX_MESH_PARTS)
        return false;

    MeshPart* part = &parts[*part_count];
    res_init_mesh_part(part);
    if (part_name && part_name[0]) {
        strncpy(part->name, part_name, MAX_NAME - 1);
        part->name[MAX_NAME - 1] = '\0';
    } else {
        snprintf(part->name, sizeof(part->name), "part_%d", *part_count);
    }
    part->start_index = start_index;
    part->index_count = index_count;
    part->material_index = material_index;
    if (local_transform)
        memcpy(part->local_transform, local_transform, sizeof(part->local_transform));
    (*part_count)++;
    return true;
}

bool res_set_mesh_primitive(Resource* r, MeshPrimitiveType type) {
    if (!r) return false;

    bool ok = false;
    switch (type) {
    case MESH_PRIM_CUBE:        ok = res_make_cube_mesh(r); break;
    case MESH_PRIM_TETRAHEDRON: ok = res_make_tetrahedron_mesh(r); break;
    case MESH_PRIM_SPHERE:      ok = res_make_sphere_mesh(r); break;
    case MESH_PRIM_FULLSCREEN_TRIANGLE: ok = res_make_fullscreen_triangle_mesh(r); break;
    default: break;
    }

    if (ok) {
        r->mesh_primitive_type = (int)type;
        r->compiled_ok = true;
        r->using_fallback = false;
        r->compile_err[0] = '\0';
    }
    return ok;
}

ResHandle res_create_mesh_primitive(const char* name, MeshPrimitiveType type) {
    ResHandle handle = res_alloc(name, RES_MESH);
    if (handle == INVALID_HANDLE) return INVALID_HANDLE;

    Resource* r = res_get(handle);
    if (!res_set_mesh_primitive(r, type)) {
        res_free(handle);
        return INVALID_HANDLE;
    }

    const char* kind = "mesh";
    if (type == MESH_PRIM_CUBE) kind = "cube";
    if (type == MESH_PRIM_TETRAHEDRON) kind = "tetrahedron";
    if (type == MESH_PRIM_SPHERE) kind = "sphere";
    if (type == MESH_PRIM_FULLSCREEN_TRIANGLE) kind = "fullscreen_triangle";
    log_info("Mesh primitive created: %s (%s)", name, kind);
    return handle;
}

static ResHandle res_mesh_fallback_cube(ResHandle handle, const char* msg) {
    Resource* r = res_get(handle);
    if (!r) return INVALID_HANDLE;
    res_make_cube_mesh(r);
    r->compiled_ok = false;
    r->using_fallback = true;
    snprintf(r->compile_err, sizeof(r->compile_err), "%s. Using fallback cube.", msg ? msg : "Mesh load failed");
    log_warn("Mesh: %s", r->compile_err);
    return handle;
}

// Import a glTF mesh into the engine's internal mesh representation. The
// importer preserves per-part transforms and material assignments when found.
ResHandle res_load_mesh(const char* name, const char* path) {
    ResHandle handle = res_alloc(name, RES_MESH);
    if (handle == INVALID_HANDLE) return INVALID_HANDLE;
    Resource* r = res_get(handle);
    strncpy(r->path, path, MAX_PATH_LEN - 1);
    r->path[MAX_PATH_LEN - 1] = '\0';
    r->compiled_ok = false;
    r->using_fallback = false;
    r->compile_err[0] = '\0';

    cgltf_options opts = {};
    cgltf_data*   data = nullptr;
    if (cgltf_parse_file(&opts, path, &data) != cgltf_result_success) {
        char msg[512];
        snprintf(msg, sizeof(msg), "cgltf: failed to parse '%s'", path);
        if (data) cgltf_free(data);
        return res_mesh_fallback_cube(handle, msg);
    }
    if (cgltf_load_buffers(&opts, data, path) != cgltf_result_success) {
        char msg[512];
        snprintf(msg, sizeof(msg), "cgltf: failed to load buffers for '%s'", path);
        cgltf_free(data);
        return res_mesh_fallback_cube(handle, msg);
    }

    bool has_any_primitives = false;
    for (cgltf_size mi = 0; mi < data->meshes_count; mi++) {
        if (data->meshes[mi].primitives_count > 0) {
            has_any_primitives = true;
            break;
        }
    }

    if (data->meshes_count == 0 || !has_any_primitives) {
        char msg[512];
        snprintf(msg, sizeof(msg), "cgltf: no meshes in '%s'", path);
        cgltf_free(data);
        return res_mesh_fallback_cube(handle, msg);
    }

    MeshMaterial imported_materials[MAX_MESH_MATERIALS] = {};
    MeshPart imported_parts[MAX_MESH_PARTS] = {};
    for (int i = 0; i < MAX_MESH_MATERIALS; i++)
        res_init_mesh_material(&imported_materials[i]);
    for (int i = 0; i < MAX_MESH_PARTS; i++)
        res_init_mesh_part(&imported_parts[i]);

    int imported_material_count = res_load_gltf_materials(name, path, data, imported_materials);
    int imported_part_count = 0;
    bool part_overflow = false;

    VertexArray verts = {};
    IndexArray indices = {};
    PrimitiveRangeArray primitive_cache = {};
    GltfMeshBuildContext build_ctx = {};
    build_ctx.resource_name = name;
    build_ctx.data = data;
    build_ctx.imported_material_count = imported_material_count;
    build_ctx.imported_parts = imported_parts;
    build_ctx.imported_part_count = &imported_part_count;
    build_ctx.part_overflow = &part_overflow;
    build_ctx.verts = &verts;
    build_ctx.indices = &indices;
    build_ctx.primitive_cache = &primitive_cache;

    bool visited_nodes = false;
    cgltf_scene* scene = data->scene ? data->scene : (data->scenes_count > 0 ? &data->scenes[0] : nullptr);
    if (scene && scene->nodes_count > 0) {
        visited_nodes = true;
        for (cgltf_size ni = 0; ni < scene->nodes_count; ni++)
            res_gltf_visit_node(&build_ctx, scene->nodes[ni]);
    } else if (data->nodes_count > 0) {
        visited_nodes = true;
        for (cgltf_size ni = 0; ni < data->nodes_count; ni++) {
            cgltf_node* node = &data->nodes[ni];
            if (!node->parent)
                res_gltf_visit_node(&build_ctx, node);
        }
    }

    if (!visited_nodes || imported_part_count == 0) {
        float identity[16] = {};
        res_mesh_identity(identity);
        for (cgltf_size mi = 0; mi < data->meshes_count; mi++) {
            cgltf_mesh* mesh = &data->meshes[mi];
            const char* mesh_name = mesh->name && mesh->name[0] ? mesh->name : nullptr;
            for (cgltf_size pi = 0; pi < mesh->primitives_count; pi++) {
                char part_name[MAX_NAME] = {};
                if (mesh_name)
                    snprintf(part_name, sizeof(part_name), "%s.%u", mesh_name, (unsigned)pi);
                else
                    snprintf(part_name, sizeof(part_name), "part_%d", imported_part_count);
                res_gltf_append_part(&build_ctx, &mesh->primitives[pi], part_name, identity);
            }
        }
    }

    if (verts.count == 0 || indices.count == 0 || imported_part_count == 0) {
        char msg[512];
        snprintf(msg, sizeof(msg), "cgltf: no supported triangle primitives in '%s'", path);
        cgltf_free(data);
        res_vertex_array_free(&verts);
        res_index_array_free(&indices);
        res_primitive_range_array_free(&primitive_cache);
        return res_mesh_fallback_cube(handle, msg);
    }

    if (!res_upload_mesh(r, verts.items, verts.count, indices.items, indices.count)) {
        cgltf_free(data);
        res_vertex_array_free(&verts);
        res_index_array_free(&indices);
        res_primitive_range_array_free(&primitive_cache);
        return res_mesh_fallback_cube(handle, "Mesh upload failed");
    }

    res_reset_mesh_asset(r);
    r->mesh_material_count = imported_material_count;
    r->mesh_part_count = imported_part_count;
    memcpy(r->mesh_materials, imported_materials, sizeof(imported_materials));
    memcpy(r->mesh_parts, imported_parts, sizeof(imported_parts));

    cgltf_free(data);
    res_vertex_array_free(&verts);
    res_index_array_free(&indices);
    res_primitive_range_array_free(&primitive_cache);
    r->compiled_ok = true;
    r->using_fallback = false;
    r->mesh_primitive_type = -1;
    r->compile_err[0] = '\0';
    log_info("Mesh loaded: %s (%d verts, %d idx, %d parts, %d materials)",
             name, r->vert_count, r->idx_count, r->mesh_part_count, r->mesh_material_count);
    return handle;
}

void res_rename(ResHandle h, const char* new_name) {
    Resource* r = res_get(h);
    if (!r || r->is_builtin) return;
    strncpy(r->name, new_name, MAX_NAME - 1);
    r->name[MAX_NAME - 1] = '\0';
    res_sync_size_resource(h);
}

void res_make_unique_name(const char* base, char* out, int out_sz) {
    if (res_find_by_name(base) == INVALID_HANDLE) {
        strncpy(out, base, out_sz - 1);
        out[out_sz - 1] = '\0';
        return;
    }
    for (int i = 1; i < 1000; i++) {
        snprintf(out, out_sz, "%s_%d", base, i);
        if (res_find_by_name(out) == INVALID_HANDLE) return;
    }
}
