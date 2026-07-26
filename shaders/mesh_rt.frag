#version 460
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_ray_query : require

// The mesh fragment shader on ray tracing hardware: sun shadows come from a
// ray query against the scene TLAS instead of the shadow map.

layout(set = 0, binding = 1) uniform texture2D textures[];
layout(set = 0, binding = 3) uniform sampler default_sampler;
layout(set = 0, binding = 5) uniform accelerationStructureEXT tlas[3];

struct Globals {
    mat4 sun_view_proj;
    vec4 sun_dir;  // xyz normalized, toward the sun
    uint shadow_tex;
    uint pad0, pad1, pad2;
};

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

float sun_shadow_rt(Globals g, vec3 world, vec3 n) {
    rayQueryEXT rq;
    vec3 origin = world + n * 0.03;
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
    vec4 sampled =
        texture(sampler2D(textures[nonuniformEXT(frag_tex)], default_sampler), frag_uv);
    if (sampled.a * frag_color.a < 0.5) {
        discard;  // cut-out foliage and fences
    }
    Globals g = globals[pc.globals].g[pc.gslot];
    vec3 albedo = sampled.rgb * frag_color.rgb;
    vec3 n = normalize(frag_normal);
    float shadow = sun_shadow_rt(g, frag_world, n);
    float diffuse = 0.35 + 0.65 * max(dot(n, g.sun_dir.xyz), 0.0) * shadow;
    out_color = vec4(albedo * diffuse, 1.0);
}
