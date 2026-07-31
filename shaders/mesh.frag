#version 450
layout(push_constant) uniform PC { mat4 mvp; vec4 team; } pc;
layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec2 vUV;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform sampler2D diffuse;
layout(set = 0, binding = 1) uniform sampler2D normalMap;
layout(set = 0, binding = 2) uniform sampler2D emissive;
void main() {
    if (pc.team.a >= 1.5) { outColor = vec4(pc.team.rgb, 1.0); return; }
    vec3 n = normalize(vNormal);
    vec3 nt = texture(normalMap, vUV).xyz * 2.0 - 1.0;
    n = normalize(n + vec3(nt.x, nt.y, 0.0) * 0.5);
    vec3 l = normalize(vec3(0.4, 0.6, 0.7));
    float d = abs(dot(n, l));
    float shade = 0.35 + 0.65 * d;
    vec4 t = texture(diffuse, vUV);
    vec3 base = pc.team.a >= 0.0 ? mix(pc.team.rgb, t.rgb, t.a) : t.rgb;
    vec3 lit = base * shade;
    vec3 emis = texture(emissive, vUV).rgb;
    outColor = vec4(lit + emis, 1.0);
}
