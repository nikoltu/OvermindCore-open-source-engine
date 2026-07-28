#version 450
layout(push_constant) uniform PC { mat4 mvp; vec4 team; } pc;
layout(set = 1, binding = 0) readonly buffer Bones { mat4 bone[]; };
layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec4 inWeight;
layout(location = 4) in uvec4 inBone;
layout(location = 0) out vec3 vNormal;
layout(location = 1) out vec2 vUV;
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
    gl_Position = pc.mvp * p;
    vNormal = nrm;
    vUV = inUV;
}
