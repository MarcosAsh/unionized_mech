#version 450
#extension GL_EXT_nonuniform_qualifier : require

// Screen-space overlay pass: unlit, alpha-blended. Draws damage flashes and
// match banners now, HUD text later. Geometry arrives in NDC through the mesh
// vertex shader with identity matrices.

layout(set = 0, binding = 1) uniform texture2D textures[];
layout(set = 0, binding = 3) uniform sampler default_sampler;

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
// Unused here: the overlay is unlit. They are declared anyway because this pass
// shares mesh.vert, and a vertex output with no matching fragment input is a
// validation message.
layout(location = 5) flat in float frag_metallic;
layout(location = 6) flat in float frag_roughness;
layout(location = 0) out vec4 out_color;

void main() {
    vec4 sampled =
        texture(sampler2D(textures[nonuniformEXT(frag_tex)], default_sampler), frag_uv);
    out_color = vec4(sampled.rgb * frag_color.rgb, sampled.a * frag_color.a);
}
