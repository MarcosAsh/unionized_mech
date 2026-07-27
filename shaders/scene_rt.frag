#version 460
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_ray_query : require

// The level fragment shader on ray tracing hardware: sun shadows come from a
// ray query against the scene TLAS instead of the shadow map.

layout(set = 0, binding = 5) uniform accelerationStructureEXT tlas[3];

struct Globals {
    mat4 sun_view_proj;
    vec4 sun_dir;  // xyz normalized, toward the sun
    uint shadow_tex;
    uint pad0, pad1, pad2;
    vec4 cam_pos;  // eye position, for resolving which way a normal faces
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

// See scene.frag: the level pass has no vertex normals, its faces are flat, and
// the derivative cross product recovers the face normal. Oriented toward the
// eye because the sign depends on winding and framebuffer handedness.
vec3 face_normal(Globals g, vec3 world) {
    vec3 n = normalize(cross(dFdx(world), dFdy(world)));
    return dot(n, g.cam_pos.xyz - world) < 0.0 ? -n : n;
}

void main() {
    Globals g = globals[pc.globals].g[pc.gslot];
    vec3 n = face_normal(g, frag_world);
    float shadow = sun_shadow_rt(g, frag_world);
    float diffuse = 0.35 + 0.65 * max(dot(n, g.sun_dir.xyz), 0.0) * shadow;
    out_color = vec4(frag_color * diffuse, 1.0);
}
