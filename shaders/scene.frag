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
    uint vbuf;
    uint globals;
    uint gslot;
} pc;

layout(location = 0) in vec3 frag_color;
layout(location = 1) in vec3 frag_world;
layout(location = 2) in vec3 frag_normal;
layout(location = 3) in vec2 frag_uv;
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
    Globals g = globals[pc.globals].g[pc.gslot];
    // Tiling surface detail. Multiplying rather than replacing keeps the
    // per-box colour that tells the arena's landmarks apart.
    // Correct for a non-square source so texels stay square in world space.
    // Taken from the texture itself rather than a constant, so swapping the
    // material cannot silently stretch the whole arena.
    vec2 dims = vec2(textureSize(sampler2D(textures[nonuniformEXT(g.level_tex)],
                                           default_sampler), 0));
    vec2 uv = frag_uv * vec2(1.0, dims.x / dims.y);
    vec3 detail =
        texture(sampler2D(textures[nonuniformEXT(g.level_tex)], default_sampler), uv).rgb;

    // Tangent-space relief off the same material. The frame comes from the face
    // normal because the level is axis aligned; a mesh with arbitrary winding
    // would have to carry real tangents.
    vec3 face = normalize(frag_normal);
    vec3 tangent = level_tangent(face);
    vec3 bitangent = cross(face, tangent);
    vec3 sampled_n =
        texture(sampler2D(textures[nonuniformEXT(g.level_normal_tex)], default_sampler), uv).xyz *
            2.0 - 1.0;
    vec3 n = normalize(tangent * sampled_n.x + bitangent * sampled_n.y + face * sampled_n.z);
    float rough =
        texture(sampler2D(textures[nonuniformEXT(g.level_rough_tex)], default_sampler), uv).r *
        LEVEL_ROUGHNESS;
    out_color = vec4(shade(g, frag_color * detail, LEVEL_METALLIC, rough, n, frag_world,
                           sun_shadow(g, frag_world)),
                     1.0);
}
