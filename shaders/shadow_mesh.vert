#version 450
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_GOOGLE_include_directive : require

// Depth-only pass of record-driven models from the sun's view. Reads the same
// draw records as the main mesh pass, so skinned characters cast too.

#include "common.glsl"

layout(std430, set = 0, binding = 0) readonly buffer Verts {
    Vertex v[];
} verts[];

layout(std430, set = 0, binding = 0) readonly buffer Records {
    DrawRecord r[];
} records[];

layout(push_constant) uniform Push {
    mat4 sun_view_proj;
    uint records_buf;
    uint records_base;
} pc;

void main() {
    DrawRecord rec = records[pc.records_buf].r[pc.records_base + gl_InstanceIndex];
    Vertex vert = verts[nonuniformEXT(rec.vbuf)].v[gl_VertexIndex];
    gl_Position = pc.sun_view_proj * rec.model * vec4(vert.px, vert.py, vert.pz, 1.0);
}
