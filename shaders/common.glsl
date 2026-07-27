// Shading shared by every opaque pass. Included rather than copied: all four
// fragment shaders read one Globals buffer, so a struct that drifts in one of
// them reads garbage in that pass alone, which is a miserable bug to find.

#ifndef COMMON_GLSL
#define COMMON_GLSL

struct Globals {
    mat4 sun_view_proj;
    vec4 sun_dir;  // xyz normalized, toward the sun
    uint shadow_tex;
    uint level_tex;
    uint pad0, pad1;
    vec4 sky_color;  // also the clear colour and what fog fades toward
    vec4 cam_pos;    // eye position, for specular and fog distance
};

// Light bounced back up off the ground, for surfaces the sky cannot see.
const vec3 GROUND_BOUNCE = vec3(0.20, 0.18, 0.16);

const float AMBIENT_STRENGTH = 0.35;
const float SUN_STRENGTH = 0.65;
const float SPEC_POWER = 48.0;
const float SPEC_STRENGTH = 0.25;
const float FOG_DENSITY = 0.0055;

/// One opaque surface, lit. `n` must be normalized and `shadow` is 1 in full
/// light, 0 in full shade.
vec3 shade(Globals g, vec3 albedo, vec3 n, vec3 world, float shadow) {
    vec3 to_eye = normalize(g.cam_pos.xyz - world);

    // Hemisphere ambient: sky from above, ground bounce from below. A flat
    // ambient constant makes every unlit face the same shade and reads as fog
    // inside the geometry; this way facing still means something in shadow.
    float up = n.y * 0.5 + 0.5;
    vec3 ambient = mix(GROUND_BOUNCE, g.sky_color.rgb, up) * AMBIENT_STRENGTH;

    float ndl = max(dot(n, g.sun_dir.xyz), 0.0);
    vec3 lit = albedo * (ambient + SUN_STRENGTH * ndl * shadow);

    // Blinn-Phong off the sun only. Gated on ndl so back faces cannot catch a
    // highlight the sun never reached.
    vec3 half_vec = normalize(g.sun_dir.xyz + to_eye);
    float spec = pow(max(dot(n, half_vec), 0.0), SPEC_POWER);
    lit += vec3(spec * SPEC_STRENGTH * shadow * step(0.0001, ndl));

    // Distance fog toward the sky colour, so the flat clear colour reads as
    // atmosphere rather than a void and distance becomes legible.
    float fog = 1.0 - exp(-length(g.cam_pos.xyz - world) * FOG_DENSITY);
    return mix(lit, g.sky_color.rgb, fog);
}

#endif  // COMMON_GLSL
