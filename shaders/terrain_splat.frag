#version 450
layout(push_constant) uniform PC { mat4 world; vec4 team; } pc;
layout(set = 0, binding = 0) uniform sampler2DArray layerDiffuse;
layout(set = 0, binding = 1) uniform sampler2DArray layerWeight;
layout(set = 2, binding = 0) uniform Globals {
    mat4 viewProj;
    mat4 lightVP;
    vec4 shadowParams;
    vec4 eyePos;
    vec4 lightDir;
    vec4 terrainExtent;
} g;
layout(set = 2, binding = 1) uniform sampler2D shadowMap;
layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec2 vUV;
layout(location = 2) in vec4 vLightPos;
layout(location = 3) in vec3 vWorldPos;
layout(location = 0) out vec4 outColor;

float shadow_factor() {
    vec3 ndc = vLightPos.xyz / vLightPos.w;
    vec2 uv = ndc.xy * 0.5 + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || ndc.z > 1.0) return 1.0;
    float cur = ndc.z;
    float texel = g.shadowParams.x;
    float bias = g.shadowParams.y;
    float lit = 0.0;
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            float occ = texture(shadowMap, uv + vec2(dx, dy) * texel).r;
            lit += (cur - bias > occ) ? 0.0 : 1.0;
        }
    return lit / 9.0;
}

void main() {
    vec2 maskUV = g.terrainExtent.xy != vec2(0.0)
                      ? vWorldPos.xy / g.terrainExtent.xy
                      : vec2(0.0);
    vec3 acc = vec3(0.0);
    float wsum = 0.0;
    for (int L = 0; L < 8; ++L) {
        float w = texture(layerWeight, vec3(maskUV, float(L))).r;
        if (w <= 0.0) continue;
        acc += w * texture(layerDiffuse, vec3(vUV, float(L))).rgb;
        wsum += w;
    }
    vec3 diff = wsum > 0.0 ? acc / wsum
                           : texture(layerDiffuse, vec3(vUV, 0.0)).rgb;
    vec3 n = normalize(vNormal);
    vec3 l = normalize(g.lightDir.xyz);
    float dl = abs(dot(n, l));
    float shF = mix(1.0, shadow_factor(), g.shadowParams.z);
    float shade = 0.35 + 0.65 * dl * shF;
    outColor = vec4(diff * shade, 1.0);
}
