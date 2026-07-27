#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_ray_query : require

#include "common.glsl"

// The level fragment shader on ray tracing hardware: sun shadows come from a
// ray query against the scene TLAS instead of the shadow map.

layout(set = 0, binding = 5) uniform accelerationStructureEXT tlas[3];

// Shadows come from the TLAS here, but surface detail still comes from a
// texture, so this pass needs the bindless table the same as the raster one.
layout(set = 0, binding = 1) uniform texture2D textures[];
layout(set = 0, binding = 3) uniform sampler default_sampler;

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

float sun_shadow_rt(Globals g, vec3 world) {
    rayQueryEXT rq;
    vec3 origin = world + g.sun_dir.xyz * 0.03;
    rayQueryInitializeEXT(rq, tlas[pc.gslot],
                          gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsOpaqueEXT, 0xFF, origin,
                          0.02, g.sun_dir.xyz, 300.0);
    rayQueryProceedEXT(rq);
    return rayQueryGetIntersectionTypeEXT(rq, true) ==
                   gl_RayQueryCommittedIntersectionNoneEXT
               ? 1.0
               : 0.0;
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
    out_color = vec4(shade(g, frag_color * detail, normalize(frag_normal), frag_world,
                           sun_shadow_rt(g, frag_world)),
                     1.0);
}
