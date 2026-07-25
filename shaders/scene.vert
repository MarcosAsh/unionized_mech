#version 450
#extension GL_EXT_nonuniform_qualifier : require

// Vertices live in a bindless storage buffer, indexed by gl_VertexIndex. No
// vertex input bindings or attributes. The buffer slot arrives as a push
// constant.

struct Vertex {
    vec4 pos;
    vec4 color;
};

layout(std430, set = 0, binding = 0) readonly buffer Verts {
    Vertex v[];
} verts[];

layout(push_constant) uniform Push {
    mat4 view_proj;
    uint vbuf;
} pc;

layout(location = 0) out vec3 frag_color;

void main() {
    Vertex vert = verts[pc.vbuf].v[gl_VertexIndex];
    gl_Position = pc.view_proj * vec4(vert.pos.xyz, 1.0);
    frag_color = vert.color.rgb;
}
