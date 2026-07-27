#version 460
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_ray_query : require

// The level fragment shader on ray tracing hardware: sun shadows come from a
// ray query against the scene TLAS instead of the shadow map.

layout(set = 0, binding = 5) uniform accelerationStructureEXT tlas[3];

// Shadows come from the TLAS here, but surface detail still comes from a
// texture, so this pass needs the bindless table the same as the raster one.
layout(set = 0, binding = 1) uniform texture2D textures[];
layout(set = 0, binding = 3) uniform sampler default_sampler;

struct Globals {
    mat4 sun_view_proj;
    vec4 sun_dir;  // xyz normalized, toward the sun
    uint shadow_tex;
    uint level_tex;
    uint pad0, pad1;
};

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
    vec3 n = normalize(frag_normal);
    // Tiling surface detail. Multiplying rather than replacing keeps the
    // per-box colour that tells the arena's landmarks apart.
    vec3 detail =
        texture(sampler2D(textures[nonuniformEXT(g.level_tex)], default_sampler), frag_uv).rgb;
    float shadow = sun_shadow_rt(g, frag_world);
    // Same expression the mesh pass uses, so level and props sit in one light.
    float diffuse = 0.35 + 0.65 * max(dot(n, g.sun_dir.xyz), 0.0) * shadow;
    out_color = vec4(frag_color * detail * diffuse, 1.0);
}
