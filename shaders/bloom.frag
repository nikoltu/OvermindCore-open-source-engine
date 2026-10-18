#version 450
layout(set = 0, binding = 0) uniform sampler2D src;
layout(push_constant) uniform PC {
    vec4 p;
    vec4 q;
} pc;
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

void main() {
    if (pc.p.x < 0.5) {
        vec3 c = texture(src, vUV + pc.p.zw * vec2(-1.0, -1.0)).rgb + texture(src, vUV + pc.p.zw * vec2(1.0, -1.0)).rgb +
                 texture(src, vUV + pc.p.zw * vec2(-1.0, 1.0)).rgb + texture(src, vUV + pc.p.zw * vec2(1.0, 1.0)).rgb;
        outColor = vec4(max(c * 0.25 * pc.q.z - pc.p.y, vec3(0.0)), 1.0);
        return;
    }
    const float sigma = max(pc.q.x, 0.5);
    const int radius = int(pc.q.y + 0.5);
    vec3 acc = texture(src, vUV).rgb;
    float wsum = 1.0;
    for (int i = 1; i <= radius; ++i) {
        const float w = exp(-0.5 * float(i * i) / (sigma * sigma));
        acc += (texture(src, vUV + pc.p.zw * float(i)).rgb + texture(src, vUV - pc.p.zw * float(i)).rgb) * w;
        wsum += 2.0 * w;
    }
    outColor = vec4(acc / wsum, 1.0);
}
