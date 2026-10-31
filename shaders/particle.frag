#version 450
layout(push_constant) uniform PC { mat4 mvp; vec4 flags; vec4 mask2; vec4 soft; vec4 emis; } pc;
layout(set = 0, binding = 0) uniform sampler2D diffuse;
layout(set = 0, binding = 1) uniform sampler2D alphaMask;
layout(set = 0, binding = 2) uniform sampler2D alphaMask2;
layout(set = 0, binding = 3) uniform sampler2D sceneDepth;
layout(set = 0, binding = 4) uniform sampler2D emissiveTex;
layout(location = 0) in vec2 vUV;
layout(location = 1) in vec4 vColor;
layout(location = 2) in vec4 vFog;
layout(location = 0) out vec4 outColor;
vec3 to_gamma(vec3 c) {
    vec3 lo = c * 12.92;
    vec3 hi = 1.055 * pow(max(c, vec3(0.0)), vec3(1.0 / 2.4)) - 0.055;
    return mix(hi, lo, vec3(lessThanEqual(c, vec3(0.0031308))));
}
float view_z(float d) { return pc.soft.y * pc.soft.z / (pc.soft.z - d * (pc.soft.z - pc.soft.y)); }
void main() {
    vec4 t = texture(diffuse, vUV);
    const float eGain = floor(pc.emis.w / 10.0);
    float eLow = pc.emis.w - 10.0 * eGain;
    const bool teamOp = eLow > 7.5;
    if (teamOp) eLow -= 8.0;
    if (mod(eLow, 2.0) > 0.5) t = vec4(t.aaa, t.a);
    if (pc.flags.x > 0.5) t.rgb = to_gamma(t.rgb);
    float cover = 1.0;
    if (pc.flags.y > 0.5) {
        const vec4 m = texture(alphaMask, vUV);
        const float ch = pc.flags.y < 1.5 ? m.a : (pc.flags.y < 2.5 ? m.r : (pc.flags.y < 3.5 ? m.g : m.b));
        cover = clamp(ch * pc.flags.z + pc.flags.w, 0.0, 1.0);
    }
    if (pc.mask2.x > 0.5) {
        const vec4 m = texture(alphaMask2, vUV);
        const float ch = pc.mask2.x < 1.5 ? m.a : (pc.mask2.x < 2.5 ? m.r : (pc.mask2.x < 3.5 ? m.g : m.b));
        cover *= clamp(ch * pc.mask2.y + pc.mask2.z, 0.0, 1.0);
    }
    const bool maskCovers = pc.flags.y > 0.5 && pc.mask2.w > -0.5 && pc.mask2.w < 1.5;
    vec3 lit = teamOp ? clamp(t.a, 0.0, 1.0) * pc.emis.rgb * vColor.rgb : t.rgb * vColor.rgb;
    if (eLow > 3.5) {
        lit = mix(vColor.rgb, t.rgb, t.a);
        if (pc.emis.r + pc.emis.g + pc.emis.b > 0.0) lit *= pc.emis.rgb;
        t.a = 1.0;
    }
    if (pc.soft.w > 0.5) lit = min(lit, vec3(1.0));
    if (!teamOp && eLow < 3.5) lit += pc.emis.rgb * vColor.rgb;
    if (eGain > 0.5) {
        const vec4 e = texture(emissiveTex, vUV);
        vec3 ec = eLow > 1.5 ? vec3(e.a) : e.rgb;
        if (pc.flags.x > 0.5) ec = to_gamma(ec);
        lit += ec * (eGain * 0.01) * vColor.rgb;
    }
    outColor = vec4(mix(lit, vFog.rgb, vFog.a), (maskCovers ? 1.0 : t.a) * vColor.a * cover);
    if (pc.soft.x > 0.0) {
        const float d = max(0.0, view_z(texelFetch(sceneDepth, ivec2(gl_FragCoord.xy), 0).r) - view_z(gl_FragCoord.z));
        const float fade = d < pc.soft.x ? d / pc.soft.x : 1.0;
        outColor.a *= fade;
        if (pc.mask2.w > 1.5 && pc.mask2.w < 2.5) outColor.rgb *= fade;
    }
    if (pc.mask2.w > 2.2 && pc.mask2.w < 2.3) outColor.rgb *= t.a;
    if (pc.mask2.w > 1.5 && pc.mask2.w < 2.5) outColor.a = clamp(max(outColor.r, max(outColor.g, outColor.b)), 0.0, 1.0);
}
