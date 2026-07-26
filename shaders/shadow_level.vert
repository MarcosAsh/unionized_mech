#version 450
#extension GL_EXT_nonuniform_qualifier : require

// Depth-only pass of the level geometry from the sun's view.

struct Vertex {
    vec4 pos;
    vec4 color;
};

layout(std430, set = 0, binding = 0) readonly buffer Verts {
    Vertex v[];
} verts[];

layout(push_constant) uniform Push {
    mat4 sun_view_proj;
    uint vbuf;
} pc;

void main() {
    gl_Position = pc.sun_view_proj * vec4(verts[pc.vbuf].v[gl_VertexIndex].pos.xyz, 1.0);
}
