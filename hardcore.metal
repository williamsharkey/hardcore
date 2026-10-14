// hardcore's one shader pair (macOS), the Metal twin of the HLSL in
// hardcore.c: one 20-byte instance per quad (Inst in hardcore.c), expanded
// here into two triangles. Pixel rectangle, atlas rectangle in atlas pixels
// (u0 == 65535: a solid quad), RGBA8.
#include <metal_stdlib>
using namespace metal;

struct Inst { packed_short4 r; packed_ushort4 t; uchar4 c; };
struct VO { float4 p [[position]]; float2 uv; float4 c; };

vertex VO vs(const device Inst *q [[buffer(0)]], constant float2 &inv [[buffer(1)]],   // inv: 2 / frame size
             uint vid [[vertex_id]], uint iid [[instance_id]]) {
    Inst i = q[iid];
    uint k = vid < 3 ? vid : vid - 2;                  // 0 1 2, 1 2 3: the corners of a quad
    float2 f = float2(k & 1, k >> 1);
    float2 p = mix(float2(short2(i.r.xy)), float2(short2(i.r.zw)), f);
    VO o;
    o.p = float4(p.x * inv.x - 1, 1 - p.y * inv.y, 0, 1);
    o.uv = i.t.x == 65535 ? float2(-1, -1) : mix(float2(ushort2(i.t.xy)), float2(ushort2(i.t.zw)), f) / 4096.0;
    o.c = float4(i.c) / 255.0;
    return o;
}

fragment float4 fs(VO i [[stage_in]], texture2d<float> t [[texture(0)]]) {
    constexpr sampler s(filter::nearest);
    float a = i.uv.x < 0 ? 1.0 : t.sample(s, i.uv).r;
    return float4(i.c.rgb, i.c.a * a);
}
