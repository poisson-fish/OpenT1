#ifdef __spirv__
[[vk::combinedImageSampler]]
#endif
Texture2D<float4> image : register(t0, space2);
#ifdef __spirv__
[[vk::combinedImageSampler]]
#endif
SamplerState imageSampler : register(s0, space2);
#ifdef __spirv__
[[vk::combinedImageSampler]]
#endif
Texture2D<float4> lightMap : register(t1, space2);
#ifdef __spirv__
[[vk::combinedImageSampler]]
#endif
SamplerState lightMapSampler : register(s1, space2);
#ifdef __spirv__
[[vk::combinedImageSampler]]
#endif
Texture2DArray<float4> imageArray : register(t2, space2);
#ifdef __spirv__
[[vk::combinedImageSampler]]
#endif
SamplerState imageArraySampler : register(s2, space2);
cbuffer Material : register(b0, space3) {
    float4 tint; // RGB, opacity
    float4 hazeColor;
    float4 options; // inverse gamma, enable discard, mip LOD bias, alpha source
    float4 alphaControls; // alpha cutoff, zero-alpha texture cutout, unused, unused
    float4 geometryClip; // near, far, local geometry, unused
    float4 lightRect; // page UV scale/offset; (1,1,0,0) for standalone maps
    float4 arrayControls; // layer, enabled, unused, unused
}

float4 main(float4 position : SV_Position,
            float2 uv : TEXCOORD0,
            float3 color : TEXCOORD1,
            float haze : TEXCOORD2,
            float2 lightUV : TEXCOORD3,
            float opacity : TEXCOORD4,
            float cameraDepth : TEXCOORD5,
            float sphereDistance : TEXCOORD6,
            nointerpolation float4 instanceLightRect : TEXCOORD7,
            nointerpolation float3 instanceImageArray : TEXCOORD8)
    : SV_Target0 {
    if (geometryClip.z != 0 && (cameraDepth < geometryClip.x || cameraDepth > geometryClip.y))
        discard;
    if (sphereDistance > 1)
        discard;
    float4 rect = instanceImageArray.z != 0 ? instanceLightRect : lightRect;
    float2 array = instanceImageArray.z != 0 ? instanceImageArray.xy : arrayControls.xy;
    float4 texel = array.y != 0
        ? imageArray.SampleBias(imageArraySampler, float3(uv, array.x), options.z)
        : image.SampleBias(imageSampler, uv, options.z);
    float3 light = lightMap.Sample(lightMapSampler, saturate(lightUV) * rect.xy + rect.zw).rgb;
    float alpha = options.w == 0   ? texel.a * tint.a
                  : options.w == 1 ? tint.a
                  : options.w == 2 ? saturate(opacity) * tint.a
                  : options.w == 4 ? texel.a * saturate(opacity) * tint.a
                                   : 1.0;
    if (options.y != 0 && (alpha <= alphaControls.x || (alphaControls.y != 0 && texel.a <= 0)))
        discard;
    float3 rgb = lerp(texel.rgb * light * color * tint.rgb,
                      hazeColor.rgb,
                      saturate(haze));
    return float4(pow(saturate(rgb), options.xxx), alpha);
}
