// hardcore's one shader pair (macOS). The vertex layout matches Vtx in
// hardcore.c: pixel → clip position, atlas uv (u < 0: a solid quad), RGBA8.
#include <metal_stdlib>
using namespace metal;

struct Vtx { packed_float2 p; packed_float2 uv; uchar4 c; };
struct VO { float4 p [[position]]; float2 uv; float4 c; };

vertex VO vs(const device Vtx *v [[buffer(0)]], uint id [[vertex_id]]) {
    VO o;
    o.p = float4(float2(v[id].p), 0, 1);
    o.uv = float2(v[id].uv);
    o.c = float4(v[id].c) / 255.0;
    return o;
}

fragment float4 fs(VO i [[stage_in]], texture2d<float> t [[texture(0)]]) {
    constexpr sampler s(filter::nearest);
    float a = i.uv.x < 0 ? 1.0 : t.sample(s, i.uv).r;
    return float4(i.c.rgb, i.c.a * a);
}
