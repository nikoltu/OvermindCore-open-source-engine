#version 450
layout(push_constant) uniform PC { mat4 world; vec4 team; vec4 mask; } pc;
layout(set = 1, binding = 0) readonly buffer Bones { mat4 bone[]; };
layout(set = 2, binding = 0) uniform Globals {
    mat4 viewProj;
    mat4 lightVP;
    vec4 shadowParams;
} g;
layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec4 inWeight;
layout(location = 4) in uvec4 inBone;
layout(location = 0) out vec2 vUV;
layout(location = 1) out float vVAlpha;
void main() {
    vUV = inUV;
    vVAlpha = (inWeight.w == 0.0 && (inBone.w & 0x10000u) != 0u) ? float(inBone.w & 255u) / 255.0 : 2.0;
    float wsum = inWeight.x + inWeight.y + inWeight.z + inWeight.w;
    vec4 p;
    if (wsum > 0.001) {
        p = vec4(0.0);
        for (int k = 0; k < 4; ++k)
            if (inWeight[k] > 0.0) p += inWeight[k] * (bone[inBone[k]] * vec4(inPos, 1.0));
    } else {
        p = vec4(inPos, 1.0);
    }
    gl_Position = g.lightVP * (pc.world * p);
}
