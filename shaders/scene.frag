#version 450
#extension GL_EXT_nonuniform_qualifier : require

layout(set = 0, binding = 1) uniform texture2D textures[];
layout(set = 0, binding = 3) uniform sampler default_sampler;

struct Globals {
    mat4 sun_view_proj;
    vec4 sun_dir;  // xyz normalized, toward the sun
    uint shadow_tex;
    uint pad0, pad1, pad2;
};

layout(std430, set = 0, binding = 0) readonly buffer GlobalsBuf {
    Globals g[];
} globals[];

layout(push_constant) uniform Push {
    mat4 view_proj;
    uint vbuf;
    uint globals;
    uint gslot;
} pc;

layout(location = 0) in vec3 frag_color;
layout(location = 1) in vec3 frag_world;
layout(location = 0) out vec4 out_color;

float sun_shadow(Globals g, vec3 world) {
    vec4 clip = g.sun_view_proj * vec4(world, 1.0);
    vec3 ndc = clip.xyz / clip.w;
    vec2 uv = ndc.xy * 0.5 + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || ndc.z > 1.0) {
        return 1.0;
    }
    const float bias = 0.0015;
    const float texel = 1.0 / 2048.0;
    float lit = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            float depth = texture(sampler2D(textures[nonuniformEXT(g.shadow_tex)],
                                            default_sampler),
                                  uv + vec2(x, y) * texel)
                              .r;
            lit += ndc.z - bias <= depth ? 1.0 : 0.0;
        }
    }
    return lit / 9.0;
}

void main() {
    Globals g = globals[pc.globals].g[pc.gslot];
    float shadow = sun_shadow(g, frag_world);
    float diffuse = 0.35 + 0.65 * shadow;
    out_color = vec4(frag_color * diffuse, 1.0);
}
