#include "gs_common.hlsl"

StructuredBuffer<uint> SortState : register(t0);

struct VSOut
{
    float4 pos : SV_POSITION;
};

VSOut VSMain(uint vertex_id : SV_VertexID)
{
    float2 p = vertex_id == 0u ? float2(-1.0, -1.0) :
               vertex_id == 1u ? float2(-1.0,  3.0) :
                                  float2( 3.0, -1.0);

    VSOut o;
    o.pos = float4(p, 0.0, 1.0);
    return o;
}

uint decimal_digits(uint v)
{
    uint n = 1u;
    [loop]
    while (v >= 10u)
    {
        v /= 10u;
        n++;
    }
    return n;
}

uint pow10_u(uint e)
{
    uint p = 1u;
    [loop]
    for (uint i = 0u; i < e; i++)
        p *= 10u;
    return p;
}

uint label_glyph(uint idx)
{
    // "SPLATS "
    uint glyph = 15u;
    if (idx == 0u) glyph = 10u;
    else if (idx == 1u) glyph = 11u;
    else if (idx == 2u) glyph = 12u;
    else if (idx == 3u) glyph = 13u;
    else if (idx == 4u) glyph = 14u;
    else if (idx == 5u) glyph = 10u;
    return glyph;
}

uint glyph_for_char(uint char_idx, uint value, uint digit_count)
{
    static const uint kLabelLen = 7u;
    uint glyph = 15u;
    if (char_idx < kLabelLen)
    {
        glyph = label_glyph(char_idx);
    }
    else
    {
        uint digit_idx = min(char_idx - kLabelLen, digit_count - 1u);
        uint div = pow10_u(digit_count - digit_idx - 1u);
        glyph = (value / div) % 10u;
    }
    return glyph;
}

uint glyph_row(uint glyph, uint row)
{
    uint bits = 0u;

    if (glyph == 0u)
    {
        uint rows[7] = { 14u, 17u, 19u, 21u, 25u, 17u, 14u };
        bits = rows[row];
    }
    else if (glyph == 1u)
    {
        uint rows[7] = { 4u, 12u, 4u, 4u, 4u, 4u, 14u };
        bits = rows[row];
    }
    else if (glyph == 2u)
    {
        uint rows[7] = { 14u, 17u, 1u, 2u, 4u, 8u, 31u };
        bits = rows[row];
    }
    else if (glyph == 3u)
    {
        uint rows[7] = { 30u, 1u, 1u, 14u, 1u, 1u, 30u };
        bits = rows[row];
    }
    else if (glyph == 4u)
    {
        uint rows[7] = { 2u, 6u, 10u, 18u, 31u, 2u, 2u };
        bits = rows[row];
    }
    else if (glyph == 5u)
    {
        uint rows[7] = { 31u, 16u, 16u, 30u, 1u, 1u, 30u };
        bits = rows[row];
    }
    else if (glyph == 6u)
    {
        uint rows[7] = { 14u, 16u, 16u, 30u, 17u, 17u, 14u };
        bits = rows[row];
    }
    else if (glyph == 7u)
    {
        uint rows[7] = { 31u, 1u, 2u, 4u, 8u, 8u, 8u };
        bits = rows[row];
    }
    else if (glyph == 8u)
    {
        uint rows[7] = { 14u, 17u, 17u, 14u, 17u, 17u, 14u };
        bits = rows[row];
    }
    else if (glyph == 9u)
    {
        uint rows[7] = { 14u, 17u, 17u, 15u, 1u, 1u, 14u };
        bits = rows[row];
    }
    else if (glyph == 10u)
    {
        uint rows[7] = { 15u, 16u, 16u, 14u, 1u, 1u, 30u };
        bits = rows[row];
    }
    else if (glyph == 11u)
    {
        uint rows[7] = { 30u, 17u, 17u, 30u, 16u, 16u, 16u };
        bits = rows[row];
    }
    else if (glyph == 12u)
    {
        uint rows[7] = { 16u, 16u, 16u, 16u, 16u, 16u, 31u };
        bits = rows[row];
    }
    else if (glyph == 13u)
    {
        uint rows[7] = { 14u, 17u, 17u, 31u, 17u, 17u, 17u };
        bits = rows[row];
    }
    else if (glyph == 14u)
    {
        uint rows[7] = { 31u, 4u, 4u, 4u, 4u, 4u, 4u };
        bits = rows[row];
    }

    return bits;
}

float4 PSMain(VSOut i) : SV_Target
{
    uint visible = SortState[GS_STATE_VISIBLE_COUNT];
    uint digits = decimal_digits(visible);

    static const uint kLabelLen = 7u;
    uint char_count = kLabelLen + digits;
    float scale = 3.0;
    float2 origin = float2(14.0, 14.0);
    float2 pad = float2(7.0, 6.0);
    float2 glyph_size = float2(5.0, 7.0) * scale;
    float advance = 6.0 * scale;
    float2 panel_size = float2((float)char_count * advance - scale + pad.x * 2.0,
                               glyph_size.y + pad.y * 2.0);

    float2 p = i.pos.xy;
    float2 panel_rel = p - origin;
    if (panel_rel.x < 0.0 || panel_rel.y < 0.0 ||
        panel_rel.x >= panel_size.x || panel_rel.y >= panel_size.y)
        return float4(0.0, 0.0, 0.0, 0.0);

    float2 text_rel = panel_rel - pad;
    bool in_text_y = text_rel.y >= 0.0 && text_rel.y < glyph_size.y;
    bool in_text_x = text_rel.x >= 0.0 && text_rel.x < (float)char_count * advance;

    if (in_text_x && in_text_y)
    {
        uint char_idx = (uint)(text_rel.x / advance);
        float char_x = text_rel.x - (float)char_idx * advance;
        uint local_x = (uint)(char_x / scale);
        uint local_y = (uint)(text_rel.y / scale);

        if (local_x < 5u && local_y < 7u)
        {
            uint glyph = glyph_for_char(char_idx, visible, digits);
            uint row = glyph_row(glyph, local_y);
            bool on = ((row >> (4u - local_x)) & 1u) != 0u;
            if (on)
                return float4(0.65, 1.0, 0.72, 0.96);
        }
    }

    return float4(0.0, 0.0, 0.0, 0.48);
}
