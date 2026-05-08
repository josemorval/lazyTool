#include "common.hlsl"

/*
    Circle-of-confusion generation for depth of field.

    The output stores signed normalized blur in the red channel. Negative is
    foreground, positive is background. Clear sky/background has zero CoC so it
    does not pull bright foreground edges into the black sky.
*/

Texture2D DepthTex : register(t0);

cbuffer UserCB : register(b2)
{
    float4 FocusParams; // x focus distance, y focus range, z near blur boost, w curve power.
};

PostVSOut VSMain(VSIn v)
{
    return make_fullscreen_vertex(v);
}

float4 PSMain(PostVSOut i) : SV_Target
{
    float d = DepthTex.SampleLevel(LinearSampler, i.uv, 0).r;
    if (d >= 0.99995)
        return float4(0.0, 0.0, 0.0, 1.0);

    float3 world = reconstruct_world_from_depth(i.uv, d);
    float vd = view_depth_from_world(world);
    float focus = max(FocusParams.x, 0.01);
    float range = max(FocusParams.y, 0.01);
    float signed_coc = (vd - focus) / range;

    // Foreground blur tends to be visually more important than far blur.
    if (signed_coc < 0.0)
        signed_coc *= max(FocusParams.z, 1.0);

    float coc = pow(saturate(abs(signed_coc)), max(FocusParams.w, 0.2));
    return float4(coc * sign(signed_coc), 0.0, 0.0, 1.0);
}
