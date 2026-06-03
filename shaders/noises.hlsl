#ifndef LAZYTOOL_NOISES_HLSL
#define LAZYTOOL_NOISES_HLSL

// Hashes, lattice noise, Worley/cellular noise, and screen-space dither helpers.
//
// References and related reading:
// - Mark Jarzynski and Marc Olano, "Hash Functions for GPU Rendering", JCGT
//   2020: https://jcgt.org/published/0009/03/02/
// - Steven Worley, "A Cellular Texture Basis Function", SIGGRAPH 1996:
//   https://dl.acm.org/doi/10.1145/237170.237267
// - Jorge Jimenez, "Next Generation Post Processing in Call of Duty: Advanced
//   Warfare", SIGGRAPH 2014: https://www.iryoku.com/next-generation-post-processing-in-call-of-duty-advanced-warfare/
// - Christoph Peters, "Free blue noise textures":
//   https://momentsingraphics.de/BlueNoise.html
// - Alan Wolfe et al., "Spatiotemporal Blue Noise Masks", EGSR 2022:
//   https://diglib.eg.org/items/a96087bb-abe8-4851-968c-cccc7f17e08c

static const float LT_NOISE_UINT_SCALE = 1.0 / 4294967296.0;

// PCG RXS-M-XS style 32-bit integer hash. Good default for shader randomness:
// deterministic, cheap, and much better behaved than sin/fract float hashes.
uint lt_hash_u32(uint v)
{
    v = v * 747796405u + 2891336453u;
    uint word = ((v >> ((v >> 28u) + 4u)) ^ v) * 277803737u;
    return (word >> 22u) ^ word;
}

uint lt_hash_u32(uint2 v)
{
    return lt_hash_u32(v.x ^ lt_hash_u32(v.y + 0x9E3779B9u));
}

uint lt_hash_u32(uint3 v)
{
    return lt_hash_u32(v.x ^ lt_hash_u32(v.y + lt_hash_u32(v.z + 0x9E3779B9u)));
}

uint lt_hash_u32(int2 v)
{
    return lt_hash_u32(asuint(v));
}

uint lt_hash_u32(int3 v)
{
    return lt_hash_u32(asuint(v));
}

// Convert a full 32-bit hash to [0, 1). The result never reaches exactly 1.
float lt_hash_to_float01(uint h)
{
    return (float)h * LT_NOISE_UINT_SCALE;
}

float lt_hash11(uint x)
{
    return lt_hash_to_float01(lt_hash_u32(x));
}

float lt_hash12(uint2 p)
{
    return lt_hash_to_float01(lt_hash_u32(p));
}

float lt_hash13(uint3 p)
{
    return lt_hash_to_float01(lt_hash_u32(p));
}

float2 lt_hash22(uint2 p)
{
    uint h = lt_hash_u32(p);
    return float2(lt_hash_to_float01(h),
                  lt_hash_to_float01(lt_hash_u32(h + 0xA511E9B3u)));
}

float3 lt_hash33(uint3 p)
{
    uint h = lt_hash_u32(p);
    uint h1 = lt_hash_u32(h + 0xA511E9B3u);
    uint h2 = lt_hash_u32(h1 + 0x63D83595u);
    return float3(lt_hash_to_float01(h),
                  lt_hash_to_float01(h1),
                  lt_hash_to_float01(h2));
}

float2 lt_hash22(int2 p)
{
    return lt_hash22(asuint(p));
}

float3 lt_hash33(int3 p)
{
    return lt_hash33(asuint(p));
}

float3 lt_hash_unit_vector3(uint3 p)
{
    float3 h = lt_hash33(p) * 2.0 - 1.0;
    return h * rsqrt(max(dot(h, h), 1e-8));
}

float lt_noise_smooth(float t)
{
    return t * t * (3.0 - 2.0 * t);
}

float2 lt_noise_smooth(float2 t)
{
    return t * t * (3.0 - 2.0 * t);
}

float3 lt_noise_smooth(float3 t)
{
    return t * t * (3.0 - 2.0 * t);
}

// Simple value noise. Useful for low-cost masks, variation, and animation
// control. For normals/displacement, prefer domain-scaled values and enough
// octaves to avoid visible cell structure.
float lt_value_noise2(float2 p)
{
    int2 i = (int2)floor(p);
    float2 f = frac(p);
    float2 u = lt_noise_smooth(f);

    float a = lt_hash12(asuint(i + int2(0, 0)));
    float b = lt_hash12(asuint(i + int2(1, 0)));
    float c = lt_hash12(asuint(i + int2(0, 1)));
    float d = lt_hash12(asuint(i + int2(1, 1)));

    return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
}

float lt_value_noise3(float3 p)
{
    int3 i = (int3)floor(p);
    float3 f = frac(p);
    float3 u = lt_noise_smooth(f);

    float n000 = lt_hash13(asuint(i + int3(0, 0, 0)));
    float n100 = lt_hash13(asuint(i + int3(1, 0, 0)));
    float n010 = lt_hash13(asuint(i + int3(0, 1, 0)));
    float n110 = lt_hash13(asuint(i + int3(1, 1, 0)));
    float n001 = lt_hash13(asuint(i + int3(0, 0, 1)));
    float n101 = lt_hash13(asuint(i + int3(1, 0, 1)));
    float n011 = lt_hash13(asuint(i + int3(0, 1, 1)));
    float n111 = lt_hash13(asuint(i + int3(1, 1, 1)));

    float nx00 = lerp(n000, n100, u.x);
    float nx10 = lerp(n010, n110, u.x);
    float nx01 = lerp(n001, n101, u.x);
    float nx11 = lerp(n011, n111, u.x);
    float nxy0 = lerp(nx00, nx10, u.y);
    float nxy1 = lerp(nx01, nx11, u.y);
    return lerp(nxy0, nxy1, u.z);
}

float lt_fbm2(float2 p, int octaves)
{
    float sum = 0.0;
    float amp = 0.5;
    float norm = 0.0;
    [loop]
    for (int i = 0; i < octaves; ++i) {
        sum += lt_value_noise2(p) * amp;
        norm += amp;
        p = p * 2.02 + float2(17.17, 31.13);
        amp *= 0.5;
    }
    return sum / max(norm, 1e-5);
}

// Worley/cellular noise. Returns F1 and F2 distances to the closest and second
// closest feature points. Use F2 - F1 for cell borders/ridges.
float2 lt_worley2(float2 p)
{
    int2 cell = (int2)floor(p);
    float2 local = frac(p);
    float f1 = 1e20;
    float f2 = 1e20;

    [unroll]
    for (int y = -1; y <= 1; ++y) {
        [unroll]
        for (int x = -1; x <= 1; ++x) {
            int2 o = int2(x, y);
            float2 feature = (float2)o + lt_hash22(cell + o);
            float d2 = dot(feature - local, feature - local);
            if (d2 < f1) {
                f2 = f1;
                f1 = d2;
            } else if (d2 < f2) {
                f2 = d2;
            }
        }
    }

    return sqrt(float2(f1, f2));
}

float2 lt_worley3(float3 p)
{
    int3 cell = (int3)floor(p);
    float3 local = frac(p);
    float f1 = 1e20;
    float f2 = 1e20;

    [unroll]
    for (int z = -1; z <= 1; ++z) {
        [unroll]
        for (int y = -1; y <= 1; ++y) {
            [unroll]
            for (int x = -1; x <= 1; ++x) {
                int3 o = int3(x, y, z);
                float3 feature = (float3)o + lt_hash33(cell + o);
                float d2 = dot(feature - local, feature - local);
                if (d2 < f1) {
                    f2 = f1;
                    f1 = d2;
                } else if (d2 < f2) {
                    f2 = d2;
                }
            }
        }
    }

    return sqrt(float2(f1, f2));
}

float lt_worley_ridge2(float2 p)
{
    float2 f = lt_worley2(p);
    return f.y - f.x;
}

float lt_worley_ridge3(float3 p)
{
    float2 f = lt_worley3(p);
    return f.y - f.x;
}

// Interleaved Gradient Noise: compact screen-space noise useful for dithering,
// stochastic alpha, SSAO/SSR rotations, and reducing banding. It is not a
// blue-noise texture, but it has a more structured screen distribution than
// plain white noise and is cheap enough to use anywhere.
float lt_interleaved_gradient_noise(float2 pixel)
{
    return frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
}

float lt_interleaved_gradient_noise(float2 pixel, uint frame)
{
    float2 jitter = lt_hash22(uint2(frame, frame * 1664525u + 1013904223u)) * 32.0;
    return lt_interleaved_gradient_noise(pixel + jitter);
}

// Blue-noise use case: bind a real blue-noise texture and sample it with wrapped
// UVs. These helpers only provide UV/frame rotation; they do not synthesize a
// true blue-noise spectrum procedurally.
float2 lt_blue_noise_uv(float2 pixel, float2 texture_size, uint frame)
{
    float2 safe_size = max(texture_size, float2(1.0, 1.0));
    float2 frame_offset = lt_hash22(uint2(frame, frame ^ 0x68BC21EBu)) * safe_size;
    return frac((pixel + frame_offset + 0.5) / safe_size);
}

float lt_sample_blue_noise(Texture2D<float> blue_noise,
                           SamplerState blue_noise_sampler,
                           float2 pixel,
                           float2 texture_size,
                           uint frame)
{
    return blue_noise.SampleLevel(blue_noise_sampler,
                                  lt_blue_noise_uv(pixel, texture_size, frame),
                                  0.0);
}

float4 lt_sample_blue_noise4(Texture2D<float4> blue_noise,
                             SamplerState blue_noise_sampler,
                             float2 pixel,
                             float2 texture_size,
                             uint frame)
{
    return blue_noise.SampleLevel(blue_noise_sampler,
                                  lt_blue_noise_uv(pixel, texture_size, frame),
                                  0.0);
}

#endif
