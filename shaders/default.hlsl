// Minimal lit shader for the default scene.
#include "common.hlsl"

struct VSIn {
    float3 pos : POSITION;
    float3 nor : NORMAL;
    float2 uv  : TEXCOORD0;
};

struct VSOut {
    float4 pos  : SV_POSITION;
    float3 nor  : NORMAL;
    float3 wpos : TEXCOORD0;
};

VSOut VSMain(VSIn v) {
    VSOut o;
    float4 wpos = lt_object_to_world(v.pos);
    o.pos = lt_world_to_clip(wpos.xyz);
    o.wpos = wpos.xyz;
    o.nor = lt_object_normal_to_world(v.nor);
    return o;
}

float4 PSMain(VSOut i) : SV_Target {
    float3 n = normalize(i.nor);
    float3 ld = LightParams.x >= 0.5 ? normalize(LightPos.xyz - i.wpos) : normalize(-LightDir.xyz);
    float ndl = saturate(dot(n, ld));
    float shadow = lt_sample_shadow_pcf3x3(i.wpos, n, ld);

    float3 base = 0.30 + 0.70 * (0.5 + 0.5 * n);
    float attenuation = 1.0;
    if (LightParams.x >= 0.5) {
        float cone = dot(normalize(i.wpos - LightPos.xyz), normalize(LightDir.xyz));
        attenuation = saturate((cone - LightParams.z) / max(LightParams.y - LightParams.z, LT_EPS));
    }
    float3 sun = LightColor.xyz * LightDir.w * ndl * shadow * attenuation;
    float3 ambient = float3(0.06, 0.065, 0.08);
    return float4(base * (ambient + sun), 1.0);
}
