#version 450
#extension GL_EXT_nonuniform_qualifier : require

layout(set = 0, binding = 1) uniform texture2D textures[];
layout(set = 0, binding = 3) uniform sampler default_sampler;
layout(set = 0, binding = 4) uniform samplerShadow shadow_sampler;

struct Globals {
    mat4 sun_view_proj;
    vec4 sun_dir;  // xyz normalized, toward the sun
    uint shadow_tex;
    uint pad0, pad1, pad2;
    vec4 cam_pos;  // eye position, for resolving which way a normal faces
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
    if (ndc.z > 1.0) {
        return 1.0;  // beyond the sun volume's far plane
    }
    // 3x3 of hardware-filtered comparison taps. The border-white sampler makes
    // anything outside the camera-following sun volume read as lit.
    const float bias = 0.0008;
    const float texel = 1.0 / 4096.0;
    float lit = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            lit += texture(sampler2DShadow(textures[nonuniformEXT(g.shadow_tex)],
                                           shadow_sampler),
                           vec3(uv + vec2(x, y) * texel, ndc.z - bias));
        }
    }
    return lit / 9.0;
}

// The level pass carries no vertex normals: its boxes share eight vertices
// across six faces, so there is nowhere to put one. The faces are flat, so the
// screen-space derivatives of world position recover the exact face normal for
// free. The cross product's sign depends on winding and framebuffer handedness,
// so orient the result toward the eye rather than trusting a convention.
vec3 face_normal(Globals g, vec3 world) {
    vec3 n = normalize(cross(dFdx(world), dFdy(world)));
    return dot(n, g.cam_pos.xyz - world) < 0.0 ? -n : n;
}

void main() {
    Globals g = globals[pc.globals].g[pc.gslot];
    vec3 n = face_normal(g, frag_world);
    float shadow = sun_shadow(g, frag_world);
    // Same expression the mesh pass uses, so level and models sit in the same
    // light instead of the level reading flat next to lit props.
    float diffuse = 0.35 + 0.65 * max(dot(n, g.sun_dir.xyz), 0.0) * shadow;
    out_color = vec4(frag_color * diffuse, 1.0);
}
