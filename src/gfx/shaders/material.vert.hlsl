cbuffer Frame : register(b0, space1) {
    float4 frame;
    float4 camera[3];
    float4 optics;
    float4 geometryClip;
    float4 parameters[8];
    float4 ambient;
    float4 geometry;
    float4 mapping;
} // width, height, perspective, unused

struct Input {
    float2 position : TEXCOORD0;
    float2 uv : TEXCOORD1;
    float3 color : TEXCOORD2;
    float haze : TEXCOORD3;
    float inverseDepth : TEXCOORD4;
    float2 lightMap : TEXCOORD5;
    float opacity : TEXCOORD6;
};

struct Output {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float3 color : TEXCOORD1;
    float haze : TEXCOORD2;
    float2 lightMap : TEXCOORD3;
    float opacity : TEXCOORD4;
    float cameraDepth : TEXCOORD5;
    float sphereDistance : TEXCOORD6;
};

struct Instance {
    float4 camera[3], optics, geometryClip, parameters[8], ambient, geometry, mapping;
};

StructuredBuffer<Instance> instances : register(t0, space0);
static const uint nativeIndices[576] = {
    0, 2, 4, 4, 6, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 4, 1, 2, 4, 4, 6, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 2, 3, 0, 3, 4, 4, 6, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3, 4, 0, 1, 3, 1, 2, 3, 4,
    6, 0, 0, 0, 0, 0, 0, 0, 0, 2, 4, 4, 5, 0, 5, 6, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 4, 1, 2, 4,
    4, 5, 0, 5, 6, 0, 0, 0, 0, 0, 0, 0, 0, 2, 3, 0, 3, 4, 4, 5, 0, 5, 6, 0, 0, 0, 0, 0, 0, 0, 0, 3,
    4, 0, 1, 3, 1, 2, 3, 4, 5, 0, 5, 6, 0, 0, 0, 0, 0, 2, 4, 4, 6, 7, 4, 7, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 1, 4, 1, 2, 4, 4, 6, 7, 4, 7, 0, 0, 0, 0, 0, 0, 0, 0, 2, 3, 0, 3, 4, 4, 6, 7, 4, 7, 0,
    0, 0, 0, 0, 0, 0, 0, 3, 4, 0, 1, 3, 1, 2, 3, 4, 6, 7, 4, 7, 0, 0, 0, 0, 0, 2, 4, 4, 7, 0, 4, 5,
    7, 5, 6, 7, 0, 0, 0, 0, 0, 0, 0, 1, 4, 1, 2, 4, 4, 7, 0, 4, 5, 7, 5, 6, 7, 0, 0, 0, 0, 2, 3, 0,
    3, 4, 4, 7, 0, 4, 5, 7, 5, 6, 7, 0, 0, 0, 0, 3, 4, 0, 1, 3, 1, 2, 3, 4, 7, 0, 4, 5, 7, 5, 6, 7,
    6, 0, 2, 2, 4, 6, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 6, 0, 1, 6, 1, 2, 2, 4, 6, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 6, 0, 2, 2, 3, 6, 3, 4, 6, 0, 0, 0, 0, 0, 0, 0, 0, 0, 6, 0, 1, 6, 1, 2, 2, 3, 6, 3,
    4, 6, 0, 0, 0, 0, 0, 0, 6, 0, 2, 2, 4, 5, 2, 5, 6, 0, 0, 0, 0, 0, 0, 0, 0, 0, 6, 0, 1, 6, 1, 2,
    2, 4, 5, 2, 5, 6, 0, 0, 0, 0, 0, 0, 6, 0, 2, 2, 5, 6, 2, 3, 5, 3, 4, 5, 0, 0, 0, 0, 0, 0, 6, 0,
    1, 6, 1, 2, 2, 5, 6, 2, 3, 5, 3, 4, 5, 0, 0, 0, 6, 7, 2, 7, 0, 2, 2, 4, 6, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 6, 1, 2, 6, 7, 1, 7, 0, 1, 2, 4, 6, 0, 0, 0, 0, 0, 0, 6, 7, 2, 7, 0, 2, 2, 3, 6, 3, 4, 6,
    0, 0, 0, 0, 0, 0, 6, 1, 2, 6, 7, 1, 7, 0, 1, 2, 3, 6, 3, 4, 6, 0, 0, 0, 6, 7, 2, 7, 0, 2, 2, 4,
    5, 2, 5, 6, 0, 0, 0, 0, 0, 0, 6, 1, 2, 6, 7, 1, 7, 0, 1, 2, 4, 5, 2, 5, 6, 0, 0, 0, 6, 7, 2, 7,
    0, 2, 2, 5, 6, 2, 3, 5, 3, 4, 5, 0, 0, 0, 6, 1, 2, 6, 7, 1, 7, 0, 1, 2, 5, 6, 2, 3, 5, 3, 4, 5};
static const float2 nativeRing[8] = {float2(0, 0),
                                     float2(0, .5),
                                     float2(0, 1),
                                     float2(.5, 1),
                                     float2(1, 1),
                                     float2(1, .5),
                                     float2(1, 0),
                                     float2(.5, 0)};

float nativeLength(float3 p) {
    float squared = p.x * p.x + p.y * p.y + p.z * p.z;
    return asfloat((asuint(squared) >> 1) + 0x1fc00000U);
}

float3 shapeLight(Instance t, float3 position, float3 normal, float mode) {
    if (mode == 0)
        return float3(1, 1, 1);
    float3 color = t.ambient.xyz;
    for (uint i = 0; i < (uint)t.ambient.w; ++i) {
        float4 source = t.parameters[i * 4], intensity = t.parameters[i * 4 + 1];
        float4 attenuation = t.parameters[i * 4 + 2], state = t.parameters[i * 4 + 3];
        float amount = 0;
        if (source.w == 3) {
            if (attenuation.w == 0)
                continue;
            float3 delta = position - source.xyz;
            float distance = nativeLength(delta);
            if (attenuation.w >= 2) {
                if (attenuation.w == 3 && distance > intensity.w)
                    continue;
                amount = -dot(state.xyz, normal);
                if (amount < 0)
                    continue;
                amount *= state.w;
            } else {
                if (distance > intensity.w)
                    continue;
                amount = -dot(delta, normal);
                if (amount < 0)
                    continue;
                if (distance > .001) {
                    amount /= distance;
                    float formula =
                        attenuation.x + (attenuation.y + attenuation.z * distance) * distance;
                    if (formula > 1)
                        amount /= formula;
                } else
                    amount = 1;
            }
        } else {
            amount = -dot(source.xyz, normal);
            if (source.w == 2)
                amount = (amount + 1) * .5;
            else {
                if (amount < 0)
                    continue;
                if (source.w == 1) {
                    float a = amount - 1;
                    amount = 1 + a * a * a;
                }
            }
            amount *= state.w;
        }
        color += intensity.xyz * amount;
    }
    color = saturate(color);
    if (mode == 2) {
        float shade = min(1.0, color.x * .299 + color.z * .587 + color.y * .114);
        color = float3(shade, shade, shade);
    }
    return color;
}

Output main(Input v, uint instanceId : SV_InstanceID) {
    Output o;
    float w = frame.z != 0 ? 1.0 / v.inverseDepth : 1.0;
    o.position = float4(v.position.x * 2.0 / frame.x - 1.0,
                        1.0 - v.position.y * 2.0 / frame.y,
                        v.inverseDepth / 65536.0,
                        1.0) *
                 w;
    o.uv = v.uv;
    o.color = v.color;
    o.haze = v.haze;
    o.lightMap = v.lightMap;
    o.opacity = v.opacity;
    o.cameraDepth = 0;
    o.sphereDistance = 0;
    if (frame.w != 0) {
        float4 local = float4(v.position, v.inverseDepth, 1);
        float4 c0 = camera[0], c1 = camera[1], c2 = camera[2], lens = optics;
        if (frame.w == 2) {
            Instance t = instances[(uint)geometryClip.w + instanceId];
            if (t.geometry.z == 1) {
                uint ring = nativeIndices[(uint)t.geometry.w * 18 + (uint)v.inverseDepth];
                float2 xy = nativeRing[ring];
                float2 uv = float2(xy.x, 1 - xy.y);
                float4 h = t.parameters[ring];
                local = float4(xy * t.geometry.x, h.z >= 1 ? h.y : h.x + (h.y - h.x) * h.z, 1);
                o.uv = uv;
                o.lightMap = t.mapping.xy + uv * t.mapping.zw;
                o.sphereDistance = h.w / t.geometry.y;
                o.haze = t.ambient.w < t.ambient.x
                             ? saturate((h.w - t.ambient.w) / (t.ambient.x - t.ambient.w))
                             : 0;
            } else {
                o.color = shapeLight(t, local.xyz, v.color, v.opacity);
                o.opacity = 1;
                o.haze = t.geometry.x;
            }
            c0 = t.camera[0];
            c1 = t.camera[1];
            c2 = t.camera[2];
            lens = t.optics;
        }
        float3 p = float3(dot(local, c0), dot(local, c1), dot(local, c2));
        o.position = float4((p.x * lens.x + p.y * lens.z) * 2 / frame.x - p.y,
                            p.y - (p.y * lens.w - p.z * lens.y) * 2 / frame.y,
                            1.0 / 65536.0,
                            p.y);
        o.cameraDepth = p.y;
    }
    return o;
}
