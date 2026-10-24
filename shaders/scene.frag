#version 450
layout(push_constant) uniform PC { mat4 world; vec4 team; vec4 mat; vec4 anim; vec4 tint; } pc;
layout(set = 0, binding = 0) uniform sampler2D diffuse;
layout(set = 0, binding = 1) uniform sampler2D normalMap;
layout(set = 0, binding = 2) uniform sampler2D emissive;
layout(set = 0, binding = 3) uniform sampler2D specular;
layout(set = 0, binding = 4) uniform sampler2D alphaMask;
layout(set = 0, binding = 5) uniform sampler2D envMap;
layout(set = 0, binding = 6) uniform sampler2D envMask;
layout(set = 0, binding = 8) uniform sampler2D aoMap;
layout(set = 0, binding = 9) uniform sampler2D alphaMask2;
layout(set = 0, binding = 10) uniform sampler2D emissive2;
layout(set = 0, binding = 7) uniform sampler2D decalTex;
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
    vec4 rigSpec[3];
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

vec3 point_spec(vec3 p, vec3 n, vec3 v, float gloss) {
    vec3 c = vec3(0.0);
    const int cnt = int(g.lightMeta.x + 0.5);
    for (int i = 0; i < 64; ++i) {
        if (i >= cnt) break;
        vec3 d = g.lightPos[i].xyz - p;
        const float dist = length(d);
        const float far = max(g.lightPos[i].w, 1e-3);
        const float q = clamp((1.0 - dist / far) * far / max(far - g.lightCol[i].w, 1e-3), 0.0, 1.0);
        float att = q * q;
        d /= max(dist, 1e-4);
        if (g.lightCone[i].y > 0.5) {
            const float cone = clamp((dot(-d, g.lightSpot[i].xyz) - g.lightSpot[i].w) * g.lightCone[i].x, 0.0, 1.0);
            att *= cone * cone;
        }
        if (att <= 0.0 || dot(n, d) <= 0.0) continue;
        c += g.lightCol[i].rgb * att * pow(max(dot(n, normalize(d + v)), 0.0), gloss);
    }
    return c * g.lightMeta.w;
}

bool gDblLambert = false;
bool gGroundBake = false;
vec3 rig_shade(vec3 n, float shF, float ao) {
    if (g.keySpec.w > 0.5) {
        vec3 s = g.rigAmbient.rgb * g.rigParams.y * ao;
        for (int i = 0; i < 3; ++i) {
            if (g.rigDir[i].w < 0.5) continue;
            const float nl = dot(n, -g.rigDir[i].xyz);
            const bool bakeLit = gGroundBake && g.lightDir.w > 0.5 && (int(fract(g.lightDir.w) * 8.0 + 0.5) & 4) == 0;
            s += g.rigColor[i].rgb * (bakeLit ? g.terrainColorize.x : gGroundBake ? 1.0 : g.rigParams.z) *
                 clamp((floor(g.lightDir.w) > 1.5 || gDblLambert || bakeLit) ? 2.0 * nl - 1.0 : nl, 0.0, 1.0) * (i == 0 ? shF : ao);
        }
        return s;
    }
    vec3 c = g.rigAmbient.rgb;
    for (int i = 0; i < 3; ++i) {
        if (g.rigDir[i].w < 0.5) continue;
        c += g.rigColor[i].rgb * max(dot(n, -g.rigDir[i].xyz), 0.0);
    }
    return c * mix(g.rigParams.x, 1.0, shF);
}
layout(set = 2, binding = 1) uniform sampler2DShadow shadowMap;
layout(set = 2, binding = 2) uniform sampler2D fogMap;
struct FxRec { vec4 head; vec4 p0; vec4 p1; vec4 uv; vec4 teamEmissive; vec4 maskX; vec4 mask2X; vec4 mask2Lv; vec4 fresD; vec4 fresM; vec4 fresM2; vec4 fresS; vec4 fresE; vec4 fresEM; vec4 maskO; vec4 emis2; vec4 emis2T; vec4 emisUV; };
layout(std430, set = 2, binding = 3) readonly buffer InstFx { FxRec fx[]; };
layout(set = 2, binding = 4) uniform sampler2D sceneCopy;
layout(set = 2, binding = 5) uniform sampler2D creepMap;
layout(set = 2, binding = 6) uniform sampler2D sceneDepth;
const int FX_CLOAK = 1, FX_DISSOLVE = 2, FX_GROUNDCLIP = 3, FX_HIGHLIGHT = 4, FX_FLASH = 5, FX_WATER = 6, FX_CREEP = 7;
float fx_hash(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }
layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec2 vUV;
layout(location = 2) in vec4 vLightPos;
layout(location = 3) in vec3 vWorldPos;
layout(location = 4) in float vVAlpha;
layout(location = 5) in float vVShade;
layout(location = 6) in vec2 vUV1;
layout(location = 7) in vec3 vVColor;
layout(location = 8) in float vVAO;
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

float layer_fresnel(vec4 fr, vec3 nrm) {
    if (fr.w < 0.5) return 1.0;
    float ft = pow(max(1.0 - abs(dot(normalize(nrm), normalize(g.eyePos.xyz - vWorldPos))), 0.0001), fr.x);
    if (fr.w > 1.5) ft = 1.0 - ft;
    return clamp(ft * fr.z + fr.y, 0.0, 1.0);
}

void main() {
    const FxRec F = fx[int(pc.tint.w + 0.5)];
    const int fxKind = int(floor(F.head.x + 0.0001));
    const int uvBits = int((F.head.x - float(fxKind)) * 4096.0 + 0.5);
    const bool noLocalLights = (uvBits & 16) != 0;
    const bool planarDiffuse = (uvBits & 4) != 0;
    const bool maskUV1 = (uvBits & 2) != 0;
    gDblLambert = (uvBits & 1) != 0;
    if (fxKind == FX_CREEP && F.teamEmissive.a > 0.2 && F.teamEmissive.a < 0.3) gDblLambert = true;
    gGroundBake = fxKind == 0 && F.p0.w > 2.5;
    const bool specAlphaOnly = (uvBits & 8) != 0;
    if (fxKind == FX_GROUNDCLIP && vWorldPos.z < F.p0.x) discard;
    if (fxKind == FX_WATER) {
        const float tsec = F.head.y;
        const vec2 til = F.uv.xy == vec2(0.0) ? vec2(0.125) : F.uv.xy;
        const float ca = cos(F.head.z), sa = sin(F.head.z);
        const vec2 wuv = mat2(ca, sa, -sa, ca) * vWorldPos.xy;
        const vec3 n1 = texture(normalMap, wuv * til.x + F.teamEmissive.xy * tsec).xyz * 2.0 - 1.0;
        const vec3 n2 = texture(normalMap, wuv * til.y + F.teamEmissive.zw * tsec).xyz * 2.0 - 1.0;
        const vec3 wn = normalize(vec3(n1.xy + n2.xy, max(n1.z + n2.z, 0.2)));
        const vec2 suv = gl_FragCoord.xy / max(g.screen.xy, vec2(1.0));
        const vec3 bed = texture(sceneCopy, clamp(suv + wn.xy * F.uv.z, vec2(0.0), vec2(1.0))).rgb;
        const float shW = pc.team.r > 0.5 ? mix(1.0, shadow_factor(), g.shadowParams.z) : 1.0;
        const vec3 lightW = g.rigAmbient.w > 0.5 ? rig_shade(wn, shW, 1.0) : vec3(0.35 + 0.65 * max(wn.z, 0.0) * shW);
        float tintBlend = clamp(F.p0.a, 0.0, 1.0);
        if (g.creepParams.z > 0.0 && F.head.w > 0.0) {
            const float zn = g.creepParams.z, zf = g.creepParams.w;
            const float dBed = zn * zf / (zf - texelFetch(sceneDepth, ivec2(gl_FragCoord.xy), 0).r * (zf - zn));
            const float dWat = zn * zf / (zf - gl_FragCoord.z * (zf - zn));
            tintBlend = clamp(F.head.w * max(0.0, dBed - dWat), tintBlend, 1.0);
        }
        vec3 bedC = bed;
        if (F.emisUV.y > 0.0 && g.creepParams.z > 0.0) {
            const float zn = g.creepParams.z, zf = g.creepParams.w;
            const float dBedC = zn * zf / (zf - texelFetch(sceneDepth, ivec2(gl_FragCoord.xy), 0).r * (zf - zn));
            const float dWatC = zn * zf / (zf - gl_FragCoord.z * (zf - zn));
            const float depthC = max(0.0, dBedC - dWatC);
            const vec3 eyeDir = normalize(vWorldPos - g.eyePos.xyz);
            const vec3 bottom = vWorldPos + eyeDir * depthC;
            vec2 cuv = F.emisUV.x * bottom.xy;
            cuv.y = 1.0 - cuv.y;
            const vec3 caus = (F.emisUV.z > 0.5 ? texture(alphaMask, cuv).rgb : texture(diffuse, cuv).rgb) * shW;
            bedC += caus * clamp(F.emisUV.y * depthC, 0.0, 1.0);
        }
        vec3 col = g.keySpec.w > 0.5 ? mix(bedC, F.p0.rgb, tintBlend) * lightW
                                     : mix(bedC, F.p0.rgb * lightW, tintBlend);
        const vec3 v = normalize(g.eyePos.xyz - vWorldPos);
        const float fres = clamp(pow(1.0 - max(dot(wn, v), 0.0), max(F.p1.z, 1.0)) + F.p1.w, 0.0, 1.0);
        const vec3 envc = g.rigAmbient.w > 0.5 ? g.rigAmbient.rgb + g.rigColor[0].rgb * 0.5 : vec3(0.5);
        col = mix(col, envc, fres * (1.0 - clamp(length(wn.xy) * F.uv.w * 40.0, 0.0, 0.5)));
        const vec3 l = -normalize(g.lightDir.xyz);
        const vec3 h = normalize(l + v);
        const vec3 keyc = g.rigAmbient.w > 0.5 ? g.rigColor[0].rgb : vec3(1.0);
        col += keyc * pow(max(dot(wn, h), 0.0), max(F.p1.x, 1.0)) * F.p1.y * shW;
        outColor = vec4(fog_veil(height_fog(col)), 1.0);
        return;
    }
    if (pc.team.a >= 1.5) { outColor = vec4(fog_veil(pc.team.rgb), pc.mat.w > 0.0 ? pc.mat.w : 1.0); return; }
    vec3 N = normalize(vNormal);
    const bool cullOpaque = pc.mat.x > 250000.0;
    if (cullOpaque) {
        vec3 faceN = cross(dFdx(vWorldPos), dFdy(vWorldPos));
        if (dot(faceN, N) < 0.0) faceN = -faceN;
        if (dot(faceN, g.eyePos.xyz - vWorldPos) < 0.0) discard;
    }
    const bool cullBack = !cullOpaque && pc.mat.x > 150000.0;
    if (cullBack && dot(N, g.eyePos.xyz - vWorldPos) < 0.0) discard;
    const bool twoSided = !cullOpaque && !cullBack && pc.mat.x > 50000.0;
    const float matSpec = cullOpaque ? pc.mat.x - 300000.0 : cullBack ? pc.mat.x - 200000.0 : twoSided ? pc.mat.x - 100000.0 : pc.mat.x;
    if (twoSided && dot(N, g.eyePos.xyz - vWorldPos) < 0.0) N = -N;
    const bool triDiffuse = planarDiffuse && F.uv.x < 0.0;
    const vec2 nuv = (planarDiffuse && !triDiffuse) ? vWorldPos.xy * (F.uv.xy == vec2(0.0) ? vec2(1.0) : F.uv.xy)
                                                    : (fxKind == FX_CREEP && F.p1.y > 0.0) ? vUV * F.p1.y : vUV;
    float creepEdgeA = 1.0;
    vec3 triW = vec3(0.0);
    vec2 triZ = vec2(0.0), triY = vec2(0.0), triX = vec2(0.0);
    if (triDiffuse) {
        const vec3 gn = normalize(vNormal);
        triW = clamp((abs(gn) - 0.2) * 7.0, 0.0, 1.0);
        triW = triW * triW * triW;
        triW /= max(triW.x + triW.y + triW.z, 1e-5);
        vec3 tp = vWorldPos;
        if (F.head.z > 17.75) tp.z = (inverse(pc.world) * vec4(vWorldPos, 1.0)).z;
        const vec3 src = (tp + vec3(0.0, 0.0, F.uv.y)) * (-F.uv.x);
        const vec2 calc = fract(src.xy) * 0.5;
        vec4 rev = fract(-src.xyzy) * vec4(0.5, 1.0, 1.0, 0.5);
        const float trif = fract(F.head.z);
        if ((trif > 0.2 && trif < 0.3) || trif > 0.7) rev.z = 1.0 - rev.z;
        triZ = vec2(gn.z >= 0.0 ? calc.x : rev.x, rev.y);
        triY = vec2((gn.y >= 0.0 ? rev.x : calc.x) + 0.5, rev.z);
        triX = vec2((gn.x >= 0.0 ? calc.y : rev.w) + 0.5, rev.z);
    }
    vec3 n;
    if (triDiffuse && fract(F.head.z) > 0.45) {
        const vec3 ns = (textureLod(normalMap, triZ, 0.0) * triW.z + textureLod(normalMap, triY, 0.0) * triW.y + textureLod(normalMap, triX, 0.0) * triW.x).xyz * 2.0 - 1.0;
        const vec3 tz = cross(vec3(0.0, 1.0, 0.0), N), tv = cross(vec3(0.0, 0.0, 1.0), N);
        vec3 Tt = tz * triW.z + tv * (triW.y + triW.x);
        vec3 Bt = cross(tz, N) * triW.z + cross(tv, N) * (triW.y + triW.x);
        const float tl = dot(Tt, Tt), bl = dot(Bt, Bt);
        n = (tl > 1e-10 && bl > 1e-10) ? normalize(Tt * inversesqrt(tl) * ns.x + Bt * inversesqrt(bl) * ns.y + N * ns.z) : N;
    } else {
        const vec4 ntS = texture(normalMap, nuv);
        creepEdgeA = ntS.a;
        vec3 nt = ntS.xyz * 2.0 - 1.0;
        vec3 dp1 = dFdx(vWorldPos), dp2 = dFdy(vWorldPos);
        vec2 du1 = dFdx(nuv), du2 = dFdy(nuv);
        vec3 dp2perp = cross(dp2, N), dp1perp = cross(N, dp1);
        vec3 T = dp2perp * du1.x + dp1perp * du2.x;
        vec3 B = dp2perp * du1.y + dp1perp * du2.y;
        float tb = max(dot(T, T), dot(B, B));
        n = tb > 1e-12
                ? normalize(mat3(T * inversesqrt(tb), B * inversesqrt(tb), N) * nt)
                : N;
        if (fxKind == FX_CREEP && F.teamEmissive.y > 0.0) {
            const vec2 he = 1.0 / vec2(textureSize(alphaMask2, 0));
            const float ct = F.head.y * F.teamEmissive.x;
            const vec2 cs = vec2(fract(ct), fract(ct * 0.05));
            float ch[3];
            for (int k = 0; k < 3; ++k) {
                const vec2 hu = vUV + (k == 1 ? vec2(he.x, 0.0) : k == 2 ? vec2(0.0, he.y) : vec2(0.0));
                const vec4 hs = texture(alphaMask2, hu);
                ch[k] = clamp(hs.r + (texture(alphaMask, hu + cs).r + texture(alphaMask, hu - cs).r - 1.0) * 2.0 * F.teamEmissive.z * hs.a, 0.0, 1.0);
            }
            const float d0 = clamp((ch[1] - ch[0]) * F.teamEmissive.y, -1.0, 1.0), d1 = clamp((ch[2] - ch[0]) * F.teamEmissive.y, -1.0, 1.0);
            const vec3 ni = normalize(cross(vec3(sqrt(1.0 - d0 * d0), 0.0, d0), vec3(0.0, sqrt(1.0 - d1 * d1), d1)));
            const vec2 hu1 = dFdx(vUV), hu2 = dFdy(vUV);
            const vec3 Th = dp2perp * hu1.x + dp1perp * hu2.x, Bh = dp2perp * hu1.y + dp1perp * hu2.y;
            const float thb = max(dot(Th, Th), dot(Bh, Bh));
            const float ccov = texture(creepMap, (vWorldPos.xy - F.p0.xy) / max(F.p0.zw, vec2(1e-3))).r;
            if (thb > 1e-12) n = normalize(mix(n, normalize(mat3(Th * inversesqrt(thb), Bh * inversesqrt(thb), N) * ni), clamp((ccov - 0.74) * 4.0, 0.0, 1.0)));
        }
    }
    vec3 l = -normalize(g.lightDir.xyz);
    float dl = abs(dot(n, l));
    float shF = mix(1.0, shadow_factor(), g.shadowParams.z);
    float shade = 0.35 + 0.65 * dl * shF;
    const float aoSel = fract(pc.mat.z);
    const vec2 aoUV = aoSel > 0.7 ? vec2(vUV.x, 1.0 - vUV.y) : aoSel > 0.4 ? vec2(vUV1.x, 1.0 - vUV1.y) : aoSel > 0.2 ? vUV1 : vUV;
    const float aoLayer = texture(aoMap, aoUV).g * clamp(vVAO, 0.0, 1.0);
    vec3 shadeRGB = g.rigAmbient.w > 0.5 ? rig_shade(n, shF, aoLayer) : vec3(shade);
    vec2 duv = vUV;
    if (F.head.z != 0.0 || (F.uv.xy != vec2(0.0) && F.uv.xy != vec2(1.0))) {
        const vec2 til = F.uv.xy == vec2(0.0) ? vec2(1.0) : F.uv.xy;
        const float ca = cos(F.head.z), sa = sin(F.head.z);
        duv = mat2(ca, sa, -sa, ca) * (vUV * til);
    }
    if (planarDiffuse && !triDiffuse) duv = vWorldPos.xy * (F.uv.xy == vec2(0.0) ? vec2(1.0) : F.uv.xy);
    vec4 t;
    if (triDiffuse) {
        t = textureLod(diffuse, triZ, 0.0) * triW.z + textureLod(diffuse, triY, 0.0) * triW.y + textureLod(diffuse, triX, 0.0) * triW.x;
    } else {
        t = texture(diffuse, duv + pc.anim.xy);
    }
    t.rgb *= layer_fresnel(F.fresD, n);
    const bool gammaModel = g.keySpec.w > 0.5;
    if (gammaModel) t.rgb = to_gamma(t.rgb);
    float dissolveEdge = 0.0;
    if (fxKind == FX_DISSOLVE) {
        const float h = fx_hash(floor(vUV * 96.0));
        if (h > F.p0.x) discard;
        dissolveEdge = smoothstep(F.p0.x - 0.08, F.p0.x, h);
    }
    const bool constAlpha = pc.mat.y > 0.5 && floor(pc.mat.z) > 4.5 && floor(pc.mat.z) < 5.5 && g.screen.x > -1.0;
    const bool maskCover = abs(pc.mat.y - 1.25) < 0.1 && g.screen.x > -1.0;
    float cutA = t.a;
    if (pc.mat.y > 0.0 && pc.mat.y <= 0.5 && pc.mat.z >= 1.0) {
        const vec4 cm = texture(alphaMask, vUV);
        const int cc = int(floor(pc.mat.z));
        cutA = cc == 1 ? cm.a : cc == 2 ? cm.r : cc == 3 ? cm.g : cm.b;
    }
    if (!constAlpha && !maskCover && pc.team.a <= -1.5 && pc.mat.y > -0.5 && cutA < ((pc.mat.y > 0.0 && pc.mat.y <= 0.5) ? pc.mat.y * 2.0 : 0.5)) discard;
    const bool blended = pc.mat.y > 1.5 || (pc.mat.y > 0.5 && pc.mat.z > 0.5);
    const float teamMix = g.fogExtent.w > 0.0 ? clamp(pow(max(t.a, 0.0), g.fogExtent.w), 0.0, 1.0) : t.a;
    const bool blendedTeam = blended && abs(pc.team.a - 1.25) < 0.1;
    vec3 base = (blendedTeam || (!blended && pc.team.a >= 0.0 && pc.mat.w > -0.5)) ? mix(pc.team.rgb, t.rgb, teamMix) : t.rgb;
    {
        vec4 dc = texture(decalTex, vUV1);
        if (gammaModel) dc.rgb = to_gamma(dc.rgb);
        base = mix(base, dc.rgb, dc.a);
    }
    base *= pc.tint.rgb;
    base *= vVShade;
    if (fxKind == FX_FLASH) base = mix(base, F.p0.rgb, clamp(F.p0.w, 0.0, 1.0));
    const bool unshaded = matSpec < -0.5;
    const float difK = floor(pc.anim.w / 1000.0);
    const float difM = pc.anim.w - difK * 1000.0;
    const vec3 difBase = difK > 0.5 ? clamp(base * difM + vec3(difK * 0.01), 0.0, 1.0) : base * difM;
    vec3 lit = unshaded ? difBase : difBase * (shadeRGB + (noLocalLights ? vec3(0.0) : point_lights(vWorldPos, n)));
    if (fxKind == FX_HIGHLIGHT) lit *= F.p0.x;
    const int emisSel = (uvBits >> 7) & 7;
    vec2 euv = (uvBits & 32) != 0 ? vUV1 : vUV;
    if (F.mask2Lv.z > 0.0) {
        const float ep = floor(F.mask2Lv.z / 16.0);
        const vec2 es = vec2(F.mask2Lv.z - 16.0 * ep, F.mask2Lv.w);
        euv = (ep > 2.5 ? vWorldPos.xy : ep > 1.5 ? vWorldPos.xz : ep > 0.5 ? vWorldPos.yz : euv) * es;
    }
    euv += F.emisUV.xy;
    if ((uvBits & 1024) != 0) euv = clamp(euv, vec2(0.0), vec2(1.0));
    if (F.mask2Lv.z < -0.5) {
        const float code = -F.mask2Lv.z;
        const float rows = floor(code / 64.0), cols = code - 64.0 * rows;
        const float fr = mod(floor(F.mask2Lv.w + 0.001), rows * cols);
        const vec2 cell = vec2(mod(fr, cols), floor(fr / cols));
        euv = (((uvBits & 1024) != 0 ? clamp(euv, vec2(0.0), vec2(0.9999)) : fract(euv)) + cell) / vec2(cols, rows);
    }
    const vec4 e4 = texture(emissive, euv);
    vec3 emis = emisSel == 1 ? vec3(e4.a) : emisSel == 2 ? vec3(e4.r) : emisSel == 3 ? vec3(e4.g) : emisSel == 4 ? vec3(e4.b) : e4.rgb;
    if (gammaModel && g.lightMeta.z > 0.5 && emisSel != 1) emis = to_gamma(emis);
    if (F.teamEmissive.a > 2.5) {
        float ft = pow(max(1.0 - abs(dot(normalize(n), normalize(g.eyePos.xyz - vWorldPos))), 0.0001), F.teamEmissive.x);
        const bool fsq = F.teamEmissive.a > 4.5;
        const float fmode = fsq ? F.teamEmissive.a - 2.0 : F.teamEmissive.a;
        if (fmode > 3.5) ft = 1.0 - ft;
        const float fterm = clamp(ft * F.teamEmissive.z + F.teamEmissive.y, 0.0, 1.0);
        emis *= fsq ? fterm * fterm : fterm;
    }
    const float emisK = floor(pc.anim.z / 1000.0);
    emis = max(emis - emisK * 0.01, vec3(0.0));
    if (gammaModel) emis *= g.rigParams.w;
    emis *= (pc.anim.z - emisK * 1000.0) * pc.tint.rgb;
    if ((uvBits & 64) != 0 && g.shadowExtra.z > 0.0) emis = pow(clamp(emis, vec3(0.0), vec3(1.0)), vec3(g.shadowExtra.z));
    if ((uvBits & 64) != 0)
        emis = F.teamEmissive.a > 1.5 ? min(emis, vec3(1.0)) * F.teamEmissive.rgb
                                      : (pc.team.a > 0.0 && pc.team.a < 1.5 ? min(emis, vec3(1.0)) * pc.team.rgb : vec3(0.0));
    else if (F.teamEmissive.a > 0.5 && F.teamEmissive.a < 1.5) emis *= F.teamEmissive.rgb;
    if (F.emis2T.w > 0.5) {
        const float code = F.emis2.w;
        const float uvs = floor(code / 256.0);
        const float sel2 = floor((code - 256.0 * uvs) / 16.0);
        const int op2 = int(code - 256.0 * uvs - 16.0 * sel2 + 0.5);
        const vec4 s2 = texture(emissive2, uvs > 0.5 ? vUV1 : vUV);
        const int cs = int(sel2 + 0.5);
        vec4 c2 = cs == 1 ? vec4(s2.a) : cs == 2 ? vec4(s2.r) : cs == 3 ? vec4(s2.g) : cs == 4 ? vec4(s2.b) : cs == 5 ? s2 : vec4(s2.rgb, 1.0);
        c2 = c2 * F.emis2.x + F.emis2.y;
        vec3 k2 = vec3(F.emis2.z) * pc.tint.rgb;
        if (gammaModel) k2 *= g.rigParams.w;
        if (op2 == 2) emis += max(c2.rgb * c2.a, vec3(0.0)) * k2;
        else if (op2 == 6) emis += max(c2.rgb, vec3(0.0)) * k2;
        else if (op2 == 4) emis += clamp(c2.a, 0.0, 1.0) * F.emis2T.rgb * k2;
        else if (op2 == 3) emis = mix(emis, max(c2.rgb, vec3(0.0)) * k2, clamp(c2.a, 0.0, 1.0));
        else if (op2 == 0) emis *= max(c2.rgb, vec3(0.0));
        else if (op2 == 1) emis *= 2.0 * max(c2.rgb, vec3(0.0));
    }
    emis += F.p0.yzw * dissolveEdge * 4.0 * float(fxKind == FX_DISSOLVE);

    vec4 specS = texture(specular, vUV);
    if (triDiffuse && fract(F.head.z) > 0.45)
        specS = textureLod(specular, triZ, 0.0) * triW.z + textureLod(specular, triY, 0.0) * triW.y + textureLod(specular, triX, 0.0) * triW.x;
    vec3 specColor = specAlphaOnly ? vec3(specS.a) : specS.rgb;
    specColor *= layer_fresnel(F.fresS, n);
    vec3 spec = vec3(0.0);
    if (dot(specColor, specColor) > 0.0 && !unshaded) {
        vec3 v = normalize(g.eyePos.xyz - vWorldPos);
        vec3 h = normalize(l + v);
        float gloss = matSpec > 0.0 ? matSpec : max(g.shadowParams.w, 1.0);
        float sp = pow(max(dot(n, h), 0.0), gloss);
        spec = specColor * sp * dl * shF * g.eyePos.w;
        if (gammaModel) {
            const float pw = clamp(gloss, 1.0, 512.0);
            const float dim = g.keySpec.w > 1.5 ? 1.0 : clamp(-0.000004444 * pw * pw + 0.004333 * pw + 0.0020834, 0.0, 1.0);
            const vec3 specIn = F.p0.w > 2.5 ? vec3(dot(specColor, vec3(0.3, 0.59, 0.11))) : F.p0.w > 1.5 ? to_gamma(specColor) : specColor;
            const float specASel = specAlphaOnly ? specS.a : 1.0;
            const bool specLevels = fxKind == 0 && F.p0.w > 0.5;
            const float specA = ((int(fract(g.lightDir.w) * 8.0 + 0.5) & 1) == 0 || F.p0.w > 2.5) ? 1.0 : specLevels ? clamp(specASel * F.p0.x + F.p0.y, 0.0, 1.0) : specASel;
            const vec3 specL = (specLevels ? clamp(specIn * F.p0.x + F.p0.y, 0.0, 1.0) * F.p0.z : specColor * (1.0 + F.head.w)) * specA;
            const float specCut = ((floor(g.lightDir.w) > 1.5 || gDblLambert) && (int(fract(g.lightDir.w) * 8.0 + 0.5) & 2) == 0) ? 0.5 : 0.0;
            spec = specL * dim * g.keySpec.rgb * sp * step(specCut, dot(n, -g.rigDir[0].xyz)) * shF * g.rigDir[0].w * g.eyePos.w;
            if (g.rigSpec[0].w > 0.5)
                for (int li = 1; li < 3; ++li) {
                    const vec3 ld = -g.rigDir[li].xyz;
                    const float spl = pow(max(dot(n, normalize(ld + v)), 0.0), gloss);
                    spec += specColor * dim * (1.0 + F.head.w) * g.rigSpec[li].rgb * spl * step(0.0, dot(n, ld)) * g.rigDir[li].w * g.eyePos.w;
                }
            if (g.lightMeta.w > 0.0) spec += specL * dim * point_spec(vWorldPos, n, v, gloss) * g.eyePos.w;
        }
    }
    float cover = 1.0;
    if (fxKind == FX_CREEP) {
        const float cov = texture(creepMap, (vWorldPos.xy - F.p0.xy) / max(F.p0.zw, vec2(1e-3))).r;
        const float nz = texture(alphaMask, vWorldPos.xy * 0.25).r - 0.5;
        cover = smoothstep(0.35, 0.65, cov + nz * F.p1.x);
        float specEdge = 1.0;
        if (F.p1.y > 0.0) {
            const float ca = cov * creepEdgeA;
            cover = ca >= F.p1.z ? 1.0 : clamp((ca - F.p1.w) / max(F.p1.z - F.p1.w, 1e-3), 0.0, 1.0);
            if (ca < F.p1.z) lit *= vec3(1.1, 1.0, 1.1);
            specEdge = clamp(creepEdgeA * 5.0 - 0.5, 0.0, 1.0);
        }
        if (cover <= 0.003) discard;
        if (g.creepParams.x > 0.0) {
            const vec3 cv = normalize(g.eyePos.xyz - vWorldPos);
            const vec3 ch = normalize(l + cv);
            spec = max(specColor, vec3(0.04)) * pow(max(dot(n, ch), 0.0), max(g.creepParams.y, 1.0)) * g.creepParams.x * shF * specEdge;
        }
    } else
    {
        float mcov = 1.0;
        if (pc.mat.y > 0.5 && pc.mat.z > 0.5) {
            vec2 muv = maskUV1 ? vUV1 : vUV;
            if (F.maskX.w > 1.5 && F.maskX.w < 2.5) muv = gl_FragCoord.xy / max(g.screen.xy, vec2(1.0));
            if (F.maskX.w > 0.5) {
                const float mc = cos(F.maskX.z), ms = sin(F.maskX.z);
                muv = mat2(mc, ms, -ms, mc) * (muv * F.maskX.xy) + F.maskO.xy;
            }
            if (F.maskX.w > 2.5) muv = clamp(muv, vec2(0.0), vec2(1.0));
            if (F.maskO.z > 0.5) muv.x = clamp(muv.x, 0.0, 1.0);
            if (F.maskO.w > 0.5) muv.y = clamp(muv.y, 0.0, 1.0);
            const vec4 am = texture(alphaMask, muv);
            const int ch = int(pc.mat.z + 0.5);
            mcov = (ch == 1 || ch == 7) ? am.a : ch == 2 ? am.r : ch == 3 ? am.g : ch == 4 ? am.b : ch == 6 ? t.a : 1.0;
            mcov = clamp(mcov * (1.0 + F.uv.z) + F.uv.w, 0.0, 1.0) * layer_fresnel(F.fresM, n);
            if (ch == 7) mcov *= t.a;
        }
        if (pc.mat.y > 0.5 && F.mask2X.w > 0.5) {
            const float c2 = cos(F.mask2X.z), s2 = sin(F.mask2X.z);
            const vec4 a2 = texture(alphaMask2, mat2(c2, s2, -s2, c2) * (vUV * F.mask2X.xy));
            const int ch2 = int(F.mask2X.w + 0.5);
            const float m2 = ch2 == 1 ? a2.a : ch2 == 2 ? a2.r : ch2 == 3 ? a2.g : a2.b;
            mcov *= clamp(m2 * F.mask2Lv.x + F.mask2Lv.y, 0.0, 1.0) * layer_fresnel(F.fresM2, n);
        }
        if (abs(pc.mat.y - 3.0) < 0.5) {
            cover = (fract(pc.mat.y) > 0.2 ? 1.0 : t.a) * mcov;
        } else if (abs(pc.mat.y - 1.0) < 0.5 && pc.mat.z > 0.5) {
            cover = mcov;
        } else if (abs(pc.mat.y - 2.0) < 0.5) {
            lit *= mcov; spec *= mcov; emis *= mcov;
            if (fract(pc.mat.y) > 0.2) lit *= t.a;
        }
    }
    if (pc.mat.w > 0.0) cover *= pc.mat.w;
    if (vVAlpha <= 1.0) cover *= vVAlpha;
    if (pc.mat.y > 99.0) {
        const vec2 suv = gl_FragCoord.xy / max(g.screen.xy, vec2(1.0));
        const vec2 duv = pc.mat.w > 0.5 ? vUV1 : vUV;
        vec3 nd = n;
        if (pc.mat.w > 0.5) {
            const vec3 nt = texture(normalMap, duv).xyz * 2.0 - 1.0;
            const vec3 dp1 = dFdx(vWorldPos), dp2 = dFdy(vWorldPos);
            const vec2 du1 = dFdx(duv), du2 = dFdy(duv);
            const vec3 dp2perp = cross(dp2, N), dp1perp = cross(N, dp1);
            const vec3 T = dp2perp * du1.x + dp1perp * du2.x, B = dp2perp * du1.y + dp1perp * du2.y;
            const float tb = max(dot(T, T), dot(B, B));
            nd = tb > 1e-12 ? normalize(mat3(T * inversesqrt(tb), B * inversesqrt(tb), N) * nt) : N;
        }
        const float k = (pc.mat.y - 100.0) * pc.mat.z * texture(alphaMask, duv).a;
        const vec4 c0 = g.viewProj * vec4(vWorldPos, 1.0);
        const vec4 c1 = g.viewProj * vec4(vWorldPos + nd * k, 1.0);
        const vec2 off = (c1.xy / c1.w - c0.xy / c0.w) * 0.5;
        outColor = vec4(texture(sceneCopy, clamp(suv + off, vec2(0.0), vec2(1.0))).rgb, 1.0);
        return;
    }
    if (fxKind == FX_CLOAK) {
        const vec2 suv = gl_FragCoord.xy / max(g.screen.xy, vec2(1.0));
        const vec2 wob = vec2(sin(F.head.y * 2.1 + vWorldPos.y * 3.0), cos(F.head.y * 1.7 + vWorldPos.x * 3.0));
        const vec2 off = (n.xy + 0.35 * wob) * F.p0.y;
        const vec3 behind = texture(sceneCopy, clamp(suv + off, vec2(0.0), vec2(1.0))).rgb;
        const vec3 v = normalize(g.eyePos.xyz - vWorldPos);
        const float rim = pow(1.0 - clamp(abs(dot(n, v)), 0.0, 1.0), 3.0);
        outColor = vec4(mix(behind, fog_veil(lit + spec + emis), clamp(F.p0.x + rim * 0.25, 0.0, 1.0)), 1.0);
        return;
    }
    vec3 env = vec3(0.0);
    if (F.p1.x > 0.0 && fxKind != FX_CREEP) {
        const int ecode = int(F.p1.y + 0.5);
        const bool mClamp = ecode >= 128, mInvert = (ecode & 64) != 0;
        const int emode = (ecode & 63) / 16;
        const vec3 er = (emode == 0 || emode == 1) ? reflect(normalize(vWorldPos - g.eyePos.xyz), n) : n;
        vec2 euv = vec2(atan(er.y, er.x) * 0.15915494 + 0.5, acos(clamp(er.z, -1.0, 1.0)) * 0.31830989);
        if (emode == 1 || emode == 2) euv = vec2(er.x, er.z) * vec2(0.5, -0.5) + 0.5;
        const vec4 ec4 = (emode == 1 || emode == 2) ? texture(envMap, euv) : textureLod(envMap, euv, 1.0);
        vec3 ec = ec4.rgb;
        if (gammaModel) ec = to_gamma(ec);
        ec *= layer_fresnel(F.fresE, n);
        const vec4 mk = texture(envMask, vUV);
        const int mc = (ecode & 63) - emode * 16;
        vec3 mask = mc == 0 ? vec3(1.0) : mc == 1 ? vec3(mk.a) : mc == 2 ? vec3(mk.r) : mc == 3 ? vec3(mk.g)
                  : mc == 4 ? vec3(mk.b) : mc == 5 ? mk.rgb : mk.rgb * mk.a;
        if (mInvert && mc != 0) mask = vec3(1.0) - mask;
        mask *= ec4.a * layer_fresnel(F.fresEM, n);
        vec3 mlev = max(mask * F.p1.z + F.p1.w, vec3(0.0));
        if (mClamp) mlev = min(mlev, vec3(1.0));
        env = ec * F.p1.x * mlev;
    }
    if (int(pc.mat.z + 0.5) == 7 && abs(pc.mat.y - 1.0) < 0.5) env /= max(cover, 0.02);
    const vec3 outRGB = (lit + spec + emis + env) * vVColor;
    outColor = vec4(fog_veil((uvBits & 2048) != 0 ? outRGB : height_fog(outRGB)), cover);
    if (g.screen.z > 4.5 && g.screen.z < 5.5 && pc.mat.y > 0.5) {
        outColor = vec4(cover > 0.8 ? vec3(1, 0, 0) : cover > 0.6 ? vec3(0, 1, 0) : cover > 0.4 ? vec3(0, 0, 1) : vec3(0.0), 1.0);
        return;
    }
    if (g.screen.z > 6.5) {
        const vec3 vv = normalize(g.eyePos.xyz - vWorldPos);
        outColor = vec4(vec3(abs(dot(g.screen.z > 7.5 ? normalize(vNormal) : n, vv))) * 0.5, 1.0);
        return;
    }
    if (abs(floor(pc.mat.y + 0.01) - 2.0) < 0.5) outColor.a = clamp(max(outColor.r, max(outColor.g, outColor.b)), 0.0, 1.0);
    if (g.screen.w > 0.5 && pc.mat.y < 0.6 && !unshaded && g.rigAmbient.w > 0.5) {
        const vec3 occl = difBase * max(shadeRGB - rig_shade(n, shF, 0.0), vec3(0.0));
        const vec3 tot = lit + spec + emis + env;
        outColor.a = 1.0 - clamp(max(occl.r, max(occl.g, occl.b)) / max(max(tot.r, max(tot.g, tot.b)), 1e-4), 0.0, 1.0);
    }
    if (g.screen.z > 0.5 && pc.mat.y < 0.5) {
        const float le = max(emis.r, max(emis.g, emis.b));
        const vec3 tot = lit + spec + emis + env;
        const float lt = max(tot.r, max(tot.g, tot.b));
        outColor.a = cover * (1.0 - clamp(le / max(lt, 1e-4), 0.0, 1.0));
    }
}
