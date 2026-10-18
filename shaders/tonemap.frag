#version 450
layout(set = 0, binding = 0) uniform sampler2D hdrTex;
layout(set = 0, binding = 1) uniform sampler2D depthTex;
layout(set = 0, binding = 2) uniform sampler2D fogMap;
layout(set = 0, binding = 3) uniform sampler2D bloomTex;
layout(push_constant) uniform PC {
    float exposure;
    int   curve;
    int   keepAlpha;
    float whitePoint;
    float colBlend;
    float colContrast;
    float colHue;
    float colSaturation;
    float colLightness;
    float bloomThreshold;
    float bloomGain;
    float bloomScale;
    int   hdrOperator;
    int   fogOn;
    int   bloomMap;
    int   dofOn;
    mat4  fogFromNdc;
} pc;

float retail_tonemap(float L) {
    if (pc.hdrOperator == 1) return 1.0 - exp(-pc.exposure * L);
    if (pc.hdrOperator == 2) return log(L + 1.0) / log(pc.exposure + 1.0);
    if (pc.hdrOperator == 3) return L;
    if (pc.hdrOperator == 4) {
        const float x = pow(L, 2.2) * pc.exposure;
        return (x * (12.3 * x + 0.5)) / (x * (12.3 * x + 4.0) + 0.02);
    }
    const float t = L * pc.exposure;
    return t * (1.0 + t / (pc.whitePoint * pc.whitePoint)) / (1.0 + t);
}
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

vec3 aces(vec3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}
vec3 reinhard(vec3 x) { return x / (1.0 + x); }
vec3 reinhard_white(vec3 x, float w) {
    const float w2 = max(w * w, 1e-4);
    return x * (1.0 + x / w2) / (1.0 + x);
}
float col_power() { return floor(pc.colContrast / 2048.0) / 1000.0; }
float col_gain() { return mod(pc.colContrast, 2048.0) / 1000.0; }
float col_light() { return mod(pc.colLightness, 2048.0) / 1000.0 - 1.0; }
bool col_light_toward_white() { return mod(pc.colLightness, 4096.0) >= 2048.0; }
float col_low() { return floor(pc.colLightness / 4096.0) / 1000.0; }
vec3 colorize(vec3 c) {
    const vec3 kLuma = vec3(0.299, 0.587, 0.114);
    vec3 r = (c - 0.5) * col_power() + 0.5;
    if (pc.colHue != 0.0) {
        const float a = pc.colHue * 6.2831853;
        const float cs = cos(a), sn = sin(a);
        float y = dot(r, kLuma);
        float i = dot(r, vec3(0.596, -0.274, -0.322));
        float q = dot(r, vec3(0.211, -0.523, 0.312));
        const float i2 = i * cs - q * sn, q2 = i * sn + q * cs;
        r = vec3(y + 0.956 * i2 + 0.621 * q2, y - 0.272 * i2 - 0.647 * q2, y - 1.106 * i2 + 1.703 * q2);
    }
    const float l = dot(r, kLuma);
    r = mix(vec3(l), r, pc.colSaturation) + vec3(col_light());
    return mix(c, clamp(r, 0.0, 1.0), clamp(pc.colBlend, 0.0, 1.0));
}

vec3 to_srgb(vec3 c) {
    vec3 lo = c * 12.92;
    vec3 hi = 1.055 * pow(max(c, vec3(0.0)), vec3(1.0 / 2.4)) - 0.055;
    return mix(hi, lo, vec3(lessThanEqual(c, vec3(0.0031308))));
}

vec3 bloom() {
    const vec2 px = (1.0 / vec2(textureSize(hdrTex, 0))) * max(pc.bloomScale, 0.25);
    const vec2 kRing[8] = vec2[8](vec2(1.0, 0.0), vec2(0.7071, 0.7071), vec2(0.0, 1.0), vec2(-0.7071, 0.7071),
                                  vec2(-1.0, 0.0), vec2(-0.7071, -0.7071), vec2(0.0, -1.0), vec2(0.7071, -0.7071));
    vec3 acc = max(texture(hdrTex, vUV).rgb * pc.exposure - pc.bloomThreshold, vec3(0.0)) * 0.2;
    for (int i = 0; i < 8; ++i) {
        acc += max(texture(hdrTex, vUV + kRing[i] * px * 3.0).rgb * pc.exposure - pc.bloomThreshold, vec3(0.0)) * 0.06;
        acc += max(texture(hdrTex, vUV + kRing[i] * px * 8.0).rgb * pc.exposure - pc.bloomThreshold, vec3(0.0)) * 0.04;
    }
    return acc * pc.bloomGain;
}

vec3 colorize_display(vec3 c) {
    const float b = clamp(pc.colBlend, 0.0, 1.0);
    if (b <= 0.0) return c;
    vec3 r = (c - 0.5) * col_power() + 0.5;
    const float l = dot(r, vec3(0.299, 0.587, 0.114));
    r = mix(vec3(l), r, pc.colSaturation);
    return clamp(mix(c, r, b), 0.0, 1.0);
}

vec3 colorize_hsl(vec3 c) {
    const float b = clamp(pc.colBlend, 0.0, 1.0);
    const int flags = int(floor((pc.colHue + 32.0) / 64.0));
    const bool scale = (flags & 1) != 0;
    if ((flags & 2) == 0) c = min(c, vec3(15.0 / 16.0));
    const float hi = max(c.r, max(c.g, c.b)), lo = min(c.r, min(c.g, c.b));
    const float lin = 0.5 * (hi + lo);
    const float low = col_low();
    const float lout = col_gain() * pow(clamp((lin - low) / max(1.0 - low, 1e-3), 0.0, 1.0), col_power());
    const float sat = (hi - lo) / max(1.0 - abs(2.0 * lin - 1.0), 1e-4);
    const float ks = (hi - lo) > 1e-5 ? sat * (1.0 - abs(2.0 * clamp(lout, 0.0, 1.0) - 1.0)) / (hi - lo) : 0.0;
    const vec3 r = scale ? clamp(c * (lout / max(lin, 1e-4)), 0.0, 1.0) : clamp(vec3(lout) + (c - lin) * ks, 0.0, 1.0);
    if (b <= 0.0) return r;
    if (pc.colSaturation < -50.0) return r;
    float l = 0.5 * (max(r.r, max(r.g, r.b)) + min(r.r, min(r.g, r.b)));
    const float lightness = col_light();
    l = lightness >= 0.0 ? (col_light_toward_white() ? l + lightness * (1.0 - l) : min(l + lightness, 1.0)) : l * (1.0 + lightness);
    const float h = fract(pc.colHue) * 6.0;
    const float ch = (1.0 - abs(2.0 * l - 1.0)) * clamp(pc.colSaturation, 0.0, 1.0);
    const float x = ch * (1.0 - abs(mod(h, 2.0) - 1.0));
    const vec3 k = h < 1.0 ? vec3(ch, x, 0.0) : h < 2.0 ? vec3(x, ch, 0.0) : h < 3.0 ? vec3(0.0, ch, x)
                 : h < 4.0 ? vec3(0.0, x, ch) : h < 5.0 ? vec3(x, 0.0, ch) : vec3(ch, 0.0, x);
    return clamp(mix(r, k + vec3(l - 0.5 * ch), b), 0.0, 1.0);
}

vec3 fog_of_war(vec3 disp) {
    if (pc.fogOn == 0) return disp;
    const float d = texelFetch(depthTex, ivec2(gl_FragCoord.xy), 0).r;
    if (d >= 0.99999) {
        const vec4 a4 = pc.fogFromNdc * vec4(vUV * 2.0 - 1.0, 0.0, 1.0);
        const vec4 b4 = pc.fogFromNdc * vec4(vUV * 2.0 - 1.0, 1.0, 1.0);
        const vec3 a = a4.xyz / a4.w, b = b4.xyz / b4.w;
        const float h = float(pc.dofOn - 1048576) / 256.0;
        const float t = abs(b.z - a.z) > 1e-6 ? clamp((h - a.z) / (b.z - a.z), 0.0, 1.0) : 1.0;
        const vec2 fuv = mix(a.xy, b.xy, t);
        if (pc.fogOn == 1 && (any(lessThan(fuv, vec2(0.0))) || any(greaterThan(fuv, vec2(1.0))))) return disp;
        return disp * (1.0 - texture(fogMap, fuv).r);
    }
    const vec4 p = pc.fogFromNdc * vec4(vUV * 2.0 - 1.0, d, 1.0);
    return disp * (1.0 - texture(fogMap, p.xy / p.w).r);
}

float dof_view_z(vec2 uv) {
    const float d = texture(depthTex, uv).r;
    const float zn = pc.fogFromNdc[1].x, zf = pc.fogFromNdc[1].y;
    return zn * zf / (zf - d * (zf - zn));
}
float dof_coc(float z) {
    const vec4 f = pc.fogFromNdc[0];
    return clamp((abs(z - f.x) - f.y) / max(f.z - f.y, 1e-4), 0.0, 1.0) * f.w;
}
vec3 lit_color() {
    const vec3 c0 = texture(hdrTex, vUV).rgb;
    if (pc.dofOn != 1 || pc.fogOn != 0) return c0;
    const float coc = min(dof_coc(dof_view_z(vUV)), 1.0);
    if (coc <= 0.01) return c0;
    const vec2 px = 1.0 / vec2(textureSize(hdrTex, 0));
    const float r = coc * pc.fogFromNdc[1].z;
    const vec2 kDisc[16] = vec2[16](
        vec2(-0.9420, -0.3991), vec2(0.9456, -0.7689), vec2(-0.0942, -0.9294), vec2(0.3450, 0.2939),
        vec2(-0.9159, 0.4577), vec2(-0.8154, -0.8791), vec2(-0.3828, 0.2768), vec2(0.9748, 0.7565),
        vec2(0.4432, -0.9751), vec2(0.5374, -0.4737), vec2(-0.2650, -0.4189), vec2(0.7920, 0.1909),
        vec2(-0.2419, 0.9971), vec2(-0.8141, 0.9144), vec2(0.1998, 0.7864), vec2(0.1438, -0.1410));
    vec3 acc = c0;
    float wsum = 1.0;
    for (int i = 0; i < 16; ++i) {
        const vec2 uv = vUV + kDisc[i] * r * px;
        const float w = max(min(dof_coc(dof_view_z(uv)), 1.0), 0.05);
        acc += texture(hdrTex, uv).rgb * w;
        wsum += w;
    }
    return acc / wsum;
}

void main() {
    if (pc.hdrOperator >= 0 && pc.curve == 0) {
        vec3 c = lit_color();
        if (pc.bloomGain > 0.0 && pc.bloomMap != 0) {
            c += texture(bloomTex, vUV).rgb * pc.bloomGain;
        } else if (pc.bloomGain > 0.0) {
            const vec2 px = (1.0 / vec2(textureSize(hdrTex, 0))) * max(pc.bloomScale, 0.25);
            const vec2 kRing[8] = vec2[8](vec2(1.0, 0.0), vec2(0.7071, 0.7071), vec2(0.0, 1.0), vec2(-0.7071, 0.7071),
                                          vec2(-1.0, 0.0), vec2(-0.7071, -0.7071), vec2(0.0, -1.0), vec2(0.7071, -0.7071));
            vec3 acc = max(c - pc.bloomThreshold, vec3(0.0)) * 0.2;
            for (int i = 0; i < 8; ++i) {
                acc += max(texture(hdrTex, vUV + kRing[i] * px * 3.0).rgb - pc.bloomThreshold, vec3(0.0)) * 0.06;
                acc += max(texture(hdrTex, vUV + kRing[i] * px * 8.0).rgb - pc.bloomThreshold, vec3(0.0)) * 0.04;
            }
            c += acc * pc.bloomGain;
        }
        const float L = dot(vec3(0.35, 0.5, 0.125), c);
        if (pc.hdrOperator != 0 && (int(floor((pc.colHue + 32.0) / 64.0)) & 4) == 0) {
            const float ratio = clamp((L - 1.5) / max(L, 1e-5), 0.0, 1.0);
            c = mix(c, vec3(L), ratio);
        }
        c = L > 1e-6 ? c * (retail_tonemap(L) / L) : vec3(0.0);
        const float litShare = pc.keepAlpha != 0 ? 1.0 : clamp(texture(hdrTex, vUV).a, 0.0, 1.0);
        const vec3 tmc = clamp(c, 0.0, 1.0);
        const vec3 glow = (int(floor((pc.colHue + 32.0) / 64.0)) & 2) == 0 ? min(tmc, vec3(15.0 / 16.0)) : tmc;
        const vec3 disp = fog_of_war(mix(glow, colorize_hsl(tmc), litShare));
        outColor = vec4(disp, pc.keepAlpha != 0 ? texture(hdrTex, vUV).a : 1.0);
        return;
    }
    vec3 hdr = texture(hdrTex, vUV).rgb * pc.exposure;
    if (pc.bloomGain > 0.0) hdr += bloom();
    vec3 mapped;
    if (pc.curve == 1) mapped = reinhard(hdr);
    else if (pc.curve == 2) mapped = clamp(hdr, 0.0, 1.0);
    else if (pc.curve == 3) mapped = aces(hdr);
    else mapped = clamp(reinhard_white(hdr, pc.whitePoint), 0.0, 1.0);
    vec3 disp = to_srgb(clamp(mapped, 0.0, 1.0));
    if (pc.curve == 0) disp = mix(disp, colorize_hsl(disp), pc.keepAlpha != 0 ? 1.0 : clamp(texture(hdrTex, vUV).a, 0.0, 1.0));
    if (pc.curve == 4) disp = colorize(to_srgb(clamp(reinhard_white(hdr, pc.whitePoint), 0.0, 1.0)));
    if (pc.curve == 5) disp = colorize_display(to_srgb(clamp(reinhard_white(hdr, pc.whitePoint), 0.0, 1.0)));
    outColor = vec4(fog_of_war(disp), pc.keepAlpha != 0 ? texture(hdrTex, vUV).a : 1.0);
}
