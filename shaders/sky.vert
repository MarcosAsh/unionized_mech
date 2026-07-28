#version 450

// One oversized triangle covering the screen, built from the vertex index so
// the pass needs no vertex buffer. The fragment shader turns the clip position
// back into a view ray.

layout(push_constant) uniform Push {
    vec4 ray_right;
    vec4 ray_up;
    vec4 ray_forward;
    uint globals;
    uint gslot;
} pc;

layout(location = 0) out vec2 frag_ndc;

void main() {
    const vec2 corners[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    frag_ndc = corners[gl_VertexIndex];
    // Depth 1.0: the sky sits at the far plane, behind everything.
    gl_Position = vec4(frag_ndc, 1.0, 1.0);
}
