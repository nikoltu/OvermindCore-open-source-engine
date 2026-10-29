#version 450
#ifndef TQ
#define TQ 0
#endif
layout(push_constant) uniform PC { mat4 world; vec4 team; } pc;
layout(set = 0, binding = 0) uniform sampler2DArray layerDiffuse;
layout(set = 0, binding = 1) uniform sampler2DArray layerWeight;
layout(set = 0, binding = 2) uniform sampler2DArray layerNormal;
layout(set = 2, binding = 0) uniform Globals {
    mat4 viewProj;
    mat4 lightVP;
    vec4 shadowParams;
    vec4 eyePos;
    vec4 lightDir;
    vec4 terrainExtent;
    vec4 rigAmbient;
    vec4 rigDir[3];
    vec4 rigColor[3];
    vec4 rigParams;
    vec4 fogExtent;
    vec4 lightPos[64];
    vec4 lightCol[64];
    vec4 lightMeta;
    vec4 screen;
    vec4 shadowExtra;
    vec4 heightFog;
    vec4 heightFogColor;
    vec4 creepParams;
    vec4 terrainColorize;
    vec4 terrainColorize2;
    vec4 keySpec;
    vec4 lightSpot[64];
    vec4 lightCone[64];
    vec4 terrainHeight[8];
} g;

vec3 to_gamma(vec3 c) {
    vec3 lo = c * 12.92;
    vec3 hi = 1.055 * pow(max(c, vec3(0.0)), vec3(1.0 / 2.4)) - 0.055;
    return mix(hi, lo, vec3(lessThanEqual(c, vec3(0.0031308))));
}

vec3 point_lights(vec3 p, vec3 n) {
    vec3 c = vec3(0.0);
    const int cnt = int(g.lightMeta.x + 0.5);
    for (int i = 0; i < 64; ++i) {
        if (i >= cnt) break;
        vec3 d = g.lightPos[i].xyz - p;
        const float reach = max(max(g.lightPos[i].w, g.lightCol[i].w + 1e-3), 1e-3) * 1.0001;
        if (dot(d, d) > reach * reach) continue;
        float dist = length(d);
        float att = 1.0 - smoothstep(g.lightCol[i].w, max(g.lightPos[i].w, g.lightCol[i].w + 1e-3), dist);
        if (g.lightMeta.y > 0.5) { float q = clamp(1.0 - dist / max(g.lightPos[i].w, 1e-3), 0.0, 1.0); att = g.lightMeta.y > 1.5 ? q * q : q; }
        if (g.keySpec.w > 0.5) {
            const float far = max(g.lightPos[i].w, 1e-3);
            const float q = clamp((1.0 - dist / far) * far / max(far - g.lightCol[i].w, 1e-3), 0.0, 1.0);
            att = q * q;
        }
        if (g.lightCone[i].y > 0.5) {
            const float cone = clamp((dot(-d / max(dist, 1e-4), g.lightSpot[i].xyz) - g.lightSpot[i].w) * g.lightCone[i].x, 0.0, 1.0);
            att *= cone * cone;
        }
        if (att <= 0.0) continue;
        c += g.lightCol[i].rgb * att * max(dot(n, d / max(dist, 1e-4)), 0.0);
    }
    return c;
}

float ground_lambert(float nl) {
    return clamp((g.lightDir.w > 0.5 || pc.team.x > 0.5) ? 2.0 * nl - 1.0 : nl, 0.0, 1.0);
}
vec3 rig_shade(vec3 n, float shF, float dg) {
    if (g.keySpec.w > 0.5) {
        vec3 s = g.rigAmbient.rgb;
        for (int i = 0; i < 3; ++i) {
            if (g.rigDir[i].w < 0.5) continue;
            s += g.rigColor[i].rgb * ground_lambert(dot(n, -g.rigDir[i].xyz)) * (i == 0 ? shF : 1.0) * dg;
        }
        return s;
    }
    vec3 c = g.rigAmbient.rgb;
    for (int i = 0; i < 3; ++i) {
        if (g.rigDir[i].w < 0.5) continue;
        c += g.rigColor[i].rgb * max(dot(n, -g.rigDir[i].xyz), 0.0);
    }
    return c * mix(g.rigParams.x, 1.0, shF) * dg;
}
layout(set = 2, binding = 1) uniform sampler2DShadow shadowMap;
layout(set = 2, binding = 2) uniform sampler2D fogMap;
layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec2 vUV;
layout(location = 2) in vec4 vLightPos;
layout(location = 3) in vec3 vWorldPos;
layout(location = 0) out vec4 outColor;
vec3 fog_veil(vec3 rgb) {
    if (g.fogExtent.z < 0.5) return rgb;
    float a = texture(fogMap, vWorldPos.xy / g.fogExtent.xy).r;
    return rgb * (1.0 - a);
}

float shadow_factor() {
    vec3 ndc = vLightPos.xyz / vLightPos.w;
    vec2 uv = ndc.xy * 0.5 + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || ndc.z > 1.0) return 1.0;
    float cur = ndc.z;
    float texel = g.shadowParams.x;
    float bias = g.shadowParams.y;
    float lit = 0.0;
    if (g.shadowExtra.x > 0.0) {
        const vec2 kDisc[16] = vec2[16](
            vec2(-0.9420, -0.3991), vec2(0.9456, -0.7689), vec2(-0.0942, -0.9294), vec2(0.3450, 0.2939),
            vec2(-0.9159, 0.4577), vec2(-0.8154, -0.8791), vec2(-0.3828, 0.2768), vec2(0.9748, 0.7565),
            vec2(0.4432, -0.9751), vec2(0.5374, -0.4737), vec2(-0.2650, -0.4189), vec2(0.7920, 0.1909),
            vec2(-0.2419, 0.9971), vec2(-0.8141, 0.9144), vec2(0.1998, 0.7864), vec2(0.1438, -0.1410));
        const float ang = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453) * 6.2831853;
        const mat2 rot = mat2(cos(ang), sin(ang), -sin(ang), cos(ang));
        vec2 dzduv = vec2(0.0);
        {
            const vec3 dx = dFdx(vec3(uv, cur)), dy = dFdy(vec3(uv, cur));
            const float det = dx.x * dy.y - dx.y * dy.x;
            if (abs(det) > 1e-12) dzduv = vec2(dy.y * dx.z - dx.y * dy.z, dx.x * dy.z - dy.x * dx.z) / det;
            const float lim = 0.02 / max(texel * g.shadowExtra.x, 1e-6);
            dzduv = clamp(dzduv, vec2(-lim), vec2(lim));
        }
        for (int i = 0; i < 16; ++i) {
            const vec2 off = rot * kDisc[i] * texel * g.shadowExtra.x;
            lit += texture(shadowMap, vec3(uv + off, cur - bias + (g.shadowExtra.y > 0.5 ? dot(dzduv, off) : 0.0)));
        }
        return lit / 16.0;
    }
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            lit += texture(shadowMap, vec3(uv + vec2(dx, dy) * texel, cur - bias));
        }
    return lit / 9.0;
}
vec3 height_fog(vec3 rgb) {
    if (g.heightFog.w < 0.5) return rgb;
    if (g.heightFog.w > 1.5) {
        const vec3 vv = vWorldPos - g.eyePos.xyz;
        const float dist = length(vv);
        float fi = g.heightFog.x * dist * exp((g.heightFog.z - g.eyePos.z) * g.heightFog.y);
        if (abs(vv.z) > 0.01) {
            const float fT = clamp(g.heightFog.y * vv.z, -80.0, 80.0);
            fi = fT == 0.0 ? 0.0 : fi * (1.0 - exp(-fT)) / fT;
        }
        return mix(rgb, g.heightFogColor.rgb, clamp(fi, 0.0, 1.0));
    }
    float depth = max(g.heightFog.z + g.heightFog.y - vWorldPos.z, 0.0);
    float f = 1.0 - exp(-g.heightFog.x * depth);
    return mix(rgb, g.heightFogColor.rgb, clamp(f, 0.0, 1.0));
}

void main() {
    vec2 maskUV = g.terrainExtent.xy != vec2(0.0)
                      ? vWorldPos.xy / g.terrainExtent.xy
                      : vec2(0.0);
    vec3 acc = vec3(0.0);
    vec3 nacc = vec3(0.0);
    float wsum = 0.0;
    float sacc = 0.0;
#if TQ == 0
    const float kLodBias = -0.75;
#else
    const float kLodBias = 0.0;
#endif
    float lw[8];
    float lwMax = 0.0;
#if TQ == 2
    const bool heightBlend = false;
#else
    const bool heightBlend = g.terrainHeight[0].w > 0.5;
#endif
    const vec4 wLo = texture(layerWeight, vec3(maskUV, 0.0));
    const vec4 wHi = texture(layerWeight, vec3(maskUV, 1.0));
    lw[0] = wLo.r; lw[1] = wLo.g; lw[2] = wLo.b; lw[3] = wLo.a;
    lw[4] = wHi.r; lw[5] = wHi.g; lw[6] = wHi.b; lw[7] = wHi.a;
    for (int L = 0; L < 8; ++L) {
        if (heightBlend && lw[L] > 0.0 && g.terrainHeight[L].z > 0.5)
            lw[L] *= max(texture(layerNormal, vec3(vUV, float(L)), kLodBias).a * g.terrainHeight[L].x + g.terrainHeight[L].y, 0.0);
        lwMax = max(lwMax, lw[L]);
    }
    if (heightBlend)
        for (int L = 0; L < 8; ++L) lw[L] *= 1.0 - clamp(lwMax - lw[L], 0.0, 1.0);
#if TQ != 0
    {
        const int kKeep = TQ == 1 ? 3 : 2;
        float keep[8];
        float top = 0.0;
        for (int L = 0; L < 8; ++L) {
            keep[L] = 0.0;
            top = max(top, lw[L]);
        }
        for (int k = 0; k < kKeep; ++k) {
            int best = -1;
            float bw = TQ == 1 ? top * 0.08 : 0.0;
            for (int L = 0; L < 8; ++L)
                if (keep[L] == 0.0 && lw[L] > bw) {
                    bw = lw[L];
                    best = L;
                }
            if (best < 0) break;
            keep[best] = 1.0;
        }
        for (int L = 0; L < 8; ++L) lw[L] *= keep[L];
    }
#endif
    for (int L = 0; L < 8; ++L) {
        const float w = lw[L];
        if (w <= 0.0) continue;
        const vec4 ld = texture(layerDiffuse, vec3(vUV, float(L)), kLodBias + g.terrainColorize2.w);
        acc += w * (g.keySpec.w > 0.5 ? to_gamma(ld.rgb) : ld.rgb);
        sacc += w * ld.a;
#if TQ != 2
        nacc += w * (texture(layerNormal, vec3(vUV, float(L)), kLodBias + g.terrainColorize2.w).xyz * 2.0 - 1.0);
#endif
        wsum += w;
    }
    vec3 diff = acc / max(wsum, 1e-30);
    if (!(wsum > 0.0)) {
        diff = texture(layerDiffuse, vec3(vUV, 0.0)).rgb;
        if (g.keySpec.w > 0.5) diff = to_gamma(diff);
    }
    diff *= texture(layerWeight, vec3(maskUV, 2.0)).r;
    vec3 N = normalize(vNormal);
    vec3 n = N;
    if (wsum > 0.0 && dot(nacc, nacc) > 1e-6) {
        vec3 nt = nacc / wsum;
        nt.xy *= max(g.terrainColorize2.z, 1.0);
        nt = normalize(nt);
        const vec3 T = normalize(vec3(1.0, 0.0, 0.0) - N * N.x);
        const vec3 B = cross(N, T);
        n = normalize(T * nt.x + B * nt.y + N * max(nt.z, 0.05));
    }
    vec3 l = -normalize(g.lightDir.xyz);
    float dl = abs(dot(n, l));
    float shF = mix(1.0, shadow_factor(), g.shadowParams.z);
    float shade = 0.35 + 0.65 * dl * shF;
    const float dgain = g.rigAmbient.w > 0.5 ? max(g.terrainColorize.x, 0.0) : 1.0;
    vec3 shadeRGB = g.rigAmbient.w > 0.5 ? rig_shade(n, shF, dgain) : vec3(shade);
    vec3 spec = vec3(0.0);
    if (g.terrainColorize.y > 0.0 && g.rigAmbient.w > 0.5 && g.rigDir[0].w > 0.5 && wsum > 0.0) {
        const vec3 v = normalize(g.eyePos.xyz - vWorldPos);
        const vec3 h = normalize(v - g.rigDir[0].xyz);
        spec = g.rigColor[0].rgb * pow(max(dot(n, h), 0.0), max(g.terrainColorize.z, 1.0)) * g.terrainColorize.y * (sacc / wsum) * shF;
        if (g.keySpec.w > 0.5) {
            const bool dblGate = (g.lightDir.w > 0.5 || pc.team.x > 0.5) && (int(fract(g.lightDir.w) * 8.0 + 0.5) & 2) == 0;
            spec = g.keySpec.rgb * pow(max(dot(n, h), 0.0), max(g.terrainColorize.z, 1.0)) * step(dblGate ? 0.5 : 0.0, dot(n, -g.rigDir[0].xyz)) *
                   g.terrainColorize.y * (sacc / wsum) * shF;
            if (g.terrainColorize.w < 0.5) {
                const float pw = clamp(g.terrainColorize.z, 1.0, 512.0);
                spec *= clamp(-0.000004444 * pw * pw + 0.004333 * pw + 0.0020834, 0.0, 1.0);
            }
        }
    }
    const vec3 total = diff * (shadeRGB + point_lights(vWorldPos, n) * (g.keySpec.w > 0.5 ? 1.0 : dgain)) + spec;
    outColor = vec4(fog_veil(height_fog(total)), 1.0);
    if (g.screen.w > 0.5 && g.keySpec.w > 0.5 && g.rigAmbient.w > 0.5) {
        const vec3 key = g.rigDir[0].w > 0.5 ? g.rigColor[0].rgb * ground_lambert(dot(n, -g.rigDir[0].xyz)) * shF * dgain : vec3(0.0);
        const vec3 occl = diff * max(shadeRGB - key, vec3(0.0));
        outColor.a = 1.0 - clamp(max(occl.r, max(occl.g, occl.b)) / max(max(total.r, max(total.g, total.b)), 1e-4), 0.0, 1.0);
    }
}
