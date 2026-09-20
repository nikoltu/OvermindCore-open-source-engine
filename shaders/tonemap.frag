#version 450
layout(set = 0, binding = 0) uniform sampler2D hdrTex;
layout(push_constant) uniform PC {
    float exposure;
    int   curve;
    int   keepAlpha;
} pc;
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

vec3 aces(vec3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}
vec3 reinhard(vec3 x) { return x / (1.0 + x); }

vec3 to_srgb(vec3 c) {
    vec3 lo = c * 12.92;
    vec3 hi = 1.055 * pow(max(c, vec3(0.0)), vec3(1.0 / 2.4)) - 0.055;
    return mix(hi, lo, vec3(lessThanEqual(c, vec3(0.0031308))));
}

void main() {
    vec3 hdr = texture(hdrTex, vUV).rgb * pc.exposure;
    vec3 mapped;
    if (pc.curve == 1) mapped = reinhard(hdr);
    else if (pc.curve == 2) mapped = clamp(hdr, 0.0, 1.0);
    else mapped = aces(hdr);
    outColor = vec4(to_srgb(clamp(mapped, 0.0, 1.0)), pc.keepAlpha != 0 ? texture(hdrTex, vUV).a : 1.0);
}
