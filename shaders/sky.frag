#version 450
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_nonuniform_qualifier : require

#include "common.glsl"

layout(std430, set = 0, binding = 0) readonly buffer GlobalsBuf {
    Globals g[];
} globals[];

layout(push_constant) uniform Push {
    vec4 ray_right;
    vec4 ray_up;
    vec4 ray_forward;
    uint globals;
    uint gslot;
} pc;

layout(location = 0) in vec2 frag_ndc;
layout(location = 0) out vec4 out_color;

void main() {
    Globals g = globals[pc.globals].g[pc.gslot];
    // The camera axes arrive scaled by the field of view, so the view ray is
    // just the forward axis nudged by the screen position.
    vec3 dir = normalize(pc.ray_forward.xyz + pc.ray_right.xyz * frag_ndc.x +
                         pc.ray_up.xyz * frag_ndc.y);
    out_color = vec4(sky_toward(g, dir), 1.0);
}
