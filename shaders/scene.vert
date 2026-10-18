#version 450
layout(push_constant) uniform PC { mat4 world; vec4 team; } pc;
layout(set = 1, binding = 0) readonly buffer Bones { mat4 bone[]; };
layout(set = 2, binding = 0) uniform Globals {
    mat4 viewProj;
    mat4 lightVP;
    vec4 shadowParams;
    vec4 eyePos;
} g;
layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec4 inWeight;
layout(location = 4) in uvec4 inBone;
layout(location = 5) in vec2 inUV1;
layout(location = 6) in vec4 inColor;
layout(location = 0) out vec3 vNormal;
layout(location = 1) out vec2 vUV;
layout(location = 2) out vec4 vLightPos;
layout(location = 3) out vec3 vWorldPos;
layout(location = 4) out float vVAlpha;
layout(location = 5) out float vVShade;
layout(location = 6) out vec2 vUV1;
layout(location = 7) out vec3 vVColor;
layout(location = 8) out float vVAO;
void main() {
    float wsum = inWeight.x + inWeight.y + inWeight.z + inWeight.w;
    vec4 p;
    vec3 nrm;
    if (wsum > 0.001) {
        p = vec4(0.0);
        nrm = vec3(0.0);
        for (int k = 0; k < 4; ++k) {
            if (inWeight[k] > 0.0) {
                mat4 M = bone[inBone[k]];
                p += inWeight[k] * (M * vec4(inPos, 1.0));
                nrm += inWeight[k] * (mat3(M) * inNormal);
            }
        }
    } else {
        p = vec4(inPos, 1.0);
        nrm = inNormal;
    }
    vec4 wp = pc.world * p;
    gl_Position = g.viewProj * wp;
    vNormal = mat3(pc.world) * nrm;
    vUV = inUV;
    vUV1 = inUV1;
    vVColor = inColor.rgb;
    vVAO = inColor.a;
    vLightPos = g.lightVP * wp;
    vWorldPos = wp.xyz;
    vVAlpha = (inWeight.w == 0.0 && (inBone.w & 0x10000u) != 0u) ? float(inBone.w & 255u) / 255.0 : 2.0;
    vVShade = (inWeight.w == 0.0 && (inBone.w & 0x10000u) != 0u) ? 1.0 - float((inBone.w >> 8) & 255u) / 255.0 : 1.0;
}
