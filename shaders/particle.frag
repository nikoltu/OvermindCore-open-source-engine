#version 450
layout(set = 0, binding = 0) uniform sampler2D diffuse;
layout(location = 0) in vec2 vUV;
layout(location = 1) in vec4 vColor;
layout(location = 0) out vec4 outColor;
void main() {
    vec4 t = texture(diffuse, vUV);
    outColor = vec4(t.rgb * vColor.rgb, t.a * vColor.a);
}
