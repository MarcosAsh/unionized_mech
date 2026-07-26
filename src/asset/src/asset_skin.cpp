#include "asset_skin.h"

#include <cgltf.h>

namespace asset {

namespace {

constexpr u32 MAX_CLIPS = 16;

// The glTF skin index of `node`, or MAX_JOINTS when it is not a joint.
u32 joint_index_of(const cgltf_skin* skin, const cgltf_node* node) {
    for (u64 j = 0; j < skin->joints_count; ++j) {
        if (skin->joints[j] == node) {
            return static_cast<u32>(j);
        }
    }
    return anim::MAX_JOINTS;
}

// The engine index of the nearest ancestor of `node` that is also a joint.
i16 engine_parent_of(const cgltf_skin* skin, const cgltf_node* node,
                     const u32* gltf_to_engine) {
    for (const cgltf_node* p = node->parent; p != nullptr; p = p->parent) {
        const u32 g = joint_index_of(skin, p);
        if (g != anim::MAX_JOINTS) {
            return static_cast<i16>(gltf_to_engine[g]);
        }
    }
    return -1;
}

}  // namespace

bool import_skeleton(const cgltf_data* data, SkinImport* out) {
    if (data->skins_count == 0) {
        return false;
    }
    const cgltf_skin* skin = &data->skins[0];
    const u32 n = static_cast<u32>(skin->joints_count);
    if (n == 0 || n > anim::MAX_JOINTS) {
        return false;
    }

    // Assign engine indices so every parent lands before its children: repeat
    // passes, assigning any joint whose joint-parent is already assigned.
    bool assigned[anim::MAX_JOINTS] = {};
    u32 next = 0;
    while (next < n) {
        const u32 before = next;
        for (u32 g = 0; g < n; ++g) {
            if (assigned[g]) {
                continue;
            }
            const cgltf_node* node = skin->joints[g];
            u32 parent_g = anim::MAX_JOINTS;
            for (const cgltf_node* p = node->parent; p != nullptr; p = p->parent) {
                parent_g = joint_index_of(skin, p);
                if (parent_g != anim::MAX_JOINTS) {
                    break;
                }
            }
            if (parent_g == anim::MAX_JOINTS || assigned[parent_g]) {
                out->gltf_to_engine[g] = next++;
                assigned[g] = true;
            }
        }
        if (next == before) {
            return false;  // cyclic hierarchy, refuse
        }
    }

    anim::Skeleton& skel = out->skeleton;
    skel.joint_count = n;
    for (u32 g = 0; g < n; ++g) {
        const u32 e = out->gltf_to_engine[g];
        const cgltf_node* node = skin->joints[g];

        skel.parents[e] = engine_parent_of(skin, node, out->gltf_to_engine);

        if (skin->inverse_bind_matrices != nullptr) {
            cgltf_accessor_read_float(skin->inverse_bind_matrices, g, skel.inverse_bind[e].m, 16);
        }

        skel.rest_pos[e] = core::Vec3{};
        skel.rest_rot[e] = core::Quat{};
        skel.rest_scale[e] = core::Vec3{1.0f, 1.0f, 1.0f};
        if (node->has_translation) {
            skel.rest_pos[e] =
                core::Vec3{node->translation[0], node->translation[1], node->translation[2]};
        }
        if (node->has_rotation) {
            skel.rest_rot[e] = core::Quat{node->rotation[0], node->rotation[1], node->rotation[2],
                                          node->rotation[3]};
        }
        if (node->has_scale) {
            skel.rest_scale[e] = core::Vec3{node->scale[0], node->scale[1], node->scale[2]};
        }
    }
    out->has = true;
    return true;
}

core::Span<const anim::Clip> import_clips(const cgltf_data* data, const SkinImport& skin,
                                          core::Arena& arena) {
    if (!skin.has || data->animations_count == 0) {
        return core::Span<const anim::Clip>();
    }
    const cgltf_skin* gltf_skin = &data->skins[0];
    u32 clip_count = static_cast<u32>(data->animations_count);
    if (clip_count > MAX_CLIPS) {
        clip_count = MAX_CLIPS;
    }

    const core::Span<anim::Clip> clips = arena.alloc_n<anim::Clip>(clip_count);
    for (u32 c = 0; c < clip_count; ++c) {
        const cgltf_animation& src = data->animations[c];
        anim::Clip* clip = &clips[c];
        *clip = anim::Clip{};

        for (u64 ch = 0; ch < src.channels_count; ++ch) {
            const cgltf_animation_channel& channel = src.channels[ch];
            if (channel.target_node == nullptr || channel.sampler == nullptr) {
                continue;
            }
            // Cubic spline output is three elements per key; unsupported yet.
            if (channel.sampler->interpolation == cgltf_interpolation_type_cubic_spline) {
                continue;
            }
            const u32 g = joint_index_of(gltf_skin, channel.target_node);
            if (g == anim::MAX_JOINTS) {
                continue;  // animates a non-joint node
            }
            const u32 e = skin.gltf_to_engine[g];

            const cgltf_accessor* input = channel.sampler->input;
            const cgltf_accessor* output = channel.sampler->output;
            const u32 keys = static_cast<u32>(input->count);
            u32 stride = 0;
            anim::Track* track = nullptr;
            if (channel.target_path == cgltf_animation_path_type_translation) {
                track = &clip->translation[e];
                stride = 3;
            } else if (channel.target_path == cgltf_animation_path_type_rotation) {
                track = &clip->rotation[e];
                stride = 4;
            } else if (channel.target_path == cgltf_animation_path_type_scale) {
                track = &clip->scale[e];
                stride = 3;
            } else {
                continue;
            }

            const core::Span<f32> times = arena.alloc_n<f32>(keys);
            const core::Span<f32> values = arena.alloc_n<f32>(static_cast<u64>(keys) * stride);
            cgltf_accessor_unpack_floats(input, times.data(), keys);
            cgltf_accessor_unpack_floats(output, values.data(), values.size());
            track->times = core::Span<const f32>(times.data(), times.size());
            track->values = core::Span<const f32>(values.data(), values.size());

            if (keys > 0 && times[keys - 1] > clip->duration) {
                clip->duration = times[keys - 1];
            }
        }
    }
    return core::Span<const anim::Clip>(clips.data(), clip_count);
}

}  // namespace asset
