#version 450
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_nonuniform_qualifier : require

#include "common.glsl"

layout(set = 0, binding = 1) uniform texture2D textures[];
layout(set = 0, binding = 3) uniform sampler default_sampler;
layout(set = 0, binding = 4) uniform samplerShadow shadow_sampler;

layout(std430, set = 0, binding = 0) readonly buffer GlobalsBuf {
    Globals g[];
} globals[];

layout(push_constant) uniform Push {
    mat4 view_proj;
    uint records_buf;
    uint records_base;
    uint globals;
    uint gslot;
} pc;

layout(location = 0) in vec3 frag_normal;
layout(location = 1) in vec2 frag_uv;
layout(location = 2) flat in vec4 frag_color;
layout(location = 3) flat in uint frag_tex;
layout(location = 4) in vec3 frag_world;
layout(location = 0) out vec4 out_color;

float sun_shadow(Globals g, vec3 world) {
    vec4 clip = g.sun_view_proj * vec4(world, 1.0);
    vec3 ndc = clip.xyz / clip.w;
    vec2 uv = ndc.xy * 0.5 + 0.5;
    if (ndc.z > 1.0) {
        return 1.0;  // beyond the sun volume's far plane
    }
    // 3x3 of hardware-filtered comparison taps. The border-white sampler makes
    // anything outside the camera-following sun volume read as lit.
    const float bias = 0.0008;
    const float texel = 1.0 / 4096.0;
    float lit = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            lit += texture(sampler2DShadow(textures[nonuniformEXT(g.shadow_tex)],
                                           shadow_sampler),
                           vec3(uv + vec2(x, y) * texel, ndc.z - bias));
        }
    }
    return lit / 9.0;
}

void main() {
    vec4 sampled =
        texture(sampler2D(textures[nonuniformEXT(frag_tex)], default_sampler), frag_uv);
    if (sampled.a * frag_color.a < 0.5) {
        discard;  // cut-out foliage and fences
    }
    Globals g = globals[pc.globals].g[pc.gslot];
    vec3 albedo = sampled.rgb * frag_color.rgb;
    out_color = vec4(shade(g, albedo, normalize(frag_normal), frag_world,
                           sun_shadow(g, frag_world)),
                     1.0);
}
