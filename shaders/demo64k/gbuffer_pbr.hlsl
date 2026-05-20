#define LT_NO_DEFAULT_SHADOWMAP
#include "../common.hlsl"
#include "procedural_materials.hlsl"

cbuffer UserCB : register(b2)
{
    float4 MaterialGlobalTuning; // x roughness scale, y metal scale, z detail, w emissive boost.
    int    MaterialId;
};

struct VSIn
{
    float3 pos : POSITION;
    float3 nor : NORMAL;
    float2 uv  : TEXCOORD0;
};

struct VSOut
{
    float4 pos       : SV_POSITION;
    float3 world_pos : TEXCOORD0;
    float3 normal_ws : TEXCOORD1;
    float2 uv        : TEXCOORD2;
};

VSOut VSMain(VSIn v)
{
    VSOut o;
    float4 wpos = lt_object_to_world(v.pos);
    o.pos = lt_world_to_clip(wpos.xyz);
    o.world_pos = wpos.xyz;
    o.normal_ws = lt_object_normal_to_world(v.nor);
    o.uv = v.uv;
    return o;
}

struct PSOut
{
    float4 normal_roughness : SV_Target0;
    float4 albedo_metalness : SV_Target1;
    float4 emissive         : SV_Target2;
};

PSOut PSMain(VSOut i)
{
    float3 N = lt_safe_normalize(i.normal_ws);
    N = demo64k_material_normal(MaterialId, N, i.world_pos, MaterialGlobalTuning);
    PBRMaterial m = demo64k_make_material(MaterialId, i.world_pos, N, MaterialGlobalTuning);

    PSOut o;
    o.normal_roughness = float4(N * 0.5 + 0.5, m.roughness);
    o.albedo_metalness = float4(m.albedo, m.metalness);
    o.emissive = float4(m.emissive, 1.0);
    return o;
}
