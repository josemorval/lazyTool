// User parameters supplied by the command in register(b2).
// Color tints the UV visualizer. Leave it at 0,0,0,0 for neutral white,
// or set it to a non-zero value from Shader Parameters to choose a color.
cbuffer UserCB : register(b2)
{
    float4 Color;
};

float4 ResolveUserColor()
{
    return dot(abs(Color), float4(1.0, 1.0, 1.0, 1.0)) > 0.0 ? Color : float4(1.0, 1.0, 1.0, 1.0);
}

// VS/PS template: procedural fullscreen quad with tinted UV color.
//
// Editor setup:
//   1. Use a Draw Mesh, Draw Instanced, or Indirect Draw command.
//   2. Change Source to Procedural.
//   3. Set Topology to Triangle List and Vertex Count to 6.
//   4. Assign this shader to the command.
//   5. In Shader Parameters, edit Color to tint the result. A zero color is
//      treated as neutral white so the first compile is immediately visible.
//
// Instancing notes:
//   - SV_InstanceID is available in VSMain. The sample does not move each
//     instance, but you can bind a StructuredBuffer as SRV and use instance_id
//     to read per-instance transforms, colors, or rectangles.
//   - For indirect procedural draws, generate DrawInstancedIndirect arguments
//     with the indirect-args compute template and set VertexCount to 6.
//
// The vertex shader only uses SV_VertexID and SV_InstanceID, so no mesh or
// input layout is required. The quad fills clip space and paints red=U, green=V.
struct VSOut
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

VSOut VSMain(uint vertex_id : SV_VertexID, uint instance_id : SV_InstanceID)
{
    uint i = min(vertex_id, 5u);
    float2 pos = float2(-1.0, -1.0);
    float2 uv = float2(0.0, 1.0);

    if (i == 1u) { pos = float2(-1.0,  1.0); uv = float2(0.0, 0.0); }
    if (i == 2u) { pos = float2( 1.0,  1.0); uv = float2(1.0, 0.0); }
    if (i == 3u) { pos = float2(-1.0, -1.0); uv = float2(0.0, 1.0); }
    if (i == 4u) { pos = float2( 1.0,  1.0); uv = float2(1.0, 0.0); }
    if (i == 5u) { pos = float2( 1.0, -1.0); uv = float2(1.0, 1.0); }

    VSOut o;
    o.pos = float4(pos, 0.0, 1.0);
    o.uv = uv;
    return o;
}

float4 PSMain(VSOut i) : SV_Target
{
    return float4(i.uv, 0.0, 1.0) * ResolveUserColor();
}
