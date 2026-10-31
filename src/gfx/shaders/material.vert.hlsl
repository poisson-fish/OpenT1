cbuffer Frame : register(b0, space1) {
    float4 frame;
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
};

Output main(Input v) {
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
    return o;
}
