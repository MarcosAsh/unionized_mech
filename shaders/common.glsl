// Layouts and shading shared by the passes. Included rather than copied: every
// pass reads the same Globals buffer and the same draw records, so a struct
// that drifts in one of them reads garbage in that pass alone, which is a
// miserable bug to find.

#ifndef COMMON_GLSL
#define COMMON_GLSL

struct Globals {
    mat4 sun_view_proj;
    vec4 sun_dir;  // xyz normalized, toward the sun
    uint shadow_tex;
    uint level_tex;
    uint level_normal_tex;
    uint level_rough_tex;
    vec4 sky_color;  // also the clear colour and what fog fades toward
    vec4 cam_pos;    // eye position, for specular and fog distance
};

// Eight floats, matching asset::MeshVertex.
struct Vertex {
    float px, py, pz;
    float nx, ny, nz;
    float u, v;
};

// One queued draw, matching render::DrawRecord. The cull pass reads the bounds,
// the vertex passes read the transform, the fragment pass reads the material.
struct DrawRecord {
    mat4 model;
    vec4 rot;
    vec4 color;
    vec4 bounds_min;  // model-space box, w unused
    vec4 bounds_max;
    uint vbuf;
    uint tex;
    uint index_count;
    uint first_index;
    uint vertex_base;  // vertexOffset for per-frame skinned vertex slices
    float metallic;
    float roughness;
    uint pad0;
};

// Light bounced back up off the ground, for surfaces the sky cannot see.
const vec3 GROUND_BOUNCE = vec3(0.20, 0.18, 0.16);

const float AMBIENT_STRENGTH = 0.35;

// Sun radiance. Was 0.65 when diffuse was a bare N.L; the energy-conserving
// lobe divides by PI, so this carries the same factor to keep the exposure the
// old shading was tuned around.
const float SUN_RADIANCE = 2.04;

const float FOG_DENSITY = 0.0038;

const float PI = 3.14159265359;

// The level's own surface: concrete, so no metal and fairly rough.
const float LEVEL_METALLIC = 0.0;
const float LEVEL_ROUGHNESS = 0.85;

/// The u axis the level's geometry is built along, recovered from the face
/// normal. Every level surface is axis aligned and takes its texture
/// coordinates from a fixed per-face axis table, so the tangent frame can be
/// rebuilt from the normal instead of riding along on every vertex. This has to
/// stay the same table render_props.cpp builds with.
vec3 level_tangent(vec3 n) {
    if (abs(n.y) > 0.5) {
        return vec3(1.0, 0.0, 0.0);
    }
    if (abs(n.x) > 0.5) {
        return vec3(0.0, 0.0, -sign(n.x));
    }
    return vec3(sign(n.z), 0.0, 0.0);
}

/// Trowbridge-Reitz GGX: what share of microfacets face the half vector. `a` is
/// the squared perceptual roughness, which is what makes the roughness slider
/// feel linear rather than collapsing to a mirror over its bottom quarter.
float distribution_ggx(float ndh, float a) {
    float a2 = a * a;
    float d = ndh * ndh * (a2 - 1.0) + 1.0;
    return a2 / max(PI * d * d, 1e-7);
}

/// Smith geometry with the Schlick-GGX approximation: the share of those
/// microfacets that are neither shadowed nor masked by their neighbours.
float geometry_smith(float ndv, float ndl, float a) {
    float k = a * 0.5;  // direct lighting remap
    float gv = ndv / (ndv * (1.0 - k) + k);
    float gl = ndl / (ndl * (1.0 - k) + k);
    return gv * gl;
}

/// Schlick's Fresnel: how reflective the surface gets toward grazing angles.
vec3 fresnel_schlick(vec3 f0, float cos_theta) {
    return f0 + (1.0 - f0) * pow(clamp(1.0 - cos_theta, 0.0, 1.0), 5.0);
}

/// Narkowicz's ACES fit. The GGX lobe puts real energy above 1.0 where the old
/// Blinn-Phong never did, and clipping that to white flattens every highlight
/// into the same paper shape. EXPOSURE keeps mid grey where the lighting was
/// already tuned, so this adds the shoulder without moving the whole image.
/// The swapchain is _SRGB, so the hardware does the gamma encode after this.
const float EXPOSURE = 0.85;

vec3 tonemap(vec3 c) {
    c *= EXPOSURE;
    const float a = 2.51;
    const float b = 0.03;
    const float d = 2.43;
    const float e = 0.59;
    const float f = 0.14;
    return clamp((c * (a * c + b)) / (c * (d * c + e) + f), 0.0, 1.0);
}

/// The sky looking along `dir`. A flat clear colour gives no horizon and no
/// sense of which way is up; this deepens toward the zenith, pales toward the
/// horizon, and carries a broad glow around the sun. `sky_color` stays the
/// midpoint the ambient term and the fog are both tuned around.
const float SKY_ZENITH_SCALE = 0.58;   // deeper overhead
const float SKY_HORIZON_SCALE = 1.24;  // paler and hazier at eye level
const float SUN_GLOW = 0.35;

vec3 sky_toward(Globals g, vec3 dir) {
    vec3 base = g.sky_color.rgb;
    // Curved rather than linear in height, so the gradient concentrates near
    // the horizon where the eye reads it and stays flat overhead.
    float up = clamp(dir.y, 0.0, 1.0);
    float t = 1.0 - pow(1.0 - up, 3.0);
    vec3 col = base * mix(SKY_HORIZON_SCALE, SKY_ZENITH_SCALE, t);
    // Below the horizon the sky is ground haze, not sky, so it stops getting
    // paler and settles instead.
    col = mix(base * SKY_HORIZON_SCALE, col, step(0.0, dir.y));
    float sun = max(dot(normalize(dir), g.sun_dir.xyz), 0.0);
    col += vec3(1.0, 0.85, 0.6) * pow(sun, 8.0) * SUN_GLOW;
    return tonemap(col);
}

/// One opaque surface, lit by a Cook-Torrance GGX lobe from the sun plus a
/// hemisphere ambient. `n` must be normalized and `shadow` is 1 in full light,
/// 0 in full shade. Metals take their reflection colour from the albedo and
/// lose their diffuse, which is what stops metal reading as tinted plastic.
vec3 shade(Globals g, vec3 albedo, float metallic, float roughness, vec3 n, vec3 world,
           float shadow) {
    vec3 to_eye = normalize(g.cam_pos.xyz - world);
    vec3 l = g.sun_dir.xyz;
    vec3 h = normalize(l + to_eye);

    // Clamped off zero: at exactly zero roughness the lobe is a delta function
    // and the highlight vanishes into a single pixel that aliases horribly.
    float a = max(roughness * roughness, 2e-3);
    float ndl = max(dot(n, l), 0.0);
    float ndv = max(dot(n, to_eye), 1e-4);
    float ndh = max(dot(n, h), 0.0);
    float vdh = max(dot(to_eye, h), 0.0);

    // Dielectrics reflect about 4% head on; metals reflect their own colour.
    vec3 f0 = mix(vec3(0.04), albedo, metallic);
    vec3 fresnel = fresnel_schlick(f0, vdh);
    vec3 specular = (distribution_ggx(ndh, a) * geometry_smith(ndv, ndl, a) * fresnel) /
                    max(4.0 * ndv * ndl, 1e-4);

    // What is not reflected is available to scatter, and metals scatter none.
    vec3 diffuse_share = (1.0 - fresnel) * (1.0 - metallic);
    vec3 lit = (diffuse_share * albedo / PI + specular) * SUN_RADIANCE * ndl * shadow;

    // Hemisphere ambient: sky from above, ground bounce from below. A flat
    // ambient constant makes every unlit face the same shade and reads as fog
    // inside the geometry; this way facing still means something in shadow.
    vec3 irradiance = mix(GROUND_BOUNCE, g.sky_color.rgb, n.y * 0.5 + 0.5) * AMBIENT_STRENGTH;
    lit += diffuse_share * albedo * irradiance;

    // Standing in for an environment probe: the sky reflected at grazing
    // angles, faded out as the surface roughens. Without it metals have nothing
    // to reflect in shadow and go black.
    vec3 ambient_fresnel = f0 + (max(vec3(1.0 - roughness), f0) - f0) * pow(1.0 - ndv, 5.0);
    lit += ambient_fresnel * irradiance * (1.0 - roughness * 0.7);

    // Distance fog toward the sky colour, so distance becomes legible. It is
    // applied *after* the tonemap, not before: fog is a blend toward the sky as
    // displayed, and tonemapping it again would land distant geometry on a
    // different colour from the sky drawn behind it, leaving a seam at the
    // horizon.
    float fog = 1.0 - exp(-length(g.cam_pos.xyz - world) * FOG_DENSITY);
    return mix(tonemap(lit), sky_toward(g, -to_eye), fog);
}

#endif  // COMMON_GLSL
