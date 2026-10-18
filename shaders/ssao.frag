#version 450
layout(set = 0, binding = 1) uniform sampler2D depthTex;
layout(push_constant) uniform PC {
    vec4 cam;
    vec4 ao;
    vec4 misc;
    vec4 detail;
} pc;
layout(location = 0) out vec4 outColor;

float view_z(float d) { return pc.cam.z * pc.cam.w / (pc.cam.w - d * (pc.cam.w - pc.cam.z)); }
vec3 view_pos(ivec2 px, vec2 size) {
    const float z = view_z(texelFetch(depthTex, px, 0).r);
    const vec2 ndc = (vec2(px) + 0.5) / size * 2.0 - 1.0;
    return vec3(ndc * pc.cam.xy * z, z);
}
float hash(vec2 p) { return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453); }

float blocked(vec3 p, vec3 n, vec3 rnd, vec2 size, int first, int last, float radius, float full_d, float none_d) {
    float acc = 0.0;
    int used = 0;
    for (int i = first; i < last; ++i) {
        const float fi = float(i);
        vec3 s = vec3(hash(vec2(fi, 1.0)), hash(vec2(fi, 2.0)), hash(vec2(fi, 3.0))) * 2.0 - 1.0;
        s = normalize(s) * mix(0.25, 1.0, hash(vec2(fi, 4.0)));
        s = s - 2.0 * dot(s, rnd) * rnd;
        if (dot(s, n) < 0.0) s = -s;
        const vec3 sp = p + radius * s;
        const vec2 uv = sp.xy / (sp.z * pc.cam.xy) * 0.5 + 0.5;
        ++used;
        if (uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0) continue;
        const float sz = view_z(texelFetch(depthTex, ivec2(uv * size), 0).r);
        const float dist = sp.z - sz;
        if (dist > pc.misc.z) {
            if (dist < full_d) acc += 1.0;
            else acc += max(1.0 - (dist - full_d) / max(none_d - full_d, 1e-6), 0.0);
        }
    }
    return acc / float(max(used, 1));
}

void main() {
    if (pc.misc.x > 0.5) { outColor = vec4(0.0, 0.0, 0.0, 1.0); return; }
    const vec2 size = vec2(textureSize(depthTex, 0));
    const ivec2 px = ivec2(gl_FragCoord.xy);
    const float d = texelFetch(depthTex, px, 0).r;
    if (d >= 1.0) { outColor = vec4(0.0, 0.0, 0.0, 1.0); return; }
    const vec3 p = view_pos(px, size);
    const vec3 pl = view_pos(px + ivec2(-1, 0), size), pr = view_pos(px + ivec2(1, 0), size);
    const vec3 pu = view_pos(px + ivec2(0, -1), size), pd = view_pos(px + ivec2(0, 1), size);
    const vec3 dx = abs(pr.z - p.z) < abs(p.z - pl.z) ? pr - p : p - pl;
    const vec3 dy = abs(pd.z - p.z) < abs(p.z - pu.z) ? pd - p : p - pu;
    vec3 n = normalize(cross(dx, dy));
    if (dot(n, p) > 0.0) n = -n;
    const vec3 rnd = normalize(vec3(hash(gl_FragCoord.xy), hash(gl_FragCoord.yx + 17.0), hash(gl_FragCoord.xy + 41.0)) * 2.0 - 1.0);
    const int count = int(pc.misc.y + 0.5);
    const bool separate = pc.detail.x > 0.0;
    const int main_count = separate ? count * 3 / 4 : count;
    float radius = pc.ao.x;
    if (pc.misc.w > 0.0 && radius / p.z > pc.misc.w) radius = pc.misc.w * p.z;
    float occ = pow(clamp(1.0 - blocked(p, n, rnd, size, 0, main_count, radius, pc.ao.w, pc.ao.z), 0.0, 1.0), pc.ao.y);
    if (separate) {
        const float occ2 = pow(clamp(1.0 - blocked(p, n, rnd, size, main_count, count, pc.detail.x, pc.detail.w, pc.detail.z), 0.0, 1.0), pc.detail.y);
        occ = min(occ, occ2);
    }
    outColor = vec4(0.0, 0.0, 0.0, occ);
}
