#define LT_NO_DEFAULT_SHADOWMAP
#include "../common.hlsl"
#include "fullscreen_common.hlsl"

Texture2D SceneDepthTex : register(t0);

cbuffer UserCB : register(b2)
{
    float4 PostDOFParams; // x focus distance, y aperture, z max blur pixels, w spare.
};

float4 PSMain(FullscreenVSOut i) : SV_Target
{
    float2 uv = saturate(i.uv);
    float depth01 = SceneDepthTex.SampleLevel(LinearSampler, uv, 0).r;
    if (depth01 >= 0.9999)
        return float4(0, 0, 0, 1);

    float view_depth = lt_scene_depth_to_view_depth(uv, depth01);
    float focus = max(PostDOFParams.x, 0.05);
    float aperture = max(PostDOFParams.y, 0.0);

    // Signed CoC model: near foreground gets a little more weight than far background.
    float signed_coc = (view_depth - focus) / max(view_depth, 0.1) * aperture;
    float far_coc = saturate( signed_coc * 1.10);
    float near_coc = saturate(-signed_coc * 1.55);
    float coc = saturate(max(near_coc, far_coc));
    coc = smoothstep(0.018, 0.90, coc);
    return float4(coc, near_coc, far_coc, 1.0);
}
