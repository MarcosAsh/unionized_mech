#version 450
#extension GL_EXT_nonuniform_qualifier : require

layout(set = 0, binding = 1) uniform texture2D textures[];
layout(set = 0, binding = 3) uniform sampler default_sampler;

layout(push_constant) uniform Push {
    mat4 mvp;
    vec4 rot;
    vec4 color;
    uint vbuf;
    uint tex;
} pc;

layout(location = 0) in vec3 frag_normal;
layout(location = 1) in vec2 frag_uv;
layout(location = 0) out vec4 out_color;

void main() {
    vec4 sampled =
        texture(sampler2D(textures[nonuniformEXT(pc.tex)], default_sampler), frag_uv);
    if (sampled.a * pc.color.a < 0.5) {
        discard;  // cut-out foliage and fences
    }
    vec3 albedo = sampled.rgb * pc.color.rgb;
    vec3 n = normalize(frag_normal);
    vec3 light_dir = normalize(vec3(0.4, 1.0, 0.25));
    float diffuse = 0.35 + 0.65 * max(dot(n, light_dir), 0.0);
    out_color = vec4(albedo * diffuse, 1.0);
}
