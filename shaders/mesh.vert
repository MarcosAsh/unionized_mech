#version 450
#extension GL_EXT_nonuniform_qualifier : require

// Textured mesh pass. Vertices live in a bindless storage buffer as eight
// floats (position, normal, uv) matching asset::MeshVertex. The mvp is
// premultiplied on the CPU; the rotation quat rotates normals to world space.

struct Vertex {
    float px, py, pz;
    float nx, ny, nz;
    float u, v;
};

layout(std430, set = 0, binding = 0) readonly buffer Verts {
    Vertex v[];
} verts[];

layout(push_constant) uniform Push {
    mat4 mvp;
    vec4 rot;  // model rotation quaternion (x, y, z, w)
    uint vbuf;
    uint tex;
} pc;

layout(location = 0) out vec3 frag_normal;
layout(location = 1) out vec2 frag_uv;

vec3 quat_rotate(vec4 q, vec3 v) {
    vec3 t = 2.0 * cross(q.xyz, v);
    return v + q.w * t + cross(q.xyz, t);
}

void main() {
    Vertex vert = verts[pc.vbuf].v[gl_VertexIndex];
    gl_Position = pc.mvp * vec4(vert.px, vert.py, vert.pz, 1.0);
    frag_normal = quat_rotate(pc.rot, vec3(vert.nx, vert.ny, vert.nz));
    frag_uv = vec2(vert.u, vert.v);
}
