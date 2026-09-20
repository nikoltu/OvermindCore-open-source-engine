#version 450
layout(push_constant) uniform PC { mat4 world; vec4 team; vec4 mat; vec4 anim; } pc;
layout(set = 0, binding = 0) uniform sampler2D diffuse;
layout(set = 0, binding = 1) uniform sampler2D normalMap;
layout(set = 0, binding = 2) uniform sampler2D emissive;
layout(set = 0, binding = 3) uniform sampler2D specular;
layout(set = 0, binding = 4) uniform sampler2D alphaMask;
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
    vec4 lightPos[16];
    vec4 lightCol[16];
    vec4 lightMeta;
} g;

vec3 point_lights(vec3 p, vec3 n) {
    vec3 c = vec3(0.0);
    const int cnt = int(g.lightMeta.x + 0.5);
    for (int i = 0; i < 16; ++i) {
        if (i >= cnt) break;
        vec3 d = g.lightPos[i].xyz - p;
        float dist = length(d);
        float att = 1.0 - smoothstep(g.lightCol[i].w, max(g.lightPos[i].w, g.lightCol[i].w + 1e-3), dist);
        if (att <= 0.0) continue;
        c += g.lightCol[i].rgb * att * max(dot(n, d / max(dist, 1e-4)), 0.0);
    }
    return c;
}

vec3 rig_shade(vec3 n, float shF) {
    vec3 c = g.rigAmbient.rgb;
    for (int i = 0; i < 3; ++i) {
        if (g.rigDir[i].w < 0.5) continue;
        c += g.rigColor[i].rgb * max(dot(n, -g.rigDir[i].xyz), 0.0);
    }
    return c * mix(g.rigParams.x, 1.0, shF);
}
layout(set = 2, binding = 1) uniform sampler2D shadowMap;
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
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            float occ = texture(shadowMap, uv + vec2(dx, dy) * texel).r;
            lit += (cur - bias > occ) ? 0.0 : 1.0;
        }
    return lit / 9.0;
}

void main() {
    if (pc.team.a >= 1.5) { outColor = vec4(fog_veil(pc.team.rgb), 1.0); return; }
    vec3 N = normalize(vNormal);
    vec3 nt = texture(normalMap, vUV).xyz * 2.0 - 1.0;
    vec3 dp1 = dFdx(vWorldPos), dp2 = dFdy(vWorldPos);
    vec2 du1 = dFdx(vUV), du2 = dFdy(vUV);
    vec3 dp2perp = cross(dp2, N), dp1perp = cross(N, dp1);
    vec3 T = dp2perp * du1.x + dp1perp * du2.x;
    vec3 B = dp2perp * du1.y + dp1perp * du2.y;
    float tb = max(dot(T, T), dot(B, B));
    vec3 n = tb > 1e-12
                 ? normalize(mat3(T * inversesqrt(tb), B * inversesqrt(tb), N) * nt)
                 : N;
    vec3 l = -normalize(g.lightDir.xyz);
    float dl = abs(dot(n, l));
    float shF = mix(1.0, shadow_factor(), g.shadowParams.z);
    float shade = 0.35 + 0.65 * dl * shF;
    vec3 shadeRGB = g.rigAmbient.w > 0.5 ? rig_shade(n, shF) : vec3(shade);
    vec4 t = texture(diffuse, vUV + pc.anim.xy);
    if (pc.team.a <= -1.5 && t.a < 0.5) discard;
    const bool blended = pc.mat.y > 1.5 || (pc.mat.y > 0.5 && pc.mat.z > 0.5);
    vec3 base = (!blended && pc.team.a >= 0.0) ? mix(pc.team.rgb, t.rgb, t.a) : t.rgb;
    vec3 lit = base * (shadeRGB + point_lights(vWorldPos, n)) * pc.anim.w;
    vec3 emis = texture(emissive, vUV).rgb * pc.anim.z;

    vec3 specColor = texture(specular, vUV).rgb;
    vec3 spec = vec3(0.0);
    if (dot(specColor, specColor) > 0.0) {
        vec3 v = normalize(g.eyePos.xyz - vWorldPos);
        vec3 h = normalize(l + v);
        float gloss = pc.mat.x > 0.0 ? pc.mat.x : max(g.shadowParams.w, 1.0);
        float sp = pow(max(dot(n, h), 0.0), gloss);
        spec = specColor * sp * dl * shF * g.eyePos.w;
    }
    float cover = 1.0;
    if (abs(pc.mat.y - 3.0) < 0.5) {
        cover = t.a;
    } else if (abs(pc.mat.y - 1.0) < 0.5 && pc.mat.z > 0.5) {
        const vec4 am = texture(alphaMask, vUV);
        const int ch = int(pc.mat.z + 0.5);
        cover = ch == 1 ? am.a : ch == 2 ? am.r : ch == 3 ? am.g : am.b;
    }
    outColor = vec4(fog_veil(lit + spec + emis), cover);
}
