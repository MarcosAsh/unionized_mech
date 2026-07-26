#version 450
#extension GL_EXT_nonuniform_qualifier : require

// Textured mesh pass, GPU-driven. Each draw's record index arrives in
// firstInstance, written there by the culling compute shader. The record holds
// the model transform, material, and buffer indices; vertices live in a
// bindless storage buffer as eight floats matching asset::MeshVertex.

struct Vertex {
    float px, py, pz;
    float nx, ny, nz;
    float u, v;
};

struct DrawRecord {
    mat4 model;
    vec4 rot;
    vec4 color;
    vec4 bounds_min;
    vec4 bounds_max;
    uint vbuf;
    uint tex;
    uint index_count;
    uint first_index;
    uint vertex_base;  // vertexOffset for per-frame skinned vertex slices
    uint pad0, pad1, pad2;
};

layout(std430, set = 0, binding = 0) readonly buffer Verts {
    Vertex v[];
} verts[];

layout(std430, set = 0, binding = 0) readonly buffer Records {
    DrawRecord r[];
} records[];

layout(push_constant) uniform Push {
    mat4 view_proj;
    uint records_buf;
    uint records_base;  // this frame's slice of the record buffer
} pc;

layout(location = 0) out vec3 frag_normal;
layout(location = 1) out vec2 frag_uv;
layout(location = 2) flat out vec4 frag_color;
layout(location = 3) flat out uint frag_tex;

vec3 quat_rotate(vec4 q, vec3 v) {
    vec3 t = 2.0 * cross(q.xyz, v);
    return v + q.w * t + cross(q.xyz, t);
}

void main() {
    DrawRecord rec = records[pc.records_buf].r[pc.records_base + gl_InstanceIndex];
    Vertex vert = verts[nonuniformEXT(rec.vbuf)].v[gl_VertexIndex];
    gl_Position = pc.view_proj * rec.model * vec4(vert.px, vert.py, vert.pz, 1.0);
    frag_normal = quat_rotate(rec.rot, vec3(vert.nx, vert.ny, vert.nz));
    frag_uv = vec2(vert.u, vert.v);
    frag_color = rec.color;
    frag_tex = rec.tex;
}
