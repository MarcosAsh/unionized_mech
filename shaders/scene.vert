#version 450
#extension GL_EXT_nonuniform_qualifier : require

// Level pass. Vertices live in a bindless storage buffer, indexed by
// gl_VertexIndex; positions are already world space.

struct Vertex {
    vec4 pos;
    vec4 color;
    vec4 normal;
    vec4 uv;
};

layout(std430, set = 0, binding = 0) readonly buffer Verts {
    Vertex v[];
} verts[];

layout(push_constant) uniform Push {
    mat4 view_proj;
    uint vbuf;
    uint globals;
    uint gslot;
} pc;

layout(location = 0) out vec3 frag_color;
layout(location = 1) out vec3 frag_world;
layout(location = 2) out vec3 frag_normal;
layout(location = 3) out vec2 frag_uv;

void main() {
    Vertex vert = verts[pc.vbuf].v[gl_VertexIndex];
    gl_Position = pc.view_proj * vec4(vert.pos.xyz, 1.0);
    frag_color = vert.color.rgb;
    frag_world = vert.pos.xyz;
    frag_normal = vert.normal.xyz;
    frag_uv = vert.uv.xy;
}
