#version 450
layout(push_constant) uniform PC { mat4 world; vec4 team; vec4 mask; } pc;
layout(set = 0, binding = 0) uniform sampler2D diffuse;
layout(set = 0, binding = 4) uniform sampler2D alphaMask;
layout(location = 0) in vec2 vUV;
layout(location = 1) in float vVAlpha;
void main() {
    if (vVAlpha <= 1.0 && vVAlpha < 0.9) discard;
    const bool maskCut = pc.mask.x > 1.5;
    if (!maskCut && pc.team.a <= -1.5 && pc.mask.z > 0.0 && texture(diffuse, vUV).a < pc.mask.z) discard;
    if (pc.mask.x > 0.0) {
        vec4 m = texture(alphaMask, vUV);
        float cov = pc.mask.y < 1.5 ? m.a : pc.mask.y < 2.5 ? m.r : pc.mask.y < 3.5 ? m.g : m.b;
        if (cov < (maskCut ? pc.mask.z : 0.5)) discard;
    }
}
