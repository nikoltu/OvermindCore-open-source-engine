#version 450
layout(push_constant) uniform PC { mat4 mvp; } pc;
layout(location = 0) in vec3 inPos;
layout(location = 1) in vec2 inUV;
layout(location = 2) in vec4 inColor;
layout(location = 3) in vec4 inFog;
layout(location = 0) out vec2 vUV;
layout(location = 1) out vec4 vColor;
layout(location = 2) out vec4 vFog;
void main() {
    gl_Position = pc.mvp * vec4(inPos, 1.0);
    vUV = inUV;
    vColor = inColor;
    vFog = inFog;
}
